/*
 * hsdaoh - High Speed Data Acquisition over MS213x USB3 HDMI capture sticks
 * Implementation for the Raspberry Pi RP2350 HSTX peripheral
 *
 * 16 bit logic analyzer example, with optional edge trigger and burst capture
 *
 * Copyright (c) 2024 by Steve Markgraf <steve@steve-m.de>
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the author nor the names of its contributors may
 *    be used to endorse or promote products derived from this software
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "hardware/dma.h"
#include "hardware/pio.h"

#include "picohsdaoh.h"

/* ------------------------------------------------------------------------
 * Configuration
 * ------------------------------------------------------------------------ */

/* 336 MHz gives integer sample rates: 56 MS/s continuous, 48 MS/s triggered.
 * (320 MHz also works: 53.33 / 45.71 MS/s) */
#define SYS_CLK			336000

#define LA_MODE_CONTINUOUS	0	/* stream everything, like the original example */
#define LA_MODE_TRIGGERED	1	/* wait for an edge, then stream a fixed-length burst */

#ifndef LA_MODE
#define LA_MODE			LA_MODE_CONTINUOUS
#endif

/* PIO clock divider, integer only (1 = full speed) */
#define SAMPLE_CLKDIV		1

/* Triggered mode defaults, can be changed at runtime with commands on the
 * USB serial port (type ? for help) */
#define TRIGGER_GPIO		0	/* any of the sampled GPIOs */
#define TRIGGER_FALLING_EDGE	false
#define BURST_SLICES		64	/* burst length in hsdaoh lines (1916 samples each) */
#define BURST_COUNT		0	/* number of bursts, 0 = re-arm forever, 1 = single shot */
#define PRINT_BURSTS		1	/* also print burst timestamps on the USB serial port */

/* ------------------------------------------------------------------------ */

#include "16bit_input.pio.h"
#if LA_MODE == LA_MODE_TRIGGERED
#define CYCLES_PER_SAMPLE	7
#else
#define CYCLES_PER_SAMPLE	6
#endif
#define SAMPLES_PER_WORD	1	/* samples per 16 bit hsdaoh word */
#define STREAM_FORMAT		RAW_16BIT

/* exact sample rate, reported to the host via the hsdaoh metadata */
#define SAMPLE_RATE_HZ		((SYS_CLK * 1000u) / (CYCLES_PER_SAMPLE * SAMPLE_CLKDIV))

/* 32 bit DMA words, so use an even number of 16 bit words per line */
#define LA_DATA_LEN		(RBUF_MAX_DATA_LEN & ~1u)
#define SAMPLES_PER_LINE	(LA_DATA_LEN * SAMPLES_PER_WORD)

#define LA_PIO			pio0
#define DMACH_PIO_PING		0
#define DMACH_PIO_PONG		1
#define DMA_PIO_MASK		((1u << DMACH_PIO_PING) | (1u << DMACH_PIO_PONG))

static uint sm_data;
static uint __unused pio_offset;
static bool pio_dma_pong = false;
uint16_t __attribute__((aligned(4))) ringbuffer[RBUF_DEFAULT_TOTAL_LEN];

/* index of the slice the most recently (re)programmed DMA channel writes to */
static int ringbuf_head = 0;

/* completed lines of stream 0, used to number the samples */
static volatile uint64_t lines_done = 0;

/* hsdaoh sends the slices up to (head - 2), and starts at slice (slices - 1).
 * Ping starts there and pong at slice 0, so no slice is skipped and no slice
 * is sent while it is still being written. */
void __scratch_y("") pio_dma_irq_handler()
{
	uint ch_num = pio_dma_pong ? DMACH_PIO_PONG : DMACH_PIO_PING;
	dma_channel_hw_t *ch = &dma_hw->ch[ch_num];
	dma_hw->intr = 1u << ch_num;
	pio_dma_pong = !pio_dma_pong;

	ringbuf_head = (ringbuf_head + 1) % RBUF_DEFAULT_SLICES;

	ch->write_addr = (uintptr_t)&ringbuffer[ringbuf_head * RBUF_SLICE_LEN];
	ch->transfer_count = LA_DATA_LEN / 2;

	lines_done++;
	hsdaoh_update_head(0, ringbuf_head);
}

