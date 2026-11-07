/*
 * SPDX-License-Identifier: GPL-3.0-only
 * BAVA Protocol Zephyr RTOS Demo Application
 *
 * Demonstrates:
 * - Non-blocking UART streaming byte parser (bava_process_byte)
 * - Asynchronous polling architecture (bava_poll) for ISR-safe ACK and response dispatch
 * - Object Dictionary multi-variable memory synchronization with access permissions
 * - Cross-thread / ISR tear-free multi-byte struct reads (bava_read_var)
 * - Non-blocking transmission with re-entrancy protection (bava_send_write)
 * - Error and timeout callback handling (bava_set_error_callback)
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
#define BAVA_ID_TELEMETRY    0x01
#define BAVA_ID_SYS_CONTROL  0x02
#define BAVA_ID_CALIBRATION  0x03

// Packed multi-variable telemetry payload structure (11 bytes)
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
    if (!device_is_ready(uart_dev) || data == NULL) {
        return;
    }
    for (uint16_t i = 0; i < size; i++) {
        uart_poll_out(uart_dev, data[i]);
    }
}

/**
 * @brief Error Callback for unacknowledged timeout events or packet errors.
 */
static void bava_error_callback(uint8_t id, bava_status_t error_code)
{
    printk("[BAVA ERROR] Variable ID 0x%02X triggered error code 0x%02X\n", id, (uint8_t)error_code);
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

    // 2. Register protocol error callback via public API
    bava_set_error_callback(&bava_handle, bava_error_callback);

    // 3. Declare target variables for Object Dictionary registration
    static sensor_telemetry_t local_telemetry = {
        .temperature_mC = 25400, // 25.4 °C
        .humidity_ppm    = 55000, // 55.0 %
        .pressure_hPa   = 1013,  // 1013 hPa
        .system_status  = 0x01   // System Normal
    };
    static uint32_t control_command = 0;
    static const uint32_t factory_cal_offset = 1042; // Read-Only factory calibration value

    // 4. Register variables to unique 8-bit IDs in the Object Dictionary with Access Permissions
    // Variable 0x01: Packed sensor telemetry (Read-Write)
    bava_register_var(&bava_handle, BAVA_ID_TELEMETRY, &local_telemetry, sizeof(local_telemetry), BAVA_PERM_READ_WRITE);

    // Variable 0x02: System control command word (Read-Write)
    bava_register_var(&bava_handle, BAVA_ID_SYS_CONTROL, &control_command, sizeof(control_command), BAVA_PERM_READ_WRITE);

    // Variable 0x03: Factory Calibration Offset (Read-Only: remote writes rejected with BAVA_STATUS_ERR_RO)
    bava_register_var(&bava_handle, BAVA_ID_CALIBRATION, (void *)&factory_cal_offset, sizeof(factory_cal_offset), BAVA_PERM_READ_ONLY);

    printk("Object Dictionary initialized: Telemetry ID 0x%02X registered (%u bytes).\n",
           BAVA_ID_TELEMETRY, (unsigned int)sizeof(local_telemetry));

    uint32_t iteration = 0;
    int64_t last_tx_time = 0;

    while (1) {
        int64_t now_ms = k_uptime_get();

        // 1. Advance BAVA non-blocking timer using Zephyr kernel uptime ticks
        bava_tick(&bava_handle, (uint32_t)now_ms);

        // 2. Process any incoming UART stream bytes through BAVA FSM
        process_uart_rx();

        // 3. CRITICAL: Dispatch asynchronous ACKs/responses and check proactive RX timeouts outside ISR
        bava_poll(&bava_handle);

        // 4. Check if remote node modified local telemetry structure
        if (bava_var_updated(&bava_handle, BAVA_ID_TELEMETRY)) {
            // Safe, tear-free atomic read of multi-byte struct under Zephyr irq_lock
            sensor_telemetry_t rx_snapshot;
            if (bava_read_var(&bava_handle, BAVA_ID_TELEMETRY, &rx_snapshot, sizeof(rx_snapshot))) {
                bava_var_clear_update_status(&bava_handle, BAVA_ID_TELEMETRY);

                printk("[BAVA RX EVENT] Telemetry updated remotely!\n");
                printk("  -> Temp: %u.%03u deg C | Hum: %u.%03u %% | Press: %u hPa | Status: 0x%02X\n",
                       rx_snapshot.temperature_mC / 1000, rx_snapshot.temperature_mC % 1000,
                       rx_snapshot.humidity_ppm / 1000, rx_snapshot.humidity_ppm % 1000,
                       rx_snapshot.pressure_hPa, rx_snapshot.system_status);
            }
        }

        // 5. Periodically transmit updated telemetry frame (Every 1000 ms)
        if (now_ms - last_tx_time >= 1000) {
            last_tx_time = now_ms;

            local_telemetry.temperature_mC += 100; // Simulate sensor drift
            local_telemetry.system_status   = (iteration % 2 == 0) ? 0x01 : 0x02;

            // Non-blocking transmission guarded against transaction re-entrancy
            bava_status_t tx_status = bava_send_write(&bava_handle, BAVA_ID_TELEMETRY);
            if (tx_status == BAVA_STATUS_ERR_BUSY) {
                printk("[BAVA TX] Deferred: previous transaction still awaiting ACK\n");
            } else if (tx_status != BAVA_STATUS_OK) {
                printk("[BAVA TX] Transmission failed (Status: 0x%02X)\n", (uint8_t)tx_status);
            }

            iteration++;
        }

        // Short cooperative sleep (10 ms) ensures responsive UART RX and prompt polling
        k_msleep(10);
    }

    return 0;
}
