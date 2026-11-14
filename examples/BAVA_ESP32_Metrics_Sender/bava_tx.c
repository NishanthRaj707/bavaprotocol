#include "bava.h"
#include <string.h>

/* ========================================================================== */
/*                       CENTRALIZED MUTEX / LOCK HELPERS                     */
/* ========================================================================== */

static inline bool bava_tx_lock(bava_handle_t *bava_handle)
{
#ifdef ESP_PLATFORM
    BaseType_t ret = xSemaphoreTake(bava_handle->tx_mutex, portMAX_DELAY);
    return (ret == pdTRUE);
#elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
    osStatus_t status = osMutexAcquire(bava_handle->tx_mutex, osWaitForever);
    return (status == osOK);
#elif defined(__ZEPHYR__)
    int ret = k_mutex_lock(&bava_handle->tx_mutex, K_FOREVER);
    return (ret == 0);
#elif defined(ARDUINO)
    uint32_t spin_count = 0U;
    while (atomic_flag_test_and_set(&bava_handle->tx_lock)) {
        yield();
        if (++spin_count > 1000000UL) {
            return false;
        }
    }
    return true;
#else
    uint32_t spin_count = 0U;
    while (atomic_flag_test_and_set(&bava_handle->tx_lock)) {
        if (bava_handle->yield_callback != NULL) {
            bava_handle->yield_callback();
        }
        if (++spin_count > 1000000UL) { // Prevent WDT starvation
            return false;
        }
    }
    return true;
#endif
}

static inline void bava_tx_unlock(bava_handle_t *bava_handle)
{
#ifdef ESP_PLATFORM
    xSemaphoreGive(bava_handle->tx_mutex);
#elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
    osMutexRelease(bava_handle->tx_mutex);
#elif defined(__ZEPHYR__)
    k_mutex_unlock(&bava_handle->tx_mutex);
#elif defined(ARDUINO)
    atomic_flag_clear(&bava_handle->tx_lock);
#else
    atomic_flag_clear(&bava_handle->tx_lock);
#endif
}

/* ========================================================================== */
/*                       INTERNAL TRANSMISSION ENGINE                         */
/* ========================================================================== */
//COBS encoding function
static size_t tx_cobs_encode(bava_handle_t* bava_handle, size_t raw_length)
{
    uint8_t* src = bava_handle->tx_buffer;
    uint8_t* dest = bava_handle->cobs_buffer;

    size_t read_idx = 0U;
    size_t write_idx = 1U;
    size_t code = 1U;
    size_t code_idx = 0U;

    while(read_idx < raw_length)
    {
        if(src[read_idx] == 0x00U)
        {
             dest[code_idx] = (uint8_t)code;
             code = 1U;
             code_idx = write_idx++;
        }
        else{
            dest[write_idx++] = src[read_idx];
            code++;
            
            if(code == 0xFFU)
            {
                dest[code_idx] = (uint8_t)code;
                code = 1U;
                code_idx = write_idx++;
            }
        }

        read_idx++;
    }
    dest[code_idx] = (uint8_t)code;
    dest[write_idx++] = 0x00U;

    return write_idx;
}

