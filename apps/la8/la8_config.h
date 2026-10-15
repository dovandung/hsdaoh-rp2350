/*
 * la8 logic analyzer speed setting
 *
 * In VS Code: change LA8_PRESET below, save, press Compile.
 * The firmware is build/apps/la8/la8.uf2
 *
 *  preset  sample rate   HDMI clock          per HDMI pin   system clock
 *  140     140 MS/s      384 MHz (USB PLL)   768 Mbit/s     280 MHz / 2
 *  158     158 MS/s      432 MHz (USB PLL)   864 Mbit/s     316 MHz / 2
 *  106     106.67 MS/s   320 MHz (sys clk)   640 Mbit/s     320 MHz / 3
 *
 * 140 uses the same HDMI clock as the 70 MS/s counter that works on your
 * board. 158 needs 432 MHz HDMI, which did not work on your board.
 * The rate can also be lowered at runtime over USB serial ("div 4" etc.).
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LA8_PRESET
#define LA8_PRESET	140
#endif

#if LA8_PRESET == 140
#define LA_HSTX_USB_PLL	1
#define HSTX_CLK_MHZ	384		/* USB PLL VCO 1536 MHz, USB = 384 / 8 */
#define SYS_CLK		280000		/* kHz */
#define SAMPLE_DIV	2
#elif LA8_PRESET == 158
#define LA_HSTX_USB_PLL	1
#define HSTX_CLK_MHZ	432		/* USB PLL VCO 1728 MHz, USB = 432 / 9 */
#define SYS_CLK		316000
#define SAMPLE_DIV	2
#elif LA8_PRESET == 106
#define LA_HSTX_USB_PLL	0		/* HDMI clock = system clock */
#define SYS_CLK		320000
#define SAMPLE_DIV	3
#else
#error "LA8_PRESET must be 140, 158 or 106"
#endif
