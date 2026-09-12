# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ============================================================
# Pico 2
# ============================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# ============================================================
# TinyUSB 0.20.0
# MUST be set before pico_sdk_import.cmake
set(PICO_TINYUSB_PATH ${CMAKE_CURRENT_LIST_DIR}/tinyusb/
    CACHE PATH "TinyUSB path" FORCE
)
# ============================================================

include(pico_sdk_import.cmake)
include(FreeRTOS_Kernel_import.cmake)
project(v1 C CXX ASM)
pico_sdk_init()
# ============================================================
# Application
# ============================================================
add_executable(v1 v1.c

    network.c
    audiorcv.c
    config.c
    ptp.c
    aes67.c
    sap.c

    usb_descriptors.c
    ioLibrary_Driver/Ethernet/wizchip_conf.c
    ioLibrary_Driver/Ethernet/socket.c
    ioLibrary_Driver/Ethernet/W5500/w5500.c

    terminal.c
    cli.c
    tui.c
)
pico_set_program_name(v1 "v1")
pico_set_program_version(v1 "0.1")
# ============================================================
# STDIO
# ============================================================
pico_enable_stdio_uart(v1 0)
pico_enable_stdio_usb(v1 0)
# ============================================================
# Libraries
# ============================================================
target_link_libraries(
    v1
    pico_stdlib
    pico_unique_id
    tinyusb_device
    tinyusb_board
    hardware_pwm
    hardware_irq
    hardware_clocks
    hardware_spi
    hardware_timer
    hardware_flash
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)
# ============================================================
# Include directories
# ============================================================
target_include_directories(
    v1
    PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/ioLibrary_Driver/Ethernet
    ${CMAKE_CURRENT_LIST_DIR}/ioLibrary_Driver/Ethernet/W5500
)
# ============================================================
# WIZnet configuration
# ============================================================
target_compile_definitions(v1 PRIVATE
    _WIZCHIP_=W5500
)
# ============================================================
# UF2 etc.
# ============================================================
pico_add_extra_outputs(v1)
