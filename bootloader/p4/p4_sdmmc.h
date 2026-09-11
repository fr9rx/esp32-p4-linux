/* SDMMC slot-0 bring-up for the ESP32-P4 second-stage bootloader. */
#pragma once

#include <stdbool.h>

/* Power, clock and pin-mux the SD controller so Linux's dw_mmc can drive it.
 *
 * NOT OPTIONAL, and not merely "the card will be missing" if skipped. There
 * is no ESP32-P4 pinctrl or clock-controller driver upstream, so mainline
 * dw_mmc probes the controller assuming somebody else already turned it on.
 * Probing an unclocked peripheral on this part is a bus fault, and the board
 * resets on the spot: Linux got as far as
 *
 *     gpio gpiochip0: Static allocation of GPIO base is deprecated
 *
 * and then rebooted, with the snps,dw-mshc node next in the device tree.
 *
 * What this sets up:
 *   power  LDO channel 4 at 3.3 V (no UHS, so no 1.8 V switch)
 *   clock  SDMMC bus clock on, module reset pulsed, LS clock from PLL_F160M
 *          with a host divider of 4 -> 40 MHz. dw_mmc then picks its own card
 *          divider from clk_set_rate; at the DTS's max-frequency of 25 MHz it
 *          chooses 1, giving 40/(2*1) = 20 MHz on the wire.
 *   pins   the six dedicated slot-0 IOMUX pads, function 0, with internal
 *          pull-ups, input enabled and maximum drive.
 *
 * Returns false only if the LDO channel is rejected; everything else is
 * register writes that cannot fail in a way we could detect here. */
bool p4_sdmmc_init(void);
