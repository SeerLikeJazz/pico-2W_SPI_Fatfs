#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "ff.h"
#include "diskio.h"
#include "tf_card.h"

#define SD_TEST_FILE "0:/fatfs_rw_test.txt"

static void print_fresult(const char *operation, FRESULT result) {
    printf("%s failed: FatFs error %d\n", operation, (int)result);
}

int main(void) {
    stdio_init_all();

    // Startup text is repeated so a USB terminal opened after reset sees it.
    for (int i = 0; i < 5; ++i) {
        printf("Pico 2 W starting FatFs SD-card test (%d/5)\n", i + 1);
        sleep_ms(1000);
    }
    puts("SD wiring: GP2=SCK, GP3=MOSI, GP4=MISO, GP5=CS, 3V3 and GND");

    pico_fatfs_spi_config_t config = {
        .spi_inst = spi0,
        .clk_slow = CLK_SLOW_DEFAULT,
        .clk_fast = 10 * MHZ, // Conservative speed for breadboard/module wiring.
        .pin_miso = PIN_SPI0_MISO_DEFAULT,
        .pin_cs = PIN_SPI0_CS_DEFAULT,
        .pin_sck = PIN_SPI0_SCK_DEFAULT,
        .pin_mosi = PIN_SPI0_MOSI_DEFAULT,
        .pullup = true,
    };
    bool hardware_spi = pico_fatfs_set_config(&config);
    if (!hardware_spi) {
        pico_fatfs_config_spi_pio(SPI_PIO_DEFAULT_PIO, SPI_PIO_DEFAULT_SM);
    }

    const char write_data[] = "Pico FatFs read/write test passed.\r\n";
    while (true) {
        FATFS fs;
        FIL file;
        char read_data[sizeof(write_data)] = {0};
        UINT bytes_written = 0;
        UINT bytes_read = 0;

        DSTATUS disk_state = disk_initialize(0);
        printf("SD init status: 0x%02x (0 means ready), SPI: %u Hz\n",
               disk_state, pico_fatfs_get_clk_fast_freq());
        FRESULT result = f_mount(&fs, "0:", 1);

        if (result == FR_OK) {
            result = f_open(&file, SD_TEST_FILE, FA_CREATE_ALWAYS | FA_WRITE);
        }
        if (result == FR_OK) {
            result = f_write(&file, write_data, sizeof(write_data) - 1, &bytes_written);
        }
        if (result == FR_OK && bytes_written != sizeof(write_data) - 1) {
            result = FR_DISK_ERR;
        }
        if (result == FR_OK) {
            result = f_sync(&file);
        }
        if (result == FR_OK) {
            result = f_close(&file);
        }
        if (result == FR_OK) {
            result = f_open(&file, SD_TEST_FILE, FA_READ);
        }
        if (result == FR_OK) {
            result = f_read(&file, read_data, sizeof(write_data) - 1, &bytes_read);
        }
        if (result == FR_OK) {
            result = f_close(&file);
        }

        if (result != FR_OK || bytes_read != sizeof(write_data) - 1 ||
            memcmp(write_data, read_data, sizeof(write_data) - 1) != 0) {
            print_fresult("Read/write test", result != FR_OK ? result : FR_INT_ERR);
            printf("Rebooting SD SPI: %s\n",
                   pico_fatfs_reboot_spi() ? "card responded" : "no card response");
            f_unmount("0:");
        } else {
            printf("PASS: wrote and verified %u bytes in %s\n", bytes_read, SD_TEST_FILE);
        }
        sleep_ms(3000);
    }
}