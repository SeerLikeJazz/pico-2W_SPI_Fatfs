#include "bdf_writer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"

#define EEG_CHANNELS 8u
#define BDF_SIGNALS (EEG_CHANNELS + 1u)
#define BDF_SAMPLE_RATE_HZ 250u
#define BDF_RECORDS 10u
#define EEG_SAMPLES_PER_RECORD BDF_SAMPLE_RATE_HZ
#define ANNOTATION_SAMPLES_PER_RECORD 64u
#define BDF_HEADER_BYTES (256u + BDF_SIGNALS * 256u)
#define BDF_RECORD_BYTES ((EEG_CHANNELS * EEG_SAMPLES_PER_RECORD + ANNOTATION_SAMPLES_PER_RECORD) * 3u)
#define BDF_BENCHMARK_TARGET_BYTES (100u * 1024u * 1024u)
#define BDF_BENCHMARK_BUFFER_RECORDS 16u
#define BDF_DATA_BYTES (BDF_RECORDS * BDF_RECORD_BYTES)
#define BDF_DIGITAL_MIN (-8388608L)
#define BDF_DIGITAL_MAX 8388607L
#define BDF_PHYSICAL_MIN_UV (-187500L)
#define BDF_PHYSICAL_MAX_UV 187500L

static FRESULT write_exact(FIL *file, const void *data, UINT length, UINT *total)
{
    UINT written;
    FRESULT result = f_write(file, data, length, &written);
    if (result != FR_OK) return result;
    if (written != length) return FR_DISK_ERR;
    *total += written;
    return FR_OK;
}

static FRESULT write_text_field(FIL *file, const char *text, UINT width, UINT *total)
{
    char field[80];
    size_t length = strlen(text);
    if (width > sizeof(field)) return FR_INVALID_PARAMETER;
    memset(field, ' ', width);
    if (length > width) length = width;
    memcpy(field, text, length);
    return write_exact(file, field, width, total);
}

static FRESULT write_number_field(FIL *file, long value, UINT width, UINT *total)
{
    char number[24];
    (void)snprintf(number, sizeof(number), "%ld", value);
    return write_text_field(file, number, width, total);
}

/* Replace this deterministic test waveform with the signed samples from the AFE. */
static int32_t demo_eeg_sample(uint32_t channel, uint32_t sample)
{
    uint32_t phase = (sample * (channel + 1u) * 3u) % BDF_SAMPLE_RATE_HZ;
    int32_t triangle = phase < 125u ? (int32_t)phase : (int32_t)(250u - phase);
    int32_t microvolts = (triangle - 62) * (int32_t)(channel + 1u) / 3;
    return (microvolts * BDF_DIGITAL_MAX) / BDF_PHYSICAL_MAX_UV;
}

static void encode_int24(int32_t sample, uint8_t output[3])
{
    if (sample > BDF_DIGITAL_MAX) sample = BDF_DIGITAL_MAX;
    if (sample < BDF_DIGITAL_MIN) sample = BDF_DIGITAL_MIN;
    output[0] = (uint8_t)sample;
    output[1] = (uint8_t)(sample >> 8);
    output[2] = (uint8_t)(sample >> 16);
}

/* A BDF annotation character occupies the least-significant byte of one 24-bit sample. */
static void write_annotation_block(uint32_t record, uint8_t block[ANNOTATION_SAMPLES_PER_RECORD * 3u])
{
    char tal[32];
    uint32_t chars;
    memset(block, 0, ANNOTATION_SAMPLES_PER_RECORD * 3u);
    chars = (uint32_t)snprintf(tal, sizeof(tal), "+%lu\x14\x14", (unsigned long)record);
    tal[chars++] = '\0';
    if (record == 2u || record == 7u) {
        const char *name = record == 2u ? "T1" : "T2";
        chars += (uint32_t)snprintf(&tal[chars], sizeof(tal) - chars, "+%lu\x14%s\x14", (unsigned long)record, name);
        tal[chars++] = '\0';
    }
    /* BDF annotation samples are 24-bit, so all three bytes form one
       consecutive TAL byte stream (not one character per 24-bit sample). */
    memcpy(block, tal, chars);
}

