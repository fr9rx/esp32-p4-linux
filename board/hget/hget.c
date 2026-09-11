/*
 * hget -- a small, single-threaded HTTP/HTTPS client for NOMMU Linux.
 *
 * WHY THIS EXISTS
 *
 * The board had two HTTP clients and neither was right. busybox wget works
 * but has no TLS at all:
 *
 *     # wget -q -O /tmp/s.html https://example.com/
 *     wget: not an http or ftp url: https://example.com/
 *
 * and curl, at 1.36 MB, fails every single transfer with CURLE_OUT_OF_MEMORY
 * (exit 27) -- including file:///etc/fstab, which touches no network at all.
 * That failure survived every explanation tried against it: shrinking
 * RLIMIT_STACK from 8 MB to 128 KB (uClibc sizes pthread stacks from it),
 * bypassing the threaded resolver with --resolve, and raising the bFLT stack
 * from 4 KB to 256 KB. All three made no difference.
 *
 * So rather than keep bisecting a 1.36 MB binary over a serial console, this
 * does the one job the board actually needs, in a way that suits the target:
 *
 *   SINGLE-THREADED. No pthreads, no fork. getaddrinfo() on uClibc is
 *   synchronous, so name resolution needs no helper thread. On NOMMU a
 *   thread is not free -- fork() does not exist at all (the kernel returns
 *   -EINVAL) and every thread stack is one more contiguous allocation out of
 *   a 10 MB pool.
 *
 *   ALMOST HEAP-FREE. The buffers are static. The only allocation is the
 *   mbedTLS context for https, and that only when https is used. A tool that
 *   barely mallocs cannot fail the way curl is failing, whatever the root
 *   cause turns out to be.
 *
 *   ONE CONTIGUOUS EXEC. binfmt_flat needs one physically contiguous
 *   allocation per exec, and that allocator tops out at MAX_PAGE_ORDER=10,
 *   i.e. 4 MB. Small matters here for whether the thing runs at all, not
 *   just for flash.
 *
 * NOT A CURL REPLACEMENT. It does GET over HTTP/1.1 and HTTPS, follows
 * redirects, and writes a file. No POST, no auth, no cookies, no proxies.
 * That is the whole brief.
 *
 * BUILDING. Compiled by board/post-build.sh with the buildroot toolchain, so
 * it is part of the normal build and lands in the image like any package:
 *
 *     riscv32-...-gcc -Os -fPIC -Wl,-elf2flt="-r -s131072" ...
 *
 * The -fPIC and -Wl,-elf2flt are what make it a bFLT binary rather than an
 * ELF the kernel cannot load; see package/Makefile.in in buildroot. The
 * explicit stack size is deliberate -- elf2flt's default is 4 KB, which is
 * what curl got, and a TLS handshake does not fit in 4 KB.
 */

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HGET_TLS
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#endif

#define HGET_VERSION "1.0"

/* Exit codes, deliberately distinct so a script can tell what went wrong.
 * curl's habit of reporting everything as one number is what made its own
 * failure so hard to place. */
enum {
	EX_OK		= 0,
	EX_USAGE	= 1,
	EX_URL		= 2,	/* could not parse the URL */
	EX_RESOLVE	= 3,	/* name did not resolve */
	EX_CONNECT	= 4,	/* could not connect */
	EX_TLS		= 5,	/* handshake or certificate failure */
	EX_HTTP		= 6,	/* server answered >= 400 */
	EX_IO		= 7,	/* local read/write failure */
	EX_REDIRECT	= 8,	/* too many redirects */
	EX_PROTO	= 9	/* malformed response */
};

#define MAX_REDIRECTS	8
#define URLBUF		2048
#define HOSTBUF		256
#define IOBUF		16384

static int opt_quiet;
static int opt_insecure;
static int opt_follow = 1;
static int opt_timeout = 30;
static const char *opt_cafile = "/etc/ssl/certs/ca-certificates.crt";

static char iobuf[IOBUF];
static char hdrbuf[8192];

