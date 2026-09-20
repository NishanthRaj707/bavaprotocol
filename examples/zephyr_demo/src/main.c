/*
 * SPDX-License-Identifier: Apache-2.0
 * BAVA Protocol Zephyr RTOS Demo Application
 *
 * Demonstrates Object Dictionary multi-variable teleports, hardware UART polling,
 * zero-allocation binary framing, and Zephyr thread sleep timing.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "bava.h"

/* ========================================================================== */
/*                   OBJECT DICTIONARY PAYLOAD DEFINITION                     */
/* ========================================================================== */

// Unique Object Dictionary IDs
#define BAVA_ID_TELEMETRY   0x01
#define BAVA_ID_SYS_CONTROL 0x02

// Packed multi-variable telemetry payload structure
typedef struct __attribute__((packed)) {
    uint32_t temperature_mC; // Temperature in millidegrees Celsius (e.g., 25400 = 25.4 °C)
    uint32_t humidity_ppm;    // Relative humidity in ppm (e.g., 55000 = 55.0 %)
    uint16_t pressure_hPa;   // Barometric pressure in hPa (e.g., 1013 hPa)
    uint8_t  system_status;  // System status bitfield flags
} sensor_telemetry_t;

/* ========================================================================== */
/*                      GLOBAL HANDLES & UART DRIVER                          */
/* ========================================================================== */

static bava_handle_t bava_handle;

// Obtain Zephyr console UART device node from devicetree
static const struct device *const uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* ========================================================================== */
/*                   BAVA HARDWARE CALLBACK IMPLEMENTATIONS                    */
/* ========================================================================== */

/**
 * @brief Hardware Transmission Callback required by BAVA Protocol.
 * Transmits formatted, byte-escaped binary frames over the Zephyr UART peripheral.
 */
static void zephyr_uart_tx_callback(const uint8_t *data, uint16_t size)
{
    if (!device_is_ready(uart_dev)) {
        return;
    }
    for (uint16_t i = 0; i < size; i++) {
        uart_poll_out(uart_dev, data[i]);
    }
}

/**
 * @brief Error Callback for unacknowledged timeout events or packet errors.
 */
static void bava_error_callback(uint8_t id, uint8_t error_code)
{
    printk("[BAVA ERROR] Variable ID 0x%02X triggered error code 0x%02X\n", id, error_code);
}

/**
 * @brief Polls incoming stream bytes from the Zephyr UART device and feeds them
 * into BAVA's non-blocking state machine parser (bava_process_byte).
 */
static void process_uart_rx(void)
{
    unsigned char rx_byte;
    while (uart_poll_in(uart_dev, &rx_byte) == 0) {
        // Feed byte into BAVA FSM parser (handles deframing, CRC validation, and memory binding)
        bava_process_byte(&bava_handle, (uint8_t)rx_byte);
    }
}

/* ========================================================================== */
/*                           APPLICATION MAIN LOOP                            */
/* ========================================================================== */

int main(void)
{
    printk("==================================================\n");
    printk("   BAVA Protocol Zephyr RTOS Demo Application     \n");
    printk("==================================================\n");

    if (!device_is_ready(uart_dev)) {
        printk("Error: Console UART device driver is not ready!\n");
        return 0;
    }

    // 1. Initialize BAVA instance and register hardware TX callback
    bava_init(&bava_handle, zephyr_uart_tx_callback);
    bava_handle.error_callback = bava_error_callback;

    // 2. Declare local target variables for Object Dictionary registration
    static sensor_telemetry_t local_telemetry = {
        .temperature_mC = 25400, // 25.4 °C
        .humidity_ppm    = 55000, // 55.0 %
        .pressure_hPa   = 1013,  // 1013 hPa
        .system_status  = 0x01   // System Normal
    };
    static uint32_t control_command = 0;

    // 3. Register variables to unique 8-bit IDs in the Object Dictionary
    bava_register_var(&bava_handle, BAVA_ID_TELEMETRY, &local_telemetry, sizeof(local_telemetry));
    bava_register_var(&bava_handle, BAVA_ID_SYS_CONTROL, &control_command, sizeof(control_command));

    printk("Object Dictionary initialized: Telemetry ID 0x%02X registered (%u bytes).\n",
           BAVA_ID_TELEMETRY, (unsigned int)sizeof(local_telemetry));

    uint32_t iteration = 0;

    while (1) {
        // Advance BAVA non-blocking timer using Zephyr kernel uptime ticks
        bava_tick(&bava_handle, k_uptime_get_32());

        // Process any incoming UART stream bytes through BAVA FSM
        process_uart_rx();

        // Check if remote node modified local telemetry structure
        if (bava_var_updated(&bava_handle, BAVA_ID_TELEMETRY)) {
            printk("[BAVA RX EVENT] Telemetry updated remotely!\n");
            printk("  -> Temp: %u.%03u deg C | Hum: %u.%03u %% | Press: %u hPa | Status: 0x%02X\n",
                   local_telemetry.temperature_mC / 1000, local_telemetry.temperature_mC % 1000,
                   local_telemetry.humidity_ppm / 1000, local_telemetry.humidity_ppm % 1000,
                   local_telemetry.pressure_hPa, local_telemetry.system_status);

            bava_var_clear_update_status(&bava_handle, BAVA_ID_TELEMETRY);
        }

        // Periodically update local telemetry sensor values and transmit over BAVA
        local_telemetry.temperature_mC += 100; // Simulate sensor drift
        local_telemetry.system_status   = (iteration % 2 == 0) ? 0x01 : 0x02;

        // Transmit updated telemetry frame to remote node
        bava_send_write(&bava_handle, BAVA_ID_TELEMETRY);

        iteration++;

        // Cooperative Zephyr RTOS thread sleep (1 second iteration loop)
        k_msleep(1000);
    }

    return 0;
}
