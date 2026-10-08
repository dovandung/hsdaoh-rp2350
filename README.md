# hsdaoh-rp2350 - High Speed Data Acquisition over HDMI
## Stream up to 175 MByte/s from your Raspberry Pi Pico2 to your PC

Using $5 USB3 HDMI capture sticks based on the MacroSilicon MS2130, this project allows to stream out up to 175 MByte/s of real time data from an RP2350 (with overclocking) to a host computer with USB3.
For more information and the host library, see the [main repository](https://github.com/steve-m/hsdaoh) and the [talk at OsmoDevcon '24](https://media.ccc.de/v/osmodevcon2024-200-low-cost-high-speed-data-acquisition-over-hdmi).

![Raspberry Pi Pico2 with MS2130 stick](https://steve-m.de/projects/hsdaoh/pico2_hsdaoh.jpg)

## Building

Make sure you have the latest version of the [pico-sdk](https://github.com/raspberrypi/pico-sdk) installed together with an appropriate compiler. You should be able to build the [pico-examples](https://github.com/raspberrypi/pico-examples).

To build hsdaoh-rp2350:

    git clone https://github.com/steve-m/hsdaoh-rp2350.git
    mkdir hsdaoh-rp2350/build
    cd hsdaoh-rp2350/build
    export PICO_SDK_PATH=/<path-to>/pico-sdk
    cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2 ../
    make -j 8

After the build succeeds you can copy the resulting *.uf2 file of the application you want to run to the board.

### Building with VS Code

1. Install [VS Code](https://code.visualstudio.com/) and the [Raspberry Pi Pico extension](https://marketplace.visualstudio.com/items?itemName=raspberry-pi.raspberry-pi-pico).
2. Open the hsdaoh-rp2350 folder. The extension recognizes the project and downloads Pico SDK 2.3.1, the ARM toolchain, CMake, Ninja and picotool on first use.
3. Click **Compile** in the status bar (or run the *Compile Project* task). All apps are built for the Pico 2; the .uf2 files end up in `build/apps/<app>/`.
4. To flash, hold BOOTSEL while plugging in the board and copy the .uf2 file, or use **Run**, which asks which program to load. **Debug** needs a debug probe (e.g. the Raspberry Pi Debug Probe).

The project defaults to `PICO_BOARD=pico2`. Apps for an RP2350B board (dual_external_adc, sdr) need a different board: change `PICO_BOARD` in the top-level CMakeLists.txt or use *Switch Board* in the extension.

Apart from the Pico2 with the [Pico-DVI-Sock](https://github.com/Wren6991/Pico-DVI-Sock), it also should work with the Adafruit Feather RP2350 with HSTX Port, but so far only the Pico2 was tested.

## Example applications

The repository contains a library - libpicohsdaoh - which implements the main functionality. It reads the data from a ringbuffer, and streams it out via the HSTX port.
In addition to that, the apps folder contains a couple of example applications:

### counter

This application uses the PIO to generate a 16-bit counter value which is written to a DMA ringbuffer, which is then streamed out via hsdaoh. The counter can be verified using the hsdaoh_test host application.

### logic_analyzer

Sample 16 GPIOs with the PIO and transfer the data, can be used as a 16 bit @ 56 MHz logic analyzer (two samples are packed per PIO FIFO word, 6 cycles per sample at 336 MHz).
The IOs used for input are GP0-11, GP20-22 and GP26.

The logic_analyzer_triggered build waits for a rising or falling edge on one of the inputs and then streams a fixed-length burst at 48 MHz, re-arming automatically. The trigger time of every burst is sent as a second hsdaoh stream (stream ID 1) and printed on the USB serial port. The defaults for trigger channel, edge, burst length and burst count are set at the top of logic_analyzer.c, and can be changed at runtime with commands on the USB serial port:

| Command | Function |
|---|---|
| `t <gpio> [r\|f]` | trigger GPIO, optionally the edge (rising/falling) |
| `e <r\|f>` | trigger edge |
| `l <lines>` | burst length in hsdaoh lines |
| `n <count>` | number of bursts, 0 = re-arm forever |
| `a` | (re)arm with the current settings |
| `s` | stop |
| `?` | help and current settings |

Changes re-arm immediately if the capture is running.

The logic_analyzer_8bit and logic_analyzer_8bit_triggered builds sample 8 channels on GP0-7 with a single PIO instruction per sample, by default at 112 MHz (3 cycles per sample at 336 MHz). The sample period is set with LA8_CYCLES_PER_SAMPLE in logic_analyzer.c and is the same in both modes.

### internal_adc

The data from the internal ADC is streamed out via USB. Default configuration is overclocking the ADC to 3.33 MS/s. Using the USB PLL and overvolting beyond VREG_VOLTAGE_MAX, up to 7.9 MS/s can be achieved.

### external_adc

This app contains a PIO program that reads the data from a 12-bit ADC connected to GP0-GP11, outputs the ADC clock on GP20, and packs the 12-bit samples to 16-bit words to achieve maximum throughput.
It is meant to be used with cheap AD9226 ADC boards. The default setting is overclocking the RP2350 to 160 MHz and driving the ADC with a 40 MHz clock. With higher overclocking up to 96 MHz ADC clock can be used.

This can be used for sampling the IF of a tuner/downcoverter, as a direct-sampling HF SDR, or for capturing a video signal e.g. with [vhsdecode](https://github.com/oyvindln/vhs-decode).
For the vhsdecode use-case, there is also an [adapter PCB](https://github.com/Sev5000/Pico2_12bitADC_PCMAudio). It also supports sampling a PCM1802 audio ADC board.

![Pico2 with AD9226 ADC board](https://steve-m.de/projects/hsdaoh/rp2350_external_adc_rot.jpg)

### external_dualchan_adc

Similar to the external_adc app, but samples a AD9238 dual channel 12-bit ADC connected to GP0-GP11, with clock for both channels on GP20. The benefit of using the AD9238 is that only 12 data lines are required to transfer the data DDR-style, so it works with a regular RP2350A/Pico2 board.

### dual_external_adc

Also similar to the external_adc app, but samples two 12-bit ADCs connected to a RP2350B, as well as two PCM1802 modules. Intended for use with vhs-decode, see [this PCB](https://github.com/Sev5000/RP2350B_DualADC_DualPCM) for the matching hardware.
This example needs to be built with an RP2350B-board in order to work correctly:

    cmake -DPICO_PLATFORM=rp2350 -DPICO_BOARD=solderparty_rp2350_stamp_xl ../

### sdr

This app can be used for attaching the [hsdaohSDR prototype](https://github.com/steve-m/hsdaohSDR) to an RP2350B instead of the Tang nano 20K FPGA board. Contains PIO code for packing the 2x 10 bit AD9218 data, and also implements a USB UART to I2C bridge for controlling the tuner. Like the dual_external_adc, it also needs to be built with a RP2350B board.

## Credits

hsdaoh-rp2350 is developed by Steve Markgraf, and is based on the [dvi_out_hstx_encoder](https://github.com/raspberrypi/pico-examples/tree/master/hstx/dvi_out_hstx_encoder) example, and code by Shuichi Takano [implementing the HDMI data island encoding](https://github.com/shuichitakano/pico_lib/blob/master/dvi/data_packet.cpp), required to send HDMI info frames.
