#include <inttypes.h>
#include "pico/stdlib.h"
#include "ads1299.h"
#include "ads1299_config.h"
#include "debug_console.h"
#include "net/wifi_stream.h"
#if ENABLE_WIFI_STREAM
#include "wifi_control.h"
#endif
#include "usb_commands.h"
#include "ads1299_control.h"

#if ENABLE_SD_CARD
void sd_legacy_run(void);
#endif

static ads1299_settings_t settings = {
    .nominal_sps = ADS_DEFAULT_RATE, .gain = ADS_DEFAULT_GAIN, .mode = ADS_DEFAULT_MODE,
    .srb1 = ADS_NORMAL_SRB1, .srb2 = ADS_NORMAL_SRB2, .bias = ADS_NORMAL_BIAS
};
static ads1299_frame_t latest;
static bool have_sample;
static bool sample_preview;
static usb_command_parser_t command_parser;

static void parameter_help(void) {
    debug_log(":rate SPS [250 500 1000 2000 4000 8000 16000]; :gain N [1 2 4 6 8 12 24]. Enter to apply.\r\n");
}

static void help(void) {
    debug_log("Commands: ? help; s status; r registers (pause/resume); "
              "x stop; g start; i reset/retry; t test; h short; n normal; p preview25Hz on/off.\r\n");
    parameter_help();
    debug_log("Example :rate 1000<Enter>, :gain 24<Enter>. ':' required; RAM only; i retries RAM settings.\r\n");
#if ENABLE_SD_CARD
    debug_log("b: legacy SD/BDF 100 MiB test with ADS stopped (manual resume).\r\n");
#endif
}

static void report_config(void) {
    ads1299_info_t v;
    ads1299_get_info(&v);
    uint8_t rate = 6;
    const ads1299_settings_t *current = v.configured ? &v.settings : &settings;
    (void)ads1299_rate_code(current->nominal_sps, &rate);
    uint32_t actual_milli_sps = (uint32_t)((uint64_t)ADS_MCLK_HZ * 1000u / (128u << rate));
    debug_log("Pico2W ADS1299 SPI0 DMA; SD=%d UART=%d; 5V_EN(GP21)=%d\r\n",
              ENABLE_SD_CARD, ENABLE_UART_LOG, gpio_get(ADS_PIN_5V_EN));
    debug_log("Pins GP16=DOUT GP17=CS GP18=SCLK GP19=DIN GP20=DRDY GP21=5V_EN\r\n");
    debug_log("EXTERNAL MCLK=%u Hz ASSUMED: verify U12! CLKSEL=LOW. SPI actual=%" PRIu32 " Hz, mode1.\r\n",
              (unsigned)ADS_MCLK_HZ, v.spi_hz);
    debug_log("%s %u SPS nominal, calculated=%" PRIu32 ".%03" PRIu32 " SPS; "
              "gain=%u mode=%s test=fCLK/2^21; normal SRB1=%d SRB2=%d BIAS=%d\r\n",
              v.configured ? "Verified" : "INVALID hardware config; RAM retry target:",
              current->nominal_sps, actual_milli_sps / 1000u, actual_milli_sps % 1000u,
              current->gain, ads1299_mode_name(current->mode), current->srb1, current->srb2, current->bias);
    debug_log("ID=0x%02x (8ch low5=0x1e; commonly 0x3e); init=%d configured=%d running=%d fatal=%d last=%s\r\n",
              v.id, v.initialized, v.configured, v.running, v.fatal, ads1299_error_name(v.last_error));
    for (unsigned a = 1; a < ADS_REG_COUNT; ++a) {
        if (v.checked_mask & (1u << a))
            debug_log("REG %02x write=%02x read=%02x %s\r\n", a, v.expected[a], v.readback[a],
                      v.mismatch_mask & (1u << a) ? "FAIL" : "OK(masked if RO bits)");
    }
    if (settings.mode == ADS_MODE_NORMAL)
        debug_log("Normal mode: verify H2/H5 reference jumpers and INxN wiring; SRB/BIAS are opt-in.\r\n");
}

