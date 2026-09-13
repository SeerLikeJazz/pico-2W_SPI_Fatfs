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

option(ENABLE_SD_CARD "Build manually triggered legacy SD/BDF test" OFF)
option(ENABLE_UART_LOG "Mirror logs on UART0 TX GP0 (conflicts with screen)" OFF)
option(ENABLE_WIFI_STREAM "Core 1 AP and EEG TCP streaming" ON)

add_executable(spi_dma
        spi_dma.c
        ads1299.c
        ads1299_format.c
        ads1299_control.c
        usb_commands.c
        debug_console.c
        )
target_include_directories(spi_dma PRIVATE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(spi_dma
        pico_stdlib
        hardware_spi
        hardware_dma
        )

if(ENABLE_SD_CARD)
    add_subdirectory(pico_fatfs_reference)
    target_sources(spi_dma PRIVATE sd_legacy.c bdf_writer.c)
    target_link_libraries(spi_dma pico_fatfs)
endif()

if(ENABLE_WIFI_STREAM)
    target_sources(spi_dma PRIVATE ../Code/firmware/wifi_control.c)
    target_include_directories(spi_dma PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../Code/firmware)
    set_source_files_properties(../Code/firmware/wifi_control.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
    target_sources(spi_dma PRIVATE net/eeg_stream.c net/wifi_stream.c net/dhcpserver.c)
    target_include_directories(spi_dma PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/net)
    target_link_libraries(spi_dma pico_multicore pico_rand pico_cyw43_arch_lwip_poll)
    target_compile_definitions(spi_dma PRIVATE PICO_CYW43_LOGGING_ENABLED=0)
    pico_configure_ip4_address(spi_dma PRIVATE CYW43_DEFAULT_IP_AP_ADDRESS 192.168.4.1)
endif()

# Strict checks on maintained application sources; leave vendored code flags alone.
set_source_files_properties(spi_dma.c ads1299.c ads1299_format.c ads1299_control.c usb_commands.c debug_console.c sd_legacy.c
    net/eeg_stream.c net/wifi_stream.c net/dhcpserver.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
target_compile_definitions(spi_dma PRIVATE
    ENABLE_SD_CARD=$<BOOL:${ENABLE_SD_CARD}>
    ENABLE_UART_LOG=$<BOOL:${ENABLE_UART_LOG}>
    ENABLE_WIFI_STREAM=$<BOOL:${ENABLE_WIFI_STREAM}>
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=0
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=0
    PICO_STDIO_USB_STDOUT_TIMEOUT_US=0
    PICO_STDIO_DEADLOCK_TIMEOUT_MS=0
)

pico_enable_stdio_usb(spi_dma 1)
pico_enable_stdio_uart(spi_dma ${ENABLE_UART_LOG})

pico_add_extra_outputs(spi_dma)
