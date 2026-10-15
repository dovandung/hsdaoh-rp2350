/*
 * hsdaoh - High Speed Data Acquisition over MS213x USB3 HDMI capture sticks
 * Implementation for the Raspberry Pi RP2350 HSTX peripheral
 *
 * la8: 8 channel logic analyzer front end. Samples GP0..GP7 continuously
 * and streams them to the host, two samples per 16-bit link word (earlier
 * sample in the high byte). Triggering and saving is done on the PC by
 * hsdaoh_la.
 *
 * Sample rate = SYS_CLK / integer divider. The HDMI (HSTX) clock comes either
 * from the USB PLL (LA_HSTX_USB_PLL=1, default, like the sdr app) so it is
 * independent of the system clock, or from the system clock:
 *
 *   140 (default)  HSTX 384 MHz (USB PLL, VCO 1536 MHz), sys 280 MHz / 2 = 140 MS/s
 *   158            HSTX 432 MHz (USB PLL, VCO 1728 MHz), sys 316 MHz / 2 = 158 MS/s
 *   106            HSTX = sys 320 MHz,                    sys 320 MHz / 3 = 106.67 MS/s
 * Selected with LA8_PRESET in la8_config.h.
 *
 * The divider can be changed at runtime over the USB serial port:
 * send "div 4" (and Enter), "?" shows the current setting.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pll.h"
#include "hardware/structs/hstx_ctrl.h"
#include "hardware/structs/hstx_fifo.h"

#include "picohsdaoh.h"
#include "la8.pio.h"

/* speed: set LA8_PRESET in la8_config.h (the la8_* targets in
 * CMakeLists.txt pass their own LA8_PRESET instead) */
#include "la8_config.h"

#if LA_HSTX_USB_PLL
#define HSTX_CLK_KHZ	(HSTX_CLK_MHZ * 1000)
#if HSTX_CLK_MHZ % 48
#error "HSTX_CLK_MHZ must be a multiple of 48 so the USB clock can stay at 48 MHz"
#endif
#if HSTX_CLK_MHZ * 4 < 750
#error "USB PLL VCO (4 x HSTX_CLK_MHZ) must be at least 750 MHz"
#endif
#else
#define HSTX_CLK_KHZ	SYS_CLK
#endif

#define LA_PIN_BASE	0		/* GP0..GP7; GP0..GP11 are free, GP12..19 are HSTX */

#if LA_PIN_BASE > 4
#error "LA_PIN_BASE + 7 must be <= 11 (GP12..GP19 are used by HSTX)"
#endif

#define DMACH_PIO_PING 0
#define DMACH_PIO_PONG 1

static bool pio_dma_pong = false;
uint16_t ringbuffer[RBUF_DEFAULT_TOTAL_LEN] __attribute__((aligned(4)));
int ringbuf_head = 2;

static PIO la_pio = pio0;
static uint la_sm;
static uint cur_div = SAMPLE_DIV;

void __scratch_y("") pio_dma_irq_handler()
{
	uint ch_num = pio_dma_pong ? DMACH_PIO_PONG : DMACH_PIO_PING;
	dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
	dma_hw->intr = 1u << ch_num;
	pio_dma_pong = !pio_dma_pong;

	ringbuf_head = (ringbuf_head + 1) % RBUF_DEFAULT_SLICES;

	ch->write_addr = (uintptr_t)&ringbuffer[ringbuf_head * RBUF_SLICE_LEN];
	ch->transfer_count = RBUF_MAX_DATA_LEN;

	hsdaoh_update_head(0, ringbuf_head);
}

/* Smallest divider whose sample rate fits on the HDMI link, with 5% margin.
 * Link: MODE_V_ACTIVE_LINES lines of RBUF_MAX_DATA_LEN words per frame,
 * 2 samples per word, frame rate = HSTX / 5 / (H_TOTAL * V_TOTAL). */
static uint min_divider(void)
{
	double samples_per_hstx_cycle = 2.0 * RBUF_MAX_DATA_LEN * MODE_V_ACTIVE_LINES /
					(5.0 * MODE_H_TOTAL_PIXELS * MODE_V_TOTAL_LINES);
	double samples_per_sys_cycle = samples_per_hstx_cycle * HSTX_CLK_KHZ / SYS_CLK;
	uint d = 1;
	while (1.0 / d > 0.95 * samples_per_sys_cycle)
		d++;
	return d;
}

