/*
 * counter speed setting
 *
 * In VS Code: change COUNTER_PRESET below, save, press Compile.
 * The firmware is build/apps/counter/counter.uf2
 *
 *  preset  HDMI clock  per HDMI pin  sample rate (hsdaoh_test -s)  USB 48 MHz from
 *  60      336 MHz     672 Mbit/s    60000000                     USB PLL
 *  70      384 MHz     768 Mbit/s    70000000                     USB PLL
 *  74      399 MHz     798 Mbit/s    73706941                     system PLL (336 / 7)
 *  75      408 MHz     816 Mbit/s    75452632                     system PLL (336 / 7)
 *  78      420 MHz     840 Mbit/s    77701897                     system PLL (336 / 7)
 *  80      432 MHz     864 Mbit/s    80000000                     USB PLL
 *
 * All use the USB PLL for the HDMI clock and run the link at 93-96% load.
 * On your board 70 works and 80 does not; 74/75/78 are the steps between.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef COUNTER_PRESET
#define COUNTER_PRESET	70
#endif

/* The counter_60m ... counter_78m targets in CMakeLists.txt set SYS_CLK etc.
 * themselves; then the preset is not used. */
#ifndef SYS_CLK

#if COUNTER_PRESET == 60
#define SYS_CLK			270000		/* kHz */
#define HSTX_CLK_MHZ		336
#define COUNTER_CLKDIV		1.5f
#define USB_FROM_SYS_PLL	0
#elif COUNTER_PRESET == 70
#define SYS_CLK			315000
#define HSTX_CLK_MHZ		384
#define COUNTER_CLKDIV		1.5f
#define USB_FROM_SYS_PLL	0
#elif COUNTER_PRESET == 74
#define SYS_CLK			336000
#define HSTX_CLK_MHZ		399
#define COUNTER_CLKDIV		1.51953125f
#define USB_FROM_SYS_PLL	1
#elif COUNTER_PRESET == 75
#define SYS_CLK			336000
#define HSTX_CLK_MHZ		408
#define COUNTER_CLKDIV		1.484375f
#define USB_FROM_SYS_PLL	1
#elif COUNTER_PRESET == 78
#define SYS_CLK			336000
#define HSTX_CLK_MHZ		420
#define COUNTER_CLKDIV		1.44140625f
#define USB_FROM_SYS_PLL	1
#elif COUNTER_PRESET == 80
#define SYS_CLK			300000
#define HSTX_CLK_MHZ		432
#define COUNTER_CLKDIV		1.25f
#define USB_FROM_SYS_PLL	0
#else
#error "COUNTER_PRESET must be 60, 70, 74, 75, 78 or 80"
#endif

#endif /* SYS_CLK */
