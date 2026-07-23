#include "bava.h"
#include <string.h>

//Initialization function
void bava_init(bava_handle_t* bava_handle,bava_tx_cb_t bava_tx_callback)
{
    bava_handle->tx_callback = bava_tx_callback;
    bava_handle->var_count=0;

    bava_handle->rx_state = BAVA_STATE_WAIT_SYNC1;
    bava_handle->rx_idx = 0;
    bava_handle->rx_cmd = 0;
    bava_handle->rx_id = 0;
    bava_handle->rx_len = 0;
        
    bava_handle->pending_ack_id=0;
    bava_handle->pending_ack_cmd=0;
    bava_handle->tx_timeout_ms = 0;
    bava_handle->is_escaping = false;
    bava_handle->is_waiting_ack = false;

}

//Registering the variables
int8_t bava_register_var(bava_handle_t* bava_handle,uint8_t id,void* variable_pointer,uint8_t variable_size)
{
    if(bava_handle->var_count>=BAVA_MAX_VARIABLES)
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

//Check the updated status
bool bava_var_updated(bava_handle_t* bava_handle,uint8_t id)
{
    for(uint8_t i=0;i<=bava_handle->var_count;i++)
    {
        if(bava_handle->variables[i].id == id)
        {
            return bava_handle->variables[i].updated;  
        }
    }
    return false;
}

//Clear the update status
void bava_var_clear_update_status(bava_handle_t* bava_handle,uint8_t id)
{
    for(uint8_t i = 0;i<=bava_handle->var_count;i++)
    {
        if(bava_handle->variables[i].id == id)
        {
            bava_handle->variables[i].updated = false;
            return;
        }
    }
}

//Internal update function
void bava_internal_update(bava_handle_t* bava_handle,uint8_t id,uint8_t* payload,uint8_t size)
{
    for(uint8_t i = 0;i<=bava_handle->var_count;i++)
    {
        if(bava_handle->variables[i].id == id && bava_handle->variables[i].size == payload)
        {
            memcpy(bava_handle->variables[i].var_ptr,payload,size);

            bava_handle->variables[i].updated = true;

            return;
        }
    }
}


