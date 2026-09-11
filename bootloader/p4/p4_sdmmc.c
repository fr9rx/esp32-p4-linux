/* SDMMC slot-0 bring-up.
 *
 * Ported from the boot shim in the hypervisor line of this project
 * (esp32-p4-emulator/native/linux-native/main/main.c, boot_shim_init_sdmmc),
 * which is known to bring this exact board's microSD slot up under mainline
 * dw_mmc. Two things changed on the way across:
 *
 *   - the LDO is brought up through p4_ldo_enable() -- the same ESP-IDF
 *     ldo_ll path we already use for PSRAM's channel 2 -- rather than by
 *     poking PMU_EXT_LDO_P1_0P2A by hand. Same rail, less magic.
 *   - the IOMUX pads are programmed directly instead of through IDF's
 *     gpio_ll wrappers. gpio_ll_func_sel() reaches into USB_SERIAL_JTAG and
 *     USB_WRAP to disable the USB PHY on pins 24-27; our pins are 39-44, so
 *     that branch never runs, but it would still have to link, and dragging
 *     the USB register blocks in for six pads is a poor trade.
 *
 * The register offsets and bit positions all come from the vendored headers
 * rather than from the shim's hardcoded addresses. They agree exactly --
 * SOC_CLK_CTRL1 bit 14, PERI_CLK_CTRL01 at +0x34, PERI_CLK_CTRL02 at +0x38,
 * SDMMC reset at LP_CLKRST +0x4c bit 28 -- which is a pleasant cross-check on
 * both.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "p4_sdmmc.h"
#include "p4_ldo.h"

#include "soc/soc.h"
#include "soc/hp_sys_clkrst_reg.h"
#include "soc/lp_clkrst_reg.h"
#include "soc/io_mux_reg.h"

extern int  ets_printf(const char *fmt, ...);
extern void ets_delay_us(uint32_t us);

/* The six dedicated slot-0 pads: D0 D1 D2 D3 CLK CMD.
 *
 * The datasheet labels these SD1_*, which reads like slot 1. They are
 * physically slot 0 -- IDF's own sdmmc_pins.h agrees -- and this board wires
 * its microSD socket to them. */
static const uint8_t s_slot0_pins[6] = { 39, 40, 41, 42, 43, 44 };

/* IO_MUX has one 32-bit register per pad, starting one word into the block:
 * GPIO0 at +0x04 and GPIO39 at +0xA0 in the vendored header, which is
 * 0x04 + 4*39. */
#define IO_MUX_PAD_REG(n)   (REG_IO_MUX_BASE + 0x4u + 4u * (uint32_t)(n))

/* Function 0 on every one of the six pads routes it to the SD controller. */
#define SDMMC_PAD_FUNC      0u

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