static uint32_t rate_for(uint div)
{
	return (uint32_t)(((uint64_t)SYS_CLK * 1000 + div / 2) / div);
}

static void init_pio_input(void)
{
	uint offset = pio_add_program(la_pio, &la8_input_program);
	la_sm = pio_claim_unused_sm(la_pio, true);

	dma_channel_config c;
	c = dma_channel_get_default_config(DMACH_PIO_PING);
	channel_config_set_chain_to(&c, DMACH_PIO_PONG);
	channel_config_set_dreq(&c, pio_get_dreq(la_pio, la_sm, false));
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
	dma_channel_configure(DMACH_PIO_PING, &c, &ringbuffer[0 * RBUF_SLICE_LEN],
			      &la_pio->rxf[la_sm], RBUF_MAX_DATA_LEN, false);

	c = dma_channel_get_default_config(DMACH_PIO_PONG);
	channel_config_set_chain_to(&c, DMACH_PIO_PING);
	channel_config_set_dreq(&c, pio_get_dreq(la_pio, la_sm, false));
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_16);
	dma_channel_configure(DMACH_PIO_PONG, &c, &ringbuffer[1 * RBUF_SLICE_LEN],
			      &la_pio->rxf[la_sm], RBUF_MAX_DATA_LEN, false);

	dma_hw->ints0 |= (1u << DMACH_PIO_PING) | (1u << DMACH_PIO_PONG);
	dma_hw->inte0 |= (1u << DMACH_PIO_PING) | (1u << DMACH_PIO_PONG);
	irq_set_exclusive_handler(DMA_IRQ_0, pio_dma_irq_handler);
	irq_set_enabled(DMA_IRQ_0, true);

	dma_channel_start(DMACH_PIO_PING);

	/* start sampling only after the DMA is ready to take the data */
	la8_input_program_init(la_pio, la_sm, offset, LA_PIN_BASE, cur_div);
}

static void print_status(void)
{
	uint32_t r = rate_for(cur_div);
	printf("la8: GP%d..GP%d, %lu.%03lu MS/s (sys %d MHz / %u), minimum divider %u, HDMI %d MHz (%s)\n",
	       LA_PIN_BASE, LA_PIN_BASE + 7, (unsigned long)(r / 1000000),
	       (unsigned long)((r / 1000) % 1000), SYS_CLK / 1000, cur_div, min_divider(),
	       HSTX_CLK_KHZ / 1000, LA_HSTX_USB_PLL ? "USB PLL" : "sys clock");
	printf("commands: div <n>   ?\n");
}

/* diagnostics for the '?' command: what the clocks, HDMI output and DMA
 * are actually doing on the running chip */
extern metadata_t metadata;
extern uint16_t idle_line_buf1[];

static void print_diag(void)
{
	printf("measured kHz: sys %lu  hstx %lu  usb %lu  ref %lu   vreg sel %d\n",
	       (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_SYS),
	       (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_HSTX),
	       (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_USB),
	       (unsigned long)frequency_count_khz(CLOCKS_FC0_SRC_VALUE_CLK_REF),
	       (int)vreg_get_voltage());

	uint16_t fc0 = metadata.framecounter;
	sleep_ms(1000);
	uint16_t fc1 = metadata.framecounter;
	printf("HDMI frames in 1 s: %u (framecounter %u)  magic %08lx  stream0 format %u srate %lu\n",
	       (uint16_t)(fc1 - fc0), fc1, (unsigned long)metadata.magic,
	       metadata.stream0_format, (unsigned long)metadata.stream_info[0].srate);

	uint32_t st = hstx_fifo_hw->stat;
	hstx_fifo_hw->stat = HSTX_FIFO_STAT_WOF_BITS;	/* clear the write-overflow flag */
	printf("hstx csr %08lx  expand_shift %08lx  expand_tmds %08lx  fifo stat %03lx\n",
	       (unsigned long)hstx_ctrl_hw->csr, (unsigned long)hstx_ctrl_hw->expand_shift,
	       (unsigned long)hstx_ctrl_hw->expand_tmds, (unsigned long)st);

	for (uint ch = 0; ch < 16; ch++) {
		if (ch > 1 && ch < 13)
			continue;
		dma_channel_hw_t *c = &dma_hw->ch[ch];
		printf("dma%-2u read %08lx write %08lx count %5lu ctrl %08lx\n", ch,
		       (unsigned long)c->read_addr, (unsigned long)c->write_addr,
		       (unsigned long)c->transfer_count, (unsigned long)c->al1_ctrl);
	}
	printf("pio0 fdebug %08lx flevel %08lx   ringbuffer %p  idle_line_buf1 %p\n",
	       (unsigned long)la_pio->fdebug, (unsigned long)la_pio->flevel,
	       (void *)ringbuffer, (void *)idle_line_buf1);
	la_pio->fdebug = 0xffffffff;			/* clear the sticky flags */
}

