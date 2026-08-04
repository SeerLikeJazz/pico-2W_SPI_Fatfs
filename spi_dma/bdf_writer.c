#include "bdf_writer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define EEG_CHANNELS 8u
#define BDF_SIGNALS (EEG_CHANNELS + 1u)
#define BDF_SAMPLE_RATE_HZ 250u
#define BDF_RECORDS 10u
#define EEG_SAMPLES_PER_RECORD BDF_SAMPLE_RATE_HZ
#define ANNOTATION_SAMPLES_PER_RECORD 64u
#define BDF_HEADER_BYTES (256u + BDF_SIGNALS * 256u)
#define BDF_DATA_BYTES (BDF_RECORDS * ((EEG_CHANNELS * EEG_SAMPLES_PER_RECORD + ANNOTATION_SAMPLES_PER_RECORD) * 3u))
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

static FRESULT write_bdf_header(FIL *file, UINT *total)
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
    if ((result = write_number_field(file, BDF_RECORDS, 8, total)) != FR_OK) return result;
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
    result = write_bdf_header(&file, &total);
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