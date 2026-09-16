# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(spi_dma C CXX ASM)

pico_sdk_init()

# USB isolation build: legacy Wi-Fi/SD sources remain in the repository only.
option(ENABLE_WIFI_STREAM "Unavailable in USB isolation build" OFF)
option(ENABLE_SD_CARD "Unavailable in USB isolation build" OFF)
option(ENABLE_UART_LOG "Unavailable in binary-only USB build" OFF)
option(ENABLE_ACQ_PROFILE "Detailed acquisition wall-time profiling" OFF)
if(ENABLE_WIFI_STREAM OR ENABLE_SD_CARD OR ENABLE_UART_LOG)
    message(FATAL_ERROR "USB isolation build requires Wi-Fi, SD and UART log OFF")
endif()
add_executable(spi_dma spi_dma.c ads1299.c ads1299_format.c
    usb_link.c usb_descriptors.c)
target_include_directories(spi_dma PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(spi_dma pico_stdlib hardware_spi hardware_dma tinyusb_device pico_unique_id)
target_compile_options(spi_dma PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(spi_dma PRIVATE ENABLE_ACQ_PROFILE=$<BOOL:${ENABLE_ACQ_PROFILE}>
    PICO_STDIO_DEFAULT_CRLF=0)
pico_enable_stdio_usb(spi_dma 0)
pico_enable_stdio_uart(spi_dma 0)
pico_add_extra_outputs(spi_dma)
