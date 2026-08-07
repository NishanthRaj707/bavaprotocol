#include "bava.h"

// The standard CRC-16-CCITT polynomial: x^16 + x^12 + x^5 + 1
#define BAVA_CRC16_POLY 0x1021

/* ========================================================================= *
 *                           INTERNAL FUNCTIONS                              *
 * ========================================================================= */

/**
 * @brief Updates the running CRC value with a single new byte.
 * Uses the bitwise (zero-RAM) calculation method.
 */
static uint16_t bava_crc_update(uint16_t crc, uint8_t data) {
    // XOR the byte into the most significant byte of the CRC
    crc ^= ((uint16_t)data << 8);
    
    // Process all 8 bits of the byte
    for (uint8_t i = 0; i < 8; i++) {
        // If the topmost bit is 1, shift left and XOR with the polynomial
        if (crc & 0x8000) {
            crc = (crc << 1) ^ BAVA_CRC16_POLY;
        } 
        // If the topmost bit is 0, just shift left
        else {
            crc = (crc << 1);
        }
    }
    
    return crc;
}

/* ========================================================================= *
 *                              PUBLIC API                                   *
 * ========================================================================= */

/**
 * @brief Calculates the full 16-bit CRC for a BAVA packet.
 * 
 * @param cmd The command byte (e.g., WRITE, READ_REQ)
 * @param id The variable dictionary ID
 * @param len The length of the payload
 * @param payload Pointer to the raw data array
 * @return uint16_t The calculated checksum
 */
uint16_t bava_calculate_crc(uint8_t cmd, uint8_t id, uint8_t len,const uint8_t *payload) {
    
    // Standard initialization value for CCITT
    uint16_t crc = 0xFFFF;
    
    // 1. Process the packet headers
    crc = bava_crc_update(crc, cmd);
    crc = bava_crc_update(crc, id);
    crc = bava_crc_update(crc, len);
    
    // 2. Process the payload (if any)
    if (len > 0 && payload != NULL) {
        for (uint8_t i = 0; i < len; i++) {
            crc = bava_crc_update(crc, payload[i]);
        }
    }
    
    return crc;
}