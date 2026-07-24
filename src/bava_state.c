#include "bava.h"

extern uint16_t bava_calculate_crc(uint8_t cmd, uint8_t id, uint8_t len, uint8_t *payload);
extern void bava_internal_update(bava_handle_t* bava_handle,uint8_t id,uint8_t* payload,uint8_t size);


//Process the incoming byte
void bava_process_byte(bava_handle_t* bava_handle,uint8_t byte)
{
    static uint16_t incoming_crc = 0;

    if(byte == 0xAA)
    {
        bava_handle->rx_state = BAVA_STATE_WAIT_SYNC2;
        bava_handle->is_escaping = false;
        return;
    }

    if(byte == 0x7D)
    {
        bava_handle->is_escaping = true;
        return;
    }

    if(bava_handle->is_escaping)
    {
        byte ^= 0x20;
        bava_handle->is_escaping = false;
    }

    switch(bava_handle->rx_state)
    {
        case BAVA_STATE_WAIT_SYNC1:
             break;

        case BAVA_STATE_WAIT_SYNC2:
             if(byte == 0x55){
                bava_handle->rx_state = BAVA_STATE_READ_CMD;
             }
             else{
                bava_handle->rx_state = BAVA_STATE_WAIT_SYNC1;
             }
             break;

        case BAVA_STATE_READ_CMD:
             bava_handle->rx_cmd = byte;
             bava_handle->rx_state = BAVA_STATE_READ_ID;
             break;
        case BAVA_STATE_READ_ID:
             bava_handle->rx_id = byte;
             bava_handle->rx_state = BAVA_STATE_READ_LEN;
             break;

        case BAVA_STATE_READ_LEN:
             bava_handle->rx_len = byte;
             bava_handle->rx_idx = 0;

             if(bava_handle->rx_len == 0){
                bava_handle->rx_state = BAVA_STATE_READ_CRC1;
             }
             else{
                bava_handle->rx_state = BAVA_STATE_READ_PAYLOAD;
             }
             break;
        
        case BAVA_STATE_READ_PAYLOAD:
             bava_handle->rx_payload[bava_handle->rx_idx] = byte;
             bava_handle->rx_idx++;

             if(bava_handle->rx_idx == bava_handle->rx_len-1){
                bava_handle->rx_state = BAVA_STATE_READ_CRC1;
             }
             break;

        case BAVA_STATE_READ_CRC1:
             incoming_crc = byte;
             bava_handle->rx_state = BAVA_STATE_READ_CRC2;
             break;
        
        case BAVA_STATE_READ_CRC2:
             incoming_crc |= ((uint16_t)byte<<8);

             uint16_t calculated_crc = bava_calculate_crc(bava_handle->rx_cmd,bava_handle->rx_id,bava_handle->rx_len,bava_handle->rx_payload);

             if(incoming_crc == calculated_crc){
               if(bava_handle->rx_cmd == BAVA_WRITE){
                    bava_internal_update(&bava_handle,bava_handle->rx_id,bava_handle->rx_payload,bava_handle->rx_len);
               }
               else if (bava_handle->rx_cmd == BAVA_READ) {
                    // TODO: Trigger sending a READ_RESP back to the sender
                }
                // (ACKs and Responses will be handled in the TX module)
            }
            
            // Reset for the next packet
            bava_handle->rx_state = BAVA_STATE_WAIT_SYNC1;
            break;
             }

}


