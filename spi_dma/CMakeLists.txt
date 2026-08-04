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

# Vendored FatFs R0.15 with an SD-card SPI/PIO-SPI disk I/O driver.
add_subdirectory(pico_fatfs_reference)

add_executable(spi_dma
        spi_dma.c
                bdf_writer.c
        )

target_link_libraries(spi_dma
        pico_stdlib
        pico_fatfs
        )

# Send logs to both USB CDC and UART0, so either terminal connection works.
pico_enable_stdio_usb(spi_dma 1)
pico_enable_stdio_uart(spi_dma 1)

pico_add_extra_outputs(spi_dma)