static void msg(const char *fmt, ...)
{
	va_list ap;

	if (opt_quiet)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void err(const char *fmt, ...)
{
	va_list ap;

	fputs("hget: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

/* ------------------------------------------------------------------
 * Connection: a plain fd, optionally with TLS layered on top.
 *
 * Both paths share one fd so the socket setup, timeouts and teardown are
 * written once. mbedTLS is driven through set_bio over that same fd rather
 * than through mbedtls_net_connect, which keeps the connect logic (and its
 * timeout) in one place and avoids reaching into mbedTLS 3.x private struct
 * members.
 * --------------------------------------------------------------- */

struct conn {
	int fd;
	int tls;
#ifdef HGET_TLS
	mbedtls_ssl_context ssl;
	mbedtls_ssl_config conf;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt ca;
#endif
};

#ifdef HGET_TLS
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t n = send(fd, buf, len, 0);

	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		return MBEDTLS_ERR_NET_SEND_FAILED;
	}
	return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
	int fd = *(int *)ctx;
	ssize_t n = recv(fd, buf, len, 0);

	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return MBEDTLS_ERR_SSL_WANT_READ;
		return MBEDTLS_ERR_NET_RECV_FAILED;
	}
	return (int)n;
}
#endif

/* Connect by RACING every address at once, not by trying them in turn.
 *
 * A blocking connect() can sit for well over a minute on a dropped SYN, which
 * on a board whose only console is a serial line reads as a hang. So these are
 * non-blocking connects driven by poll(), which keeps -T honest without a
 * signal or a second thread.
 *
 * WHY RACE THEM. Trying addresses one at a time, each with the full timeout,
 * is correct and unusably slow the moment one family is broken -- and here one
 * is. This board gets a global IPv6 address by SLAAC from a router that then
 * advertises no usable default route:
 *
 *   # ip -6 route            -> on-link /64 only, no default
 *   # ping6 <an example.com address>  -> dead
 *
 * getaddrinfo() returns the AAAA records first, because RFC 6724 says to
 * prefer IPv6. So a serial walk spent 30 s per dead IPv6 address before
 * reaching a working IPv4 one:
 *
 *   hget http://192.168.1.1/   0.15 s   (IPv4 literal, no DNS)
 *   hget http://example.com/  60.08 s   (two dead AAAA, then an A)
 *
 * Starting every connect together and taking the first to complete is the
 * same idea as Happy Eyeballs (RFC 8305) without the staggered start -- which
 * exists to spare the network a burst of parallel SYNs, a consideration that
 * does not apply to a handful of addresses from one small board. The dead
 * family now costs nothing: the working socket finishes while the others are
 * still waiting to time out, and they are closed unconnected.
 */
#define MAX_ADDRS 8