static FRESULT write_bdf_header(FIL *file, UINT *total, uint32_t records)
{
    FRESULT result;
    char label[17];
    const uint8_t version[8] = {0xff, 'B', 'I', 'O', 'S', 'E', 'M', 'I'};
    result = write_exact(file, version, sizeof(version), total);
    if (result != FR_OK) return result;
    if ((result = write_text_field(file, "X X X X", 80, total)) != FR_OK) return result;
    if ((result = write_text_field(file, "Startdate 01-JAN-2026 X X X X", 80, total)) != FR_OK) return result;
    if ((result = write_text_field(file, "01.01.26", 8, total)) != FR_OK) return result;
    if ((result = write_text_field(file, "00.00.00", 8, total)) != FR_OK) return result;
    if ((result = write_number_field(file, BDF_HEADER_BYTES, 8, total)) != FR_OK) return result;
    if ((result = write_text_field(file, "BDF+C", 44, total)) != FR_OK) return result;
    if ((result = write_number_field(file, records, 8, total)) != FR_OK) return result;
    if ((result = write_text_field(file, "1", 8, total)) != FR_OK) return result;
    if ((result = write_number_field(file, BDF_SIGNALS, 4, total)) != FR_OK) return result;

    for (uint32_t ch = 0; ch < EEG_CHANNELS; ++ch) {
        (void)snprintf(label, sizeof(label), "EEG Ch%lu", (unsigned long)(ch + 1u));
        if ((result = write_text_field(file, label, 16, total)) != FR_OK) return result;
    }
    if ((result = write_text_field(file, "BDF Annotations", 16, total)) != FR_OK) return result;
    for (uint32_t signal = 0; signal < BDF_SIGNALS; ++signal) if ((result = write_text_field(file, "", 80, total)) != FR_OK) return result;
    for (uint32_t ch = 0; ch < EEG_CHANNELS; ++ch) if ((result = write_text_field(file, "uV", 8, total)) != FR_OK) return result;
    if ((result = write_text_field(file, "", 8, total)) != FR_OK) return result;
    for (uint32_t ch = 0; ch < EEG_CHANNELS; ++ch) if ((result = write_number_field(file, BDF_PHYSICAL_MIN_UV, 8, total)) != FR_OK) return result;
    if ((result = write_number_field(file, -1, 8, total)) != FR_OK) return result;
    for (uint32_t ch = 0; ch < EEG_CHANNELS; ++ch) if ((result = write_number_field(file, BDF_PHYSICAL_MAX_UV, 8, total)) != FR_OK) return result;
    if ((result = write_number_field(file, 1, 8, total)) != FR_OK) return result;
    for (uint32_t signal = 0; signal < BDF_SIGNALS; ++signal) if ((result = write_number_field(file, BDF_DIGITAL_MIN, 8, total)) != FR_OK) return result;
    for (uint32_t signal = 0; signal < BDF_SIGNALS; ++signal) if ((result = write_number_field(file, BDF_DIGITAL_MAX, 8, total)) != FR_OK) return result;
    for (uint32_t signal = 0; signal < BDF_SIGNALS; ++signal) if ((result = write_text_field(file, "", 80, total)) != FR_OK) return result;
    for (uint32_t ch = 0; ch < EEG_CHANNELS; ++ch) if ((result = write_number_field(file, EEG_SAMPLES_PER_RECORD, 8, total)) != FR_OK) return result;
    if ((result = write_number_field(file, ANNOTATION_SAMPLES_PER_RECORD, 8, total)) != FR_OK) return result;
    for (uint32_t signal = 0; signal < BDF_SIGNALS; ++signal) if ((result = write_text_field(file, "", 32, total)) != FR_OK) return result;
    return *total == BDF_HEADER_BYTES ? FR_OK : FR_INT_ERR;
}

static uint8_t benchmark_buffer[BDF_BENCHMARK_BUFFER_RECORDS * BDF_RECORD_BYTES];

