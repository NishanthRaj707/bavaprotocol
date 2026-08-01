#ifndef BAVA_H
#define BAVA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

//CONFIGURATION
#define BAVA_MAX_VARIABLES 32  //Maximum number of variables in the BAVA system
#define BAVA_MAX_PAYLOAD 255 //Maximum payload size for the bava message

//BAVA COMMANDS
#define BAVA_READ 0X01 //Command to read remote variable 
#define BAVA_READ_RESP 0x81
#define BAVA_WRITE 0X02 //Command to write/update the remote variable
#define BAVA_WRITE_ACK 0x82 //Acknowledgement to the updation

// Define standard error codes
#define BAVA_ERR_TIMEOUT 0x01

// Define the Error Callback function signature
typedef void (*bava_error_cb_t)(uint8_t id, uint8_t error_code);

//BAVA TX CALLBACK ----> user should register the function
typedef void (*bava_tx_cb_t)(uint8_t* data,uint16_t size);
typedef void (*bava_lock_cb_t)(void);

//BAVA VARIABLE STORAGE STRUCTURE
typedef struct 
{
    uint8_t id;
    void* var_ptr;
    uint8_t size;
    volatile bool updated;
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
    volatile bava_rx_state_t rx_state;
    uint8_t rx_cmd;
    uint8_t rx_id;
    uint8_t rx_len;
    uint8_t rx_idx;                 // Tracks bytes read into the payload buffer
    uint8_t rx_payload[BAVA_MAX_PAYLOAD];

    // Inside bava_handle_t
    uint8_t pending_ack_id;      // The ID we just sent
    uint8_t pending_ack_cmd;     // Was it READ or WRITE?
    volatile bool is_waiting_ack; // Blocks new TX until true or timeout

    uint8_t tx_buffer[550];

    //ISR SYNC
    bava_lock_cb_t enter_critical;
    bava_lock_cb_t exit_critical;

    // Timeout Tracking
    uint32_t current_time_ms;  // Continuously updated by the user
    uint32_t tx_timestamp;     // Records the exact time a packet was fired
    uint32_t tx_timeout_ms;    // Max wait time (e.g., 50ms)
    
    // Error Handling
    bava_error_cb_t error_callback;
    volatile bool is_escaping;
    uint16_t incoming_crc;

}bava_handle_t;



//PUBLIC API

//System tick
void bava_tick(bava_handle_t* bava_handle,uint32_t system_tick_ms);

//Initalise the Bava system
void bava_init(bava_handle_t* bava_handle,bava_tx_cb_t bava_tx_callback);

//Register the variable
int8_t bava_register_var(bava_handle_t* bava_handle,uint8_t id,void* variable_pointer,uint8_t variable_size);

//Process received bytes
void bava_process_byte(bava_handle_t *bava_handle,uint8_t byte);

//Send data 
void bava_send_write(bava_handle_t* bava_handle,uint8_t id);

void bava_send_raw_write(bava_handle_t* bava_handle,uint8_t id,const uint8_t* pointer,uint8_t len);

//Send a read request 
void bava_send_read(bava_handle_t* bava_handle,uint8_t id);

//Check the updated status
bool bava_var_updated(bava_handle_t* bava_handle,uint8_t id);

//Clear the updation status
void bava_var_clear_update_status(bava_handle_t* bava_handle,uint8_t id);


#endif