static int tcp_connect(const char *host, const char *port)
{
	struct addrinfo hints, *res, *ai;
	struct addrinfo *sel[MAX_ADDRS];
	struct pollfd pfd[MAX_ADDRS];
	int flags[MAX_ADDRS];
	int nsel = 0, isel;
	int nfd = 0, winner = -1;
	long deadline_ms = (long)opt_timeout * 1000;
	int i, rc;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	rc = getaddrinfo(host, port, &hints, &res);
	if (rc) {
		err("cannot resolve %s: %s", host, gai_strerror(rc));
		return -EX_RESOLVE;
	}

	/* Build the candidate list by ALTERNATING address families.
	 *
	 * Taking the first MAX_ADDRS entries in the order getaddrinfo returns
	 * them is not good enough, because that order is all of one family
	 * before any of the other. www.google.com resolves to sixteen
	 * addresses here -- eight AAAA, then eight A:
	 *
	 *   Address 1..8   (null)            <- AAAA, and IPv6 is dead here
	 *   Address 9..16  142.251.15x.119   <- A, every one of them reachable
	 *
	 * so a cap of eight filled up entirely with unreachable IPv6 and the
	 * connect failed after the full timeout without ever trying IPv4.
	 * example.com returns only four (2 + 2), stayed under the cap, and
	 * therefore worked -- which is what made this look like a
	 * site-specific problem rather than a cap.
	 *
	 * RFC 8305 section 4 calls for alternating families for this reason.
	 * Interleaving means the cap can never exclude a whole family: with
	 * both present, each gets half the slots.
	 */
	{
		struct addrinfo *fam[2][MAX_ADDRS];
		int nf[2] = { 0, 0 };
		int slot, f;

		for (ai = res; ai; ai = ai->ai_next) {
			f = (ai->ai_family == AF_INET6) ? 0 : 1;
			if (nf[f] < MAX_ADDRS)
				fam[f][nf[f]++] = ai;
		}
		nsel = 0;
		for (slot = 0; slot < MAX_ADDRS && nsel < MAX_ADDRS; slot++)
			for (f = 0; f < 2 && nsel < MAX_ADDRS; f++)
				if (slot < nf[f])
					sel[nsel++] = fam[f][slot];
	}

	for (isel = 0; isel < nsel && nfd < MAX_ADDRS; isel++) {
		int fd;

		ai = sel[isel];
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		flags[nfd] = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags[nfd] | O_NONBLOCK);

		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			/* Rare, but a loopback or on-link peer can finish
			 * immediately. Take it and stop. */
			fcntl(fd, F_SETFL, flags[nfd]);
			winner = fd;
			break;
		}
		if (errno != EINPROGRESS) {
			close(fd);
			continue;
		}
		pfd[nfd].fd = fd;
		pfd[nfd].events = POLLOUT;
		pfd[nfd].revents = 0;
		nfd++;
	}

	while (winner < 0 && nfd > 0 && deadline_ms > 0) {
		int slice = deadline_ms > 1000 ? 1000 : (int)deadline_ms;

		rc = poll(pfd, (unsigned)nfd, slice);
		deadline_ms -= slice;
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (rc == 0)
			continue;

		for (i = 0; i < nfd; i++) {
			int soerr = 0;
			socklen_t slen = sizeof(soerr);

			if (!pfd[i].revents)
				continue;
			if (getsockopt(pfd[i].fd, SOL_SOCKET, SO_ERROR,
				       &soerr, &slen) == 0 && soerr == 0 &&
			    !(pfd[i].revents & (POLLERR | POLLHUP))) {
				fcntl(pfd[i].fd, F_SETFL, flags[i]);
				winner = pfd[i].fd;
				pfd[i].fd = -1;	/* do not close the winner */
				break;
			}
			/* This one failed. Drop it and keep racing. */
			close(pfd[i].fd);
			pfd[i] = pfd[nfd - 1];
			flags[i] = flags[nfd - 1];
			nfd--;
			i--;
		}
	}

	for (i = 0; i < nfd; i++)
		if (pfd[i].fd >= 0 && pfd[i].fd != winner)
			close(pfd[i].fd);

	freeaddrinfo(res);

	if (winner < 0) {
		err("cannot connect to %s:%s", host, port);
		return -EX_CONNECT;
	}
	/* Once connected, bound every read and write the same way. */
	{
		struct timeval tv;

		tv.tv_sec = opt_timeout;
		tv.tv_usec = 0;
		setsockopt(winner, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(winner, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	}
	return winner;
}

static int conn_open(struct conn *c, const char *host, const char *port, int tls)
{
	int rc;

	memset(c, 0, sizeof(*c));
	c->fd = tcp_connect(host, port);
	if (c->fd < 0)
		return -c->fd;
	c->tls = tls;
	if (!tls)
		return EX_OK;

#ifndef HGET_TLS
	err("built without TLS support, cannot fetch https");
	close(c->fd);
	return EX_TLS;
#else
	mbedtls_ssl_init(&c->ssl);
	mbedtls_ssl_config_init(&c->conf);
	mbedtls_entropy_init(&c->entropy);
	mbedtls_ctr_drbg_init(&c->drbg);
	mbedtls_x509_crt_init(&c->ca);

	rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->entropy,
				   (const unsigned char *)"hget", 4);
	if (rc) {
		err("cannot seed RNG (-0x%04x)", -rc);
		goto fail;
	}

	rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
					 MBEDTLS_SSL_TRANSPORT_STREAM,
					 MBEDTLS_SSL_PRESET_DEFAULT);
	if (rc) {
		err("TLS config failed (-0x%04x)", -rc);
		goto fail;
	}

	if (opt_insecure) {
		mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
	} else {
		/* Verify by default, and say plainly when there is nothing to
		 * verify against rather than quietly downgrading. A client
		 * that silently accepts any certificate is worse than one
		 * that refuses, because it looks like it is working. */
		rc = mbedtls_x509_crt_parse_file(&c->ca, opt_cafile);
		if (rc < 0) {
			err("no CA bundle at %s -- install ca-certificates, "
			    "or pass -k to skip verification", opt_cafile);
			goto fail;
		}
		mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
		mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
	}

	mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

	rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
	if (rc) {
		err("TLS setup failed (-0x%04x)", -rc);
		goto fail;
	}
	/* SNI, and the name the certificate is checked against. */
	rc = mbedtls_ssl_set_hostname(&c->ssl, host);
	if (rc) {
		err("cannot set TLS hostname (-0x%04x)", -rc);
		goto fail;
	}

	mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);

	while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
		if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
		    rc == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;

		/* Say WHY, not just that it failed.
		 *
		 * A verification failure aborts the handshake, so the check
		 * further down -- which only runs on a *successful* one --
		 * never reports the reason. The first version of this printed
		 * nothing but "TLS handshake failed (-0x2700)" for every
		 * https URL on the board, valid certificates included, and
		 * that hex code is the same whether the certificate is
		 * expired, self-signed, for the wrong host, or perfectly good
		 * and merely being judged by a clock that reads 1970. */
		if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
			uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
			char why[320];

			mbedtls_x509_crt_verify_info(why, sizeof(why), "  ",
						     flags);
			err("certificate rejected:\n%s", why);

			/* This board has no RTC, so it starts at the epoch and
			 * every certificate on the internet is "not yet
			 * valid". It is the single most likely cause here and
			 * the least obvious, so name it outright. */
			if (flags & (MBEDTLS_X509_BADCERT_FUTURE |
				     MBEDTLS_X509_BADCERT_EXPIRED)) {
				time_t now = time(NULL);

				err("the clock reads %.24s -- if that is wrong,"
				    " set it (date -s) and retry; there is no"
				    " RTC on this board", ctime(&now));
			}
		} else {
			err("TLS handshake failed (-0x%04x)", -rc);
		}
		goto fail;
	}

	{
		uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);

		if (flags && !opt_insecure) {
			char why[256];

			mbedtls_x509_crt_verify_info(why, sizeof(why), "  ",
						     flags);
			err("certificate rejected:\n%s", why);
			goto fail;
		}
	}
	msg("* TLS %s, %s\n", mbedtls_ssl_get_version(&c->ssl),
	    mbedtls_ssl_get_ciphersuite(&c->ssl));
	return EX_OK;

