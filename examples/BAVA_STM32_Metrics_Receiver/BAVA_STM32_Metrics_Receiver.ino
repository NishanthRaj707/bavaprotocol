/**
 * @file BAVA_STM32_Metrics_Receiver.ino
 * @brief Zero-Anomaly Benchmark Slave for BAVA Protocol v2.0 (STM32F103)
 *        Reports all STM32 internal DWT & CRC-32 metrics back to ESP32 natively via BAVA ID 0x06.
 */

#include <Arduino.h>
#include "bava.h"

#if defined(ARDUINO_ARCH_STM32)
  Uart BavaSerial(PA10, PA9); // Explicit USART1 RX=PA10, TX=PA9
#else
  #define BavaSerial Serial1
#endif

#define BAUD_DATALINK      921600
#define VAR_ID_BENCHMARK   0x05
#define VAR_ID_RX_STATS    0x06

#ifdef LED_BUILTIN
  #define PIN_STATUS_LED   LED_BUILTIN
#else
  #define PIN_STATUS_LED   PC13
#endif

struct __attribute__((packed)) TestPayload {
    uint32_t seq_num;
    uint32_t timestamp_us;
    int32_t  telemetry[5];
    uint32_t app_crc32;
};

struct __attribute__((packed)) RxStatsPayload {
    uint32_t frames_recv_ok;
    uint32_t rejected_faults;
    uint32_t silent_corrupt;
    uint32_t duplicates;
    uint32_t min_decode_cyc;
    uint32_t avg_decode_cyc;
    uint32_t max_decode_cyc;
    uint32_t max_byte_cyc;
};

static bava_handle_t bava;
static TestPayload rx_payload;
static RxStatsPayload tx_stats;

static uint32_t frames_received_ok = 0;
static uint32_t protocol_rejected_errors = 0;
static uint32_t silent_corruption_count = 0;
static uint32_t duplicate_seq_count = 0;
static uint32_t last_seq_num = 0;

static uint32_t current_frame_cycles = 0;
static uint32_t max_single_byte_cycles = 0;
static uint32_t min_frame_decode_cycles = 0xFFFFFFFFUL;
static uint32_t max_frame_decode_cycles = 0;
static uint64_t sum_frame_decode_cycles = 0;

static bool led_state = false;
static bool pending_stats_push = false;

static uint32_t compute_app_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1UL) crc = (crc >> 1U) ^ 0xEDB88320UL;
            else           crc >>= 1U;
        }
    }
    return ~crc;
}

static void bava_hardware_tx_cb(const uint8_t *data, uint16_t size) {
    if (data != NULL && size > 0) {
        BavaSerial.write(data, size);
    }
}

static void bava_rx_error_cb(uint8_t id, bava_status_t error) {
    (void)id;
    if (error != BAVA_STATUS_OK) {
        protocol_rejected_errors++;
        current_frame_cycles = 0;
    }
}

void setup() {
    pinMode(PIN_STATUS_LED, OUTPUT);
    digitalWrite(PIN_STATUS_LED, LOW);

    BavaSerial.begin(BAUD_DATALINK);

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

    bava_init(&bava, bava_hardware_tx_cb);
    bava_set_error_callback(&bava, bava_rx_error_cb);

    bava_register_var(&bava, VAR_ID_BENCHMARK, &rx_payload, sizeof(TestPayload), BAVA_PERM_READ_WRITE);
    bava_register_var(&bava, VAR_ID_RX_STATS, &tx_stats, sizeof(RxStatsPayload), BAVA_PERM_READ_WRITE);

    delay(300);
    digitalWrite(PIN_STATUS_LED, HIGH);
}

void loop() {
    while (BavaSerial.available()) {
        uint8_t b = (uint8_t)BavaSerial.read();

        uint32_t c_start = DWT->CYCCNT;
        bava_process_byte(&bava, b);
        uint32_t byte_cycles = DWT->CYCCNT - c_start;

        current_frame_cycles += byte_cycles;
        if (byte_cycles > max_single_byte_cycles) {
            max_single_byte_cycles = byte_cycles;
        }
    }

    bava_tick(&bava, millis());
    bava_poll(&bava); // Dispatches the 1-byte WRITE_ACK immediately

    if (bava_var_updated(&bava, VAR_ID_BENCHMARK)) {
        TestPayload snapshot;

        uint32_t c_start = DWT->CYCCNT;
        bool read_ok = bava_read_var(&bava, VAR_ID_BENCHMARK, &snapshot, sizeof(snapshot));
        uint32_t read_cycles = DWT->CYCCNT - c_start;

        if (read_ok) {
            bava_var_clear_update_status(&bava, VAR_ID_BENCHMARK);

            uint32_t total_decode_cycles = current_frame_cycles + read_cycles;
            current_frame_cycles = 0;

            frames_received_ok++;
            sum_frame_decode_cycles += total_decode_cycles;
            if (total_decode_cycles < min_frame_decode_cycles) min_frame_decode_cycles = total_decode_cycles;
            if (total_decode_cycles > max_frame_decode_cycles) max_frame_decode_cycles = total_decode_cycles;

            uint32_t expected_crc = compute_app_crc32((const uint8_t*)&snapshot, 28);
            if (expected_crc != snapshot.app_crc32) {
                silent_corruption_count++;
            }

            if (snapshot.seq_num == last_seq_num && frames_received_ok > 1) {
                duplicate_seq_count++;
            }
            last_seq_num = snapshot.seq_num;

            if (frames_received_ok % 1000 == 0) {
                led_state = !led_state;
                digitalWrite(PIN_STATUS_LED, led_state ? LOW : HIGH);
            }

            // Every 5,000 valid frames, push STM32 metrics natively over BAVA ID 0x06
            if (frames_received_ok % 5000 == 0) {
                tx_stats.frames_recv_ok  = frames_received_ok;
                tx_stats.rejected_faults = protocol_rejected_errors;
                tx_stats.silent_corrupt  = silent_corruption_count;
                tx_stats.duplicates      = duplicate_seq_count;
                tx_stats.min_decode_cyc  = min_frame_decode_cycles;
                tx_stats.avg_decode_cyc  = (uint32_t)(sum_frame_decode_cycles / frames_received_ok);
                tx_stats.max_decode_cyc  = max_frame_decode_cycles;
                tx_stats.max_byte_cyc    = max_single_byte_cycles;
                bava_send_write(&bava, VAR_ID_RX_STATS);
                // Clear waiting_ack on slave so it never blocks incoming master frames
                bava.is_waiting_ack = false;
            }
        }
    }
}