#ifndef BAVA_TX_H
#define BAVA_TX_H

#include "bava.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/*                             PUBLIC TX API                                  */
/* ========================================================================== */

/**
 * @brief Non-blocking system tick function for TX timeout tracking.
 * @param bava_handle Pointer to the protocol instance handle.
 * @param system_tick_ms Current system time in milliseconds.
 */
void bava_tick(bava_handle_t* bava_handle, uint32_t system_tick_ms);

/**
 * @brief Sends a WRITE command frame for a registered dictionary variable ID.
 * @param bava_handle Pointer to the protocol instance handle.
 * @param id Variable ID to transmit.
 */
void bava_send_write(bava_handle_t* bava_handle, uint8_t id);

/**
 * @brief Sends a READ request command frame for a remote variable ID.
 * @param bava_handle Pointer to the protocol instance handle.
 * @param id Remote variable ID to request.
 */
void bava_send_read(bava_handle_t* bava_handle, uint8_t id);

/**
 * @brief Alias for bava_send_read for API compatibility.
 */
void bava_send_read_request(bava_handle_t* bava_handle, uint8_t id);

/**
 * @brief Sends a READ response containing the current variable payload for an ID.
 */
void bava_send_read_response(bava_handle_t* bava_handle, uint8_t id);

/**
 * @brief Sends a raw write payload frame to a destination variable ID with automatic endianness handling.
 * @param bava_handle Pointer to the protocol instance handle.
 * @param id Destination variable ID.
 * @param pointer Pointer to raw byte buffer.
 * @param len Payload byte length.
 */
void bava_send_raw_write(bava_handle_t* bava_handle, uint8_t id, const uint8_t* pointer, uint8_t len);

/**
 * @brief Transmits a WRITE acknowledgement frame.
 */
void bava_cmd_write_ack(bava_handle_t* bava_handle, uint8_t id);

/**
 * @brief Handles incoming READ request and transmits the corresponding response frame.
 */
void bava_cmd_read_request(bava_handle_t* bava_handle, uint8_t id);

#ifdef __cplusplus
}
#endif

#endif // BAVA_TX_H
