#include <bava.h>

extern uint16_t bava_calculate_crc(uint8_t cmd, uint8_t id, uint8_t len, uint8_t *payload);

static void bava_add_escape_byte(uint8_t* buffer,uint16_t* idx,uint8_t byte)
{
    if(byte == 0xAA || byte == 0x7D)
    {
        buffer[*idx++]=0x7D;
        buffer[*idx++]=byte ^ 0x02;
    }
    else{
        buffer[*idx++]=byte;
    }
}

static void bava_internal_send_packet(bava_handle_t* bava_handle,uint8_t cmd,uint8_t id,uint8_t len,uint8_t* payload)
{
    uint8_t tx_buffer[550];
    uint16_t tx_idx = 0;

    tx_buffer[tx_idx++]=0xAA;
    tx_buffer[tx_idx++]=0x55;

    bava_add_escape_byte(tx_buffer,&tx_idx,cmd);
    bava_add_escape_byte(tx_buffer,&tx_idx,id);
    bava_add_escape_byte(tx_buffer,&tx_idx,len);

    for(uint8_t i=0;i<len;i++)
    {
        bava_add_escape_byte(tx_buffer,&tx_idx,payload[i]);
    }
    
    uint16_t crc = bava_calculate_crc(cmd,id,len,payload);

    uint8_t crc_low = (uint8_t)(crc & 0xFF);
    uint8_t crc_high = (uint8_t)((crc >> 8) & 0xFF);
    
    add_escaped_byte(tx_buffer, &tx_idx, crc_low);
    add_escaped_byte(tx_buffer, &tx_idx, crc_high);

    if(bava_handle->tx_callback != NULL)
    {
        bava_handle->tx_callback(tx_buffer,tx_idx);
    }
    bava_handle->is_waiting_ack = true;
    bava_handle->pending_ack_id = id;
    bava_handle->pending_ack_cmd = cmd;

}

void bava_cmd_write(bava_handle_t* bava_handle,uint8_t id)
{
    for(uint8_t i=0;i<bava_handle->var_count;i++)
    {
        if(bava_handle->variables[i].id == id)
        {
            uint8_t* payload =(uint8_t *)bava_handle->variables[i].var_ptr;
            uint8_t len =bava_handle->variables[i].size;

            bava_internal_send_packet(bava_handle,BAVA_WRITE,id,len,payload);
            return;
            
        }
    }
    
}

void bava_cmd_raw_write(bava_handle_t* bava_handle,uint8_t id,uint8_t* pointer,uint8_t len)
{
    uint8_t* payload=(uint8_t *)pointer;
    bava_internal_send_packet(bava_handle,BAVA_WRITE,id,len,payload);
}

void bava_cmd_write_ack(bava_handle_t* bava_handle,uint8_t id){
    bava_internal_send_packet(bava_handle,BAVA_WRITE_ACK,id,NULL,0);
}

void bava_cmd_read_request(bava_handle_t* bava_handle,uint8_t id)
{
    for(uint8_t i=0;i<bava_handle->var_count;i++)
    {
        if(bava_handle->variables[i].id == id)
        {
            uint8_t* payload = (uint8_t *)bava_handle->variables[i].var_ptr;
            uint8_t len =bava_handle->variables[i].size;
            bava_internal_send_packet(bava_handle, BAVA_READ_RESP, id, payload, len);
            return;
        }
    }
}

