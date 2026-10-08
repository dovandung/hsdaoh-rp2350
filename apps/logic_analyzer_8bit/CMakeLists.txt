# logic_analyzer_8bit_add(<target> <mode>)
#   mode: 0 = continuous, 1 = triggered (see logic_analyzer_8bit.c)
function(logic_analyzer_8bit_add target mode)
	add_executable(${target}
		${CMAKE_CURRENT_LIST_DIR}/logic_analyzer_8bit.c
	)

	target_compile_options(${target} PRIVATE -Wall)
	target_compile_definitions(${target} PRIVATE LA_MODE=${mode})

	target_link_libraries(${target}
		pico_stdlib
		pico_util
		hardware_pio
		hardware_dma
		libpicohsdaoh
	)
	pico_generate_pio_header(${target} ${CMAKE_CURRENT_LIST_DIR}/8bit_input.pio)

	# enable usb output, disable uart output
	pico_enable_stdio_usb(${target} 1)

	# create map/bin/hex file etc.
	pico_add_extra_outputs(${target})
endfunction()

logic_analyzer_8bit_add(logic_analyzer_8bit 0)
logic_analyzer_8bit_add(logic_analyzer_8bit_triggered 1)