fail:
	close(c->fd);
	c->fd = -1;
	return EX_TLS;
#endif
}

static int conn_write(struct conn *c, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		int n;

#ifdef HGET_TLS
		if (c->tls) {
			n = mbedtls_ssl_write(&c->ssl,
					      (const unsigned char *)buf + off,
					      len - off);
			if (n == MBEDTLS_ERR_SSL_WANT_READ ||
			    n == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
		} else
#endif
		{
			ssize_t w = send(c->fd, buf + off, len - off, 0);

			if (w < 0 && errno == EINTR)
				continue;
			n = (int)w;
		}
		if (n <= 0)
			return -1;
		off += (size_t)n;
	}
	return 0;
}

/* Returns bytes read, 0 at end of stream, -1 on error. */
static int conn_read(struct conn *c, char *buf, size_t len)
{
	int n;

#ifdef HGET_TLS
	if (c->tls) {
		do {
			n = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, len);
		} while (n == MBEDTLS_ERR_SSL_WANT_READ ||
			 n == MBEDTLS_ERR_SSL_WANT_WRITE);
		if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
			return 0;
		if (n < 0)
			return -1;
		return n;
	}
#endif
	for (;;) {
		ssize_t r = recv(c->fd, buf, len, 0);

		if (r < 0 && errno == EINTR)
			continue;
		n = (int)r;
		break;
	}
	return n < 0 ? -1 : n;
}

static void conn_close(struct conn *c)
{
#ifdef HGET_TLS
	if (c->tls) {
		mbedtls_ssl_close_notify(&c->ssl);
		mbedtls_ssl_free(&c->ssl);
		mbedtls_ssl_config_free(&c->conf);
		mbedtls_x509_crt_free(&c->ca);
		mbedtls_ctr_drbg_free(&c->drbg);
		mbedtls_entropy_free(&c->entropy);
	}
#endif
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
}

/* ------------------------------------------------------------------
 * URL handling
 * --------------------------------------------------------------- */

struct url {
	int tls;
	char host[HOSTBUF];
	char port[8];
	char path[URLBUF];
};

