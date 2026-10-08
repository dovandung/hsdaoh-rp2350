/*
 * hsdaoh - High Speed Data Acquisition over MS213x USB3 HDMI capture sticks
 * Implementation for the Raspberry Pi RP2350 HSTX peripheral
 *
 * 8 bit logic analyzer example, with optional edge trigger and burst capture
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

/* sample rate = SYS_CLK / (LA8_CYCLES_PER_SAMPLE * SAMPLE_CLKDIV),
 * the same in continuous and triggered mode */
#define SYS_CLK			336000

/* first of the 8 contiguous inputs (0 - 4, GP12 - GP19 are used by HSTX) */
#define LA8_PIN_BASE		0

/* PIO cycles per sample, 2 - 32 (1 is also allowed in continuous mode).
 * 3 = 112 MS/s = 112 MByte/s. 2 = 168 MS/s is above what the hsdaoh examples
 * have been shown to sustain (~144 MByte/s); if the capture stick can't keep
 * up, the host reports overflows. */
#define LA8_CYCLES_PER_SAMPLE	3

#define LA_MODE_CONTINUOUS	0	/* stream everything */
#define LA_MODE_TRIGGERED	1	/* wait for an edge, then stream a fixed-length burst */

#ifndef LA_MODE
#define LA_MODE			LA_MODE_CONTINUOUS
#endif

/* PIO clock divider, integer only (1 = full speed) */
#define SAMPLE_CLKDIV		1

/* Triggered mode settings */
#define TRIGGER_GPIO		0	/* any of the sampled GPIOs */
#define TRIGGER_FALLING_EDGE	false
#define BURST_SLICES		64	/* burst length in hsdaoh lines (3832 samples each) */
#define BURST_COUNT		0	/* number of bursts, 0 = re-arm forever, 1 = single shot */
#define PRINT_BURSTS		1	/* also print burst timestamps on the USB serial port */

/* ------------------------------------------------------------------------ */

#include "8bit_input.pio.h"

#define CYCLES_PER_SAMPLE	LA8_CYCLES_PER_SAMPLE
#define SAMPLES_PER_WORD	2	/* samples per 16 bit hsdaoh word */
#define STREAM_FORMAT		RAW_8BIT

static_assert(LA8_PIN_BASE + 7 < 12, "8 bit inputs must not overlap the HSTX pins GP12 - GP19");
static_assert(LA_MODE != LA_MODE_TRIGGERED ||
	      (TRIGGER_GPIO >= LA8_PIN_BASE && TRIGGER_GPIO < LA8_PIN_BASE + 8),
	      "TRIGGER_GPIO must be one of the 8 inputs");
static_assert(CYCLES_PER_SAMPLE >= (LA_MODE == LA_MODE_TRIGGERED ? 2 : 1) &&
	      CYCLES_PER_SAMPLE <= 32, "LA8_CYCLES_PER_SAMPLE out of range");

/* exact sample rate, reported to the host via the hsdaoh metadata */
#define SAMPLE_RATE_HZ		((SYS_CLK * 1000u) / (CYCLES_PER_SAMPLE * SAMPLE_CLKDIV))

/* 32 bit DMA words, so use an even number of 16 bit words per line */
#define LA_DATA_LEN		(RBUF_MAX_DATA_LEN & ~1u)
#define SAMPLES_PER_LINE	(LA_DATA_LEN * SAMPLES_PER_WORD)
#define BURST_SAMPLES		(BURST_SLICES * SAMPLES_PER_LINE)

#define LA_PIO			pio0
#define DMACH_PIO_PING		0
#define DMACH_PIO_PONG		1

static uint sm_data;
static bool pio_dma_pong = false;
uint16_t __attribute__((aligned(4))) ringbuffer[RBUF_DEFAULT_TOTAL_LEN];

/* index of the slice the most recently (re)programmed DMA channel writes to */
static int ringbuf_head = 0;

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

	hsdaoh_update_head(0, ringbuf_head);
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

typedef struct {
	uint64_t burst;
	uint64_t first_sample;
	uint64_t time_us;
} burst_record_t;

uint16_t __attribute__((aligned(8))) ts_ringbuffer[TS_SLICES * RBUF_SLICE_LEN];
static int ts_slice = TS_SLICES - 1;
static volatile uint32_t bursts_started = 0;
static burst_record_t burst_log[TS_SLICES];	/* copy for printing */
static uint32_t bursts_armed = 0;

