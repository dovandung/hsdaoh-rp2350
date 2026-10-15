# la8 targets:
#   la8      speed from LA8_PRESET in la8_config.h (default 140 MS/s)
#   la8_140  HDMI clock 384 MHz from the USB PLL, 280 MHz / 2 = 140 MS/s
#   la8_158  HDMI clock 432 MHz from the USB PLL, 316 MHz / 2 = 158 MS/s
#   la8_106  HDMI clock = system clock 320 MHz, 320 MHz / 3 = 106.67 MS/s
function(add_la8 name)
	add_executable(${name} la8.c)
	target_compile_options(${name} PRIVATE -Wall)
	target_compile_definitions(${name} PRIVATE ${ARGN})
	target_link_libraries(${name}
		pico_stdlib
		pico_util
		hardware_pio
		hardware_dma
		hardware_pll
		libpicohsdaoh
	)
	pico_generate_pio_header(${name} ${CMAKE_CURRENT_LIST_DIR}/la8.pio)
	# USB serial for the "div" command; GP0/GP1 are logic analyzer inputs, so no UART
	pico_enable_stdio_usb(${name} 1)
	pico_enable_stdio_uart(${name} 0)
	pico_add_extra_outputs(${name})
endfunction()

add_la8(la8)			# speed from LA8_PRESET in la8_config.h (default 140 MS/s)
add_la8(la8_140 LA8_PRESET=140)
add_la8(la8_158 LA8_PRESET=158)
add_la8(la8_106 LA8_PRESET=106)
