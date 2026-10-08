add_executable(logic_analyzer
	logic_analyzer.c
)

target_compile_options(logic_analyzer PRIVATE -Wall)

target_link_libraries(logic_analyzer
	pico_stdlib
	pico_util
	hardware_pio
	hardware_dma
	libpicohsdaoh
)
pico_generate_pio_header(logic_analyzer ${CMAKE_CURRENT_LIST_DIR}/16bit_input.pio)

# enable usb output, disable uart output
pico_enable_stdio_usb(logic_analyzer 1)

# create map/bin/hex file etc.
pico_add_extra_outputs(logic_analyzer)

# Same app with the edge trigger / burst capture mode enabled
add_executable(logic_analyzer_triggered
	logic_analyzer.c
)

target_compile_options(logic_analyzer_triggered PRIVATE -Wall)
target_compile_definitions(logic_analyzer_triggered PRIVATE LA_MODE=1)

target_link_libraries(logic_analyzer_triggered
	pico_stdlib
	pico_util
	hardware_pio
	hardware_dma
	libpicohsdaoh
)
pico_generate_pio_header(logic_analyzer_triggered ${CMAKE_CURRENT_LIST_DIR}/16bit_input.pio)

pico_enable_stdio_usb(logic_analyzer_triggered 1)
pico_add_extra_outputs(logic_analyzer_triggered)