/* Point the ping channel at the slice that is (or was) being filled, and pong
 * at the next one, then start ping. Works both for the first start and for
 * restarting after the DMA has been aborted mid-line. */
static void dma_start(void)
{
	int cur = (ringbuf_head + RBUF_DEFAULT_SLICES - 1) % RBUF_DEFAULT_SLICES;

	dma_channel_set_trans_count(DMACH_PIO_PING, LA_DATA_LEN / 2, false);
	dma_channel_set_write_addr(DMACH_PIO_PING, &ringbuffer[cur * RBUF_SLICE_LEN], false);
	dma_channel_set_trans_count(DMACH_PIO_PONG, LA_DATA_LEN / 2, false);
	dma_channel_set_write_addr(DMACH_PIO_PONG, &ringbuffer[ringbuf_head * RBUF_SLICE_LEN], false);
	pio_dma_pong = false;

	dma_hw->ints0 = DMA_PIO_MASK;
	irq_clear(DMA_IRQ_0);
	irq_set_enabled(DMA_IRQ_0, true);
	dma_channel_start(DMACH_PIO_PING);
}

#if LA_MODE == LA_MODE_TRIGGERED
/* ------------------------------------------------------------------------
 * Burst timestamps, sent as a second hsdaoh stream (like the audio streams
 * of the ADC examples). One line per burst, three 64 bit words:
 *   [0] burst number
 *   [1] index of the first sample of this burst in stream 0
 *   [2] time of the trigger in microseconds since boot
 * ------------------------------------------------------------------------ */
#define TS_STREAM_ID		1
#define TS_SLICES		8
#define TS_DATA_LEN		(3 * 4)	/* in 16 bit words */

/* the PIO counts samples in a 32 bit register */
#define MAX_BURST_LINES		(UINT32_MAX / SAMPLES_PER_LINE)

typedef struct {
	uint64_t burst;
	uint64_t first_sample;
	uint64_t time_us;
} burst_record_t;

typedef struct {
	uint gpio;
	bool falling;
	uint32_t lines;		/* burst length in hsdaoh lines */
	uint32_t count;		/* 0 = forever */
} trigger_cfg_t;

/* Only changed while the capture is stopped, so the IRQ handler can read it */
static trigger_cfg_t cfg = { TRIGGER_GPIO, TRIGGER_FALLING_EDGE, BURST_SLICES, BURST_COUNT };

uint16_t __attribute__((aligned(8))) ts_ringbuffer[TS_SLICES * RBUF_SLICE_LEN];
static int ts_slice = TS_SLICES - 1;
static burst_record_t burst_log[TS_SLICES];	/* copy for printing */
static volatile uint32_t bursts_started = 0;	/* total, numbers the records */
static volatile uint32_t run_bursts = 0;	/* started since the last (re)arm */
static uint32_t bursts_armed = 0;		/* armed since the last (re)arm */
static uint32_t burst_samples;
static uint64_t next_first_sample;
static bool running = false;

static inline void arm_burst(void)
{
	/* the program samples x + 1 times */
	pio_sm_put(LA_PIO, sm_data, burst_samples - 1);
	bursts_armed++;
}

/* Equivalent of gusmanb's "irq 1 -> NMI": the PIO raises IRQ 0 when the
 * trigger fires, we run at the highest priority and timestamp it. */
void __scratch_y("") pio_trigger_irq_handler()
{
	uint64_t now = time_us_64();
	pio_interrupt_clear(LA_PIO, 0);

	burst_record_t rec = {
		.burst = bursts_started,
		.first_sample = next_first_sample,
		.time_us = now,
	};
	next_first_sample += burst_samples;

	memcpy(&ts_ringbuffer[ts_slice * RBUF_SLICE_LEN], &rec, sizeof(rec));
	burst_log[bursts_started % TS_SLICES] = rec;
	hsdaoh_update_head(TS_STREAM_ID, (ts_slice + 2) % TS_SLICES);
	ts_slice = (ts_slice + 1) % TS_SLICES;

	bursts_started++;
	run_bursts++;

	/* queue the next burst while this one is running: zero re-arm time */
	if (cfg.count == 0 || bursts_armed < cfg.count)
		arm_burst();
}

