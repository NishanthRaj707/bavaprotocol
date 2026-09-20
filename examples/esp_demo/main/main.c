/*
 * SPDX-License-Identifier: Apache-2.0
 * BAVA Protocol ESP-IDF Demo Application
 *
 * Demonstrates Object Dictionary multi-variable teleports, dedicated UART RX task,
 * hardware critical section locking, and periodic transmission over ESP-IDF.
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

static const char *TAG = "BAVA_APP";

// Bava instance
static bava_handle_t bava;

// Registered Target Dictionary Variables
static float motor_speed_rpm = 0.0f;
static uint32_t sensor_status_flags = 0;

// 1. Hardware Transmission Callback (ESP-IDF UART TX Wrapper)
void esp32_uart_tx_cb(const uint8_t *data, uint16_t size) {
    uart_write_bytes(UART_PORT_NUM, (const char *)data, size);
}

// 2. Hardware Critical Section Callbacks for ISR Safety
static portMUX_TYPE bava_spinlock = portMUX_INITIALIZER_UNLOCKED;

void esp32_enter_critical(void) {
    taskENTER_CRITICAL(&bava_spinlock);
}

void esp32_exit_critical(void) {
    taskEXIT_CRITICAL(&bava_spinlock);
}

// 3. Error Callback for dropped packets / timeout events (Optional)
void bava_error_handler(uint8_t id, uint8_t error_code) {
    ESP_LOGE(TAG, "Timeout / Error on Variable ID 0x%02X (Code: 0x%02X)", id, error_code);
}

// 4. Dedicated UART Receive Task (Processes Incoming Byte Stream)
static void uart_rx_task(void *arg) {
    uint8_t rx_buf[128];
    while (1) {
        int rx_bytes = uart_read_bytes(UART_PORT_NUM, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(10));
        for (int i = 0; i < rx_bytes; i++) {
            // Process incoming byte through BAVA state machine
            bava_process_byte(&bava, rx_buf[i]);
        }
    }
}

void app_main(void) {
    // Configure UART Peripheral
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

    // Initialize BAVA instance
    bava_init(&bava, esp32_uart_tx_cb);
    bava.enter_critical = esp32_enter_critical;
    bava.exit_critical  = esp32_exit_critical;
    bava.error_callback = bava_error_handler;

    // Advance BAVA non-blocking timer using ESP-IDF system timestamp API
    bava_tick(&bava, (uint32_t)esp_log_timestamp());    

    // Register Object Dictionary Variables
    bava_register_var(&bava, 0x01, &motor_speed_rpm, sizeof(motor_speed_rpm));
    bava_register_var(&bava, 0x02, &sensor_status_flags, sizeof(sensor_status_flags));

    ESP_LOGI(TAG, "BAVA Protocol Stack online. Listening on UART %d...", UART_PORT_NUM);

    // Create RX Processing Task
    xTaskCreate(uart_rx_task, "bava_rx_task", 4096, NULL, 10, NULL);

    // Main Control Loop
    while (1) {
        // Check if remote node updated motor speed
        if (bava_var_updated(&bava, 0x01)) {
            ESP_LOGI(TAG, "Updated Motor Speed Target: %.2f RPM", motor_speed_rpm);
            bava_var_clear_update_status(&bava, 0x01);
        }

        // Periodically transmit raw sensor status flags (ID 0x02)
        sensor_status_flags++;
        bava_send_raw_write(&bava, 0x02, (const uint8_t *)&sensor_status_flags, sizeof(sensor_status_flags));

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
