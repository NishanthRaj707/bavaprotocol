/*
 * SPDX-License-Identifier: GPL-3.0-only
 * BAVA Protocol ESP-IDF Demo Application
 *
 * Demonstrates:
 * - Dedicated UART RX task streaming into non-blocking byte parser (bava_process_byte)
 * - ISR-safe polling architecture (bava_poll) for ACK and read response dispatch
 * - Object Dictionary registration with access control permissions (BAVA_PERM_*)
 * - Cross-task / ISR-safe tear-free multi-byte reads (bava_read_var)
 * - Outbound transmission with re-entrancy protection (bava_send_write)
 * - Error callback handling (bava_set_error_callback)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"

// Bava protocol header file
#include "bava.h"

#define UART_PORT_NUM      UART_NUM_1
#define TXD_PIN            (17)
#define RXD_PIN            (16)
#define BUF_SIZE           (1024)

#define VAR_ID_MOTOR_SPEED 0x01
#define VAR_ID_SENSOR_DATA 0x02
#define VAR_ID_DEVICE_INFO 0x03

static const char *TAG = "BAVA_APP";

// BAVA Protocol instance
static bava_handle_t bava;

// Registered Target Dictionary Variables
static float motor_speed_rpm = 0.0f;
static uint32_t sensor_status_flags = 0;
static const uint32_t firmware_build_id = 0x20261107; // Read-Only build identifier

// 1. Hardware Transmission Callback (ESP-IDF UART TX Wrapper)
void esp32_uart_tx_cb(const uint8_t *data, uint16_t size) {
    if (data != NULL && size > 0) {
        uart_write_bytes(UART_PORT_NUM, (const char *)data, size);
    }
}

// 2. Error Callback for dropped packets, NACKs, and timeout events
void bava_error_handler(uint8_t id, bava_status_t error_code) {
    ESP_LOGW(TAG, "Protocol Error on Variable ID 0x%02X (Status: 0x%02X)", id, (uint8_t)error_code);
}

// 3. Dedicated UART Receive Task (Processes Incoming Byte Stream)
static void uart_rx_task(void *arg) {
    (void)arg;
    uint8_t rx_buf[128];

    while (1) {
        int rx_bytes = uart_read_bytes(UART_PORT_NUM, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(10));
        for (int i = 0; i < rx_bytes; i++) {
            // Process incoming byte through BAVA state machine
            bava_process_byte(&bava, rx_buf[i]);
        }

        if (rx_bytes > 0) {
            // Promptly dispatch any pending ACKs/responses triggered by incoming frames
            bava_poll(&bava);
        }
    }
}

void app_main(void) {
    // Configure UART Peripheral (115200 8N1)
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_PORT_NUM, &uart_config);
    uart_set_pin(UART_PORT_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // Initialize BAVA instance with hardware TX callback
    bava_init(&bava, esp32_uart_tx_cb);

    // Register protocol error callback using public API
    bava_set_error_callback(&bava, bava_error_handler);

    // Register Object Dictionary Variables with Access Control Permissions
    // Variable 0x01: Motor Speed Target (float) - Read/Write by remote
    bava_register_var(&bava, VAR_ID_MOTOR_SPEED, &motor_speed_rpm, sizeof(motor_speed_rpm), BAVA_PERM_READ_WRITE);

    // Variable 0x02: Local Sensor Status Flags (uint32_t) - Read/Write
    bava_register_var(&bava, VAR_ID_SENSOR_DATA, &sensor_status_flags, sizeof(sensor_status_flags), BAVA_PERM_READ_WRITE);

    // Variable 0x03: Firmware Build ID (uint32_t) - Read-Only (remote WRITE is rejected with BAVA_STATUS_ERR_RO)
    bava_register_var(&bava, VAR_ID_DEVICE_INFO, (void *)&firmware_build_id, sizeof(firmware_build_id), BAVA_PERM_READ_ONLY);

    ESP_LOGI(TAG, "BAVA Protocol Stack online. Listening on UART %d...", UART_PORT_NUM);

    // Create dedicated UART RX Task
    xTaskCreate(uart_rx_task, "bava_rx_task", 4096, NULL, 10, NULL);

    uint32_t last_tx_time = 0;

    // Main Control Loop
    while (1) {
        uint32_t now_ms = (uint32_t)esp_log_timestamp();

        // 1. Advance BAVA non-blocking timer for proactive timeout management
        bava_tick(&bava, now_ms);

        // 2. Poll for pending ACKs / responses and proactive RX inactivity timeout
        bava_poll(&bava);

        // 3. Check if remote node updated motor speed target
        if (bava_var_updated(&bava, VAR_ID_MOTOR_SPEED)) {
            // Atomic, tear-free read using FreeRTOS spinlock critical section
            float target_speed = 0.0f;
            if (bava_read_var(&bava, VAR_ID_MOTOR_SPEED, &target_speed, sizeof(target_speed))) {
                bava_var_clear_update_status(&bava, VAR_ID_MOTOR_SPEED);
                ESP_LOGI(TAG, "Updated Motor Speed Target: %.2f RPM", target_speed);
            }
        }

        // 4. Periodic Telemetry Transmission (Every 1000 ms)
        if (now_ms - last_tx_time >= 1000) {
            last_tx_time = now_ms;
            sensor_status_flags++;

            // Non-blocking transmission with transaction re-entrancy protection
            bava_status_t tx_status = bava_send_write(&bava, VAR_ID_SENSOR_DATA);
            if (tx_status == BAVA_STATUS_ERR_BUSY) {
                ESP_LOGW(TAG, "TX deferred: previous transaction still awaiting ACK");
            } else if (tx_status != BAVA_STATUS_OK) {
                ESP_LOGE(TAG, "TX failed with status 0x%02X", (uint8_t)tx_status);
            }
        }

        // Cooperative FreeRTOS yield
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
