/**
 * @file bava_arduino_demo.ino
 * @brief BAVA Protocol Point-to-Point (P2P) UART Example Sketch for Arduino IDE
 * @details Complete example demonstrating:
 *          - Hardware TX callback over Serial (bava_tx_cb_t)
 *          - Non-blocking byte-by-byte RX streaming (bava_process_byte)
 *          - ISR-safe asynchronous polling architecture (bava_poll)
 *          - Object Dictionary registration with access permissions (BAVA_PERM_*)
 *          - Tear-free multi-byte variable reads (bava_read_var)
 *          - Error and timeout callbacks (bava_set_error_callback)
 *          - Periodic non-blocking transmission with re-entrancy protection (bava_send_write)
 * @project repo -> github.com/NishanthRaj707/bavaprotocol
 */

#ifndef ARDUINO
#define ARDUINO
#endif

#include "bava.h"

// Variable ID definitions
#define VAR_ID_TELEMETRY   0x01  // Read-Write: 8-bit Counter / Status
#define VAR_ID_SENSOR_DATA 0x02  // Read-Write: 32-bit Multi-byte Sensor Value
#define VAR_ID_DEVICE_INFO 0x03  // Read-Only: 16-bit Fixed Device Hardware Revision

// Global BAVA Protocol Handle
static bava_handle_t bava_handle;

// Target Dictionary Variables
static uint8_t  sys_telemetry = 1;
static uint32_t sensor_reading = 1000;
static const uint16_t hw_revision = 0x0200; // v2.0 (Read-Only)

// Timing trackers
static uint32_t last_tx_ms = 0;

/**
 * @brief Hardware transmit callback required by BAVA Protocol.
 * @param data Pointer to encoded byte stream ready for wire transmission.
 * @param size Number of bytes to transmit.
 */
void arduino_bava_tx(const uint8_t* data, uint16_t size) {
    if (data != NULL && size > 0) {
        Serial.write(data, size);
    }
}

/**
 * @brief Optional yield callback for multithreaded/RTOS Arduino boards (e.g. ESP32 Arduino core).
 */
void arduino_bava_yield(void) {
    yield();
}

/**
 * @brief Error and timeout callback triggered by BAVA protocol stack.
 * @param id Variable ID that caused the error (or 0 for general framing/inactivity timeouts).
 * @param error Status code indicating the exact error reason.
 */
void arduino_bava_error_cb(uint8_t id, bava_status_t error) {
    // Note: Keep callback lightweight (do not execute blocking code here)
    // Common error codes:
    // BAVA_STATUS_ERR_RO: Remote tried to overwrite read-only variable
    // BAVA_STATUS_ERR_SIZE: Remote payload length mismatch
    // BAVA_STATUS_ERR_NOT_FOUND: Unregistered variable ID
    // BAVA_STATUS_ERR_BUSY: Outbound TX rejected because previous transaction awaits ACK
    // BAVA_STATUS_ERR_TIMEOUT: TX ACK or RX frame inactivity timeout elapsed
    (void)id;
    (void)error;
}

void setup() {
    // 1. Initialize Hardware Serial (115200 8N1)
    Serial.begin(115200);
    while (!Serial && millis() < 2000); // Wait for Serial USB on Leonardo/SAMD/ESP32-S3

    // 2. Initialize BAVA Protocol Handle with Hardware TX Callback
    bava_init(&bava_handle, arduino_bava_tx);

    // 3. Register Yield Callback for Arduino Targets
    bava_handle.yield_callback = arduino_bava_yield;

    // 4. Register Protocol Error Callback
    bava_set_error_callback(&bava_handle, arduino_bava_error_cb);

    // 5. Register Variables with Access Control Permissions
    // Telemetry: 8-bit Read-Write variable
    bool ok1 = bava_register_var(&bava_handle, VAR_ID_TELEMETRY, &sys_telemetry, sizeof(sys_telemetry), BAVA_PERM_READ_WRITE);
    // Sensor reading: 32-bit Read-Write variable
    bool ok2 = bava_register_var(&bava_handle, VAR_ID_SENSOR_DATA, &sensor_reading, sizeof(sensor_reading), BAVA_PERM_READ_WRITE);
    // Device Info: 16-bit Read-Only variable (remote WRITE requests will be rejected with BAVA_STATUS_ERR_RO)
    bool ok3 = bava_register_var(&bava_handle, VAR_ID_DEVICE_INFO, (void*)&hw_revision, sizeof(hw_revision), BAVA_PERM_READ_ONLY);

    if (!ok1 || !ok2 || !ok3) {
        // Registration failed (duplicate ID, size limit exceeded, or table full)
        while (1);
    }
}

void loop() {
    // 1. Stream incoming Serial bytes into BAVA State Machine Parser
    // (In an interrupt-driven UART driver, bava_process_byte can safely be called directly from UART ISR)
    while (Serial.available() > 0) {
        uint8_t incoming_byte = (uint8_t)Serial.read();
        bava_process_byte(&bava_handle, incoming_byte);
    }

    // 2. CRITICAL: Dispatch asynchronous ACKs/responses and check proactive RX timeouts outside ISR
    bava_poll(&bava_handle);

    // 3. Update BAVA Protocol system tick for timeout management
    bava_tick(&bava_handle, millis());

    // 4. Check if registered variables were updated by remote host WRITE command
    if (bava_var_updated(&bava_handle, VAR_ID_SENSOR_DATA)) {
        bava_var_clear_update_status(&bava_handle, VAR_ID_SENSOR_DATA);

        // Safe tear-free multi-byte read: prevents preemption mid-read on 8-bit/16-bit MCUs
        uint32_t safe_sensor_val = 0;
        if (bava_read_var(&bava_handle, VAR_ID_SENSOR_DATA, &safe_sensor_val, sizeof(safe_sensor_val))) {
            // Process updated safe_sensor_val safely
            (void)safe_sensor_val;
        }
    }

    // 5. Non-blocking periodic transmission (Every 100 ms)
    uint32_t current_ms = millis();
    if (current_ms - last_tx_ms >= 100) {
        last_tx_ms = current_ms;

        // Increment telemetry variable
        if (sys_telemetry < 254) {
            sys_telemetry++;
        } else {
            sys_telemetry = 1;
        }

        // Transmit updated telemetry variable across BAVA protocol.
        // Protected against transaction re-entrancy: returns BAVA_STATUS_ERR_BUSY
        // if a previous outbound transaction is still awaiting an ACK.
        bava_status_t tx_status = bava_send_write(&bava_handle, VAR_ID_TELEMETRY);
        if (tx_status == BAVA_STATUS_ERR_BUSY) {
            // Previous transaction still in flight; will retry on next cycle
        }
    }
}