static uint32_t report_status(uint64_t elapsed_us, uint32_t previous_frames) {
    ads1299_stats_t s;
    ads1299_info_t v;
    ads1299_get_stats(&s);
    ads1299_get_info(&v);
    uint32_t rate_milli = elapsed_us ?
        (uint32_t)((uint64_t)(s.frames - previous_frames) * 1000000000ull / elapsed_us) : 0;
    debug_log("run=%d mode=%s DRDY=%" PRIu32 " frames=%" PRIu32 " fps=%" PRIu32 ".%03" PRIu32
              " busy=%" PRIu32 " qdrop=%" PRIu32 " qpeak=%" PRIu32 " stopdiscard=%" PRIu32 "\r\n",
              v.running, ads1299_mode_name(v.settings.mode), s.drdy, s.frames,
              rate_milli / 1000u, rate_milli % 1000u, s.busy_drdy, s.queue_drops, s.queue_peak, s.discarded_on_stop);
    debug_log("timeout(drdy/dma/spi)=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " dmaerr=%" PRIu32
              " bad=%" PRIu32 " recovery=%" PRIu32 " logdrop=%" PRIu32 " logbytes=%" PRIu32
              " uartdrop=%" PRIu32 " last=%s\r\n", s.drdy_timeouts, s.dma_timeouts, s.spi_timeouts,
              s.dma_errors, s.bad_frames, s.recoveries, debug_log_dropped_messages(),
              debug_log_discarded_bytes(), debug_uart_dropped_messages(), ads1299_error_name(v.last_error));
    if (have_sample)
        debug_log("latest seq=%" PRIu32 " t=%" PRIu64 "us status=%06" PRIx32
                  " ch1=%" PRId32 " ch2=%" PRId32 " (raw codes; may be stale when stopped)\r\n",
                  latest.sequence, latest.timestamp_us, latest.status, latest.channel[0], latest.channel[1]);
    return s.frames;
}

static void report_result(const char *operation, bool ok) {
    ads1299_info_t v;
    ads1299_get_info(&v);
    debug_log("%s: %s; running=%d last=%s\r\n", operation, ok ? "OK" : "FAILED",
              v.running, ads1299_error_name(v.last_error));
}

static void handle_command(int c) {
    ads1299_info_t v;
    ads1299_get_info(&v);
    switch (c) {
    case '?': help(); break;
    case 's': report_config(); wifi_stream_report(); break;
    case 'p':
        sample_preview = !sample_preview;
        debug_log("25Hz decimated preview=%d; sample,seq,time_us,ch1,...,ch8 (not full-rate capture)\r\n", sample_preview);
        break;
    case 'x': report_result("stop", ads1299_stop()); break;
    case 'g': report_result("start (use i if unconfigured)", ads1299_start()); break;
    case 'i':
        have_sample = false;
        report_result("reset/config (standby)", ads1299_init(&settings));
        report_config();
        break;
    case 'r': {
        uint8_t regs[ADS_REG_COUNT];
        bool resume = v.running;
        bool ok = (!resume || ads1299_stop()) && ads1299_read_regs(0, regs, sizeof regs);
        if (ok) for (unsigned a = 0; a < ADS_REG_COUNT; ++a) debug_log("REG %02x = %02x\r\n", a, regs[a]);
        if (resume && ok) ok = ads1299_start();
        report_result("register dump", ok);
        break;
    }
    case 't': case 'h': case 'n': {
        ads1299_settings_t requested = settings;
        requested.mode = c == 't' ? ADS_MODE_TEST : c == 'h' ? ADS_MODE_SHORT : ADS_MODE_NORMAL;
        bool ok = ads1299_configure(&requested);
        if (ok) { settings = requested; have_sample = false; }
        report_result("mode/config (standby)", ok);
        report_config();
        break;
    }
#if ENABLE_SD_CARD
    case 'b':
        if (ads1299_stop()) { sd_legacy_run(); have_sample = false; }
        break;
#endif
    case -1: case '\r': case '\n': case ' ': break;
    default: debug_log("Unknown command; type ?\r\n"); break;
    }
}