static inline void arm_burst(void)
{
	/* the program samples x + 1 times */
	pio_sm_put(LA_PIO, sm_data, BURST_SAMPLES - 1);
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
		.first_sample = (uint64_t)bursts_started * BURST_SAMPLES,
		.time_us = now,
	};
	memcpy(&ts_ringbuffer[ts_slice * RBUF_SLICE_LEN], &rec, sizeof(rec));
	burst_log[bursts_started % TS_SLICES] = rec;
	hsdaoh_update_head(TS_STREAM_ID, (ts_slice + 2) % TS_SLICES);
	ts_slice = (ts_slice + 1) % TS_SLICES;

	bursts_started++;

	/* queue the next burst while this one is running: zero re-arm time */
	if (BURST_COUNT == 0 || bursts_armed < BURST_COUNT)
		arm_burst();
}
#endif

void init_pio_input(void)
{
	PIO pio = LA_PIO;
	sm_data = pio_claim_unused_sm(pio, true);

#if LA_MODE == LA_MODE_TRIGGERED
	la_8bit_triggered_program_init(pio, sm_data, LA8_PIN_BASE, CYCLES_PER_SAMPLE,
				       SAMPLE_CLKDIV, TRIGGER_GPIO, TRIGGER_FALLING_EDGE);
#else
	la_8bit_continuous_program_init(pio, sm_data, LA8_PIN_BASE, CYCLES_PER_SAMPLE,
					SAMPLE_CLKDIV);
#endif

	dma_channel_config c;
	c = dma_channel_get_default_config(DMACH_PIO_PING);
	channel_config_set_chain_to(&c, DMACH_PIO_PONG);
	channel_config_set_dreq(&c, pio_get_dreq(pio, sm_data, false));
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_32);

	dma_channel_configure(
		DMACH_PIO_PING,
		&c,
		&ringbuffer[(RBUF_DEFAULT_SLICES - 1) * RBUF_SLICE_LEN],
		&pio->rxf[sm_data],
		LA_DATA_LEN / 2,
		false
	);
	c = dma_channel_get_default_config(DMACH_PIO_PONG);
	channel_config_set_chain_to(&c, DMACH_PIO_PING);
	channel_config_set_dreq(&c, pio_get_dreq(pio, sm_data, false));
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_32);

	dma_channel_configure(
		DMACH_PIO_PONG,
		&c,
		&ringbuffer[0 * RBUF_SLICE_LEN],
		&pio->rxf[sm_data],
		LA_DATA_LEN / 2,
		false
	);

	dma_hw->ints0 |= (1u << DMACH_PIO_PING) | (1u << DMACH_PIO_PONG);
	dma_hw->inte0 |= (1u << DMACH_PIO_PING) | (1u << DMACH_PIO_PONG);
	irq_set_exclusive_handler(DMA_IRQ_0, pio_dma_irq_handler);
	irq_set_enabled(DMA_IRQ_0, true);

	dma_channel_start(DMACH_PIO_PING);

#if LA_MODE == LA_MODE_TRIGGERED
	pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
	irq_set_exclusive_handler(PIO0_IRQ_0, pio_trigger_irq_handler);
	irq_set_priority(PIO0_IRQ_0, PICO_HIGHEST_IRQ_PRIORITY);
	irq_set_enabled(PIO0_IRQ_0, true);

	arm_burst();
#endif

	pio_sm_set_enabled(pio, sm_data, true);
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

#if LA_MODE == LA_MODE_TRIGGERED && PRINT_BURSTS
	uint32_t printed = 0;
	uint64_t last_us = 0;

	while (1) {
		while (printed == bursts_started)
			__wfi();

		/* if bursts arrive faster than USB serial can print, skip ahead */
		if (bursts_started - printed > TS_SLICES)
			printed = bursts_started - TS_SLICES;

		burst_record_t rec = burst_log[printed % TS_SLICES];

		printf("burst %llu: first sample %llu, t = %llu us (+%llu us)\n",
		       rec.burst, rec.first_sample, rec.time_us,
		       printed ? rec.time_us - last_us : 0);
		last_us = rec.time_us;
		printed++;
	}
#else
	while (1)
		__wfi();
#endif
}
