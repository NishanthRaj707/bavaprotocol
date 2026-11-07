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
#elif defined(__ZEPHYR__)
    #include <zephyr/kernel.h>
    #include <stdatomic.h>
#else
    #include <stdatomic.h>
#endif

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// CROSS-PLATFORM INTERRUPT CONTROL MACROS
#if defined(ESP_PLATFORM)
    #define BAVA_DISABLE_IRQ() portENTER_CRITICAL()
    #define BAVA_ENABLE_IRQ()  portEXIT_CRITICAL()
#elif defined(__ZEPHYR__)
    extern unsigned int bava_zephyr_irq_key;
    #define BAVA_DISABLE_IRQ() do { bava_zephyr_irq_key = irq_lock(); } while(0)
    #define BAVA_ENABLE_IRQ()  do { irq_unlock(bava_zephyr_irq_key); } while(0)
#elif defined(ARDUINO)
    #if defined(__AVR__)
        #define BAVA_DISABLE_IRQ() cli()
        #define BAVA_ENABLE_IRQ()  sei()
    #else
        #define BAVA_DISABLE_IRQ() noInterrupts()
        #define BAVA_ENABLE_IRQ()  interrupts()
    #endif
#elif defined(__AVR__)
    #include <avr/interrupt.h>
    #define BAVA_DISABLE_IRQ() cli()
    #define BAVA_ENABLE_IRQ()  sei()
#elif defined(USE_HAL_DRIVER) || defined(__ARM_ARCH) || defined(__arm__) || defined(osCMSIS)
    #define BAVA_DISABLE_IRQ() __disable_irq()
    #define BAVA_ENABLE_IRQ()  __enable_irq()
#else
    #define BAVA_DISABLE_IRQ() do { } while(0)
    #define BAVA_ENABLE_IRQ()  do { } while(0)
#endif

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


// BAVA BUS COMMANDS
#define BAVA_BUS_READ 0x11
#define BAVA_BUS_WRITE 0x12
#define BAVA_BUS_READ_RESP 0x71
#define BAVA_BUS_WRITE_ACK 0x72
#define BAVA_BROADCAST_NODE_ID 0xFF

// BAVA STATUS / ERROR CODES
typedef enum
{
    BAVA_STATUS_OK                         = 0x00,
    BAVA_STATUS_ERR_RO                     = 0x01,
    BAVA_STATUS_ERR_SIZE                   = 0x02,
    BAVA_STATUS_ERR_NOT_FOUND              = 0x03,
    BAVA_STATUS_ERR_BUSY                   = 0x04,
    BAVA_STATUS_ERR_TIMEOUT                = 0x11,
    BAVA_STATUS_ERR_INVALID_PAYLOAD_LENGTH = 0x12
} bava_status_t;

#define BAVA_ERR_TIMEOUT                BAVA_STATUS_ERR_TIMEOUT
#define BAVA_ERR_INVALID_PAYLOAD_LENGTH BAVA_STATUS_ERR_INVALID_PAYLOAD_LENGTH

// PERMISSION FLAGS
#define BAVA_PERM_READ_ONLY   0x01U
#define BAVA_PERM_READ_WRITE  0x02U

// Define the Error Callback function signature
typedef void (*bava_error_cb_t)(uint8_t id, bava_status_t error);

// BAVA TX CALLBACK user registers function to transmit framed bytes over any transport (UART, SPI, Wi-Fi, BLE, etc.)
typedef void (*bava_tx_cb_t)(const uint8_t* data, uint16_t size);
typedef void (*bava_bus_tx_cb_t)(const uint8_t* data, uint16_t size);
typedef void (*bava_lock_cb_t)(void);

// BAVA VARIABLE STORAGE STRUCTURE
typedef struct 
{
    uint8_t id;
    void* var_ptr;
    uint8_t size;
    uint8_t permissions;
    volatile bool updated;
} bava_dict_entry_t;

typedef bava_dict_entry_t bava_var_t;

// BAVA MESSAGE STRUCTURE
#define BAVA_RAW_FRAME_MAX_SIZE   (BAVA_MAX_PAYLOAD + 7U)
#define BAVA_COBS_FRAME_MAX_SIZE  (BAVA_RAW_FRAME_MAX_SIZE + 2U)

typedef enum
{
    BAVA_STATE_WAIT_ZERO = 0,
    BAVA_STATE_WAIT_OVERHEAD,
    BAVA_STATE_READ_CMD,
    BAVA_STATE_READ_NODE_ID,
    BAVA_STATE_READ_SENDER_ID,
    BAVA_STATE_READ_ID,
    BAVA_STATE_READ_LEN,
    BAVA_STATE_READ_PAYLOAD,
    BAVA_STATE_READ_CRC1,
    BAVA_STATE_READ_CRC2,
    BAVA_STATE_VERIFY_DELIMITER
} bava_rx_state_t;

