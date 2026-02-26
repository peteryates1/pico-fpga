cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico)
endif()

include(pico_sdk_import.cmake)

project(pico_fpga C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(pico_fpga
    main.c
    cmd.c
    pin_manager.c
    pio_alloc.c
    gpio_cmd.c
    la.c
    uart/pio_uart.c
    uart/hw_uart.c
    jtag/jtag.c
    jtag/jtag_tap.c
    jtag/svf_player.c
    jtag/fpga_xilinx.c
    jtag/fpga_altera.c
    jtag/fpga_lattice.c
)

pico_generate_pio_header(pico_fpga ${CMAKE_CURRENT_LIST_DIR}/logic_analyzer.pio)
pico_generate_pio_header(pico_fpga ${CMAKE_CURRENT_LIST_DIR}/uart_tx.pio)
pico_generate_pio_header(pico_fpga ${CMAKE_CURRENT_LIST_DIR}/uart_rx.pio)
pico_generate_pio_header(pico_fpga ${CMAKE_CURRENT_LIST_DIR}/jtag.pio)

target_include_directories(pico_fpga PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/uart
    ${CMAKE_CURRENT_LIST_DIR}/jtag
)

target_compile_definitions(pico_fpga PRIVATE
    USBD_PRODUCT="Pico FPGA Debug Tool"
    USBD_DESC_STR_MAX=32
)

target_link_libraries(pico_fpga
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_uart
)

pico_enable_stdio_usb(pico_fpga 1)
pico_enable_stdio_uart(pico_fpga 0)

pico_add_extra_outputs(pico_fpga)
