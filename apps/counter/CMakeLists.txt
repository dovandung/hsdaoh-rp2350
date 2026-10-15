# counter variants, HDMI clock from the USB PLL:
#   counter      speed set by COUNTER_PRESET in counter_config.h (default 70 MS/s)
#   counter_70m  HSTX 384 MHz, 70 MS/s
#   counter_60m  HSTX 336 MHz, 60 MS/s (same HDMI speed as sys_clk 336 MHz)
#   counter_74m / _75m / _78m  HSTX 399 / 408 / 420 MHz, USB clocked from the system PLL
function(add_counter name)
	add_executable(${name} counter.c)
	target_compile_options(${name} PRIVATE -Wall)
	target_compile_definitions(${name} PRIVATE ${ARGN})
	target_link_libraries(${name}
		pico_stdlib
		pico_multicore
		pico_util
		hardware_pio
		hardware_dma
		hardware_pll
		libpicohsdaoh
	)
	pico_generate_pio_header(${name} ${CMAKE_CURRENT_LIST_DIR}/counter.pio)
	# enable usb output
	pico_enable_stdio_usb(${name} 1)
	# create map/bin/hex file etc.
	pico_add_extra_outputs(${name})
endfunction()

add_counter(counter)
add_counter(counter_70m HSTX_CLK_MHZ=384 SYS_CLK=315000 COUNTER_CLKDIV=1.5f)
add_counter(counter_60m HSTX_CLK_MHZ=336 SYS_CLK=270000 COUNTER_CLKDIV=1.5f)

# USB from the system PLL (sys 336 MHz / 7 = 48 MHz), HDMI clock between 384 and 432 MHz
add_counter(counter_74m USB_FROM_SYS_PLL=1 HSTX_CLK_MHZ=399 SYS_CLK=336000 COUNTER_CLKDIV=1.51953125f)
add_counter(counter_75m USB_FROM_SYS_PLL=1 HSTX_CLK_MHZ=408 SYS_CLK=336000 COUNTER_CLKDIV=1.484375f)
add_counter(counter_78m USB_FROM_SYS_PLL=1 HSTX_CLK_MHZ=420 SYS_CLK=336000 COUNTER_CLKDIV=1.44140625f)
