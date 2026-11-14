#include "bava.h"

// The standard CRC-16-CCITT polynomial: x^16 + x^12 + x^5 + 1
#define BAVA_CRC16_POLY 0x1021U

/*
 * Updates the running CRC value with a single new byte.
 * Uses the bitwise (zero-RAM) calculation method.
 */
 
uint16_t bava_crc_update(uint16_t crc, uint8_t data) {
    // XOR the byte into the most significant byte of the CRC (MISRA Rule 10.1 & 10.4 compliant)
    crc ^= (uint16_t)((uint16_t)data << 8U);
    
    // Process all 8 bits of the byte
    for (uint8_t i = 0U; i < 8U; i++) {
        // If the topmost bit is 1, shift left and XOR with the polynomial
        if ((crc & 0x8000U) != 0U) {
            crc = (uint16_t)((uint16_t)(crc << 1U) ^ BAVA_CRC16_POLY);
        } 
        // If the topmost bit is 0, just shift left
        else {
            crc = (uint16_t)(crc << 1U);
        }
    }
    
    return crc;
}
