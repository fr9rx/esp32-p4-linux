/* UART0 console for the ESP32-P4 second-stage bootloader.
 *
 * Register facts from ESP-IDF v6.1-beta1
 * components/soc/esp32p4/register/hw_ver1/soc/{uart_reg.h,hp_sys_clkrst_reg.h};
 * the divisor arithmetic mirrors _uart_ll_set_baudrate() in
 * components/esp_hal_uart/esp32p4/include/hal/uart_ll.h.
 *
 * Written against the register definitions rather than by vendoring uart_ll.h,
 * because that header reaches the registers through struct overlays that would
 * drag in the whole hp_sys_clkrst/uart struct set for what amounts to five
 * writes. The arithmetic is reproduced exactly, and the result is verifiable
 * immediately: the console either speaks at the new rate or it does not.
 */

#include <stdint.h>
#include <stdbool.h>

#include "p4_uart.h"

#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))

/* DR_REG_HPPERIPH1_BASE 0x500C0000 + 0xA000 */
#define UART0_BASE              0x500CA000u
#define UART_CLKDIV_SYNC_REG    (UART0_BASE + 0x14)
#define UART_STATUS_REG         (UART0_BASE + 0x1c)
#define UART_REG_UPDATE_REG     (UART0_BASE + 0x98)

#define UART_CLKDIV_M           0x00000FFFu   /* [11:0]  integer part   */
#define UART_CLKDIV_S           0
#define UART_CLKDIV_FRAG_M      0x0000000Fu   /* [23:20] 1/16ths        */
#define UART_CLKDIV_FRAG_S      20
#define UART_REG_UPDATE         (1u << 0)     /* R/W/SC, self-clearing  */

/* TXFIFO_CNT is 8 bits at shift 16, i.e. [23:16].
 *
 * Noted because docs/NATIVE-DRIVERS.md in this repo says [25:16], which is
 * wrong -- uart_reg.h defines UART_TXFIFO_CNT as 0xFF with _S 16. The stub in
 * native/linux-native/stub/stub.S has it right. */
#define UART_TXFIFO_CNT_M       0x000000FFu
#define UART_TXFIFO_CNT_S       16

/* DR_REG_HPPERIPH1_BASE + 0x26000 */
#define HP_SYS_CLKRST_BASE      0x500E6000u
/* ctrl110 holds the source select; ctrl111 holds the pre-divider. They are
 * genuinely in different registers -- easy to conflate, and conflating them
 * silently gives the wrong baud. */
#define PERI_CLK_CTRL110        (HP_SYS_CLKRST_BASE + 0x68)
#define PERI_CLK_CTRL111        (HP_SYS_CLKRST_BASE + 0x6c)

#define UART0_CLK_SRC_SEL_M     0x3u          /* ctrl110 [25:24] */
#define UART0_CLK_SRC_SEL_S     24
#define UART0_CLK_EN            (1u << 26)    /* ctrl110         */
#define UART0_SCLK_DIV_NUM_M    0xFFu         /* ctrl111 [7:0]   */
#define UART0_SCLK_DIV_NUM_S    0

#define UART0_CLK_SRC_XTAL      0u            /* uart_ll_set_sclk encoding */

/* ROM: waits for the shift register to empty, not merely the FIFO. */
extern void uart_tx_wait_idle(uint8_t uart_no);

bool p4_uart0_set_baud(uint32_t baud, uint32_t sclk_hz)
{
    if (baud == 0u) {
        return false;
    }

    /* The integer divisor is 12 bits, so very low baud rates from a fast
     * source need the sclk pre-divider to take up the slack first. Same
     * DIV_UP as IDF. For 40 MHz -> 4 Mbps this comes out as 1 (no
     * pre-division) and the whole ratio lands in the UART's own divisor. */
    const uint32_t max_div = UART_CLKDIV_M;
    uint32_t sclk_div = (sclk_hz + (uint32_t)((uint64_t)max_div * baud) - 1u)
                        / (uint32_t)((uint64_t)max_div * baud);
    if (sclk_div == 0u || sclk_div > (UART0_SCLK_DIV_NUM_M + 1u)) {
        return false;
    }

    /* clk_div is in 1/16ths: integer part in [11:0], fraction in [23:20].
     * 40e6 -> 4e6 gives exactly 160, i.e. 10 and 0/16 -- no fractional part
     * at all, which 115200 cannot manage (it needs 347 + 3/16). */
    uint32_t clk_div = (uint32_t)(((uint64_t)sclk_hz << 4) / (baud * sclk_div));
    uint32_t clkdiv_int = clk_div >> 4;
    uint32_t clkdiv_frag = clk_div & 0xfu;
    if (clkdiv_int > UART_CLKDIV_M) {
        return false;
    }

    /* Let everything already queued get out at the OLD rate. Without this the
     * tail of the last message is shifted out with the new divisor and
     * arrives as noise -- and it is the message describing the change. */
    uart_tx_wait_idle(0);

    /* Source stays XTAL. Its select value is 0, which is also the reset
     * default, so this is a re-assertion rather than a change -- but it is
     * cheap, and it documents the dependency the DTS relies on. */
    uint32_t c110 = REG32(PERI_CLK_CTRL110);
    c110 &= ~(UART0_CLK_SRC_SEL_M << UART0_CLK_SRC_SEL_S);
    c110 |= (UART0_CLK_SRC_XTAL << UART0_CLK_SRC_SEL_S);
    c110 |= UART0_CLK_EN;
    REG32(PERI_CLK_CTRL110) = c110;

    uint32_t c111 = REG32(PERI_CLK_CTRL111);
    c111 &= ~(UART0_SCLK_DIV_NUM_M << UART0_SCLK_DIV_NUM_S);
    c111 |= ((sclk_div - 1u) & UART0_SCLK_DIV_NUM_M) << UART0_SCLK_DIV_NUM_S;
    REG32(PERI_CLK_CTRL111) = c111;

    REG32(UART_CLKDIV_SYNC_REG) =
        ((clkdiv_int  & UART_CLKDIV_M)      << UART_CLKDIV_S) |
        ((clkdiv_frag & UART_CLKDIV_FRAG_M) << UART_CLKDIV_FRAG_S);

    /* clkdiv_sync is one of the P4's clock-domain-crossed registers: the
     * write lands in a shadow and only reaches the UART core when this bit is
     * poked. Skip it and the divisor silently does not change. The bit is
     * self-clearing, so waiting on it is how we know the crossing completed. */
    REG32(UART_REG_UPDATE_REG) = UART_REG_UPDATE;
    while (REG32(UART_REG_UPDATE_REG) & UART_REG_UPDATE) {
        /* hardware clears it */
    }

    return true;
}
