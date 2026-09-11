#include <stdio.h>

#include "pico/stdlib.h"
#include "ff.h"
#include "diskio.h"
#include "tf_card.h"
#include "bdf_writer.h"

#define BDF_SPEED_TEST_FILE "0:/bdf_write_speed_100MiB.bdf"

static void print_fresult(const char *operation, FRESULT result) {
    printf("%s failed: FatFs error %d\n", operation, (int)result);
}

void sd_legacy_run(void) {
    for (int i = 0; i < 5; ++i) {
        printf("Pico 2 W starting BDF EEG writer (%d/5)\n", i + 1);
        sleep_ms(1000);
    }
    puts("SD wiring (SPI1): GP14=SCK, GP15=MOSI, GP12=MISO, GP13=CS, 3V3 and GND");

    pico_fatfs_spi_config_t config = {
        .spi_inst = spi1,
        .clk_slow = CLK_SLOW_DEFAULT,
        .clk_fast = 30 * MHZ,
        .pin_miso = 12,
        .pin_cs = 13,
        .pin_sck = 14,
        .pin_mosi = 15,
        .pullup = true,
    };
    if (!pico_fatfs_set_config(&config)) {
        pico_fatfs_config_spi_pio(SPI_PIO_DEFAULT_PIO, SPI_PIO_DEFAULT_SM);
    }

    FATFS fs;
    bdf_write_stats_t stats;
    DSTATUS disk_state = disk_initialize(0);
    printf("SD init status: 0x%02x (0 means ready), SPI: %u Hz\n",
           disk_state, pico_fatfs_get_clk_fast_freq());
    FRESULT result = f_mount(&fs, "0:", 1);
    if (result == FR_OK) {
        result = bdf_write_speed_test_file(BDF_SPEED_TEST_FILE, &stats);
    }

    if (result != FR_OK) {
        print_fresult("BDF write", result);
        printf("Rebooting SD SPI: %s\n",
               pico_fatfs_reboot_spi() ? "card responded" : "no card response");
    } else {
        uint64_t elapsed_ms = stats.elapsed_us / 1000u;
        uint64_t bytes_per_second = stats.elapsed_us ? ((uint64_t)stats.actual_bytes * 1000000u) / stats.elapsed_us : 0u;
        printf("BDF speed test complete: %s\n", BDF_SPEED_TEST_FILE);
        printf("  requested size : %lu bytes (100 MiB)\n", (unsigned long)stats.target_bytes);
        printf("  actual size    : %lu bytes\n", (unsigned long)stats.actual_bytes);
        printf("  header/data    : %lu / %lu bytes\n", (unsigned long)stats.header_bytes,
               (unsigned long)(stats.actual_bytes - stats.header_bytes));
        printf("  data records   : %lu x %lu bytes\n", (unsigned long)stats.records,
               (unsigned long)stats.record_bytes);
        printf("  data writes    : %lu calls, up to %lu bytes/call\n",
               (unsigned long)stats.data_write_calls, (unsigned long)(16u * stats.record_bytes));
        printf("  elapsed time   : %llu.%03llu s\n", elapsed_ms / 1000u, elapsed_ms % 1000u);
        printf("  write speed    : %llu B/s, %llu KiB/s, %llu.%03llu MiB/s\n",
               bytes_per_second, bytes_per_second / 1024u,
               bytes_per_second / (1024u * 1024u),
               (bytes_per_second % (1024u * 1024u)) * 1000u / (1024u * 1024u));
    }
    f_unmount("0:");

}
