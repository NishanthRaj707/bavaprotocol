#ifndef BAVA_H
#define BAVA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

//CONFIGURATION
#define BAVA_MAX_VARIABLES 32  //Maximum number of variables in the BAVA system
#define BAVA_MAX_PAYLOAD 255 //Maximum payload size for the bava message

//BAVA COMMANDS
#define BAVA_READ_REQ 0X01 //Command to read remote variable 
#define BAVA_READ_RESP 0x81
#define BAVA_WRITE 0X02 //Command to write/update the remote variable
#define BAVA_WRITE_ACK 0x82 //Acknowledgement to the updation

//BAVA TX CALLBACK ----> user should register the function
typedef void (*bava_tx_cb_t)(uint8_t data,uint16_t size);

//BAVA VARIABLE STORAGE STRUCTURE
typedef struct 
{
    uint8_t id;
    void* var_ptr;
    uint8_t size;
    bool updated;
}bava_var_t;


//BAVA MESSAGE STRUCTURE
typedef enum
{
    BAVA_STATE_WAIT_SYNC1,
    BAVA_STATE_WAIT_SYNC2,
    BAVA_STATE_READ_CMD,
    BAVA_STATE_READ_ID,
    BAVA_STATE_READ_LEN,
    BAVA_STATE_READ_PAYLOAD,
    BAVA_STATE_READ_CRC1,
    BAVA_STATE_READ_CRC2
}bava_rx_state_t;

//BAVA Handle structure
typedef struct 
{
    bava_tx_cb_t tx_callback;  //user defined function to transmit data
    bava_var_t variables[BAVA_MAX_VARIABLES]; //Collection of variables stored
    uint8_t var_count;
    // Rx State Machine tracking
    bava_rx_state_t rx_state;
    uint8_t rx_cmd;
    uint8_t rx_id;
    uint8_t rx_len;
    uint8_t rx_idx;                 // Tracks bytes read into the payload buffer
    uint8_t rx_payload[BAVA_MAX_PAYLOAD];

    // Inside bava_ctx_t
    uint8_t pending_ack_id;      // The ID we just sent
    uint8_t pending_ack_cmd;     // Was it READ or WRITE?
    uint32_t tx_timeout_ms;      // System tick count when we sent it
    bool is_waiting_ack;         // Blocks new TX until true or timeout
    
    bool is_escaping;

}bava_handle_t;

//PUBLIC API

//Initalise the Bava system
void bava_init(bava_handle_t* bava_handle,bava_tx_cb_t bava_tx_callback);

//Register the variable
int8_t bava_register_var(bava_handle_t* bava_handle,uint8_t id,void* variable_pointer,uint8_t variable_size);

//Process received bytes
void bava_process_byte(bava_handle_t *bava_handle,uint8_t byte);

//Send data 
void bava_send_write(bava_handle_t* bava_handle,uint8_t id);

//Send a read request 
void bava_send_read(bava_handle_t* bava_handle,uint8_t id);

//Check the updated status
bool bava_var_updated(bava_handle_t* bava_handle,uint8_t id);

//Clear the updation status
void bava_var_clear_update_status(bava_handle_t* bava_handle,uint8_t id);


#endif