/* EMAC bring-up, following ESP-IDF's own sequence.
 *
 * The recipe is emac_ll_clock_enable_rmii_input() plus
 * emac_ll_enable_bus_clock() and emac_ll_reset_register() from
 * components/esp_hal_emac/esp32p4/include/hal/emac_ll.h, written out against
 * the vendored register headers instead of IDF's bitfield structs. Doing it
 * by hand rather than vendoring emac_ll.h avoids dragging in HP_SYSTEM,
 * LP_AON_CLKRST and the RCC atomic wrappers for what amounts to a dozen
 * register writes.
 *
 * The pad tables come from components/esp_hal_emac/esp32p4/emac_periph.c,
 * which lists three IOMUX candidates for most RMII signals; the ones used
 * here are the set the ethernet example defaults to for this target, and
 * every one of them is IOMUX function 3.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "p4_emac.h"

#include "soc/soc.h"
#include "soc/hp_sys_clkrst_reg.h"
#include "soc/lp_clkrst_reg.h"
#include "soc/hp_system_reg.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_reg.h"
#include "soc/emac_reg.h"
#include "soc/efuse_reg.h"

extern int  ets_printf(const char *fmt, ...);
extern void ets_delay_us(uint32_t us);

/* GPIO matrix, from the ROM. Signal indices are gpio_sig_map.h's. */
extern void esp_rom_gpio_connect_out_signal(uint32_t gpio_num, uint32_t signal_idx,
                                            bool out_inv, bool oen_inv);
extern void esp_rom_gpio_connect_in_signal(uint32_t gpio_num, uint32_t signal_idx,
                                           bool inv);

/* ---------------------------------------------------------------------
 * Pins
 * ------------------------------------------------------------------ */

/* The seven RMII pads. All are IOMUX function 3 on this part, which is why
 * this is a flat list and not a list of pairs. */
static const uint8_t s_rmii_pads[] = {
    50,     /* RMII_CLK  -- 50 MHz reference, INPUT from the board */
    49,     /* TX_EN */
    34,     /* TXD0 */
    35,     /* TXD1 */
    28,     /* CRS_DV */
    29,     /* RXD0 */
    30,     /* RXD1 */
};
#define EMAC_PAD_FUNC       3u

/* Inputs among the above: the pad's input buffer has to be enabled or the MAC
 * samples a floating wire. TX_EN/TXD are outputs, but leaving FUN_IE set on an
 * output pad is harmless and it keeps this loop simple -- the SDMMC path does
 * the same for its bidirectional lines. */
#define EMAC_MDC_GPIO       31u
#define EMAC_MDIO_GPIO      52u
#define EMAC_PHY_RST_GPIO   51u

/* GPIO matrix signal indices for the management interface (gpio_sig_map.h).
 * MDC/MDIO have no EMAC IOMUX function on the P4, so they go through the
 * matrix. MDIO is bidirectional: it needs both an out and an in route. */
#define MII_MDI_IN_IDX      107u
#define MII_MDC_OUT_IDX     108u
#define MII_MDO_OUT_IDX     109u

/* GPIO 51 is in the upper bank, so OUT1/ENABLE1 and bit 51-32. */
#define PHY_RST_BIT         (1u << (EMAC_PHY_RST_GPIO - 32u))

/* Function 1 is plain GPIO on every pad on this part. */
#define PAD_FUNC_GPIO       1u

static inline void reg_setbits(uint32_t addr, uint32_t mask)
{
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)addr;
    *r |= mask;
}

static inline void reg_clrsetbits(uint32_t addr, uint32_t clr, uint32_t set)
{
    volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)addr;
    *r = (*r & ~clr) | set;
}

static inline uint32_t reg_read(uint32_t addr)
{
    return *(volatile uint32_t *)(uintptr_t)addr;
}

