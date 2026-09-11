/* UART0 console for the ESP32-P4 second-stage bootloader. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Reprogram UART0's baud rate, keeping XTAL as the source clock.
 *
 * Returns false if the rate is unachievable with the available dividers, in
 * which case the port is left alone and still usable at whatever it was.
 *
 * Drains the TX FIFO first: changing the divisor with characters still in
 * flight garbles them, and the garbled ones are the messages saying what we
 * just did.
 *
 * The DTS models `xtal_clk` as a 40 MHz fixed-clock and the kernel's
 * esp32_uart derives its own divisor from that, so the SOURCE must stay XTAL
 * or the device tree becomes a lie. The divisor is ours to choose. */
bool p4_uart0_set_baud(uint32_t baud, uint32_t sclk_hz);
