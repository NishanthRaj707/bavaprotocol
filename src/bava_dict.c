#include "bava.h"
#include <string.h>

#if defined(__ZEPHYR__)
unsigned int bava_zephyr_irq_key = 0;
#endif

// Initialization function
void bava_init(bava_handle_t* bava_handle, bava_tx_cb_t bava_tx_callback)
{
    memset(bava_handle, 0, sizeof(bava_handle_t));
    bava_handle->tx_callback = bava_tx_callback;
    bava_handle->bava_bus_tx_callback = NULL;
    bava_handle->is_bava_bus = false;
    bava_handle->rx_state = BAVA_STATE_WAIT_OVERHEAD;

#ifdef ESP_PLATFORM
    bava_handle->tx_mutex = xSemaphoreCreateMutex();
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    bava_handle->rx_mux = mux;
#elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
    const osMutexAttr_t tx_mutex_attr = { "bava_tx_mutex", osMutexPrioInherit, NULL, 0U };
    bava_handle->tx_mutex = osMutexNew(&tx_mutex_attr);
#elif defined(__ZEPHYR__)
    k_mutex_init(&bava_handle->tx_mutex);
#elif defined(ARDUINO)
    atomic_flag_clear(&bava_handle->tx_lock);
    bava_handle->yield_callback = NULL;
#else
    atomic_flag_clear(&bava_handle->tx_lock);
    bava_handle->yield_callback = NULL;
    bava_handle->enter_critical = NULL;
    bava_handle->exit_critical = NULL;
#endif
}


void bava_bus_init(bava_handle_t* bava_handle, bava_bus_tx_cb_t bava_bus_tx_callback, uint8_t node_id)
{
    bava_init(bava_handle, NULL);
    bava_handle->bava_bus_tx_callback = bava_bus_tx_callback;
    bava_handle->node_id = node_id;
    bava_handle->is_bava_bus = true;
}

// Registering the variables with strict validation and access permissions
bool bava_register_var(bava_handle_t* bava_handle, uint8_t id, void* variable_pointer, uint8_t variable_size, uint8_t permissions)
{
    // Hard guard clauses
    if (bava_handle == NULL || variable_pointer == NULL)
    {
        return false;
    }

    if (variable_size == 0U || variable_size > BAVA_MAX_PAYLOAD)
    {
        return false;
    }

    if (bava_handle->var_count >= BAVA_MAX_VARIABLES)
    {
        return false;
    }

    // Reject duplicate ID
    for (uint8_t i = 0; i < bava_handle->var_count; i++)
    {
        if (bava_handle->variables[i].id == id)
        {
            return false;
        }
    }

    uint8_t index = bava_handle->var_count;
    bava_handle->variables[index].id = id;
    bava_handle->variables[index].var_ptr = variable_pointer;
    bava_handle->variables[index].size = variable_size;
    bava_handle->variables[index].permissions = permissions;
    bava_handle->variables[index].updated = false;

    bava_handle->var_count++;

    return true;
}

// User-facing API to set the error/status callback
void bava_set_error_callback(bava_handle_t* bava_handle, bava_error_cb_t error_callback)
{
    if (bava_handle != NULL)
    {
        bava_handle->error_callback = error_callback;
    }
}

// Check the updated status
bool bava_var_updated(bava_handle_t* bava_handle, uint8_t id)
{
    for (uint8_t i = 0; i < bava_handle->var_count; i++)
    {
        if (bava_handle->variables[i].id == id)
        {
            return bava_handle->variables[i].updated;  
        }
    }
    return false;
}

// Clear the update status
void bava_var_clear_update_status(bava_handle_t* bava_handle, uint8_t id)
{
    for (uint8_t i = 0; i < bava_handle->var_count; i++)
    {
        if (bava_handle->variables[i].id == id)
        {
            bava_handle->variables[i].updated = false;
            return;
        }
    }
}

// Thread-safe / ISR-safe read of dictionary variable to prevent data tearing
bool bava_read_var(bava_handle_t *bava, uint8_t id, void *dest_buf, uint8_t size)
{
    if (bava == NULL || dest_buf == NULL || size == 0U)
    {
        return false;
    }

    for (uint8_t i = 0; i < bava->var_count; i++)
    {
        if (bava->variables[i].id == id)
        {
            if (bava->variables[i].size != size || bava->variables[i].var_ptr == NULL)
            {
                return false;
            }

            BAVA_DISABLE_IRQ();
            memcpy(dest_buf, bava->variables[i].var_ptr, (size_t)size);
            BAVA_ENABLE_IRQ();

            return true;
        }
    }

    return false;
}

// Internal update function
bava_status_t bava_internal_update(bava_handle_t* bava_handle, uint8_t id, const uint8_t* payload, uint8_t size)
{
    if (bava_handle == NULL || (payload == NULL && size > 0U))
    {
        return BAVA_STATUS_ERR_NOT_FOUND;
    }

    for (uint8_t i = 0; i < bava_handle->var_count; i++)
    {
        if (bava_handle->variables[i].id == id)
        {
            // 1. Permission Check: reject write to read-only variable
            if (bava_handle->variables[i].permissions == BAVA_PERM_READ_ONLY)
            {
                return BAVA_STATUS_ERR_RO;
            }

            // 2. Size Check: reject payload size mismatch
            if (bava_handle->variables[i].size != size)
            {
                return BAVA_STATUS_ERR_SIZE;
            }

            if (bava_handle->variables[i].var_ptr == NULL)
            {
                return BAVA_STATUS_ERR_NOT_FOUND;
            }

            // 3. Enter Critical Section
        #ifdef ESP_PLATFORM
            portENTER_CRITICAL_ISR(&(bava_handle->rx_mux));
        #elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
            UBaseType_t uxSavedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
        #elif defined(USE_HAL_DRIVER)
            __disable_irq();
        #elif defined(ARDUINO)
            noInterrupts();
        #elif defined(__ZEPHYR__)
            unsigned int key = irq_lock();
        #else
            if (bava_handle->enter_critical != NULL) bava_handle->enter_critical();
        #endif

            // Safe memcpy to local aligned variables before endianness conversion
            if (size == 2U)
            {
                uint16_t temp_val;
                memcpy(&temp_val, payload, sizeof(temp_val));
                temp_val = bava_ntohs(temp_val);
                memcpy(bava_handle->variables[i].var_ptr, &temp_val, sizeof(temp_val));
            }
            else if (size == 4U)
            {
                uint32_t temp_val;
                memcpy(&temp_val, payload, sizeof(temp_val));
                temp_val = bava_ntohl(temp_val);
                memcpy(bava_handle->variables[i].var_ptr, &temp_val, sizeof(temp_val));
            }
            else
            {
                memcpy(bava_handle->variables[i].var_ptr, payload, (size_t)size);
            }

            bava_handle->variables[i].updated = true;
            
        #ifdef ESP_PLATFORM
            portEXIT_CRITICAL_ISR(&(bava_handle->rx_mux));
        #elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
            taskEXIT_CRITICAL_FROM_ISR(uxSavedInterruptStatus);
        #elif defined(USE_HAL_DRIVER)
            __enable_irq();
        #elif defined(ARDUINO)
            interrupts();
        #elif defined(__ZEPHYR__)
            irq_unlock(key);
        #else
            if (bava_handle->exit_critical != NULL) bava_handle->exit_critical();
        #endif
            return BAVA_STATUS_OK;
        }
    }

    return BAVA_STATUS_ERR_NOT_FOUND;
}
