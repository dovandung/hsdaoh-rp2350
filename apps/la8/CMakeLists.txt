add_executable(la8
	la8.c
)

target_compile_options(la8 PRIVATE -Wall)

target_link_libraries(la8
	pico_stdlib
	pico_util
	hardware_pio
	hardware_dma
	libpicohsdaoh
)
pico_generate_pio_header(la8 ${CMAKE_CURRENT_LIST_DIR}/la8.pio)

# enable usb output, disable uart output
pico_enable_stdio_usb(la8 1)
# GP0/GP1 are logic analyzer inputs, so no UART
pico_enable_stdio_uart(la8 0)

# create map/bin/hex file etc.
pico_add_extra_outputs(la8)