static bava_status_t bava_internal_send_packet(bava_handle_t *bava_handle, uint8_t cmd, uint8_t id, const uint8_t *payload, uint8_t len)
{
    if (bava_handle == NULL)
    {
        return BAVA_STATUS_ERR_NOT_FOUND;
    }

    bool expects_ack = ((cmd == BAVA_WRITE) || (cmd == BAVA_READ));

    // Re-entrancy guard: if already awaiting an ACK, reject immediately (BUG-H04)
    if (expects_ack && bava_handle->is_waiting_ack)
    {
        return BAVA_STATUS_ERR_BUSY;
    }

    // 1. Acquire Centralized Hardware TX Lock BEFORE touching shared tx_buffer
    if (!bava_tx_lock(bava_handle))
    {
        return BAVA_STATUS_ERR_BUSY;
    }

    uint16_t tx_idx = 0;

    bava_handle->tx_buffer[tx_idx++] = cmd;
    bava_handle->tx_buffer[tx_idx++] = id;

    // Validate that if payload == NULL, payload_size is forced to 0 (fixes BUG-H07)
    uint8_t payload_size = (payload == NULL) ? 0U : ((len > BAVA_MAX_PAYLOAD) ? BAVA_MAX_PAYLOAD : len);
    bava_handle->tx_buffer[tx_idx++] = payload_size;

    // Step-by-step CRC calculation matching RX engine in bava_state.c
    uint16_t crc = 0xFFFFU;
    crc = bava_crc_update(crc, cmd);
    crc = bava_crc_update(crc, id);
    crc = bava_crc_update(crc, payload_size);

    if (payload_size > 0U && payload != NULL)
    {
        for (uint8_t i = 0U; i < payload_size; i++)
        {
            bava_handle->tx_buffer[tx_idx++] = payload[i];
            crc = bava_crc_update(crc, payload[i]);
        }
    }

    uint8_t crc_low = (uint8_t)(crc & 0xFFU);
    uint8_t crc_high = (uint8_t)((crc >> 8U) & 0xFFU);

    bava_handle->tx_buffer[tx_idx++] = crc_low;
    bava_handle->tx_buffer[tx_idx++] = crc_high;

    // Pass to the COBS function
    size_t cobs_len = tx_cobs_encode(bava_handle, tx_idx);

    // FIX BUG-H02 & BUG-H03: Arm the ACK expectation BEFORE invoking hardware TX callback.
    // Read 32-bit current_time_ms within IRQ critical section to avoid tearing on 8-bit/16-bit MCUs.
    if (expects_ack)
    {
        uint32_t now_ms;
        BAVA_DISABLE_IRQ();
        now_ms = bava_handle->current_time_ms;
        BAVA_ENABLE_IRQ();

        bava_handle->waiting_ack_id = id;
        bava_handle->waiting_ack_cmd = cmd;
        bava_handle->tx_timestamp = now_ms;
        bava_handle->is_waiting_ack = true;
    }

    // Hardware Transmission Callback execution while safely locked
    bool tx_dispatched = false;
    if (bava_handle->tx_callback != NULL)
    {
        bava_handle->tx_callback(bava_handle->cobs_buffer, (uint16_t)cobs_len);
        tx_dispatched = true;
    }

    if (!tx_dispatched)
    {
        // If transmission cannot be dispatched, cleanly revert is_waiting_ack
        if (expects_ack)
        {
            bava_handle->is_waiting_ack = false;
        }
        bava_tx_unlock(bava_handle);
        return BAVA_STATUS_ERR_BUSY;
    }

    // 2. Release Centralized Hardware TX Lock AFTER transmission finishes
    bava_tx_unlock(bava_handle);
    return BAVA_STATUS_OK;
}

/* ========================================================================== */
/*                             PUBLIC API PIPELINE                            */
/* ========================================================================== */

static inline void bava_check_rx_timeout(bava_handle_t* bava_handle)
{
    uint32_t now_ms;
    uint32_t rx_start;
    bava_rx_state_t state;

    BAVA_DISABLE_IRQ();
    now_ms = bava_handle->current_time_ms;
    rx_start = bava_handle->start_time;
    state = bava_handle->rx_state;
    BAVA_ENABLE_IRQ();

    if ((state != BAVA_STATE_WAIT_OVERHEAD) && ((now_ms - rx_start) > BAVA_RX_TIMEOUT))
    {
        BAVA_DISABLE_IRQ();
        bava_handle->rx_state = BAVA_STATE_WAIT_OVERHEAD;
        BAVA_ENABLE_IRQ();

        if (bava_handle->error_callback != NULL)
        {
            bava_handle->error_callback(0U, BAVA_STATUS_ERR_TIMEOUT);
        }
    }
}

