/**
 * @file BAVA_ESP32_Metrics_Sender.ino
 * @brief Zero-Anomaly Benchmark Master for BAVA Protocol v2.0 (ESP32 DevKit V1)
 */

#include <Arduino.h>
#include "bava.h"

#define UART_MONITOR       Serial
#define UART_DATALINK      Serial1
#define PIN_DATALINK_RX    16
#define PIN_DATALINK_TX    17
#define PIN_LATENCY_GPIO   4

#define BAUD_MONITOR       115200
#define BAUD_DATALINK      921600
#define HARNESS_TIMEOUT_MS 50
#define WINDOW_SIZE        1000
#define VAR_ID_BENCHMARK   0x05
#define VAR_ID_RX_STATS    0x06

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

static_assert(sizeof(TestPayload) == 32, "TestPayload must be 32 bytes");
static_assert(sizeof(RxStatsPayload) == 32, "RxStatsPayload must be 32 bytes");

static bava_handle_t bava;
static TestPayload tx_payload;
static RxStatsPayload stm32_stats;

static uint32_t baseline_free_heap = 0;
static uint32_t min_free_stack_bytes = 0xFFFFFFFFUL;

static uint32_t total_sent = 0;
static uint32_t win_sent = 0;
static uint32_t win_ok = 0;
static uint32_t win_lost = 0;
static uint32_t win_injected = 0;
static uint64_t win_active_rtt_sum_us = 0;
static uint64_t win_encode_cycles_sum = 0;

static uint32_t rtt_samples[WINDOW_SIZE];
static uint32_t lost_seq_log[32];
static uint8_t  lost_seq_count = 0;

// Static formatting buffers so loop() stack frame is 100% constant
static char csv_line[256];
static char lost_str[48];
static uint8_t wire_buf[64];

static volatile uint32_t tx_io_cycles = 0;
static volatile uint8_t  fault_mode = 0;
static volatile bool     ack_err_flag = false;
static volatile bava_status_t last_err_code = BAVA_STATUS_OK;

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

static void populate_payload(TestPayload &p, uint32_t seq) {
    p.seq_num = seq;
    p.timestamp_us = micros();
    p.telemetry[0] = (int32_t)(seq * 17UL + 100UL);
    p.telemetry[1] = (int32_t)(seq * 31UL - 50UL);
    p.telemetry[2] = (int32_t)(seq ^ 0x5A5A5A5AUL);
    p.telemetry[3] = (int32_t)(0xA5000000UL | (seq & 0xFFFFUL));
    p.telemetry[4] = (int32_t)(2500UL + (seq % 100UL));
    p.app_crc32 = compute_app_crc32((const uint8_t*)&p, 28);
}

static void bava_hardware_tx_cb(const uint8_t *data, uint16_t size) {
    if (data == NULL || size == 0) return;

    const uint8_t *out_ptr = data;
    uint16_t out_size = size;

    if (fault_mode != 0 && size >= 10 && size <= sizeof(wire_buf)) {
        memcpy(wire_buf, data, size);
        switch (fault_mode) {
            case 1:
                wire_buf[0] ^= 0xFF; // Corrupt COBS code byte
                break;
            case 2:
                wire_buf[size / 2] ^= 0x03; // 2-bit payload error
                break;
            case 3:
                // Premature 0x00 delimiter truncation (1 fault = 1 truncated frame)
                out_size = (size / 2) + 1;
                wire_buf[out_size - 1] = 0x00;
                break;
            case 4:
                wire_buf[size - 2] ^= 0xAA; // Corrupt CRC-16 byte
                break;
            default:
                break;
        }
        out_ptr = wire_buf;
    }

    uint32_t io_start = ESP.getCycleCount();
    UART_DATALINK.write(out_ptr, out_size);
    tx_io_cycles = ESP.getCycleCount() - io_start;
}

static void bava_error_cb(uint8_t id, bava_status_t error) {
    (void)id;
    last_err_code = error;
    if (error != BAVA_STATUS_OK) {
        ack_err_flag = true;
    }
}

