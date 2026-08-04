#include "bava.h"
#include <string.h>

extern uint16_t bava_calculate_crc(uint8_t cmd, uint8_t id, uint8_t len, const uint8_t *payload);

void bava_tick(bava_handle_t* bava_handle, uint32_t system_tick_ms)
{
    bava_handle->current_time_ms = system_tick_ms;

    if (bava_handle->is_waiting_ack && (bava_handle->current_time_ms - bava_handle->tx_timestamp) >= BAVA_TX_TIMEOUT)
    {
        bava_handle->is_waiting_ack = false;

        if (bava_handle->error_callback != NULL) {
            bava_handle->error_callback(bava_handle->pending_ack_id, BAVA_ERR_TIMEOUT);
        }
    }
}

static void bava_add_escape_byte(uint8_t* buffer, uint16_t* idx, uint8_t byte)
{
    if (byte == BAVA_SYNC_BYTE1 || byte == BAVA_ESCAPE_BYTE)
    {
        buffer[(*idx)++] = BAVA_ESCAPE_BYTE;
        buffer[(*idx)++] = byte ^ BAVA_ESCAPE_MASK;
    }
    else
    {
        buffer[(*idx)++] = byte;
    }
}

static void bava_internal_send_packet(bava_handle_t *bava_handle, uint8_t cmd, uint8_t id, const uint8_t *payload, uint8_t len)
{
    uint16_t tx_idx = 0;

    bava_handle->tx_buffer[tx_idx++] = BAVA_SYNC_BYTE1;
    bava_handle->tx_buffer[tx_idx++] = BAVA_SYNC_BYTE2;

    bava_add_escape_byte(bava_handle->tx_buffer, &tx_idx, cmd);
    bava_add_escape_byte(bava_handle->tx_buffer, &tx_idx, id);
    bava_add_escape_byte(bava_handle->tx_buffer, &tx_idx, len);

    for (uint8_t i = 0; i < len; i++)
    {
        bava_add_escape_byte(bava_handle->tx_buffer, &tx_idx, payload[i]);
    }
    
    uint16_t crc = bava_calculate_crc(cmd, id, len, payload);

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
        #ifdef ESP_PLATFORM
            xSemaphoreTake(bava_handle->tx_mutex, portMAX_DELAY); 
        #elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
            osMutexAcquire(bava_handle->tx_mutex, osWaitForever);
        #else
            uint32_t spin_count = 0;
            while (atomic_flag_test_and_set(&bava_handle->tx_lock)) {
                if (bava_handle->yield_callback != NULL) {
                    bava_handle->yield_callback(); 
                }
                if (++spin_count > 1000000UL) { // Prevent WDT starvation
                    break;
                }
            }
        #endif
            uint8_t tx_buffer[BAVA_MAX_PAYLOAD];
            const uint8_t* payload = (const uint8_t *)bava_handle->variables[i].var_ptr;
            uint8_t len = bava_handle->variables[i].size;

            if (len > BAVA_MAX_PAYLOAD) {
                len = BAVA_MAX_PAYLOAD;
            }

            // Directive 1: Safe memcpy to local aligned variables before endianness conversion
            if (len == 2)
            {
                uint16_t temp_val;
                memcpy(&temp_val, payload, sizeof(temp_val));
                temp_val = bava_htons(temp_val);
                memcpy(tx_buffer, &temp_val, sizeof(temp_val));
            }
            else if (len == 4)
            {
                uint32_t temp_val;
                memcpy(&temp_val, payload, sizeof(temp_val));
                temp_val = bava_htonl(temp_val);
                memcpy(tx_buffer, &temp_val, sizeof(temp_val));
            }
            else
            {
                // Directive 4: Fix payload truncation by copying full 'len' bytes
                memcpy(tx_buffer, payload, len);    
            }

            bava_internal_send_packet(bava_handle, BAVA_WRITE, id, tx_buffer, len);
        #ifdef ESP_PLATFORM
            xSemaphoreGive(bava_handle->tx_mutex);
        #elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
            osMutexRelease(bava_handle->tx_mutex);
        #else
            atomic_flag_clear(&bava_handle->tx_lock);
        #endif
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