// BAVA Handle structure
typedef struct 
{
    bava_tx_cb_t tx_callback;  // user defined function to transmit data
    bava_bus_tx_cb_t bava_bus_tx_callback; //user defined function to transmit data in the bus network
    bava_dict_entry_t variables[BAVA_MAX_VARIABLES]; // Collection of variables stored
    uint8_t node_id;
    uint8_t var_count;
    bool is_bava_bus;
    // Rx State Machine tracking
    volatile bava_rx_state_t rx_state;
    uint8_t rx_cmd;
    uint8_t rx_node_id;
    uint8_t rx_sender_id;
    uint8_t rx_id;
    uint8_t rx_len;
    uint8_t rx_idx;                 // Tracks bytes read into the payload buffer
    uint8_t cobs_distance;
    uint8_t rx_payload[BAVA_MAX_PAYLOAD];

    volatile uint8_t waiting_ack_id;       // The outbound request ID waiting for an ACK
    volatile uint8_t waiting_ack_cmd;     
    volatile bool is_waiting_ack; // Blocks new TX until true or timeout

    // Asynchronous Polling Dispatch (fixes ISR deadlock)
    volatile bool pending_ack_tx;
    volatile uint8_t pending_ack_id;
    volatile bava_status_t pending_ack_status;
    volatile bool pending_read_resp_tx;
    volatile uint8_t pending_read_resp_id;

    uint8_t tx_buffer[BAVA_RAW_FRAME_MAX_SIZE];
    uint8_t cobs_buffer[BAVA_COBS_FRAME_MAX_SIZE];

    // Timeout Tracking
    volatile uint32_t current_time_ms;  // Continuously updated by the user or millis()
    volatile uint32_t tx_timestamp;     // Records the exact time a packet was fired
    volatile uint32_t start_time;       // Records start time for RX frame timeout

    // Error Handling
    bava_error_cb_t error_callback;
    uint16_t incoming_crc;
    uint16_t current_crc;
    

#ifdef ESP_PLATFORM
    SemaphoreHandle_t tx_mutex; // For bava_send_write
    portMUX_TYPE rx_mux;        // Spinlock for the RX critical section
#elif defined(USE_HAL_DRIVER) && defined(osCMSIS)
    osMutexId_t tx_mutex;       // STM32 CMSIS-RTOS Mutex
#elif defined(__ZEPHYR__)
    struct k_mutex tx_mutex;    // Zephyr RTOS Mutex
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
bool bava_register_var(bava_handle_t* bava_handle, uint8_t id, void* variable_pointer, uint8_t variable_size, uint8_t permissions);
void bava_set_error_callback(bava_handle_t* bava_handle, bava_error_cb_t error_callback);

// Non-blocking stream byte parser: feed incoming bytes from any transport (UART, SPI, Wi-Fi, BLE, etc.)
void bava_process_byte(bava_handle_t *bava_handle, uint8_t byte);
void bava_poll(bava_handle_t* bava_handle);
bool bava_read_var(bava_handle_t* bava_handle, uint8_t id, void* dest_buf, uint8_t size);
bava_status_t bava_send_write(bava_handle_t* bava_handle, uint8_t id);
bava_status_t bava_send_raw_write(bava_handle_t* bava_handle, uint8_t id, const uint8_t* pointer, uint8_t len);
bava_status_t bava_send_read(bava_handle_t* bava_handle, uint8_t id);
bool bava_var_updated(bava_handle_t* bava_handle, uint8_t id);
void bava_var_clear_update_status(bava_handle_t* bava_handle, uint8_t id);

// Bus related functions
void bava_bus_init(bava_handle_t* bava_handle, bava_bus_tx_cb_t bava_bus_tx_callback, uint8_t node_id);
bava_status_t bava_bus_send_read(bava_handle_t* bava_handle, uint8_t id, uint8_t receiver_id);
bava_status_t bava_bus_send_write(bava_handle_t* bava_handle, uint8_t id, uint8_t receiver_id);
bava_status_t bava_bus_send_raw_write(bava_handle_t* bava_handle, uint8_t id, const uint8_t* pointer, uint8_t len, uint8_t receiver_id);
bava_status_t bava_bus_broadcast_write(bava_handle_t* bava_handle, uint8_t id);
bava_status_t bava_bus_broadcast_raw_write(bava_handle_t* bava_handle, uint8_t id, const uint8_t* pointer, uint8_t len);

// Checksum functions
uint16_t bava_crc_update(uint16_t crc, uint8_t data);
uint16_t bava_calculate_crc(uint8_t cmd, uint8_t id, uint8_t len, const uint8_t *payload);
uint16_t bava_calculate_bus_crc(uint8_t cmd, uint8_t target_id, uint8_t sender_id, uint8_t id, uint8_t len, const uint8_t *payload);

// Internal variable update
bava_status_t bava_internal_update(bava_handle_t* bava_handle, uint8_t id, const uint8_t* payload, uint8_t size);

// Command / ACK responses
void bava_cmd_write_ack(bava_handle_t* bava_handle, uint8_t id, bava_status_t status);
void bava_cmd_read_request(bava_handle_t* bava_handle, uint8_t id);
void bava_bus_cmd_write_ack(bava_handle_t* bava_handle, uint8_t id, uint8_t receiver_id);
void bava_bus_cmd_read_request(bava_handle_t* bava_handle, uint8_t id, uint8_t receiver_id);

#endif // BAVA_H