static int url_parse(const char *s, struct url *u)
{
	const char *p, *slash, *colon;
	size_t hostlen;

	memset(u, 0, sizeof(*u));

	if (!strncmp(s, "http://", 7)) {
		u->tls = 0;
		strcpy(u->port, "80");
		p = s + 7;
	} else if (!strncmp(s, "https://", 8)) {
		u->tls = 1;
		strcpy(u->port, "443");
		p = s + 8;
	} else {
		err("only http:// and https:// URLs are supported");
		return EX_URL;
	}

	slash = strchr(p, '/');
	if (slash) {
		hostlen = (size_t)(slash - p);
		if (strlen(slash) >= sizeof(u->path)) {
			err("path too long");
			return EX_URL;
		}
		strcpy(u->path, slash);
	} else {
		hostlen = strlen(p);
		strcpy(u->path, "/");
	}

	if (!hostlen || hostlen >= sizeof(u->host)) {
		err("bad or missing host in URL");
		return EX_URL;
	}
	memcpy(u->host, p, hostlen);
	u->host[hostlen] = '\0';

	/* A colon in the host means an explicit port. Skip IPv6 literals in
	 * brackets, where colons are part of the address. */
	if (u->host[0] != '[') {
		colon = strchr(u->host, ':');
		if (colon) {
			if (strlen(colon + 1) >= sizeof(u->port)) {
				err("bad port in URL");
				return EX_URL;
			}
			strcpy(u->port, colon + 1);
			u->host[colon - u->host] = '\0';
		}
	} else {
		char *close_br = strchr(u->host, ']');

		if (!close_br) {
			err("unterminated IPv6 literal in URL");
			return EX_URL;
		}
		if (close_br[1] == ':') {
			if (strlen(close_br + 2) >= sizeof(u->port)) {
				err("bad port in URL");
				return EX_URL;
			}
			strcpy(u->port, close_br + 2);
		}
		*close_br = '\0';
		memmove(u->host, u->host + 1, strlen(u->host));
	}
	return EX_OK;
}

/* ------------------------------------------------------------------
 * HTTP
 * --------------------------------------------------------------- */

struct resp {
	int status;
	long long length;	/* -1 when not given */
	int chunked;
	char location[URLBUF];
	size_t body_off;	/* bytes of body already sitting in hdrbuf */
	size_t body_len;
};

static int ci_prefix(const char *s, const char *pfx)
{
	size_t n = strlen(pfx);
	size_t i;

	for (i = 0; i < n; i++) {
		char a = s[i], b = pfx[i];

		if (a >= 'A' && a <= 'Z')
			a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z')
			b = (char)(b - 'A' + 'a');
		if (a != b)
			return 0;
	}
	return 1;
}