void bava_poll(bava_handle_t* bava_handle)
{
    if (bava_handle == NULL)
    {
        return;
    }

    // 1. Proactive RX Frame Inactivity Timeout Check (fixes BUG-K06 & BUG-H08)
    bava_check_rx_timeout(bava_handle);

    // 2. Asynchronous Polling Dispatch (atomic read of pending flags outside ISR)
    bool do_ack = false;
    uint8_t ack_id = 0;
    bava_status_t ack_status = BAVA_STATUS_OK;

    bool do_read_resp = false;
    uint8_t read_id = 0;

    BAVA_DISABLE_IRQ();
    if (bava_handle->pending_ack_tx)
    {
        do_ack = true;
        ack_id = bava_handle->pending_ack_id;
        ack_status = bava_handle->pending_ack_status;
        bava_handle->pending_ack_tx = false;
    }
    if (bava_handle->pending_read_resp_tx)
    {
        do_read_resp = true;
        read_id = bava_handle->pending_read_resp_id;
        bava_handle->pending_read_resp_tx = false;
    }
    BAVA_ENABLE_IRQ();

    if (do_ack)
    {
        bava_cmd_write_ack(bava_handle, ack_id, ack_status);
    }

    if (do_read_resp)
    {
        bava_cmd_read_request(bava_handle, read_id);
    }
}

void bava_tick(bava_handle_t* bava_handle, uint32_t system_tick_ms)
{
    if (bava_handle == NULL)
    {
        return;
    }

    // 1. Atomically update current_time_ms (fixes BUG-H03)
    BAVA_DISABLE_IRQ();
    bava_handle->current_time_ms = system_tick_ms;
    BAVA_ENABLE_IRQ();

    // 2. Proactive RX Frame Inactivity Timeout Check (fixes BUG-K06 & BUG-H08)
    bava_check_rx_timeout(bava_handle);

    // 3. Outbound TX ACK Timeout Check (atomic reads and clear)
    bool is_waiting = false;
    uint32_t tx_time = 0;
    uint8_t waiting_id = 0;

    BAVA_DISABLE_IRQ();
    is_waiting = bava_handle->is_waiting_ack;
    tx_time = bava_handle->tx_timestamp;
    waiting_id = bava_handle->waiting_ack_id;
    BAVA_ENABLE_IRQ();

    if (is_waiting && ((system_tick_ms - tx_time) >= BAVA_TX_TIMEOUT))
    {
        BAVA_DISABLE_IRQ();
        bava_handle->is_waiting_ack = false;
        BAVA_ENABLE_IRQ();

        if (bava_handle->error_callback != NULL)
        {
            bava_handle->error_callback(waiting_id, BAVA_STATUS_ERR_TIMEOUT);
        }
    }
}

bava_status_t bava_send_write(bava_handle_t* bava_handle, uint8_t id)
{
    if (bava_handle == NULL)
    {
        return BAVA_STATUS_ERR_NOT_FOUND;
    }

    // Guard against transaction re-entrancy overwrite (BUG-H04)
    if (bava_handle->is_waiting_ack)
    {
        return BAVA_STATUS_ERR_BUSY;
    }

    for (uint8_t i = 0; i < bava_handle->var_count; i++)
    {
        if (bava_handle->variables[i].id == id)
        {
            uint8_t len = bava_handle->variables[i].size;
            uint8_t type_flag = (uint8_t)(bava_handle->variables[i].permissions & BAVA_TYPE_MASK);

            if (type_flag == BAVA_TYPE_U16 && len == 2U)
            {
                uint16_t temp_val;
                memcpy(&temp_val, bava_handle->variables[i].var_ptr, sizeof(temp_val));
                temp_val = bava_htons(temp_val);
                return bava_internal_send_packet(bava_handle, BAVA_WRITE, id, (const uint8_t*)&temp_val, len);
            }
            else if (type_flag == BAVA_TYPE_U32 && len == 4U)
            {
                uint32_t temp_val;
                memcpy(&temp_val, bava_handle->variables[i].var_ptr, sizeof(temp_val));
                temp_val = bava_htonl(temp_val);
                return bava_internal_send_packet(bava_handle, BAVA_WRITE, id, (const uint8_t*)&temp_val, len);
            }
            else
            {
                const uint8_t* payload = (const uint8_t *)bava_handle->variables[i].var_ptr;
                return bava_internal_send_packet(bava_handle, BAVA_WRITE, id, payload, len);
            }
        }
    }

    return BAVA_STATUS_ERR_NOT_FOUND;
}

