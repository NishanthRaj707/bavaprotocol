#ifndef BAVA_H
#define BAVA_H

#ifdef ESP_PLATFORM
    #include "freertos/FreeRTOS.h"
    #include "freertos/semphr.h"
    #include "freertos/task.h"
#elif defined(USE_HAL_DRIVER) && defined(osCMSIS) 
    #include "cmsis_os.h" 
#elif defined(ARDUINO)
    #include <Arduino.h>
    #include <stdatomic.h>
#else
    #include <stdatomic.h>
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// BAVA FRAME CONSTANTS
#define BAVA_SYNC_BYTE1     0xAA
#define BAVA_SYNC_BYTE2     0x55
#define BAVA_ESCAPE_BYTE    0x7D
#define BAVA_ESCAPE_MASK    0x20

// CONFIGURATION
#define BAVA_MAX_VARIABLES 32  // Maximum number of variables in the BAVA system

#ifndef BAVA_MAX_PAYLOAD
#define BAVA_MAX_PAYLOAD 32 // Maximum payload size for the bava message
#endif

#ifndef BAVA_RX_TIMEOUT
#define BAVA_RX_TIMEOUT 100 // Timeout for receiving data (in ms)
#endif

#ifndef BAVA_TX_TIMEOUT
#define BAVA_TX_TIMEOUT 100 // Timeout for waiting for an acknowledgment (in ms)
#endif

// BAVA COMMANDS
#define BAVA_READ 0x01 // Command to read remote variable 
#define BAVA_READ_RESP 0x81
#define BAVA_WRITE 0x02 // Command to write/update the remote variable
#define BAVA_WRITE_ACK 0x82 // Acknowledgement to the update

// Define standard error codes
#define BAVA_ERR_TIMEOUT 0x11
#define BAVA_ERR_INVALID_PAYLOAD_LENGTH 0x12

// Define the Error Callback function signature
typedef void (*bava_error_cb_t)(uint8_t id, uint8_t error_code);

// BAVA TX CALLBACK ----> user should register the function
typedef void (*bava_tx_cb_t)(const uint8_t* data, uint16_t size);
typedef void (*bava_lock_cb_t)(void);

// BAVA VARIABLE STORAGE STRUCTURE
typedef struct 
{
    uint8_t id;
    void* var_ptr;
    uint8_t size;
    volatile bool updated;
} bava_var_t;

// BAVA MESSAGE STRUCTURE
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
} bava_rx_state_t;

// BAVA Handle structure
typedef struct 
{
    bava_tx_cb_t tx_callback;  // user defined function to transmit data
    bava_var_t variables[BAVA_MAX_VARIABLES]; // Collection of variables stored
    uint8_t var_count;
    // Rx State Machine tracking
    volatile bava_rx_state_t rx_state;
    uint8_t rx_cmd;
    uint8_t rx_id;
    uint8_t rx_len;
    uint8_t rx_idx;                 // Tracks bytes read into the payload buffer
    uint8_t rx_payload[BAVA_MAX_PAYLOAD];

    uint8_t pending_ack_id;      // The ID we just sent
    uint8_t pending_ack_cmd;     // Was it READ or WRITE?
    volatile bool is_waiting_ack; // Blocks new TX until true or timeout

    uint8_t tx_buffer[((BAVA_MAX_PAYLOAD + 5) * 2) + 2];

    // Timeout Tracking
    uint32_t current_time_ms;  // Continuously updated by the user or millis()
    uint32_t tx_timestamp;     // Records the exact time a packet was fired
    uint32_t start_time;       // Records start time for RX frame timeout

    // Error Handling
    bava_error_cb_t error_callback;
    volatile bool is_escaping;
    uint16_t incoming_crc;

#ifdef ESP_PLATFORM
    SemaphoreHandle_t tx_mutex; // For bava_send_write
    portMUX_TYPE rx_mux;        // Spinlock for the RX critical section
#elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
    osMutexId_t tx_mutex;       // STM32 CMSIS-RTOS Mutex
#elif defined(ARDUINO)
    atomic_flag tx_lock;        // Atomic lock for Arduino targets
    void (*yield_callback)(void);
#else
    atomic_flag tx_lock;
    // Allows bare-metal devs to pass a custom delay or watchdog reset
    void (*yield_callback)(void);
    void (*enter_critical)(void); // Manual fallback for bare metal
    void (*exit_critical)(void);
#endif

} bava_handle_t;

// PORTABLE ENDIANNESS HELPERS (Directive 6: No POSIX socket dependence)
static inline uint16_t bava_htons(uint16_t val) {
    return (uint16_t)((val << 8) | (val >> 8));
}

static inline uint32_t bava_htonl(uint32_t val) {
    return ((val << 24) & 0xFF000000UL) |
           ((val <<  8) & 0x00FF0000UL) |
           ((val >>  8) & 0x0000FF00UL) |
           ((val >> 24) & 0x000000FFUL);
}

#define bava_ntohs(x) bava_htons(x)
#define bava_ntohl(x) bava_htonl(x)

// PUBLIC API
void bava_tick(bava_handle_t* bava_handle, uint32_t system_tick_ms);
void bava_init(bava_handle_t* bava_handle, bava_tx_cb_t bava_tx_callback);
int8_t bava_register_var(bava_handle_t* bava_handle, uint8_t id, void* variable_pointer, uint8_t variable_size);
void bava_process_byte(bava_handle_t *bava_handle, uint8_t byte);
void bava_send_write(bava_handle_t* bava_handle, uint8_t id);
void bava_send_raw_write(bava_handle_t* bava_handle, uint8_t id, const uint8_t* pointer, uint8_t len);
void bava_send_read(bava_handle_t* bava_handle, uint8_t id);
bool bava_var_updated(bava_handle_t* bava_handle, uint8_t id);
void bava_var_clear_update_status(bava_handle_t* bava_handle, uint8_t id);

#endif // BAVA_H