bool p4_sdmmc_init(void)
{
    /* 1. Power the slot at 3.3 V.
     *
     * Plain 3.3 V rail, no UHS: the DTS advertises neither 1.8 V signalling
     * nor the high-speed modes that would need it, so there is no voltage
     * switch for anyone to negotiate. */
    if (!p4_ldo_enable(P4_LDO_CHAN_SDMMC, P4_LDO_MV_SDMMC)) {
        ets_printf("p4boot: sdmmc: LDO ch%d enable REFUSED\r\n", P4_LDO_CHAN_SDMMC);
        return false;
    }
    ets_delay_us(1000);     /* let the rail ramp before it is clocked */

    /* 2. Bus clock on, then pulse the module out of reset.
     *
     * Order matters: a peripheral held in reset does not accept the clock
     * configuration that follows. */
    reg_setbits(HP_SYS_CLKRST_SOC_CLK_CTRL1_REG, HP_SYS_CLKRST_REG_SDMMC_SYS_CLK_EN);
    reg_setbits(LP_CLKRST_HP_SDMMC_EMAC_RST_CTRL_REG, LP_CLKRST_RST_EN_SDMMC);
    reg_clrsetbits(LP_CLKRST_HP_SDMMC_EMAC_RST_CTRL_REG, LP_CLKRST_RST_EN_SDMMC, 0);

    /* 3. LS clock source = PLL_F160M (src_sel 0), divider path not bypassed
     *    (hs_mode 0), clock enabled. */
    reg_clrsetbits(HP_SYS_CLKRST_PERI_CLK_CTRL01_REG,
                   HP_SYS_CLKRST_REG_SDIO_LS_CLK_SRC_SEL | HP_SYS_CLKRST_REG_SDIO_HS_MODE,
                   HP_SYS_CLKRST_REG_SDIO_LS_CLK_EN);

    /* 4. Host divider of 4: 160 MHz -> 40 MHz.
     *
     * The divider is expressed as clock edges rather than a count, following
     * sdmmc_ll_set_clock_div(): edge_h = div/2 - 1, edge_n = edge_l = div - 1.
     * For div = 4 that is h=1, n=3, l=3. Then the drive/sample/self clocks are
     * enabled with the phase selection IDF's init_phase_delay() uses --
     * drv = 1, sam = 0, slf = 0.
     *
     * Read-modify-write, never a whole-register store: bits [31:30] of this
     * register are MIPI_DSI_DPHY_CLK_SRC_SEL. We drive no display, but
     * clobbering another peripheral's clock source to save a mask is the kind
     * of shortcut that gets found months later. */
    {
        const uint32_t clr =
            ((uint32_t)HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_L_V     << HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_L_S)    |
            ((uint32_t)HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_H_V     << HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_H_S)    |
            ((uint32_t)HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_N_V     << HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_N_S)    |
            ((uint32_t)HP_SYS_CLKRST_REG_SDIO_LS_SLF_CLK_EDGE_SEL_V << HP_SYS_CLKRST_REG_SDIO_LS_SLF_CLK_EDGE_SEL_S) |
            ((uint32_t)HP_SYS_CLKRST_REG_SDIO_LS_DRV_CLK_EDGE_SEL_V << HP_SYS_CLKRST_REG_SDIO_LS_DRV_CLK_EDGE_SEL_S) |
            ((uint32_t)HP_SYS_CLKRST_REG_SDIO_LS_SAM_CLK_EDGE_SEL_V << HP_SYS_CLKRST_REG_SDIO_LS_SAM_CLK_EDGE_SEL_S);

        const uint32_t set =
            (3u << HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_L_S) |
            (1u << HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_H_S) |
            (3u << HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_N_S) |
            (1u << HP_SYS_CLKRST_REG_SDIO_LS_DRV_CLK_EDGE_SEL_S) |
            HP_SYS_CLKRST_REG_SDIO_LS_DRV_CLK_EN |
            HP_SYS_CLKRST_REG_SDIO_LS_SAM_CLK_EN |
            HP_SYS_CLKRST_REG_SDIO_LS_SLF_CLK_EN;

        reg_clrsetbits(HP_SYS_CLKRST_PERI_CLK_CTRL02_REG, clr, set);

        /* The edge configuration above is staged; this strobe commits it. */
        reg_setbits(HP_SYS_CLKRST_PERI_CLK_CTRL02_REG,
                    HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_CFG_UPDATE);
        reg_clrsetbits(HP_SYS_CLKRST_PERI_CLK_CTRL02_REG,
                       HP_SYS_CLKRST_REG_SDIO_LS_CLK_EDGE_CFG_UPDATE, 0);
    }

    /* 5. Route the six pads to the SD controller.
     *
     * Internal pull-ups because this board has no external ones on these
     * lines. Input enable because CMD and the data lines are bidirectional
     * and the controller must be able to read them back. Maximum drive
     * because the card is on the far end of a socket. */
    for (size_t i = 0; i < sizeof(s_slot0_pins); i++) {
        const uint32_t reg = IO_MUX_PAD_REG(s_slot0_pins[i]);

        reg_clrsetbits(reg,
                       FUN_PD                                       /* pulldown off */
                         | ((uint32_t)MCU_SEL_V << MCU_SEL_S)
                         | ((uint32_t)FUN_DRV_V << FUN_DRV_S),
                       FUN_PU                                       /* pullup on */
                         | FUN_IE                                   /* input enable */
                         | (SDMMC_PAD_FUNC << MCU_SEL_S)
                         | ((uint32_t)FUN_DRV_V << FUN_DRV_S));     /* strongest drive */
    }

    /* Let the new clock propagate before the kernel starts driving the
     * controller. */
    ets_delay_us(10);

    ets_printf("p4boot: SDMMC slot 0 ready: LDO4 3.3 V, bus clock on, "
               "LS clock PLL160M/4 = 40 MHz, pads 39-44 func 0\r\n");
    return true;
}
