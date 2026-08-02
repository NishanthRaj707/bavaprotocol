#include <bava.h>

extern uint16_t bava_calculate_crc(uint8_t cmd, uint8_t id, uint8_t len, uint8_t *payload);

void bava_tick(bava_handle_t* bava_handle,uint32_t system_tick_ms)
{
    bava_handle->current_time_ms = system_tick_ms;

    if(bava_handle->is_waiting_ack && (bava_handle->current_time_ms - bava_handle->tx_timestamp)>= BAVA_TX_TIMEOUT)
    {
        bava_handle->is_waiting_ack = false;

        if (bava_handle->error_callback != NULL) {
                bava_handle->error_callback(bava_handle->pending_ack_id, BAVA_ERR_TIMEOUT);
            }

    }


}


static void bava_add_escape_byte(uint8_t* buffer, uint16_t* idx, uint8_t byte)
{
    if (byte == 0xAA || byte == 0x7D)
    {
        buffer[(*idx)++] = 0x7D;
        buffer[(*idx)++] = byte ^ 0x20;
    }
    else
    {
        buffer[(*idx)++] = byte;
    }
}

static void bava_internal_send_packet(bava_handle_t *bava_handle, uint8_t cmd, uint8_t id, const uint8_t *payload, uint8_t len)
{
    uint16_t tx_idx = 0;

    bava_handle->tx_buffer[tx_idx++] = 0xAA;
    bava_handle->tx_buffer[tx_idx++] = 0x55;

    bava_add_escape_byte(bava_handle->tx_buffer, &tx_idx, cmd);
    bava_add_escape_byte(bava_handle->tx_buffer, &tx_idx, id);
    bava_add_escape_byte(bava_handle->tx_buffer, &tx_idx, len);

    for (uint8_t i = 0; i < len; i++)
    {
        bava_add_escape_byte(bava_handle->tx_buffer, &tx_idx, payload[i]);
    }
    
    uint16_t crc = bava_calculate_crc(cmd, id, len, (uint8_t *)payload);

    uint8_t crc_low = (uint8_t)(crc & 0xFF);
    uint8_t crc_high = (uint8_t)((crc >> 8) & 0xFF);
    
    bava_add_escape_byte(bava_handle->tx_buffer, &tx_idx, crc_low);
    bava_add_escape_byte(bava_handle->tx_buffer, &tx_idx, crc_high);

    if (bava_handle->tx_callback != NULL)
    {
        bava_handle->tx_callback(bava_handle->tx_buffer, tx_idx);
    }
    
    
    if (cmd == BAVA_WRITE || cmd == BAVA_READ) {
        bava_handle->is_waiting_ack = true;
        bava_handle->pending_ack_id = id;
        bava_handle->pending_ack_cmd = cmd;
        bava_handle->tx_timestamp = bava_handle->current_time_ms; // Lock in the start time
    }
}

void bava_send_write(bava_handle_t* bava_handle, uint8_t id)
{
    for (uint8_t i = 0; i < bava_handle->var_count; i++)
    {
        if (bava_handle->variables[i].id == id)
        {
            uint8_t tx_buffer[BAVA_MAX_PAYLOAD];
            const uint8_t* payload = (const uint8_t *)bava_handle->variables[i].var_ptr;
            uint8_t len = bava_handle->variables[i].size;

            if(len == 2)
            {
                uint16_t temp =htons(*(uint16_t*)payload);
                memcpy(tx_buffer,&temp,2);
            }
            else if(len == 4)
            {
                uint32_t temp = htonl(*(uint32_t*)payload);
                memcpy(tx_buffer,&temp,4);
            }
            else
            {
                memcpy(tx_buffer,payload,1);    
            }

            bava_internal_send_packet(bava_handle, BAVA_WRITE, id, tx_buffer, len);
            return;
        }
    }
}

void bava_send_read(bava_handle_t* bava_handle, uint8_t id)
{
    bava_internal_send_packet(bava_handle, BAVA_READ, id, NULL, 0);
}

void bava_send_raw_write(bava_handle_t* bava_handle, uint8_t id, const uint8_t* pointer, uint8_t len)
{
    bava_internal_send_packet(bava_handle, BAVA_WRITE, id, pointer, len);
}

void bava_cmd_write_ack(bava_handle_t* bava_handle, uint8_t id)
{
    bava_internal_send_packet(bava_handle, BAVA_WRITE_ACK, id, NULL, 0);
}

void bava_cmd_read_request(bava_handle_t* bava_handle, uint8_t id)
{
    for (uint8_t i = 0; i < bava_handle->var_count; i++)
    {
        if (bava_handle->variables[i].id == id)
        {
            const uint8_t* payload = (const uint8_t *)bava_handle->variables[i].var_ptr;
            uint8_t len = bava_handle->variables[i].size;
            bava_internal_send_packet(bava_handle, BAVA_READ_RESP, id, payload, len);
            return;
        }
    }
}