bava_status_t bava_send_read(bava_handle_t* bava_handle, uint8_t id)
{
    if (bava_handle == NULL)
    {
        return BAVA_STATUS_ERR_NOT_FOUND;
    }

    // Guard against transaction re-entrancy overwrite (BUG-H04)
    if (bava_handle->is_waiting_ack)
    {
        return BAVA_STATUS_ERR_BUSY;
    }

    return bava_internal_send_packet(bava_handle, BAVA_READ, id, NULL, 0);
}

bava_status_t bava_send_raw_write(bava_handle_t* bava_handle, uint8_t id, const uint8_t* pointer, uint8_t len)
{
    if (bava_handle == NULL)
    {
        return BAVA_STATUS_ERR_NOT_FOUND;
    }

    // Guard against transaction re-entrancy overwrite (BUG-H04)
    if (bava_handle->is_waiting_ack)
    {
        return BAVA_STATUS_ERR_BUSY;
    }

    if (len > BAVA_MAX_PAYLOAD)
    {
        return BAVA_STATUS_ERR_SIZE;
    }

    return bava_internal_send_packet(bava_handle, BAVA_WRITE, id, pointer, len);
}

void bava_cmd_write_ack(bava_handle_t* bava_handle, uint8_t id, bava_status_t status)
{
    uint8_t status_byte = (uint8_t)status;
    (void)bava_internal_send_packet(bava_handle, BAVA_WRITE_ACK, id, &status_byte, 1U);
}

void bava_cmd_read_request(bava_handle_t* bava_handle, uint8_t id)
{
    if (bava_handle == NULL)
    {
        return;
    }

    for (uint8_t i = 0; i < bava_handle->var_count; i++)
    {
        if (bava_handle->variables[i].id == id)
        {
            uint8_t len = bava_handle->variables[i].size;
            uint8_t type_flag = (uint8_t)(bava_handle->variables[i].permissions & BAVA_TYPE_MASK);

            if (type_flag == BAVA_TYPE_U16 && len == 2U)
            {
                uint16_t temp_val;
                memcpy(&temp_val, bava_handle->variables[i].var_ptr, sizeof(temp_val));
                temp_val = bava_htons(temp_val);
                (void)bava_internal_send_packet(bava_handle, BAVA_READ_RESP, id, (const uint8_t*)&temp_val, len);
            }
            else if (type_flag == BAVA_TYPE_U32 && len == 4U)
            {
                uint32_t temp_val;
                memcpy(&temp_val, bava_handle->variables[i].var_ptr, sizeof(temp_val));
                temp_val = bava_htonl(temp_val);
                (void)bava_internal_send_packet(bava_handle, BAVA_READ_RESP, id, (const uint8_t*)&temp_val, len);
            }
            else
            {
                const uint8_t* payload = (const uint8_t *)bava_handle->variables[i].var_ptr;
                (void)bava_internal_send_packet(bava_handle, BAVA_READ_RESP, id, payload, len);
            }
            return;
        }
    }

    // ID not found in dictionary (fixes BUG-H06): transmit empty READ_RESP (len = 0) so remote fails fast
    (void)bava_internal_send_packet(bava_handle, BAVA_READ_RESP, id, NULL, 0U);
}