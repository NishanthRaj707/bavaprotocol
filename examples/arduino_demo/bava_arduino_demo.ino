/**
 * @file arduino_bava_example.ino
 * @brief BAVA Protocol Example Sketch for Arduino IDE
 * @details Complete example demonstrating hardware TX callback over Serial,
 *          non-blocking RX byte streaming, dictionary variable registration,
 *          and periodic transmission using bava_send_write().
 * @creator -> J.Nishanth Raj 
 * @project repo -> github.com/NishanthRaj707/bavaprotocol
 */

#ifndef ARDUINO
#define ARDUINO
#endif

#include "bava.h"
#include "bava_tx.h"

// Variable ID definition
#define TELEMETRY_VAR_ID  0x01

// Global BAVA Protocol Handle
static bava_handle_t bava_handle;

// Telemetry State Variable (matching ESP32 and STM32 test bench)
static uint8_t sys_telemetry = 1;

// Timing tracker
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

void setup() {
    // 1. Initialize Hardware Serial Baud Rate (115200 8N1)
    Serial.begin(115200);
    while (!Serial && millis() < 2000); // Wait for Serial USB on Leonardo/SAMD/ESP32-S3

    // 2. Initialize BAVA Protocol Handle with Hardware TX Callback
    bava_init(&bava_handle, arduino_bava_tx);

    // 3. Register Yield Callback for Arduino Targets
    bava_handle.yield_callback = arduino_bava_yield;

    // 4. Register Telemetry Variable into BAVA Dictionary
    int8_t status = bava_register_var(&bava_handle, TELEMETRY_VAR_ID, &sys_telemetry, sizeof(sys_telemetry));
    if (status != 0) {
        // Variable registration failed (dictionary full or invalid)
        while (1);
    }
}

void loop() {
    // 1. Stream incoming Serial bytes into BAVA State Machine
    while (Serial.available() > 0) {
        uint8_t incoming_byte = (uint8_t)Serial.read();
        bava_process_byte(&bava_handle, incoming_byte);
    }

    // 2. Check if registered telemetry variable was updated by a remote host WRITE command
    if (bava_var_updated(&bava_handle, TELEMETRY_VAR_ID)) {
        bava_var_clear_update_status(&bava_handle, TELEMETRY_VAR_ID);
        // Telemetry variable updated remotely!
    }

    // 3. Update BAVA Protocol system tick for TX timeout management
    bava_tick(&bava_handle, millis());

    // 4. Non-blocking periodic transmission (Every 50 ms / 20 Hz)
    uint32_t current_ms = millis();
    if (current_ms - last_tx_ms >= 50) {
        last_tx_ms = current_ms;

        // Increment telemetry variable (1 to 254)
        if (sys_telemetry < 254) {
            sys_telemetry++;
        } else {
            sys_telemetry = 1;
        }

        // Transmit updated telemetry variable across BAVA protocol
        bava_send_write(&bava_handle, TELEMETRY_VAR_ID);
    }
}