# logic_analyzer_add(<target> <bits> <mode>)
#   bits: 16 or 8, mode: 0 = continuous, 1 = triggered (see logic_analyzer.c)
function(logic_analyzer_add target bits mode)
	add_executable(${target}
		${CMAKE_CURRENT_LIST_DIR}/logic_analyzer.c
	)

	target_compile_options(${target} PRIVATE -Wall)
	target_compile_definitions(${target} PRIVATE LA_BITS=${bits} LA_MODE=${mode})

	target_link_libraries(${target}
		pico_stdlib
		pico_util
		hardware_pio
		hardware_dma
		libpicohsdaoh
	)
	pico_generate_pio_header(${target} ${CMAKE_CURRENT_LIST_DIR}/${bits}bit_input.pio)

	# enable usb output, disable uart output
	pico_enable_stdio_usb(${target} 1)

	# create map/bin/hex file etc.
	pico_add_extra_outputs(${target})
endfunction()

# 16 channels: GP0-11, GP20-22, GP26
logic_analyzer_add(logic_analyzer 16 0)
logic_analyzer_add(logic_analyzer_triggered 16 1)

# 8 channels: GP0-7
logic_analyzer_add(logic_analyzer_8bit 8 0)
logic_analyzer_add(logic_analyzer_8bit_triggered 8 1)