static void sort_u32_array(uint32_t *arr, uint32_t n) {
    for (uint32_t i = 1; i < n; i++) {
        uint32_t key = arr[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

void setup() {
    pinMode(PIN_LATENCY_GPIO, OUTPUT);
    digitalWrite(PIN_LATENCY_GPIO, LOW);

    UART_MONITOR.begin(BAUD_MONITOR);
    UART_DATALINK.setRxBufferSize(1024);
    UART_DATALINK.begin(BAUD_DATALINK, SERIAL_8N1, PIN_DATALINK_RX, PIN_DATALINK_TX);

    while (!UART_MONITOR && millis() < 2000);

    // Pre-warm snprintf and stack before recording baselines
    snprintf(csv_line, sizeof(csv_line), "%lu,%lu,%s", 1000UL, 2000UL, "WARMUP");
    baseline_free_heap = ESP.getFreeHeap();

    bava_init(&bava, bava_hardware_tx_cb);
    bava_set_error_callback(&bava, bava_error_cb);

    bava_register_var(&bava, VAR_ID_BENCHMARK, &tx_payload, sizeof(TestPayload), BAVA_PERM_READ_WRITE);
    bava_register_var(&bava, VAR_ID_RX_STATS, &stm32_stats, sizeof(RxStatsPayload), BAVA_PERM_READ_WRITE);

    min_free_stack_bytes = uxTaskGetStackHighWaterMark(NULL);

    UART_MONITOR.println("\n==========================================================================================================");
    UART_MONITOR.println(" BAVA Protocol v2.0 - Zero-Anomaly Benchmark Master (ESP32 Sender)");
    UART_MONITOR.println("==========================================================================================================");
    UART_MONITOR.print("# STATIC_RAM_BYTES: sizeof(bava_handle_t) = ");
    UART_MONITOR.println((unsigned long)sizeof(bava_handle_t));
    UART_MONITOR.print("# BASELINE_FREE_HEAP_BYTES: ");
    UART_MONITOR.println((unsigned long)baseline_free_heap);
    UART_MONITOR.println("==========================================================================================================");
    UART_MONITOR.println("Sent,Win_OK,Win_Lost,Win_Inj,Min_us,p50_us,Avg_us,p95_us,p99_us,Max_us,Act_Bps,Net_Bps,Pure_Enc_Cyc,Heap_Delta,Stack_Free_B,Unexplained_Seq");
}

static uint32_t window_start_us = 0;

void loop() {
    static uint32_t seq = 1;

    if (win_sent == 0) {
        window_start_us = micros();
    }

    populate_payload(tx_payload, seq);

    if (seq % 75 == 0) {
        fault_mode = (uint8_t)(((seq / 75UL) % 4UL) + 1U);
        win_injected++;
    } else {
        fault_mode = 0;
    }

    ack_err_flag = false;
    last_err_code = BAVA_STATUS_OK;
    tx_io_cycles = 0;

    while (UART_DATALINK.available()) {
        bava_process_byte(&bava, (uint8_t)UART_DATALINK.read());
    }
    bava_tick(&bava, millis());
    bava_poll(&bava);

    // --- CRITICAL TIMED SECTION ---
    digitalWrite(PIN_LATENCY_GPIO, HIGH);
    uint32_t t_start_us = micros();
    uint32_t c_start = ESP.getCycleCount();

    bava_status_t send_status = bava_send_write(&bava, VAR_ID_BENCHMARK);

    uint32_t total_send_cycles = ESP.getCycleCount() - c_start;
    uint32_t pure_encode_cycles = (total_send_cycles > tx_io_cycles) ? (total_send_cycles - tx_io_cycles) : total_send_cycles;

    bool got_valid_ack = false;

    if (send_status == BAVA_STATUS_OK) {
        while (bava.is_waiting_ack && (uint32_t)(micros() - t_start_us) < (HARNESS_TIMEOUT_MS * 1000UL)) {
            while (UART_DATALINK.available()) {
                bava_process_byte(&bava, (uint8_t)UART_DATALINK.read());
            }
            bava_tick(&bava, millis());
            bava_poll(&bava);

            if (!bava.is_waiting_ack) {
                got_valid_ack = (!ack_err_flag && last_err_code == BAVA_STATUS_OK);
                break;
            }
            // Only yield to FreeRTOS WDT if we are in a prolonged timeout (> 2000 us), never during normal RTT!
            if ((uint32_t)(micros() - t_start_us) > 2000UL) {
                yield();
            }
        }
    }

    uint32_t rtt_us = micros() - t_start_us;
    digitalWrite(PIN_LATENCY_GPIO, LOW);
    // --- END TIMED SECTION ---

    if (bava.is_waiting_ack) {
        bava_tick(&bava, millis() + 1000UL);
        bava_poll(&bava);
    }

    total_sent++;
    win_sent++;
    win_encode_cycles_sum += pure_encode_cycles;

    if (got_valid_ack) {
        rtt_samples[win_ok] = rtt_us;
        win_ok++;
        win_active_rtt_sum_us += rtt_us;
    } else {
        win_lost++;
        if (fault_mode == 0 && lost_seq_count < 32) {
            lost_seq_log[lost_seq_count++] = seq;
        }
    }

    // Check if STM32 pushed its native BAVA RxStatsPayload (ID 0x06)
    if (bava_var_updated(&bava, VAR_ID_RX_STATS)) {
        RxStatsPayload snap;
        if (bava_read_var(&bava, VAR_ID_RX_STATS, &snap, sizeof(snap))) {
            bava_var_clear_update_status(&bava, VAR_ID_RX_STATS);
            snprintf(csv_line, sizeof(csv_line),
                     "# STM32_RX_STATS -> Recv_OK:%lu | Rejected:%lu | Silent_Corrupt:%lu | Dup:%lu | Decode_Cyc(Min/Avg/Max):%lu/%lu/%lu | Max_Byte_Cyc:%lu",
                     (unsigned long)snap.frames_recv_ok,
                     (unsigned long)snap.rejected_faults,
                     (unsigned long)snap.silent_corrupt,
                     (unsigned long)snap.duplicates,
                     (unsigned long)snap.min_decode_cyc,
                     (unsigned long)snap.avg_decode_cyc,
                     (unsigned long)snap.max_decode_cyc,
                     (unsigned long)snap.max_byte_cyc);
            UART_MONITOR.println(csv_line);
        }
    }

    if (win_sent >= WINDOW_SIZE) {
        uint32_t window_elapsed_us = micros() - window_start_us;

        uint32_t cur_heap = ESP.getFreeHeap();
        uint32_t heap_delta = (baseline_free_heap > cur_heap) ? (baseline_free_heap - cur_heap) : 0;
        uint32_t cur_stack = uxTaskGetStackHighWaterMark(NULL);
        if (cur_stack < min_free_stack_bytes) {
            min_free_stack_bytes = cur_stack;
        }

        uint32_t lat_min = 0, lat_p50 = 0, lat_avg = 0, lat_p95 = 0, lat_p99 = 0, lat_max = 0;
        if (win_ok > 0) {
            sort_u32_array(rtt_samples, win_ok);
            lat_min = rtt_samples[0];
            lat_p50 = rtt_samples[win_ok / 2];
            lat_avg = (uint32_t)(win_active_rtt_sum_us / win_ok);
            lat_p95 = rtt_samples[(win_ok * 95UL) / 100UL];
            lat_p99 = rtt_samples[(win_ok * 99UL) / 100UL];
            lat_max = rtt_samples[win_ok - 1];
        }

        uint32_t active_goodput_bps = (win_active_rtt_sum_us > 0)
            ? (uint32_t)((win_ok * 32ULL * 1000000ULL) / win_active_rtt_sum_us) : 0;
        uint32_t net_goodput_bps = (window_elapsed_us > 0)
            ? (uint32_t)((win_ok * 32ULL * 1000000ULL) / window_elapsed_us) : 0;
        uint32_t avg_encode_cycles = (uint32_t)(win_encode_cycles_sum / win_sent);

        if (lost_seq_count == 0) {
            snprintf(lost_str, sizeof(lost_str), "NONE");
        } else {
            snprintf(lost_str, sizeof(lost_str), "cnt=%u(first=%lu)",
                     (unsigned)lost_seq_count, (unsigned long)lost_seq_log[0]);
        }

        snprintf(csv_line, sizeof(csv_line),
                 "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%s",
                 (unsigned long)total_sent,
                 (unsigned long)win_ok,
                 (unsigned long)win_lost,
                 (unsigned long)win_injected,
                 (unsigned long)lat_min,
                 (unsigned long)lat_p50,
                 (unsigned long)lat_avg,
                 (unsigned long)lat_p95,
                 (unsigned long)lat_p99,
                 (unsigned long)lat_max,
                 (unsigned long)active_goodput_bps,
                 (unsigned long)net_goodput_bps,
                 (unsigned long)avg_encode_cycles,
                 (unsigned long)heap_delta,
                 (unsigned long)min_free_stack_bytes,
                 lost_str);
        UART_MONITOR.println(csv_line);

        win_sent = 0;
        win_ok = 0;
        win_lost = 0;
        win_injected = 0;
        win_active_rtt_sum_us = 0;
        win_encode_cycles_sum = 0;
        lost_seq_count = 0;
    }

    seq++;
}