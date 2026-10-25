# BAVA Protocol

BAVA is a zero-allocation, point-to-point binary framing protocol and object dictionary for microcontroller-to-microcontroller telemetry. It provides deterministic, constant-time variable synchronization over asynchronous serial links without dynamic heap allocation.

[![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-E7352C.svg)](https://components.espressif.com/components/nishanthraj707/bava/versions/1.0.1/readme?language=en)
[![Arduino Library Reference](https://img.shields.io/badge/Arduino-Library%20Reference-00979D.svg)](https://www.arduino.cc/reference/en/libraries/bava-protocol/)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Standards: C99](https://img.shields.io/badge/Standard-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![Documentation: Technical Report](https://img.shields.io/badge/Docs-Technical%20Report-blue.svg)](report.md)
[![Documentation: Performance Data](https://img.shields.io/badge/Docs-Performance%20Data-brightgreen.svg)](performance.md)

---

## Core Architecture

- Zero Dynamic Memory Allocation: All state machines, dictionaries, and transmission buffers are statically declared. The protocol performs no calls to malloc(), calloc(), or free().
- O(1) Direct Pointer Mapping: Binds 8-bit identifiers directly to memory pointers. Received payloads are committed to destination addresses via memcpy() inside hardware critical sections, eliminating string parsing and schema decoding.
- Deterministic Stream Deframing: Ingests serial bytes one at a time via a non-blocking finite state machine (bava_process_byte). Uses 0xAA 0x55 sync headers and 0x7D byte-stuffing to maintain frame synchronization across UART FIFO boundaries.
- 16-Bit CRC Data Integrity: Verifies all incoming frames with a CRC-16-CCITT checksum before memory write, discarding corrupted frames at the parser layer.
- Non-Blocking Timeout Watchdog: Tracks unacknowledged transactions via bava_tick without stalling thread execution or blocking ISRs.

---

## Technical Specifications

| Parameter | Specification | Notes |
| :--- | :--- | :--- |
| Topology | Point-to-Point (P2P) | Dual MCU direct link (e.g., STM32 to ESP32) |
| Language Standard | ANSI C99 / C++ Compatible | Builds with -Wall -Wextra -Werror -Wstrict-prototypes |
| Heap Consumption | 0 Bytes | Static memory allocation |
| Stack High-Water Mark | 96 Bytes | Measured on FreeRTOS task stacks |
| RX Parse Latency | 73.1 us - 80.2 us | Constant-time O(1) scaling |
| TX Framing Latency | 64.56 us - 84.0 us | Includes CRC-16 calculation and byte-stuffing |
| Data Integrity Verification | CRC-16-CCITT (0x1021) | Initial value 0xFFFF |
| Preamble Sequence | 0xAA 0x55 | 2-byte frame delimiter |
| Byte-Stuffing Mechanism | 0x7D with XOR 0x20 | Escapes 0xAA and 0x7D bytes |
| Dictionary Capacity | 32 Variables (BAVA_MAX_VARIABLES) | Compile-time constant |
| Payload Capacity | 32 Bytes (BAVA_MAX_PAYLOAD) | Compile-time constant |

Empirical benchmarks and hardware testbed logs are detailed in [performance.md](performance.md). The architectural specification is documented in [report.md](report.md).

---

## Quick Start Guide

The example below demonstrates complete point-to-point integration modeled directly on the working implementations in [examples/arduino_demo/](examples/arduino_demo/), [examples/esp_demo/](examples/esp_demo/), and [examples/zephyr_demo/](examples/zephyr_demo/).

### Embedded C / Arduino Example

```c
#include "bava.h"

// 1. Define unique Object Dictionary variable identifiers (0 to 31)
#define TELEMETRY_VAR_ID  0x01

// 2. Global protocol instance and bound application variable
static bava_handle_t bava_handle;
static uint8_t sys_telemetry = 1;
static uint32_t last_tx_ms = 0;

// 3. Hardware transmit callback: routes framed bytes to the physical UART
static void hardware_uart_tx(const uint8_t* data, uint16_t size) {
    if (data != NULL && size > 0) {
        // Platform transmit routine
        // Arduino: Serial.write(data, size);
        // ESP-IDF: uart_write_bytes(UART_NUM_1, (const char*)data, size);
        // Zephyr:  for (uint16_t i = 0; i < size; i++) uart_poll_out(dev, data[i]);
        for (uint16_t i = 0; i < size; i++) {
            putchar(data[i]);
        }
    }
}

// 4. Optional error callback for timeout and framing alerts
static void bava_error_handler(uint8_t id, uint8_t error_code) {
    // Handle unacknowledged transaction timeout (0x11) or invalid payload (0x12)
    (void)id;
    (void)error_code;
}

void setup(void) {
    // Initialize hardware serial interface (e.g., 115200 8-N-1)
    
    // Initialize BAVA handle with outbound TX callback
    bava_init(&bava_handle, hardware_uart_tx);
    bava_handle.error_callback = bava_error_handler;

    // Register application variable into the Object Dictionary
    int8_t status = bava_register_var(&bava_handle, TELEMETRY_VAR_ID, &sys_telemetry, sizeof(sys_telemetry));
    if (status != 0) {
        // Registration failed: dictionary full or invalid parameters
    }
}

void loop(void) {
    // Step 1: Stream incoming UART bytes into BAVA state machine
    // (Can also be called inside UART RX interrupt or dedicated RTOS task)
    while (uart_has_bytes()) {
        uint8_t incoming_byte = uart_read_byte();
        bava_process_byte(&bava_handle, incoming_byte);
    }

    // Step 2: Check if registered variable was updated by a remote WRITE command
    if (bava_var_updated(&bava_handle, TELEMETRY_VAR_ID)) {
        bava_var_clear_update_status(&bava_handle, TELEMETRY_VAR_ID);
        // sys_telemetry now contains the verified payload committed by memcpy
    }

    // Step 3: Advance BAVA protocol timer tick (CRITICAL)
    // Feeds system uptime in milliseconds to service ACK timeouts and release TX lock
    bava_tick(&bava_handle, get_system_uptime_ms());

    // Step 4: Non-blocking periodic transmission (e.g., 20 Hz / every 50 ms)
    uint32_t current_ms = get_system_uptime_ms();
    if (current_ms - last_tx_ms >= 50) {
        last_tx_ms = current_ms;

        // Update local state and dispatch over the wire
        sys_telemetry++;
        bava_send_write(&bava_handle, TELEMETRY_VAR_ID);
    }
}
```

> Note: Calling `bava_tick(&bava_handle, system_tick_ms)` in your main loop or periodic task is required to service ACK timeouts and prevent the transmitter from locking.

---

## API Reference

| API | Signature | Functionality |
| :--- | :--- | :--- |
| `bava_init` | `void bava_init(bava_handle_t* handle, bava_tx_cb_t tx_cb)` | Initializes the handle context, resets the state machine, and registers the outbound hardware TX callback. |
| `bava_register_var` | `int8_t bava_register_var(bava_handle_t* handle, uint8_t id, void* ptr, uint8_t size)` | Binds an 8-bit identifier (0 to 31) directly to a local variable pointer and byte size. Returns 0 on success, -1 if dictionary is full. |
| `bava_process_byte` | `void bava_process_byte(bava_handle_t* handle, uint8_t byte)` | Feeds an incoming UART byte into the non-blocking FSM parser, handles byte-destuffing, verifies CRC-16, and commits payload to memory. |
| `bava_tick` | `void bava_tick(bava_handle_t* handle, uint32_t system_tick_ms)` | Drives the internal timeout watchdog using millisecond timestamps. Handles ACK timeout expiration and releases the TX lock. |
| `bava_send_write` | `void bava_send_write(bava_handle_t* handle, uint8_t id)` | Reads registered variable value from memory, encodes frame with CRC-16 and byte-stuffing, and transmits via TX callback. |
| `bava_send_raw_write` | `void bava_send_raw_write(bava_handle_t* handle, uint8_t id, const uint8_t* ptr, uint8_t len)` | Transmits raw payload buffer data to a remote variable ID without requiring prior local registration. |
| `bava_send_read` | `void bava_send_read(bava_handle_t* handle, uint8_t id)` | Transmits a READ request frame asking the remote node to return its current value for the specified variable ID. |
| `bava_var_updated` | `bool bava_var_updated(bava_handle_t* handle, uint8_t id)` | Returns true if the registered variable received new validated data from a remote write since the last status clear. |
| `bava_var_clear_update_status` | `void bava_var_clear_update_status(bava_handle_t* handle, uint8_t id)` | Resets the update flag for a registered variable ID after the application processes the new value. |

---

## Platform Examples

Complete example projects are available in the [examples/](examples/) directory:

- [ESP-IDF Demo (examples/esp_demo/)](examples/esp_demo/): FreeRTOS integration demonstrating a dedicated UART RX task, hardware spinlocks (taskENTER_CRITICAL), non-blocking tick management with esp_log_timestamp(), and periodic transmission via uart_write_bytes.
- [Zephyr RTOS Demo (examples/zephyr_demo/)](examples/zephyr_demo/): Native Zephyr module integration showing uart_poll_in / uart_poll_out hardware polling, k_uptime_get_32() timing, and multi-variable packed struct synchronization.
- [Arduino Framework Demo (examples/arduino_demo/)](examples/arduino_demo/): Arduino sketch utilizing HardwareSerial (Serial.write), non-blocking millis() timing with bava_tick, and atomic synchronization.

---

## Building and Installation

### Standalone CMake / Embedded Targets (STM32, NXP, Linux)

Add the repository directory to your build system:

```cmake
add_subdirectory(bavaprotocol)
target_link_libraries(my_firmware PRIVATE bava)
```

Direct GCC build:
```bash
gcc -Wall -Wextra -Werror -Wstrict-prototypes -Iinclude -c src/*.c
```

### Espressif ESP-IDF

BAVA is published in the [Espressif Component Registry](https://components.espressif.com/components/nishanthraj707/bava/versions/1.0.1/readme?language=en):

```bash
idf.py add-dependency "nishanthraj707/bava"
```

### Zephyr RTOS

Add BAVA as a Zephyr module via west.yml or your application CMakeLists.txt, then enable it in prj.conf:

```properties
CONFIG_SERIAL=y
CONFIG_BAVA_PROTOCOL=y
```

### Arduino IDE

Search for "Bava Protocol" in the Arduino Library Manager, or clone into your local Arduino libraries directory:

```bash
cd ~/Arduino/libraries
git clone https://github.com/NishanthRaj707/bavaprotocol.git BAVA
```

---

## Directory Structure

```text
bavaprotocol/
├── CMakeLists.txt              Unified build configuration (Zephyr, ESP-IDF, Bare-Metal)
├── Kconfig                     Kernel configuration for Zephyr RTOS
├── idf_component.yml           Espressif Component Registry manifest
├── library.properties          Arduino Library Manager metadata
├── include/
│   └── bava.h                  Public API declarations, structures, constants
├── src/
│   ├── bava_crc.c              CRC-16-CCITT implementation
│   ├── bava_dict.c             Object dictionary storage, registration, updates
│   ├── bava_state.c            Non-blocking finite state machine parser
│   └── bava_tx.c               Frame construction, byte-stuffing, dispatch
├── examples/
│   ├── arduino_demo/           Arduino sketch implementation
│   ├── esp_demo/               ESP-IDF FreeRTOS component demo
│   └── zephyr_demo/            Zephyr RTOS module application demo
├── performance.md              Empirical benchmarks and hardware testbed data
├── report.md                   Technical specification and empirical analysis
└── LICENSE                     GNU General Public License Version 3
```

---

## Frame Structure

BAVA Point-to-Point frames enforce 0x7D byte-stuffing across the command, identifier, length, payload, and CRC bytes.

| Offset | Field | Size | Description |
| :---: | :--- | :---: | :--- |
| 0 | SYNC1 | 1 Byte | Frame delimiter (0xAA) |
| 1 | SYNC2 | 1 Byte | Frame delimiter (0x55) |
| 2 | CMD | 1 Byte | Command: 0x01 (READ), 0x02 (WRITE), 0x81 (READ_RESP), 0x82 (WRITE_ACK) |
| 3 | ID | 1 Byte | Variable dictionary identifier (0x00 - 0x1F) |
| 4 | LEN | 1 Byte | Payload byte count (0 <= N <= 32) |
| 5 .. 5+N-1 | PAYLOAD | N Bytes | Raw binary data payload |
| 5+N | CRC_LOW | 1 Byte | CRC-16-CCITT low byte |
| 5+N+1 | CRC_HIGH | 1 Byte | CRC-16-CCITT high byte |

Total unescaped frame size: 7 + N bytes.

---

## Empirical Benchmark Summary

Hardware tests were conducted over a 115,200 baud UART link between an STM32F103 (72 MHz) and an ESP32 (240 MHz) using polled I/O:

| Metric | Raw ASCII UART (sscanf) | BAVA Protocol | Delta / Impact |
| :--- | :--- | :--- | :--- |
| RX Parse Latency | 135.5 us - 251.0 us | 73.1 us - 80.2 us | Up to 68% latency reduction; O(1) constant time |
| TX Framing Latency | 33.0 us (snprintf, unverified) | 84.0 us (CRC-16 Framing) | +51.0 us for byte-stuffing and CRC protection |
| Packet Delivery Rate | ~50.0% (FIFO clipping dropouts) | 100.00% (0 errors) | Preambles prevent delimiter fragmentation |
| Dynamic Heap Usage | Variable | 0 Bytes | Zero calls to dynamic allocation functions |
| Stack Consumption | Variable | 96 Bytes | Bounded local execution frame |

See [performance.md](performance.md) for full testbed data, and [report.md](report.md) for architectural analysis.

---

## Frequently Asked Questions (FAQ)

### Why does ASCII UART communication exhibit data loss on embedded systems?
Microcontroller UART hardware FIFOs transfer data in blocks (e.g., 8 to 64 bytes). When text protocols use a single delimiter like \n, an interrupt or polling read that catches an incomplete FIFO block passes truncated tokens to sscanf() or strtod(). These functions fail to parse the partial line and return zero values. BAVA uses a sequential state machine with sync preambles (0xAA 0x55) and explicit payload length fields, reconstructing frames across fragmented FIFO boundaries without loss.

### How are concurrent ISR reads and application writes prevented from corrupting data?
During dictionary updates, bava_internal_update() locks memory using platform-specific primitives:
- ESP-IDF: portENTER_CRITICAL_ISR() / portEXIT_CRITICAL_ISR() with spinlocks.
- Zephyr RTOS: irq_lock() / irq_unlock().
- STM32 CMSIS: taskENTER_CRITICAL_FROM_ISR() / __disable_irq().
- Arduino: noInterrupts() / interrupts().
- Bare-metal: Atomic flags with user-configurable critical section callbacks.

---

## Roadmap

- Consistent Overhead Byte Stuffing (COBS): Replace 0x7D byte-stuffing with COBS to bound worst-case framing overhead to ceil(N/254) + 1 bytes.
- Multi-Drop Bus Topology: Add node addressing and collision-free broadcast primitives for RS-485 networks.
- Hardware CRC Accelerators: Provide build-time configuration to offload CRC-16 computation to on-chip CRC peripherals on STM32 and ESP32 hardware.