static inline void reg_write(uint32_t addr, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

static void pad_route(uint32_t gpio, uint32_t func, bool input)
{
    const uint32_t reg = REG_IO_MUX_BASE + 0x4u + 4u * gpio;

    reg_clrsetbits(reg,
                   FUN_PD
                     | ((uint32_t)MCU_SEL_V << MCU_SEL_S)
                     | ((uint32_t)FUN_DRV_V << FUN_DRV_S),
                   (input ? FUN_IE : 0u)
                     | (func << MCU_SEL_S)
                     | ((uint32_t)FUN_DRV_V << FUN_DRV_S));
}

bool p4_emac_init(void)
{
    /* 1. Bus clock on, then pulse the module out of reset.
     *
     * Same ordering rule as SDMMC: a peripheral held in reset does not accept
     * the clock configuration that follows. Note the reset bit lives in the
     * *same* LP_CLKRST register as SDMMC's, one bit over -- 30 rather than
     * 28 -- so this must be a read-modify-write and not a store, or bringing
     * up Ethernet would put the card back into reset. */
    reg_setbits(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_EMAC_SYS_CLK_EN);
    reg_setbits(LP_CLKRST_HP_SDMMC_EMAC_RST_CTRL_REG, LP_CLKRST_RST_EN_EMAC);
    reg_clrsetbits(LP_CLKRST_HP_SDMMC_EMAC_RST_CTRL_REG, LP_CLKRST_RST_EN_EMAC, 0);

    /* 2. PHY interface = RMII.
     *
     * PHY_INTF_SEL is a 3-bit field and 4 is RMII; 0 would be MII, which
     * needs twice the pins and a different clock tree. */
    reg_clrsetbits(HP_SYSTEM_GMAC_CTRL0_REG,
                   (uint32_t)HP_SYSTEM_PHY_INTF_SEL_V << HP_SYSTEM_PHY_INTF_SEL_S,
                   4u << HP_SYSTEM_PHY_INTF_SEL_S);

    /* 3. The RMII clock tree, in input mode.
     *
     * pad_emac_ref_clk_en stays OFF: that gate is for *driving* a reference
     * clock out to the PHY, and here the board drives it in. Every source
     * select is 0, meaning "pad_emac_txrx_clk" -- the single 50 MHz pad --
     * rather than the separate tx/rx clock pads MII would use. */
    reg_clrsetbits(HP_SYS_CLKRST_PERI_CLK_CTRL00_REG,
                   HP_SYS_CLKRST_REG_PAD_EMAC_REF_CLK_EN
                     | ((uint32_t)HP_SYS_CLKRST_REG_EMAC_RMII_CLK_SRC_SEL_V
                        << HP_SYS_CLKRST_REG_EMAC_RMII_CLK_SRC_SEL_S)
                     | HP_SYS_CLKRST_REG_EMAC_RX_CLK_SRC_SEL,
                   HP_SYS_CLKRST_REG_EMAC_RMII_CLK_EN
                     | HP_SYS_CLKRST_REG_EMAC_RX_CLK_EN);

    /* Dividers of 1 on both directions, and tx sourced from the same pad.
     * IDF writes 1 here for RMII (and 0 for MII, where the pad clock is
     * already 25 MHz). */
    reg_clrsetbits(HP_SYS_CLKRST_PERI_CLK_CTRL01_REG,
                   ((uint32_t)HP_SYS_CLKRST_REG_EMAC_RX_CLK_DIV_NUM_V
                      << HP_SYS_CLKRST_REG_EMAC_RX_CLK_DIV_NUM_S)
                     | ((uint32_t)HP_SYS_CLKRST_REG_EMAC_TX_CLK_DIV_NUM_V
                        << HP_SYS_CLKRST_REG_EMAC_TX_CLK_DIV_NUM_S)
                     | HP_SYS_CLKRST_REG_EMAC_TX_CLK_SRC_SEL,
                   (1u << HP_SYS_CLKRST_REG_EMAC_RX_CLK_DIV_NUM_S)
                     | (1u << HP_SYS_CLKRST_REG_EMAC_TX_CLK_DIV_NUM_S)
                     | HP_SYS_CLKRST_REG_EMAC_TX_CLK_EN);

    /* Select the shared txrx clock pad and gate off the two MII-only ones.
     * These default to enabled, so this is a clear as much as a set. */
    reg_clrsetbits(LP_CLKRST_HP_CLK_CTRL_REG,
                   LP_CLKRST_HP_PAD_EMAC_TX_CLK_EN | LP_CLKRST_HP_PAD_EMAC_RX_CLK_EN,
                   LP_CLKRST_HP_PAD_EMAC_TXRX_CLK_EN);

    /* 4. The RMII pads, IOMUX function 3. */
    for (size_t i = 0; i < sizeof(s_rmii_pads); i++) {
        pad_route(s_rmii_pads[i], EMAC_PAD_FUNC, true);
    }

    /* 5. MDC and MDIO through the GPIO matrix.
     *
     * These two are the reason Linux can identify the PHY at all, and they
     * are the one part of this that is independent of the RMII clock: MDC is
     * generated by the MAC from its own bus clock. So if the reference clock
     * is wrong, MDIO still works and the PHY ID still reads -- which makes
     * "does Linux see a PHY?" a test of this step alone. */
    pad_route(EMAC_MDC_GPIO, PAD_FUNC_GPIO, false);
    esp_rom_gpio_connect_out_signal(EMAC_MDC_GPIO, MII_MDC_OUT_IDX, false, false);

    pad_route(EMAC_MDIO_GPIO, PAD_FUNC_GPIO, true);
    esp_rom_gpio_connect_out_signal(EMAC_MDIO_GPIO, MII_MDO_OUT_IDX, false, false);
    esp_rom_gpio_connect_in_signal(EMAC_MDIO_GPIO, MII_MDI_IN_IDX, false);

    /* 6. Reset the PHY and let it come back.
     *
     * Held low for 100 us then released, with 10 ms afterwards: the usual
     * 10/100 PHY wants a few hundred microseconds of reset and a handful of
     * milliseconds before its management interface answers. Generous here
     * because it happens once and a PHY that is not ready looks exactly like
     * a PHY that is not there. */
    pad_route(EMAC_PHY_RST_GPIO, PAD_FUNC_GPIO, false);
    reg_setbits(GPIO_ENABLE1_W1TS_REG, PHY_RST_BIT);
    reg_setbits(GPIO_OUT1_W1TC_REG, PHY_RST_BIT);       /* assert (active low) */
    ets_delay_us(100);
    reg_setbits(GPIO_OUT1_W1TS_REG, PHY_RST_BIT);       /* release */
    ets_delay_us(10000);

    /* 7. The station address, copied out of eFuse into the MAC.

     *
     * Without this Linux has no address to use. stmmac reads MACADDRESS0 at
     * probe (stmmac_get_umac_addr), finds 0xFFFFFFFF because nothing ever
     * wrote it, and falls back to eth_hw_addr_random() -- a different
     * locally-administered address on every boot, so the DHCP server hands
     * out a new lease each time and nothing on the LAN can recognise the
     * board twice.
     *
     * The real address is in eFuse BLOCK1 and it is Ethernet's to use. The
     * P4 has exactly one universally administered MAC; ESP-IDF's own
     * Kconfig.mac says where it goes --
     *
     *     On ESP32-P4 this value is fixed to one, because only Ethernet
     *     receives a universally administered MAC address.
     *
     * -- and it is derived by adding 0 to the base MAC, so the base address
     * *is* the Ethernet address, exactly as espflash reports it.
     *
     * BYTE ORDER, measured rather than assumed. On this board espflash
     * prints 60:55:f9:fb:0c:31 and the two eFuse words read back as
     *
     *     EFUSE_RD_MAC_SYS_0  0xf9fb0c31    EFUSE_RD_MAC_SYS_1  0x00006055
     *
     * so the address is stored big-endian across the pair: the OUI sits in
     * the high half of word 1, and word 0 carries the remaining four bytes
     * most-significant first. The DWMAC registers are the other way round --
     * ADDRLOW[7:0] is the first byte on the wire -- which is the shuffle
     * below, and it matches stmmac_get_mac_addr(), the code that reads it.
     *
     * A blank part reads all zeroes; leave the MAC alone in that case so
     * stmmac's random fallback still applies rather than putting
     * 00:00:00:00:00:00 on the wire. */
    {
        const uint32_t w0 = reg_read(EFUSE_RD_MAC_SYS_0_REG);
        const uint32_t w1 = reg_read(EFUSE_RD_MAC_SYS_1_REG) & 0xFFFFu;

        if (w0 != 0u || w1 != 0u) {
            const uint8_t mac[6] = {
                (uint8_t)(w1 >> 8), (uint8_t)(w1),
                (uint8_t)(w0 >> 24), (uint8_t)(w0 >> 16),
                (uint8_t)(w0 >> 8),  (uint8_t)(w0),
            };

            reg_write(EMAC_MACADDRESS0LOW_REG,
                      ((uint32_t)mac[3] << 24) | ((uint32_t)mac[2] << 16)
                        | ((uint32_t)mac[1] << 8) | (uint32_t)mac[0]);
            reg_write(EMAC_MACADDRESS0HIGH_REG,
                      ((uint32_t)mac[5] << 8) | (uint32_t)mac[4]);

            ets_printf("p4boot: EMAC station address "
                       "%02x:%02x:%02x:%02x:%02x:%02x (eFuse)\r\n",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        } else {
            ets_printf("p4boot: EMAC eFuse MAC is blank, "
                       "leaving it to Linux\r\n");
        }
    }

    ets_printf("p4boot: EMAC ready: RMII, 50 MHz ref IN on GPIO50, "
               "pads 28/29/30/34/35/49/50 func 3, MDC 31, MDIO 52, PHY rst 51\r\n");
    return true;
}

/* ---------------------------------------------------------------------
 * MDIO
 * ------------------------------------------------------------------ */

#define REG32(a)   (*(volatile uint32_t *)(uintptr_t)(a))

/* Standard IEEE 802.3 MII registers, the same on every PHY ever made. */
#define MII_BMCR        0u   /* basic mode control */
#define MII_BMSR        1u   /* basic mode status */
#define MII_PHYSID1     2u   /* OUI high */
#define MII_PHYSID2     3u   /* OUI low, model, revision */

static bool mdio_idle(void)
{
    /* GB clears when the MAC has finished the frame. Bounded so a dead
     * management interface reports rather than hangs the boot. */
    for (uint32_t i = 0; i < 200000u; i++) {
        if ((REG32(EMAC_GMIIADDRESS_REG) & (1u << EMAC_GB_S)) == 0u) {
            return true;
        }
    }
    return false;
}

static bool mdio_read(uint32_t phy, uint32_t reg, uint32_t cr, uint16_t *out)
{
    if (!mdio_idle()) {
        return false;
    }
    /* GW left clear: a read. GB set starts it. */
    REG32(EMAC_GMIIADDRESS_REG) =
        ((phy & (uint32_t)EMAC_PA_V) << EMAC_PA_S) |
        ((reg & (uint32_t)EMAC_GR_V) << EMAC_GR_S) |
        ((cr  & (uint32_t)EMAC_CR_V) << EMAC_CR_S) |
        (1u << EMAC_GB_S);
    if (!mdio_idle()) {
        return false;
    }
    *out = (uint16_t)(REG32(EMAC_GMIIDATA_REG) & (uint32_t)EMAC_GD_V);
    return true;
}

void p4_emac_mdio_scan(void)
{
    /* CR encodings, from the DesignWare MAC and identical to the values
     * stmmac's snps,clk-csr takes:
     *   0 CSR/42   1 CSR/62   2 CSR/16   3 CSR/26   4 CSR/102  5 CSR/124 */
    static const uint8_t crs[] = { 5u, 4u, 1u, 0u, 3u, 2u };
    static const uint16_t div[] = { 124u, 102u, 62u, 42u, 26u, 16u };

    ets_printf("p4boot: emac: scanning MDIO...\r\n");

    unsigned found_total = 0;
    for (size_t c = 0; c < sizeof(crs); c++) {
        unsigned found = 0;
        for (uint32_t phy = 0; phy < 32u; phy++) {
            uint16_t id1 = 0, id2 = 0;
            if (!mdio_read(phy, MII_PHYSID1, crs[c], &id1)) {
                ets_printf("p4boot: emac:   CR%u (/%u): management interface "
                           "never went idle -- MAC not clocked?\r\n",
                           (unsigned)crs[c], (unsigned)div[c]);
                break;
            }
            /* 0x0000 and 0xFFFF are both "nobody home": an unterminated MDIO
             * bus floats high, and a missing PHY reads back as zeros. */
            if (id1 == 0x0000u || id1 == 0xFFFFu) {
                continue;
            }
            (void)mdio_read(phy, MII_PHYSID2, crs[c], &id2);
            uint16_t bmsr = 0;
            (void)mdio_read(phy, MII_BMSR, crs[c], &bmsr);
            ets_printf("p4boot: emac:   PHY at addr %u: id 0x%04x%04x, "
                       "bmsr 0x%04x, link %s (CR%u = CSR/%u)\r\n",
                       (unsigned)phy, (unsigned)id1, (unsigned)id2,
                       (unsigned)bmsr,
                       (bmsr & 0x0004u) ? "UP" : "down",
                       (unsigned)crs[c], (unsigned)div[c]);
            found++;
        }
        found_total += found;
        if (found) {
            /* One working divider is all we need to prove the path. */
            break;
        }
    }

    if (!found_total) {
        ets_printf("p4boot: emac: no PHY answered at any address or divider "
                   "-- check MDC 31 / MDIO 52 muxing, or the board really has "
                   "no PHY\r\n");
    }
}

/* GPIO 50 is in the upper bank: GPIO_IN1_REG, bit 50-32. */
#define REFCLK_GPIO      50u
#define REFCLK_IN_BIT    (1u << (REFCLK_GPIO - 32u))

void p4_emac_refclk_probe(void)
{
    /* Plain GPIO input, no pulls: a driven clock beats nothing, and leaving
     * the pulls off means an idle pad reads whatever the board holds it at
     * rather than whatever we would have held it at. */
    const uint32_t reg = REG_IO_MUX_BASE + 0x4u + 4u * REFCLK_GPIO;
    reg_clrsetbits(reg,
                   FUN_PU | FUN_PD | ((uint32_t)MCU_SEL_V << MCU_SEL_S),
                   FUN_IE | (PAD_FUNC_GPIO << MCU_SEL_S));
    ets_delay_us(50);

    /* Sampled in a tight loop. At 360 MHz this reads the pad every few
     * cycles, so a 50 MHz square wave cannot alias to a constant. */
    unsigned highs = 0;
    const unsigned n = 4000;
    for (unsigned i = 0; i < n; i++) {
        if (REG32(GPIO_IN1_REG) & REFCLK_IN_BIT) {
            highs++;
        }
    }

    if (highs == 0u || highs == n) {
        ets_printf("p4boot: emac: GPIO%u STATIC (%s) over %u samples -- no "
                   "RMII reference clock arriving; input mode cannot work\r\n",
                   REFCLK_GPIO, highs ? "high" : "low", n);
    } else {
        ets_printf("p4boot: emac: GPIO%u toggling (%u/%u high) -- reference "
                   "clock is present\r\n",
                   REFCLK_GPIO, highs, n);
    }
}
