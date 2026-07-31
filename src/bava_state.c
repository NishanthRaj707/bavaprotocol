#include "bava.h"

extern uint16_t bava_calculate_crc(uint8_t cmd, uint8_t id, uint8_t len, uint8_t *payload);
extern void bava_internal_update(bava_handle_t* bava_handle,uint8_t id,uint8_t* payload,uint8_t size);
extern void bava_cmd_read_request(bava_handle_t* bava_handle,uint8_t id);
extern void bava_cmd_write_ack(bava_handle_t* bava_handle,uint8_t id);

void bava_process_byte(bava_ctx_t* ctx, uint8_t byte)
{
    if(byte == 0xAA)
    {
        ctx->rx_state = BAVA_STATE_WAIT_SYNC2;
        ctx->is_escaping = false;
        return;
    }

    if(byte == 0x7D)
    {
        ctx->is_escaping = true;
        return;
    }

    if(ctx->is_escaping)
    {
        byte ^= 0x20;
        ctx->is_escaping = false;
    }

    switch(ctx->rx_state)
    {
        case BAVA_STATE_WAIT_SYNC1:
             break;

        case BAVA_STATE_WAIT_SYNC2:
             if(byte == 0x55){
                ctx->rx_state = BAVA_STATE_READ_CMD;
             }
             else{
                ctx->rx_state = BAVA_STATE_WAIT_SYNC1;
             }
             break;

        case BAVA_STATE_READ_CMD:
             ctx->rx_cmd = byte;
             ctx->rx_state = BAVA_STATE_READ_ID;
             break;

        case BAVA_STATE_READ_ID:
             ctx->rx_id = byte;
             ctx->rx_state = BAVA_STATE_READ_LEN;
             break;

        case BAVA_STATE_READ_LEN:
             ctx->rx_len = byte;
             ctx->rx_idx = 0;

             if(ctx->rx_len > BAVA_MAX_PAYLOAD){
                ctx->rx_state = BAVA_STATE_WAIT_SYNC1;
             }
             else if(ctx->rx_len == 0){
                ctx->rx_state = BAVA_STATE_READ_CRC1;
             }
             else{
                ctx->rx_state = BAVA_STATE_READ_PAYLOAD;
             }
             break;
        
        case BAVA_STATE_READ_PAYLOAD:
             ctx->rx_payload[ctx->rx_idx] = byte;
             ctx->rx_idx++;

             if(ctx->rx_idx >= ctx->rx_len){
                ctx->rx_state = BAVA_STATE_READ_CRC1;
             }
             break;

        case BAVA_STATE_READ_CRC1:
             ctx->incoming_crc = byte;
             ctx->rx_state = BAVA_STATE_READ_CRC2;
             break;
        
        case BAVA_STATE_READ_CRC2:
             ctx->incoming_crc |= ((uint16_t)byte << 8);

             uint16_t calculated_crc = bava_calculate_crc(ctx->rx_cmd, ctx->rx_id, ctx->rx_len, ctx->rx_payload);

             if(ctx->incoming_crc == calculated_crc){
                if(ctx->rx_cmd == BAVA_WRITE){
                    bava_internal_update(ctx, ctx->rx_id, ctx->rx_payload, ctx->rx_len);
                    bava_cmd_write_ack(ctx, ctx->rx_id);
                }
                else if (ctx->rx_cmd == BAVA_READ) {
                    bava_cmd_read_request(ctx, ctx->rx_id);
                }
                else if (ctx->rx_cmd == BAVA_WRITE_ACK || ctx->rx_cmd == BAVA_READ_RESP) {
                    if (ctx->is_waiting_ack && ctx->pending_ack_id == ctx->rx_id) {
                        ctx->is_waiting_ack = false;
                    }
                    if (ctx->rx_cmd == BAVA_READ_RESP) {
                        bava_internal_update(ctx, ctx->rx_id, ctx->rx_payload, ctx->rx_len);
                    }
                }
            }
            
            // Reset for the next packet
            ctx->rx_state = BAVA_STATE_WAIT_SYNC1;
            break;
    }
}