static void start_capture(void)
{
	PIO pio = LA_PIO;

	dma_start();

	/* trigger channel and edge */
	hw_write_masked(&pio->sm[sm_data].execctrl,
			cfg.gpio << PIO_SM0_EXECCTRL_JMP_PIN_LSB,
			PIO_SM0_EXECCTRL_JMP_PIN_BITS);
	pio_sm_exec(pio, sm_data, pio_encode_set(pio_y, cfg.falling ? 1 : 0));
	pio_sm_exec(pio, sm_data, pio_encode_jmp(pio_offset));

	/* the restart begins on a line boundary */
	burst_samples = cfg.lines * SAMPLES_PER_LINE;
	next_first_sample = lines_done * SAMPLES_PER_LINE;
	bursts_armed = 0;
	run_bursts = 0;
	arm_burst();

	pio_interrupt_clear(pio, 0);
	irq_clear(PIO0_IRQ_0);
	irq_set_enabled(PIO0_IRQ_0, true);

	running = true;
	pio_sm_set_enabled(pio, sm_data, true);
}

/* Stop sampling. Lines that are already complete still go out, the line that
 * was being filled is dropped and will be overwritten after the restart. */
static void stop_capture(void)
{
	PIO pio = LA_PIO;

	irq_set_enabled(PIO0_IRQ_0, false);
	pio_sm_set_enabled(pio, sm_data, false);

	/* let the DMA drain the FIFO, a line completing now is still published */
	absolute_time_t timeout = make_timeout_time_ms(5);
	while (!pio_sm_is_rx_fifo_empty(pio, sm_data) && !time_reached(timeout))
		tight_loop_contents();
	busy_wait_us(5);

	irq_set_enabled(DMA_IRQ_0, false);

	/* break the ping-pong chain first, so aborting one channel can't start
	 * the other one */
	hw_write_masked(&dma_hw->ch[DMACH_PIO_PING].al1_ctrl,
			DMACH_PIO_PING << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB, DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
	hw_write_masked(&dma_hw->ch[DMACH_PIO_PONG].al1_ctrl,
			DMACH_PIO_PONG << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB, DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
	dma_hw->abort = DMA_PIO_MASK;
	while (dma_hw->abort & DMA_PIO_MASK)
		tight_loop_contents();
	while (dma_channel_is_busy(DMACH_PIO_PING) || dma_channel_is_busy(DMACH_PIO_PONG))
		tight_loop_contents();
	hw_write_masked(&dma_hw->ch[DMACH_PIO_PING].al1_ctrl,
			DMACH_PIO_PONG << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB, DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
	hw_write_masked(&dma_hw->ch[DMACH_PIO_PONG].al1_ctrl,
			DMACH_PIO_PING << DMA_CH0_CTRL_TRIG_CHAIN_TO_LSB, DMA_CH0_CTRL_TRIG_CHAIN_TO_BITS);
	dma_hw->ints0 = DMA_PIO_MASK;
	irq_clear(DMA_IRQ_0);

	/* drop partial samples in the ISR and queued arm words */
	pio_sm_clear_fifos(pio, sm_data);
	pio_sm_restart(pio, sm_data);
	pio_interrupt_clear(pio, 0);
	irq_clear(PIO0_IRQ_0);

	running = false;
}

/* ------------------------------------------------------------------------
 * USB serial commands
 * ------------------------------------------------------------------------ */

static bool gpio_is_input(uint gpio)
{
	for (uint i = 0; i < count_of(la_input_pins); i++)
		if (la_input_pins[i] == gpio)
			return true;
	return false;
}

static void print_inputs(void)
{
	printf("GP0-11, GP20-22, GP26");
}

static void print_status(void)
{
	uint64_t samples = (uint64_t)cfg.lines * SAMPLES_PER_LINE;

	printf("trigger: GP%u %s edge, burst %lu lines = %llu samples = %llu us, ",
	       cfg.gpio, cfg.falling ? "falling" : "rising", (unsigned long)cfg.lines,
	       (unsigned long long)samples,
	       (unsigned long long)(samples * 1000000ull / SAMPLE_RATE_HZ));
	if (cfg.count)
		printf("%lu burst%s", (unsigned long)cfg.count, cfg.count == 1 ? "" : "s");
	else
		printf("re-arm forever");

	if (!running)
		printf(" [stopped]\n");
	else if (cfg.count && run_bursts >= cfg.count)
		printf(" [done, %lu captured]\n", (unsigned long)run_bursts);
	else
		printf(" [armed, %lu captured]\n", (unsigned long)run_bursts);
}

static void print_help(void)
{
	printf("\n16 bit logic analyzer, %lu samples/s, inputs ", (unsigned long)SAMPLE_RATE_HZ);
	print_inputs();
	printf("\nCommands:\n"
	       "  t <gpio> [r|f]  trigger GPIO, optionally the edge (rising/falling)\n"
	       "  e <r|f>         trigger edge\n"
	       "  l <lines>       burst length in hsdaoh lines (%u samples each)\n"
	       "  n <count>       number of bursts, 0 = re-arm forever\n"
	       "  a               (re)arm with the current settings\n"
	       "  s               stop\n"
	       "  ?               this help and the current settings\n"
	       "Changes re-arm immediately if the capture is running.\n",
	       SAMPLES_PER_LINE);
	print_status();
}

static char *skip_spaces(char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* parses r/rising or f/falling, returns false if neither */
static bool parse_edge(char *p, bool *falling)
{
	p = skip_spaces(p);
	if (tolower((unsigned char)*p) == 'r')
		*falling = false;
	else if (tolower((unsigned char)*p) == 'f')
		*falling = true;
	else
		return false;
	return true;
}

static bool parse_uint(char **p, unsigned long *val)
{
	char *start = skip_spaces(*p), *end;

	if (!isdigit((unsigned char)*start))
		return false;
	*val = strtoul(start, &end, 10);
	*p = end;
	return true;
}

static void handle_command(char *line)
{
	char *p = skip_spaces(line);
	char cmd = tolower((unsigned char)*p);
	trigger_cfg_t n = cfg;
	unsigned long v;

	if (cmd)
		p++;

	switch (cmd) {
	case 't':
		if (!parse_uint(&p, &v) || !gpio_is_input(v)) {
			printf("error: GPIO must be one of the inputs (");
			print_inputs();
			printf(")\n");
			return;
		}
		n.gpio = v;
		p = skip_spaces(p);
		if (*p && !parse_edge(p, &n.falling)) {
			printf("error: edge must be r or f\n");
			return;
		}
		break;
	case 'e':
		if (!parse_edge(p, &n.falling)) {
			printf("error: edge must be r or f\n");
			return;
		}
		break;
	case 'l':
		if (!parse_uint(&p, &v) || v < 1 || v > MAX_BURST_LINES) {
			printf("error: length must be 1 - %lu lines\n", (unsigned long)MAX_BURST_LINES);
			return;
		}
		n.lines = v;
		break;
	case 'n':
		if (!parse_uint(&p, &v)) {
			printf("error: count must be a number, 0 = forever\n");
			return;
		}
		n.count = v;
		break;
	case 'a':
		if (running)
			stop_capture();
		start_capture();
		print_status();
		return;
	case 's':
		if (running)
			stop_capture();
		print_status();
		return;
	case '?':
	case 'h':
		print_help();
		return;
	case 0:
		return;
	default:
		printf("unknown command '%c', type ? for help\n", cmd);
		return;
	}

	/* apply a changed setting */
	bool was_running = running;
	if (was_running)
		stop_capture();
	cfg = n;
	if (was_running)
		start_capture();
	print_status();
}

static void poll_serial(void)
{
	static char line[40];
	static uint len = 0;
	int c;

	while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
		if (c == '\r' || c == '\n') {
			if (len || c == '\r')
				printf("\n");
			line[len] = 0;
			if (len)
				handle_command(line);
			len = 0;
		} else if ((c == '\b' || c == 127) && len) {
			len--;
			printf("\b \b");
		} else if (c >= ' ' && len < sizeof(line) - 1) {
			line[len++] = c;
			putchar(c);
		}
	}
}

static void print_bursts(void)
{
	static uint32_t printed = 0;
	static uint64_t last_us = 0;

	while (printed != bursts_started) {
		/* if bursts arrive faster than USB serial can print, skip ahead */
		if (bursts_started - printed > TS_SLICES)
			printed = bursts_started - TS_SLICES;

		burst_record_t rec = burst_log[printed % TS_SLICES];

		printf("burst %llu: first sample %llu, t = %llu us (+%llu us)\n",
		       (unsigned long long)rec.burst, (unsigned long long)rec.first_sample,
		       (unsigned long long)rec.time_us,
		       (unsigned long long)(printed ? rec.time_us - last_us : 0));
		last_us = rec.time_us;
		printed++;
	}
}
#endif

static void dma_channel_setup(uint ch, uint chain_to, PIO pio)
{
	dma_channel_config c = dma_channel_get_default_config(ch);
	channel_config_set_chain_to(&c, chain_to);
	channel_config_set_dreq(&c, pio_get_dreq(pio, sm_data, false));
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_32);

	/* addresses and count are set by dma_start() */
	dma_channel_configure(ch, &c, ringbuffer, &pio->rxf[sm_data], LA_DATA_LEN / 2, false);
}

void init_pio_input(void)
{
	PIO pio = LA_PIO;
	sm_data = pio_claim_unused_sm(pio, true);

#if LA_MODE == LA_MODE_TRIGGERED
	pio_offset = pio_add_program(pio, &la_16bit_triggered_program);
	la_16bit_triggered_program_init(pio, sm_data, pio_offset, SAMPLE_CLKDIV,
					TRIGGER_GPIO, TRIGGER_FALLING_EDGE);
#else
	pio_offset = pio_add_program(pio, &la_16bit_continuous_program);
	la_16bit_continuous_program_init(pio, sm_data, pio_offset, SAMPLE_CLKDIV);
#endif

	dma_channel_setup(DMACH_PIO_PING, DMACH_PIO_PONG, pio);
	dma_channel_setup(DMACH_PIO_PONG, DMACH_PIO_PING, pio);

	dma_hw->inte0 |= DMA_PIO_MASK;
	irq_set_exclusive_handler(DMA_IRQ_0, pio_dma_irq_handler);

#if LA_MODE == LA_MODE_TRIGGERED
	pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
	irq_set_exclusive_handler(PIO0_IRQ_0, pio_trigger_irq_handler);
	irq_set_priority(PIO0_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);

	start_capture();
#else
	dma_start();
	pio_sm_set_enabled(pio, sm_data, true);
#endif
}

int main()
{
	/* set maximum 'allowed' voltage without voiding warranty */
	vreg_set_voltage(VREG_VOLTAGE_MAX);
	sleep_us(SYS_CLK_VREG_VOLTAGE_AUTO_ADJUST_DELAY_US);

	hsdaoh_set_sys_clock_khz(SYS_CLK);

	/* set HSTX clock to sysclk/2 */
	hw_write_masked(
		&clocks_hw->clk[clk_hstx].div,
		2 << CLOCKS_CLK_HSTX_DIV_INT_LSB,
		CLOCKS_CLK_HSTX_DIV_INT_BITS
	);

	stdio_init_all();

	hsdaoh_init(GPIO_DRIVE_STRENGTH_4MA, GPIO_SLEW_RATE_SLOW);
	hsdaoh_add_stream(0, STREAM_FORMAT, SAMPLE_RATE_HZ, LA_DATA_LEN, RBUF_DEFAULT_SLICES, ringbuffer);
#if LA_MODE == LA_MODE_TRIGGERED
	hsdaoh_add_stream(TS_STREAM_ID, RAW_64BIT, 0, TS_DATA_LEN, TS_SLICES, ts_ringbuffer);
#endif
	hsdaoh_start();
	init_pio_input();

	while (1) {
#if LA_MODE == LA_MODE_TRIGGERED
		poll_serial();
#if PRINT_BURSTS
		print_bursts();
#endif
#endif
		__wfi();
	}
}
