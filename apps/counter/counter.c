/*
 * hsdaoh - High Speed Data Acquisition over MS213x USB3 HDMI capture sticks
 * Implementation for the Raspberry Pi RP2350 HSTX peripheral
 *
 * PIO counter value example, counter can be verified with hsdaoh_test
 *
 * The HDMI (HSTX) clock runs from the USB PLL (like the sdr app), independent
 * of the system clock. The PIO needs 3 cycles per sample.
 *   counter      HSTX 432 MHz, 300 MHz / (3 * 1.25) = 80 MS/s (link load 96%)
 *   counter_70m  HSTX 384 MHz, 315 MHz / (3 * 1.5)  = 70 MS/s (95%)
 *   counter_60m  HSTX 336 MHz, 270 MHz / (3 * 1.5)  = 60 MS/s (93%)
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

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/pll.h"

#include "picohsdaoh.h"
#include "counter.pio.h"

/* defaults: 80 MS/s; the counter_70m / counter_60m targets override these */
#ifndef SYS_CLK
#define SYS_CLK		300000			/* kHz */
#endif
#ifndef HSTX_CLK_MHZ
#define HSTX_CLK_MHZ	432			/* from the USB PLL (VCO = 4x), multiple of 48 */
#endif
#ifndef COUNTER_CLKDIV
#define COUNTER_CLKDIV	1.25f			/* exact in the PIO's 16.8 fixed point divider */
#endif
#define COUNTER_RATE	((uint32_t)(SYS_CLK * 1000.0 / (3 * COUNTER_CLKDIV) + 0.5))	/* 80 MS/s */

#if HSTX_CLK_MHZ % 48
#error "HSTX_CLK_MHZ must be a multiple of 48 so the USB clock can stay at 48 MHz"
#endif

#define DMACH_PIO_PING 0
#define DMACH_PIO_PONG 1

static bool pio_dma_pong = false;
uint16_t ringbuffer[RBUF_DEFAULT_TOTAL_LEN];
int ringbuf_head = 2;

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

void init_pio_input(void)
{
	PIO pio = pio0;
	uint offset = pio_add_program(pio, &counter_program);
	uint sm_data = pio_claim_unused_sm(pio, true);
	counter_program_init(pio, sm_data, offset, COUNTER_CLKDIV);

	dma_channel_config c;
	c = dma_channel_get_default_config(DMACH_PIO_PING);
	channel_config_set_chain_to(&c, DMACH_PIO_PONG);
	channel_config_set_dreq(&c, pio_get_dreq(pio, sm_data, false));
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_16);

	dma_channel_configure(
		DMACH_PIO_PING,
		&c,
		&ringbuffer[0 * RBUF_SLICE_LEN],
		&pio->rxf[sm_data],
		RBUF_MAX_DATA_LEN,
		false
	);
	c = dma_channel_get_default_config(DMACH_PIO_PONG);
	channel_config_set_chain_to(&c, DMACH_PIO_PING);
	channel_config_set_dreq(&c, pio_get_dreq(pio, sm_data, false));
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, true);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_16);

	dma_channel_configure(
		DMACH_PIO_PONG,
		&c,
		&ringbuffer[1 * RBUF_SLICE_LEN],
		&pio->rxf[sm_data],
		RBUF_MAX_DATA_LEN,
		false
	);

	dma_hw->ints0 |= (1u << DMACH_PIO_PING) | (1u << DMACH_PIO_PONG);
	dma_hw->inte0 |= (1u << DMACH_PIO_PING) | (1u << DMACH_PIO_PONG);
	irq_set_exclusive_handler(DMA_IRQ_0, pio_dma_irq_handler);
	irq_set_enabled(DMA_IRQ_0, true);

	dma_channel_start(DMACH_PIO_PING);
}

int main()
{
	/* 1.50 V core voltage. This is above VREG_VOLTAGE_MAX (1.30 V), so the
	 * SDK's voltage limit has to be lifted first. Higher voltage = more heat
	 * and faster chip wear. */
	vreg_disable_voltage_limit();
	vreg_set_voltage(VREG_VOLTAGE_1_50);
	sleep_ms(100);		/* let the voltage settle before overclocking */

	hsdaoh_set_sys_clock_khz(SYS_CLK);

	/* HSTX from the USB PLL (as in the sdr app). USB still needs 48 MHz,
	 * so divide the USB clock down before changing the PLL. */
	hw_write_masked(&clocks_hw->clk[clk_usb].div,
			(HSTX_CLK_MHZ / 48) << CLOCKS_CLK_USB_DIV_INT_LSB,
			CLOCKS_CLK_USB_DIV_INT_BITS);
	pll_init(pll_usb, 1, HSTX_CLK_MHZ * 4 * MHZ, 2, 2);

	/* HSTX clock = PLL_USB / 1 */
	hw_write_masked(
		&clocks_hw->clk[clk_hstx].div,
		1 << CLOCKS_CLK_HSTX_DIV_INT_LSB,
		CLOCKS_CLK_HSTX_DIV_INT_BITS
	);
	hw_write_masked(&clocks_hw->clk[clk_hstx].ctrl,
			CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB << CLOCKS_CLK_HSTX_CTRL_AUXSRC_LSB,
			CLOCKS_CLK_HSTX_CTRL_AUXSRC_BITS);

	stdio_init_all();

	/* 864 Mbit/s per HDMI pin: strong drive, fast slew */
	hsdaoh_init(GPIO_DRIVE_STRENGTH_12MA, GPIO_SLEW_RATE_FAST);
	hsdaoh_add_stream(0, 1, COUNTER_RATE, RBUF_MAX_DATA_LEN, RBUF_DEFAULT_SLICES, ringbuffer);
	hsdaoh_start();
	init_pio_input();

	while (1)
		__wfi();
}
