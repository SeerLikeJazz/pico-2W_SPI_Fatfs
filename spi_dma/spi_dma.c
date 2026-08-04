#include <stdio.h>

#include "pico/stdlib.h"
#include "ff.h"
#include "diskio.h"
#include "tf_card.h"
#include "bdf_writer.h"

#define BDF_FILE "0:/eeg_8ch_250hz_10s.bdf"

static void print_fresult(const char *operation, FRESULT result) {
    printf("%s failed: FatFs error %d\n", operation, (int)result);
}

int main(void) {
    stdio_init_all();

    for (int i = 0; i < 5; ++i) {
        printf("Pico 2 W starting BDF EEG writer (%d/5)\n", i + 1);
        sleep_ms(1000);
    }
    puts("SD wiring: GP2=SCK, GP3=MOSI, GP4=MISO, GP5=CS, 3V3 and GND");

    pico_fatfs_spi_config_t config = {
        .spi_inst = spi0,
        .clk_slow = CLK_SLOW_DEFAULT,
        .clk_fast = 30 * MHZ,
        .pin_miso = PIN_SPI0_MISO_DEFAULT,
        .pin_cs = PIN_SPI0_CS_DEFAULT,
        .pin_sck = PIN_SPI0_SCK_DEFAULT,
        .pin_mosi = PIN_SPI0_MOSI_DEFAULT,
        .pullup = true,
    };
    if (!pico_fatfs_set_config(&config)) {
        pico_fatfs_config_spi_pio(SPI_PIO_DEFAULT_PIO, SPI_PIO_DEFAULT_SM);
    }

    FATFS fs;
    UINT bytes_written = 0;
    DSTATUS disk_state = disk_initialize(0);
    printf("SD init status: 0x%02x (0 means ready), SPI: %u Hz\n",
           disk_state, pico_fatfs_get_clk_fast_freq());
    FRESULT result = f_mount(&fs, "0:", 1);
    if (result == FR_OK) {
        result = bdf_write_demo_file(BDF_FILE, &bytes_written);
    }

    if (result != FR_OK) {
        print_fresult("BDF write", result);
        printf("Rebooting SD SPI: %s\n",
               pico_fatfs_reboot_spi() ? "card responded" : "no card response");
    } else {
        printf("PASS: wrote %u-byte 8-channel, 250 Hz, 10 s BDF file: %s\n",
               bytes_written, BDF_FILE);
    }
    f_unmount("0:");

    while (true) {
        tight_loop_contents();
    }
}