static void handle_line(char *line)
{
	while (*line == ' ')
		line++;
	if (!strncmp(line, "div", 3)) {
		long d = strtol(line + 3, NULL, 10);
		if (d < (long)min_divider() || d > 65535) {
			printf("divider must be %u..65535 (link limit)\n", min_divider());
			return;
		}
		cur_div = (uint)d;
		pio_sm_set_clkdiv(la_pio, la_sm, (float)cur_div);
		pio_sm_clkdiv_restart(la_pio, la_sm);
		hsdaoh_set_stream_srate(0, rate_for(cur_div));
		print_status();
	} else if (line[0] == '?' || !strncmp(line, "status", 6)) {
		print_status();
		print_diag();
	} else if (line[0]) {
		printf("unknown command '%s'\n", line);
		print_status();
	}
}

int main()
{
	/* 1.50 V core voltage, as in the counter app. VREG_VOLTAGE_MAX is only
	 * 1.30 V on the RP2350, which is not enough for HSTX at 384 MHz: the
	 * HDMI clock lane still runs but the data lanes fail. Above 1.30 V the
	 * SDK's voltage limit has to be lifted first. */
	vreg_disable_voltage_limit();
	vreg_set_voltage(VREG_VOLTAGE_1_50);
	sleep_ms(100);		/* let the voltage settle before overclocking */

	hsdaoh_set_sys_clock_khz(SYS_CLK);

#if LA_HSTX_USB_PLL
	/* As in the sdr app: run the USB PLL at HSTX_CLK_MHZ and use it for
	 * HSTX. USB itself still needs 48 MHz, so divide it down first. */
	hw_write_masked(&clocks_hw->clk[clk_usb].div,
			(HSTX_CLK_MHZ / 48) << CLOCKS_CLK_USB_DIV_INT_LSB,
			CLOCKS_CLK_USB_DIV_INT_BITS);
	pll_init(pll_usb, 1, HSTX_CLK_MHZ * 4 * MHZ, 2, 2);
#endif

	/* HSTX clock = source / 1 */
	hw_write_masked(&clocks_hw->clk[clk_hstx].div,
			1 << CLOCKS_CLK_HSTX_DIV_INT_LSB,
			CLOCKS_CLK_HSTX_DIV_INT_BITS);
#if LA_HSTX_USB_PLL
	hw_write_masked(&clocks_hw->clk[clk_hstx].ctrl,
			CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB << CLOCKS_CLK_HSTX_CTRL_AUXSRC_LSB,
			CLOCKS_CLK_HSTX_CTRL_AUXSRC_BITS);
#endif

	stdio_init_all();

	if (cur_div < min_divider())
		cur_div = min_divider();

	/* full speed HDMI: strong drive, fast slew */
	hsdaoh_init(GPIO_DRIVE_STRENGTH_12MA, GPIO_SLEW_RATE_FAST);
	hsdaoh_add_stream(0, PIO_8BIT, rate_for(cur_div), RBUF_MAX_DATA_LEN,
			  RBUF_DEFAULT_SLICES, ringbuffer);
	hsdaoh_start();
	init_pio_input();

#ifdef PICO_DEFAULT_LED_PIN
	gpio_init(PICO_DEFAULT_LED_PIN);
	gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
#endif

	char line[32];
	uint len = 0;
	uint64_t next_blink = 0;
	bool led = false;

	while (1) {
		int c = getchar_timeout_us(100000);
		if (c >= 0) {
			if (c == '\r' || c == '\n') {
				line[len] = 0;
				handle_line(line);
				len = 0;
			} else if (len < sizeof(line) - 1) {
				line[len++] = (char)c;
			}
		}
		if (time_us_64() >= next_blink) {
			next_blink = time_us_64() + 500000;
			led = !led;
#ifdef PICO_DEFAULT_LED_PIN
			gpio_put(PICO_DEFAULT_LED_PIN, led);
#endif
		}
	}
}