static const char *skip_ws(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

/* Read until the blank line that ends the headers, then parse them. Whatever
 * body bytes arrived in the same read are kept in hdrbuf and handed on --
 * throwing them away is a classic way to lose the first few KB of a
 * response. */
static int read_headers(struct conn *c, struct resp *r)
{
	size_t have = 0;
	char *end = NULL;
	char *line;

	memset(r, 0, sizeof(*r));
	r->length = -1;

	while (have < sizeof(hdrbuf) - 1) {
		int n;

		end = memmem(hdrbuf, have, "\r\n\r\n", 4);
		if (end)
			break;
		n = conn_read(c, hdrbuf + have, sizeof(hdrbuf) - 1 - have);
		if (n < 0) {
			err("read failed while reading headers");
			return EX_IO;
		}
		if (n == 0)
			break;
		have += (size_t)n;
	}
	if (!end)
		end = memmem(hdrbuf, have, "\r\n\r\n", 4);
	if (!end) {
		err("malformed response: no end of headers");
		return EX_PROTO;
	}
	hdrbuf[have] = '\0';

	r->body_off = (size_t)(end - hdrbuf) + 4;
	r->body_len = have - r->body_off;

	if (strncmp(hdrbuf, "HTTP/", 5)) {
		err("malformed response: not HTTP");
		return EX_PROTO;
	}
	line = strchr(hdrbuf, ' ');
	if (!line) {
		err("malformed status line");
		return EX_PROTO;
	}
	r->status = atoi(line + 1);

	/* Walk the header lines. end still marks the terminator, and the
	 * region before it is NUL-safe because the read loop bounded it. */
	*end = '\0';
	line = strstr(hdrbuf, "\r\n");
	while (line) {
		char *next;

		line += 2;
		next = strstr(line, "\r\n");
		if (next)
			*next = '\0';

		if (ci_prefix(line, "content-length:")) {
			r->length = atoll(skip_ws(line + 15));
		} else if (ci_prefix(line, "transfer-encoding:")) {
			if (strstr(skip_ws(line + 18), "chunked"))
				r->chunked = 1;
		} else if (ci_prefix(line, "location:")) {
			const char *v = skip_ws(line + 9);

			if (strlen(v) < sizeof(r->location))
				strcpy(r->location, v);
		}

		if (next) {
			*next = '\r';
			line = next;
		} else {
			line = NULL;
		}
	}
	*end = '\r';
	return EX_OK;
}

static int write_all(int fd, const char *buf, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

/* Chunked bodies, because an HTTP/1.1 server may use them for anything of
 * unknown length and there is no way to opt out except by asking for 1.0,
 * which loses Host-based virtual hosting on some servers. */
static int body_chunked(struct conn *c, struct resp *r, int out,
			long long *written)
{
	char *pend = hdrbuf + r->body_off;
	size_t pending = r->body_len;

	for (;;) {
		char line[64];
		size_t li = 0;
		long chunk;

		/* Pull one chunk-size line, a byte at a time. Chunk headers
		 * are tiny and rare compared with payload, so simplicity
		 * beats buffering cleverness here. */
		for (;;) {
			char ch;

			if (pending) {
				ch = *pend++;
				pending--;
			} else {
				int n = conn_read(c, &ch, 1);

				if (n <= 0)
					return EX_IO;
			}
			if (ch == '\n')
				break;
			if (ch != '\r' && li < sizeof(line) - 1)
				line[li++] = ch;
		}
		line[li] = '\0';
		if (!li)
			continue;	/* the CRLF that follows a chunk */

		chunk = strtol(line, NULL, 16);
		if (chunk <= 0)
			break;		/* 0 = last chunk */

		while (chunk > 0) {
			size_t want = (size_t)(chunk < (long)sizeof(iobuf)
					       ? chunk : (long)sizeof(iobuf));
			size_t got;

			if (pending) {
				got = pending < want ? pending : want;
				memcpy(iobuf, pend, got);
				pend += got;
				pending -= got;
			} else {
				int n = conn_read(c, iobuf, want);

				if (n <= 0)
					return EX_IO;
				got = (size_t)n;
			}
			if (write_all(out, iobuf, got))
				return EX_IO;
			*written += (long long)got;
			chunk -= (long)got;
		}
	}
	return EX_OK;
}

static int body_plain(struct conn *c, struct resp *r, int out,
		      long long *written)
{
	long long remain = r->length;

	if (r->body_len) {
		if (write_all(out, hdrbuf + r->body_off, r->body_len))
			return EX_IO;
		*written += (long long)r->body_len;
		if (remain >= 0)
			remain -= (long long)r->body_len;
	}

	while (remain != 0) {
		size_t want = sizeof(iobuf);
		int n;

		if (remain > 0 && remain < (long long)want)
			want = (size_t)remain;
		n = conn_read(c, iobuf, want);
		if (n < 0)
			return EX_IO;
		if (n == 0)
			break;		/* EOF-delimited body, or short */
		if (write_all(out, iobuf, (size_t)n))
			return EX_IO;
		*written += n;
		if (remain > 0)
			remain -= n;
	}
	return EX_OK;
}

static int fetch(const char *urlstr, const char *outpath, int depth)
{
	struct url u;
	struct conn c;
	struct resp r;
	char req[URLBUF + 512];
	int out = STDOUT_FILENO;
	long long written = 0;
	int rc, len;

	if (depth > MAX_REDIRECTS) {
		err("too many redirects");
		return EX_REDIRECT;
	}

	rc = url_parse(urlstr, &u);
	if (rc)
		return rc;

	msg("* %s:%s%s\n", u.host, u.port, u.tls ? " (TLS)" : "");

	rc = conn_open(&c, u.host, u.port, u.tls);
	if (rc)
		return rc;

	len = snprintf(req, sizeof(req),
		       "GET %s HTTP/1.1\r\n"
		       "Host: %s\r\n"
		       "User-Agent: hget/" HGET_VERSION "\r\n"
		       "Accept: */*\r\n"
		       "Connection: close\r\n"
		       "\r\n",
		       u.path, u.host);
	if (len <= 0 || (size_t)len >= sizeof(req)) {
		err("request too long");
		conn_close(&c);
		return EX_URL;
	}
	if (conn_write(&c, req, (size_t)len)) {
		err("failed to send request");
		conn_close(&c);
		return EX_IO;
	}

	rc = read_headers(&c, &r);
	if (rc) {
		conn_close(&c);
		return rc;
	}
	msg("< HTTP %d\n", r.status);

	/* Redirects. Only absolute Locations are followed; a relative one
	 * would need URL joining, and every server that matters sends an
	 * absolute URL here. */
	if (r.status >= 300 && r.status < 400 && r.location[0] && opt_follow) {
		char next[URLBUF];

		if (strncmp(r.location, "http://", 7) &&
		    strncmp(r.location, "https://", 8)) {
			err("relative redirect to '%s' not supported",
			    r.location);
			conn_close(&c);
			return EX_REDIRECT;
		}
		strcpy(next, r.location);
		conn_close(&c);
		msg("* redirect -> %s\n", next);
		return fetch(next, outpath, depth + 1);
	}

	if (r.status >= 400) {
		err("server returned HTTP %d", r.status);
		conn_close(&c);
		return EX_HTTP;
	}

	if (outpath && strcmp(outpath, "-")) {
		out = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) {
			err("cannot open %s: %s", outpath, strerror(errno));
			conn_close(&c);
			return EX_IO;
		}
	}

	if (r.chunked)
		rc = body_chunked(&c, &r, out, &written);
	else
		rc = body_plain(&c, &r, out, &written);

	if (out != STDOUT_FILENO)
		close(out);
	conn_close(&c);

	if (rc) {
		err("transfer failed after %lld bytes", written);
		return rc;
	}
	msg("* %lld bytes\n", written);
	return EX_OK;
}

static void usage(void)
{
	fprintf(stderr,
		"hget " HGET_VERSION " -- single-threaded HTTP/HTTPS GET\n"
		"\n"
		"usage: hget [options] URL\n"
		"\n"
		"  -o FILE   write body to FILE ('-' for stdout, the default)\n"
		"  -q        quiet: no progress on stderr\n"
		"  -k        do not verify the server certificate\n"
		"  -L        follow redirects (default)\n"
		"  -N        do not follow redirects\n"
		"  -T SEC    connect and I/O timeout, default 30\n"
		"  -c FILE   CA bundle (default %s)\n"
		"  -V        version\n"
		"\n"
		"exit: 0 ok, 2 bad url, 3 resolve, 4 connect, 5 tls,\n"
		"      6 http >=400, 7 i/o, 8 redirects, 9 bad response\n",
		opt_cafile);
}

int main(int argc, char **argv)
{
	const char *outpath = NULL;
	int i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] != '-' || !a[1])
			break;
		if (!strcmp(a, "-o") && i + 1 < argc)
			outpath = argv[++i];
		else if (!strcmp(a, "-T") && i + 1 < argc)
			opt_timeout = atoi(argv[++i]);
		else if (!strcmp(a, "-c") && i + 1 < argc)
			opt_cafile = argv[++i];
		else if (!strcmp(a, "-q"))
			opt_quiet = 1;
		else if (!strcmp(a, "-k"))
			opt_insecure = 1;
		else if (!strcmp(a, "-L"))
			opt_follow = 1;
		else if (!strcmp(a, "-N"))
			opt_follow = 0;
		else if (!strcmp(a, "-V")) {
			printf("hget " HGET_VERSION
#ifdef HGET_TLS
			       " (mbedTLS " MBEDTLS_VERSION_STRING ")"
#else
			       " (no TLS)"
#endif
			       "\n");
			return EX_OK;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return EX_OK;
		} else {
			err("unknown option %s", a);
			usage();
			return EX_USAGE;
		}
	}

	if (i >= argc) {
		usage();
		return EX_USAGE;
	}
	if (opt_timeout <= 0)
		opt_timeout = 30;

	return fetch(argv[i], outpath, 0);
}