static void make_bdf_record(uint32_t record, uint8_t output[BDF_RECORD_BYTES])
{
    uint8_t *cursor = output;
    for (uint32_t ch = 0; ch < EEG_CHANNELS; ++ch) {
        for (uint32_t sample = 0; sample < EEG_SAMPLES_PER_RECORD; ++sample) {
            encode_int24(demo_eeg_sample(ch, record * EEG_SAMPLES_PER_RECORD + sample), cursor);
            cursor += 3u;
        }
    }
    write_annotation_block(record, cursor);
}

FRESULT bdf_write_speed_test_file(const char *path, bdf_write_stats_t *stats)
{
    FIL file;
    FRESULT result;
    FRESULT close_result;
    UINT total = 0;
    uint32_t records = (BDF_BENCHMARK_TARGET_BYTES - BDF_HEADER_BYTES + BDF_RECORD_BYTES - 1u) / BDF_RECORD_BYTES;
    uint32_t written_records = 0;
    uint64_t start_us;

    if (stats == NULL) return FR_INVALID_PARAMETER;
    memset(stats, 0, sizeof(*stats));
    stats->target_bytes = BDF_BENCHMARK_TARGET_BYTES;
    stats->header_bytes = BDF_HEADER_BYTES;
    stats->record_bytes = BDF_RECORD_BYTES;
    stats->records = records;

    result = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (result != FR_OK) return result;
    start_us = time_us_64();
    result = write_bdf_header(&file, &total, records);
    while (result == FR_OK && written_records < records) {
        uint32_t batch_records = records - written_records;
        UINT batch_bytes;
        if (batch_records > BDF_BENCHMARK_BUFFER_RECORDS) batch_records = BDF_BENCHMARK_BUFFER_RECORDS;
        for (uint32_t i = 0; i < batch_records; ++i) {
            make_bdf_record(written_records + i, &benchmark_buffer[i * BDF_RECORD_BYTES]);
        }
        batch_bytes = batch_records * BDF_RECORD_BYTES;
        result = write_exact(&file, benchmark_buffer, batch_bytes, &total);
        if (result == FR_OK) {
            written_records += batch_records;
            ++stats->data_write_calls;
        }
    }
    if (result == FR_OK) result = f_sync(&file);
    stats->elapsed_us = time_us_64() - start_us;
    close_result = f_close(&file);
    if (result == FR_OK) result = close_result;
    if (result == FR_OK) stats->actual_bytes = total;
    return result;
}
FRESULT bdf_write_demo_file(const char *path, UINT *bytes_written)
{
    FIL file;
    FRESULT result;
    FRESULT close_result;
    UINT total = 0;
    uint8_t eeg_block[EEG_SAMPLES_PER_RECORD * 3u];
    uint8_t annotation_block[ANNOTATION_SAMPLES_PER_RECORD * 3u];
    if (bytes_written == NULL) return FR_INVALID_PARAMETER;
    *bytes_written = 0;
    result = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (result != FR_OK) return result;
    result = write_bdf_header(&file, &total, BDF_RECORDS);
    for (uint32_t record = 0; result == FR_OK && record < BDF_RECORDS; ++record) {
        for (uint32_t ch = 0; result == FR_OK && ch < EEG_CHANNELS; ++ch) {
            for (uint32_t sample = 0; sample < EEG_SAMPLES_PER_RECORD; ++sample) {
                encode_int24(demo_eeg_sample(ch, record * EEG_SAMPLES_PER_RECORD + sample), &eeg_block[sample * 3u]);
            }
            result = write_exact(&file, eeg_block, sizeof(eeg_block), &total);
        }
        if (result == FR_OK) {
            write_annotation_block(record, annotation_block);
            result = write_exact(&file, annotation_block, sizeof(annotation_block), &total);
        }
    }
    if (result == FR_OK && total != BDF_HEADER_BYTES + BDF_DATA_BYTES) result = FR_INT_ERR;
    if (result == FR_OK) result = f_sync(&file);
    close_result = f_close(&file);
    if (result == FR_OK) result = close_result;
    if (result == FR_OK) *bytes_written = total;
    return result;
}