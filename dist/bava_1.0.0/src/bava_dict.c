#include "bava.h"
#include <string.h>

// Initialization function
void bava_init(bava_handle_t* bava_handle, bava_tx_cb_t bava_tx_callback)
{
    memset(bava_handle, 0, sizeof(bava_handle_t));
    bava_handle->tx_callback = bava_tx_callback;
    bava_handle->rx_state = BAVA_STATE_WAIT_SYNC1;

#ifdef ESP_PLATFORM
    bava_handle->tx_mutex = xSemaphoreCreateMutex();
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    bava_handle->rx_mux = mux;
#elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
    const osMutexAttr_t tx_mutex_attr = { "bava_tx_mutex", osMutexPrioInherit, NULL, 0U };
    bava_handle->tx_mutex = osMutexNew(&tx_mutex_attr);
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

// Registering the variables
int8_t bava_register_var(bava_handle_t* bava_handle, uint8_t id, void* variable_pointer, uint8_t variable_size)
{
    if (bava_handle->var_count >= BAVA_MAX_VARIABLES)
    {
       return -1;
    }

    uint8_t index = bava_handle->var_count;
    bava_handle->variables[index].id = id;
    bava_handle->variables[index].var_ptr = variable_pointer;
    bava_handle->variables[index].size = variable_size;
    bava_handle->variables[index].updated = false;

    bava_handle->var_count++;

    return 0;
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

// Internal update function
void bava_internal_update(bava_handle_t* bava_handle, uint8_t id, const uint8_t* payload, uint8_t size)
{
    for (uint8_t i = 0; i < bava_handle->var_count; i++)
    {
        if (bava_handle->variables[i].id == id && bava_handle->variables[i].size == size)
        {
        // 1. Enter Critical Section
        #ifdef ESP_PLATFORM
            portENTER_CRITICAL_ISR(&(bava_handle->rx_mux));
        #elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
            UBaseType_t uxSavedInterruptStatus = taskENTER_CRITICAL_FROM_ISR();
        #elif defined(USE_HAL_DRIVER)
            __disable_irq();
        #elif defined(ARDUINO)
            noInterrupts();
        #else
            if (bava_handle->enter_critical != NULL) bava_handle->enter_critical();
        #endif

            // Safe memcpy to local aligned variables before endianness conversion
            if (size == 2)
            {
                uint16_t temp_val;
                memcpy(&temp_val, payload, sizeof(temp_val));
                temp_val = bava_ntohs(temp_val);
                memcpy(bava_handle->variables[i].var_ptr, &temp_val, sizeof(temp_val));
            }
            else if (size == 4)
            {
                uint32_t temp_val;
                memcpy(&temp_val, payload, sizeof(temp_val));
                temp_val = bava_ntohl(temp_val);
                memcpy(bava_handle->variables[i].var_ptr, &temp_val, sizeof(temp_val));
            }
            else
            {
                memcpy(bava_handle->variables[i].var_ptr, payload, size);
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
        #else
            if (bava_handle->exit_critical != NULL) bava_handle->exit_critical();
        #endif
            return;
        }
    }
}
