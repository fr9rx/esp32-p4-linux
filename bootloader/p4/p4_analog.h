/* Analog-domain bring-up for the ESP32-P4 second-stage bootloader. */
#pragma once

/* Bring the analog side into a state the PLLs can be driven from, then
 * enable the brownout detector.
 *
 * Must run before any clock is raised and before PSRAM init, because MPLL is
 * enabled beside CPLL and SPLL and the bias has to be adequate for all three.
 *
 * Mirrors ESP-IDF's bootloader_hardware_init() + bootloader_ana_reset_config()
 * for this silicon; see the comments in p4_analog.c for what each step is
 * for and which parts are rev-1.0 specific. */
void p4_analog_init(void);
