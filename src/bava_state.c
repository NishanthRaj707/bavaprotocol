#include "bava.h"

extern void bava_internal_update(bava_handle_t* bava_handle, uint8_t id, const uint8_t* payload, uint8_t size);
extern void bava_cmd_read_request(bava_handle_t* bava_handle, uint8_t id);
extern void bava_cmd_write_ack(bava_handle_t* bava_handle, uint8_t id);
extern void bava_bus_cmd_write_ack(bava_handle_t* bava_handle, uint8_t id, uint8_t receiver_id);
extern void bava_bus_cmd_read_request(bava_handle_t* bava_handle, uint8_t id, uint8_t receiver_id);

void bava_process_byte(bava_handle_t* bava_handle, uint8_t rx_byte)
{
    // 1. Frame Delimiter 0x00: Finalize packet if all fields received, then reset for next packet
    if (rx_byte == 0x00)
    {
        if (bava_handle->rx_state == BAVA_STATE_VERIFY_DELIMITER)
        {
            if (bava_handle->incoming_crc == bava_handle->current_crc)
            {
                if (bava_handle->rx_cmd == BAVA_WRITE)
                {
                    bava_internal_update(bava_handle, bava_handle->rx_id, bava_handle->rx_payload, bava_handle->rx_len);
                    bava_cmd_write_ack(bava_handle, bava_handle->rx_id);
                }
                else if (bava_handle->rx_cmd == BAVA_BUS_WRITE)
                {
                    bava_internal_update(bava_handle, bava_handle->rx_id, bava_handle->rx_payload, bava_handle->rx_len);
                    // Prevent bus contention: only ACK unicast writes, never broadcast destination or broadcast sender
                    if (bava_handle->rx_node_id != BAVA_BROADCAST_NODE_ID && bava_handle->rx_sender_id != BAVA_BROADCAST_NODE_ID)
                    {
                        bava_bus_cmd_write_ack(bava_handle, bava_handle->rx_id, bava_handle->rx_sender_id);
                    }
                }
                else if (bava_handle->rx_cmd == BAVA_READ)
                {
                    bava_cmd_read_request(bava_handle, bava_handle->rx_id);
                }
                else if (bava_handle->rx_cmd == BAVA_BUS_READ)
                {
                    // Only reply if sender is not broadcast to avoid bus contention
                    if (bava_handle->rx_sender_id != BAVA_BROADCAST_NODE_ID)
                    {
                        bava_bus_cmd_read_request(bava_handle, bava_handle->rx_id, bava_handle->rx_sender_id);
                    }
                }
                else if (bava_handle->rx_cmd == BAVA_WRITE_ACK || bava_handle->rx_cmd == BAVA_READ_RESP ||
                         bava_handle->rx_cmd == BAVA_BUS_WRITE_ACK || bava_handle->rx_cmd == BAVA_BUS_READ_RESP)
                {
                    if (bava_handle->is_waiting_ack && bava_handle->pending_ack_id == bava_handle->rx_id)
                    {
                        bava_handle->is_waiting_ack = false;
                    }
                    if (bava_handle->rx_cmd == BAVA_READ_RESP || bava_handle->rx_cmd == BAVA_BUS_READ_RESP)
                    {
                        bava_internal_update(bava_handle, bava_handle->rx_id, bava_handle->rx_payload, bava_handle->rx_len);
                    }
                }
            }
        }

        bava_handle->rx_state = BAVA_STATE_WAIT_OVERHEAD;
        bava_handle->start_time = bava_handle->current_time_ms;
        return;
    }

    // 2. Timeout logic to evaluate elapsed delta time correctly
    if (bava_handle->rx_state != BAVA_STATE_WAIT_ZERO && 
        bava_handle->rx_state != BAVA_STATE_WAIT_OVERHEAD &&
       (bava_handle->current_time_ms - bava_handle->start_time) > BAVA_RX_TIMEOUT)
    {
        bava_handle->rx_state = BAVA_STATE_WAIT_ZERO;
        if (bava_handle->error_callback != NULL)
        {
            bava_handle->error_callback(0, BAVA_ERR_TIMEOUT);
        }
        return;
    }

    // 3. Handle COBS Initial Overhead Byte
    if (bava_handle->rx_state == BAVA_STATE_WAIT_OVERHEAD)
    {
        bava_handle->cobs_distance = rx_byte;
        bava_handle->current_crc = 0xFFFF;
        bava_handle->rx_state = BAVA_STATE_READ_CMD;
        bava_handle->start_time = bava_handle->current_time_ms;
        return;
    }

    // If waiting for resynchronization delimiter, discard all incoming non-zero bytes
    if (bava_handle->rx_state == BAVA_STATE_WAIT_ZERO)
    {
        return;
    }

    // 4. Real-time COBS on-the-fly substitution
    bava_handle->cobs_distance--;
    uint8_t byte;

    if (bava_handle->cobs_distance == 0)
    {
        byte = 0x00;
        bava_handle->cobs_distance = rx_byte;
    }
    else
    {
        byte = rx_byte;
    }

    // 5. Finite State Machine parsing pure data bytes
    switch (bava_handle->rx_state)
    {
        case BAVA_STATE_READ_CMD:
             bava_handle->rx_cmd = byte;
             if (bava_handle->is_bava_bus) {
                 if (byte == BAVA_BUS_READ || byte == BAVA_BUS_WRITE || byte == BAVA_BUS_READ_RESP || byte == BAVA_BUS_WRITE_ACK) {
                     bava_handle->rx_state = BAVA_STATE_READ_NODE_ID;
                 } else {
                     // Fail-fast mismatch: Bus handle instantly rejects P2P frames or invalid commands
                     bava_handle->rx_state = BAVA_STATE_WAIT_ZERO;
                 }
             } else {
                 if (byte == BAVA_READ || byte == BAVA_WRITE || byte == BAVA_READ_RESP || byte == BAVA_WRITE_ACK) {
                     bava_handle->rx_state = BAVA_STATE_READ_ID;
                 } else {
                     // Fail-fast mismatch: P2P handle instantly rejects Bus frames or invalid commands
                     bava_handle->rx_state = BAVA_STATE_WAIT_ZERO;
                 }
             }
             bava_handle->current_crc = bava_crc_update(bava_handle->current_crc, byte);
             break;
        
        case BAVA_STATE_READ_NODE_ID:
             bava_handle->rx_node_id = byte;
             bava_handle->current_crc = bava_crc_update(bava_handle->current_crc, byte);
             if (bava_handle->is_bava_bus && (bava_handle->node_id == bava_handle->rx_node_id || bava_handle->rx_node_id == BAVA_BROADCAST_NODE_ID)) {
                 bava_handle->rx_state = BAVA_STATE_READ_SENDER_ID;
             } else {
                 bava_handle->rx_state = BAVA_STATE_WAIT_ZERO;
             }
             break;
        
        case BAVA_STATE_READ_SENDER_ID:
             bava_handle->rx_sender_id = byte;
             // Explicitly accept valid node ID or broadcast ID (0xFF) as sender
             bava_handle->rx_state = BAVA_STATE_READ_ID;
             bava_handle->current_crc = bava_crc_update(bava_handle->current_crc, byte);
             break;

        case BAVA_STATE_READ_ID:
             bava_handle->rx_id = byte;
             bava_handle->rx_state = BAVA_STATE_READ_LEN;
             bava_handle->current_crc = bava_crc_update(bava_handle->current_crc, byte);
             break;

        case BAVA_STATE_READ_LEN:
             bava_handle->rx_len = byte;
             bava_handle->rx_idx = 0;
             bava_handle->current_crc = bava_crc_update(bava_handle->current_crc, byte);

             // Properly transition to PAYLOAD state (or CRC if length is 0) with bounds safety
             if (bava_handle->rx_len > BAVA_MAX_PAYLOAD) {
                bava_handle->rx_state = BAVA_STATE_WAIT_ZERO;
                if (bava_handle->error_callback != NULL) {
                    bava_handle->error_callback(bava_handle->rx_id, BAVA_ERR_INVALID_PAYLOAD_LENGTH);
                }
             } else if (bava_handle->rx_len == 0) {
                bava_handle->rx_state = BAVA_STATE_READ_CRC1;
             } else {
                bava_handle->rx_state = BAVA_STATE_READ_PAYLOAD;
             }
             break;
        
        case BAVA_STATE_READ_PAYLOAD:
             bava_handle->rx_payload[bava_handle->rx_idx] = byte;
             bava_handle->rx_idx++;
             bava_handle->current_crc = bava_crc_update(bava_handle->current_crc, byte);

             if (bava_handle->rx_idx >= bava_handle->rx_len) {
                bava_handle->rx_state = BAVA_STATE_READ_CRC1;
             }
             break;

        case BAVA_STATE_READ_CRC1:
             bava_handle->incoming_crc = (uint16_t)byte;
             bava_handle->rx_state = BAVA_STATE_READ_CRC2;
             break;
        
        case BAVA_STATE_READ_CRC2:
             bava_handle->incoming_crc |= ((uint16_t)byte << 8);
             bava_handle->rx_state = BAVA_STATE_VERIFY_DELIMITER;
             break;

        case BAVA_STATE_VERIFY_DELIMITER:
        case BAVA_STATE_WAIT_ZERO:
        case BAVA_STATE_WAIT_OVERHEAD:
        default:
             // Any non-zero byte while waiting for delimiter triggers framing error
             bava_handle->rx_state = BAVA_STATE_WAIT_ZERO;
             break;
    }
}