static void handle_usb_input(void) {
    for (unsigned i = 0; i < USB_COMMAND_CHARS_PER_POLL; ++i) {
        int c = debug_console_getchar();
        if (c < 0) break;
        usb_command_t command = usb_command_feed(&command_parser, c);
        if (command.kind == USB_CMD_NONE) continue;
        if (command.kind == USB_CMD_LEGACY) { handle_command((int)command.value); break; }
        if (command.kind == USB_CMD_ERROR) {
            debug_log("Command FAILED: %s; acquisition unchanged.\r\n", command.error);
            parameter_help(); break;
        }
        ads1299_info_t before, after;
        ads1299_get_info(&before);
        ads1299_change_t field = command.kind == USB_CMD_RATE ? ADS_CHANGE_RATE : ADS_CHANGE_GAIN;
        unsigned old = field == ADS_CHANGE_RATE ? before.settings.nominal_sps : before.settings.gain;
        ads1299_error_t error;
        ads1299_change_result_t result = ads1299_change(&settings, field, command.value, &error);
        ads1299_get_info(&after);
        if (result != ADS_CHANGE_UNCHANGED && (!after.running || result == ADS_CHANGE_APPLIED)) have_sample = false;
        debug_log("%s old=%u requested=%" PRIu32 ": %s; error=%s run=%d configured=%d\r\n",
                  field == ADS_CHANGE_RATE ? "rate" : "gain", old, command.value,
                  result == ADS_CHANGE_APPLIED ? "APPLIED" : result == ADS_CHANGE_UNCHANGED ? "UNCHANGED" : "FAILED",
                  ads1299_error_name(error), after.running, after.configured);
        report_config();
        if (result == ADS_CHANGE_FAILED) {
            if (error == ADS_ERR_SPI_BUDGET)
                debug_log("SPI preflight/apply limit: MCLK=%u base=%u max=%u; require frame time <75%% period and valid MCLK.\r\n",
                          (unsigned)ADS_MCLK_HZ, (unsigned)ADS_SPI_HZ, (unsigned)ADS_SPI_MAX_HZ);
            parameter_help();
        }
        break; /* At most one command/configuration transaction per main-loop pass. */
    }
}

int main(void) {
    ads1299_power_on(); /* GP21 goes HIGH before any USB initialization/wait. */
    (void)debug_console_init();
    wifi_stream_init();
    bool ok = ads1299_init(&settings); /* Explicit user START required. */
    wifi_stream_launch();
    report_result("startup", ok);
    report_config();
    help();
    uint64_t last_report = time_us_64();
    uint32_t previous_frames = 0;
    uint64_t last_preview = 0;
    uint32_t preview_sequence = 0;
    bool was_connected = false;
    while (true) {
        ads1299_poll();
        for (unsigned i = 0; i < 16u && ads1299_get_frame(&latest); ++i) {
            have_sample = true;
            wifi_stream_submit(&latest);
        }
        debug_console_poll();
        bool connected = debug_console_connected();
        if (connected && !was_connected) { report_config(); help(); }
        was_connected = connected;
        handle_usb_input();
#if ENABLE_WIFI_STREAM
        wifi_control_apply(&settings);
#endif
        uint64_t now = time_us_64();
        if (sample_preview && have_sample && latest.sequence != preview_sequence && now - last_preview >= 40000u) {
            debug_log("sample,%" PRIu32 ",%" PRIu64 ",%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32
                      ",%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32 "\r\n",
                      latest.sequence, latest.timestamp_us, latest.channel[0], latest.channel[1],
                      latest.channel[2], latest.channel[3], latest.channel[4], latest.channel[5],
                      latest.channel[6], latest.channel[7]);
            last_preview = now;
            preview_sequence = latest.sequence;
        }
        if (now - last_report >= 1000000u) {
            previous_frames = report_status(now - last_report, previous_frames);
            wifi_stream_report();
            last_report = now;
        }
        tight_loop_contents();
    }
}
