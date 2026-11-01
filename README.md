# BAVA Protocol

Point-to-point binary framing protocol and object dictionary for microcontroller-to-microcontroller variable synchronization over asynchronous serial links. Version 2 uses COBS framing with a `0x00` frame delimiter.

[![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-E7352C.svg)](https://components.espressif.com/components/nishanthraj707/bava)
[![Arduino Library Reference](https://img.shields.io/badge/Arduino-Library%20Reference-00979D.svg)](https://www.arduino.cc/reference/en/libraries/bava-protocol/)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Standard: C99](https://img.shields.io/badge/Standard-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)

---

## Contents

1. [Overview](#overview)
2. [Specifications](#specifications)
3. [Installation](#installation)
4. [Quick Start](#quick-start)
5. [Public API](#public-api)
6. [Wire Format](#wire-format)
7. [Protocol Behavior](#protocol-behavior)
8. [Platform Integration](#platform-integration)
9. [Examples](#examples)
10. [Directory Structure](#directory-structure)
11. [Migrating from v1](#migrating-from-v1)
12. [License](#license)

---

## Overview

BAVA binds an 8-bit identifier to a variable in RAM. A node writes a registered variable to its peer, or requests the peer's value, over any byte-oriented serial transport. The receiver validates each frame with a CRC-16 before it modifies memory.

Design properties:

- **Static memory.** All state, the dictionary and the transmit buffer are statically allocated. The library does not call `malloc()`, `calloc()` or `free()`.
- **Direct pointer mapping.** Each identifier maps to a pointer and a size. A validated payload is copied to its destination with `memcpy()` inside a critical section. No text parsing or schema decoding is involved.
- **COBS framing.** Frames are encoded with Consistent Overhead Byte Stuffing and terminated by `0x00`. Encoding overhead is a fixed 2 bytes (one COBS code byte plus the delimiter) for all payloads of 32 bytes or less, independent of payload content.
- **Resynchronization at the delimiter.** After a corrupted or truncated frame, the receiver discards data up to the next `0x00` and resumes at the following frame.
- **Streaming receiver.** `bava_process_byte()` is a non-blocking state machine that accepts one byte per call, so frames split across UART FIFO reads are reassembled correctly.
- **Transaction timeout.** `bava_tick()` expires unacknowledged transactions and releases the transmit lock without blocking.

---

## Specifications

| Parameter | Value |
| :--- | :--- |
| Topology | Point-to-point |
| Framing | COBS, `0x00` delimiter |
| Integrity | CRC-16-CCITT, polynomial `0x1021`, initial value `0xFFFF` |
| Dictionary size | 32 variables (`BAVA_MAX_VARIABLES`, compile-time) |
| Identifier range | `0x00` to `0x1F` |
| Maximum payload | 32 bytes (`BAVA_MAX_PAYLOAD`, compile-time) |
| Frame size on the wire | `N + 7` bytes for payload length `N` |
| Dynamic memory | None |
| Language | C99, compatible with C++ |
| Warning policy | Builds with `-Wall -Wextra -Werror -Wstrict-prototypes` |
| Wire compatibility | Not compatible with v1 (see [Migrating from v1](#migrating-from-v1)) |

---

## Installation

### ESP-IDF

Published in the [Espressif Component Registry](https://components.espressif.com/components/nishanthraj707/bava):

```bash
idf.py add-dependency "nishanthraj707/bava"
```

### Arduino

Published in the [Arduino Library Manager](https://www.arduino.cc/reference/en/libraries/bava-protocol/). In the Arduino IDE, open **Library Manager** and search for `BAVA Protocol`. Manual installation:

```bash
cd ~/Arduino/libraries
git clone https://github.com/NishanthRaj707/bavaprotocol.git BAVA
```

### Zephyr RTOS

Add the repository as a Zephyr module in `west.yml` or your application `CMakeLists.txt`, then enable it in `prj.conf`:

```properties
CONFIG_SERIAL=y
CONFIG_BAVA_PROTOCOL=y
```

### CMake (STM32, NXP, Linux and other targets)

```cmake
add_subdirectory(bavaprotocol)
target_link_libraries(my_firmware PRIVATE bava)
```

Direct GCC build:

```bash
gcc -Wall -Wextra -Werror -Wstrict-prototypes -Iinclude -c src/*.c
```

---

## Quick Start

The example below is an Arduino sketch. Two boards run the same sketch with TX and RX crossed and a common ground. Each board writes its counter to the peer every 50 ms and receives the peer's counter into the same registered variable.

```c
#include <bava.h>

#define TELEMETRY_VAR_ID  0x01   /* valid identifiers: 0x00 to 0x1F */

static bava_handle_t bava_handle;
static uint8_t       sys_telemetry = 0;
static uint32_t      last_tx_ms    = 0;

/* Transmit callback: BAVA passes the fully framed bytes to the transport. */
static void hardware_uart_tx(const uint8_t *data, uint16_t size)
{
    if (data != NULL && size > 0) {
        Serial.write(data, size);
    }
}

/* Error callback: 0x11 = acknowledgement timeout, 0x12 = invalid payload. */
static void bava_error_handler(uint8_t id, uint8_t error_code)
{
    (void)id;
    (void)error_code;
}

void setup(void)
{
    Serial.begin(115200);

    bava_init(&bava_handle, hardware_uart_tx);
    bava_handle.error_callback = bava_error_handler;

    if (bava_register_var(&bava_handle, TELEMETRY_VAR_ID,
                          &sys_telemetry, sizeof(sys_telemetry)) != 0) {
        /* Registration failed: dictionary full or invalid parameters. */
    }
}

void loop(void)
{
    /* 1. Feed received bytes to the parser. */
    while (Serial.available() > 0) {
        bava_process_byte(&bava_handle, (uint8_t)Serial.read());
    }

    /* 2. Consume a value written by the remote node. */
    if (bava_var_updated(&bava_handle, TELEMETRY_VAR_ID)) {
        bava_var_clear_update_status(&bava_handle, TELEMETRY_VAR_ID);
        /* sys_telemetry now holds the validated remote value. */
    }

    /* 3. Service acknowledgement timeouts. Required. */
    bava_tick(&bava_handle, millis());

    /* 4. Periodic, non-blocking transmission. */
    uint32_t now = millis();
    if (now - last_tx_ms >= 50) {
        last_tx_ms = now;
        sys_telemetry++;
        bava_send_write(&bava_handle, TELEMETRY_VAR_ID);
    }
}
```

`bava_tick()` must be called periodically. It expires pending acknowledgements and releases the transmit lock. Without it, a lost acknowledgement leaves the transmitter locked.

For other platforms, replace `Serial.write()` with `uart_write_bytes()` (ESP-IDF) or `uart_poll_out()` in a loop (Zephyr), and replace `millis()` with the platform uptime in milliseconds. `bava_process_byte()` may be called from a UART RX interrupt or from a dedicated task.

---

## Public API

Declared in `include/bava.h`.

### Types and constants

| Name | Description |
| :--- | :--- |
| `bava_handle_t` | Protocol instance. Holds the state machine, dictionary and transmit state. One instance per link. |
| `bava_tx_cb_t` | Transmit callback: `void (*)(const uint8_t *data, uint16_t size)`. |
| `error_callback` | Optional member of `bava_handle_t`: `void (*)(uint8_t id, uint8_t error_code)`. |
| `BAVA_MAX_VARIABLES` | Dictionary capacity. Default 32. |
| `BAVA_MAX_PAYLOAD` | Maximum payload length in bytes. Default 32. |

### Functions

| Function | Signature | Description |
| :--- | :--- | :--- |
| `bava_init` | `void bava_init(bava_handle_t *handle, bava_tx_cb_t tx_cb)` | Initializes the handle, resets the receive state machine and registers the transmit callback. |
| `bava_register_var` | `int8_t bava_register_var(bava_handle_t *handle, uint8_t id, void *ptr, uint8_t size)` | Binds an identifier to a variable pointer and size in bytes. Returns `0` on success, `-1` if the dictionary is full or a parameter is invalid. |
| `bava_process_byte` | `void bava_process_byte(bava_handle_t *handle, uint8_t byte)` | Passes one received byte to the parser. On a complete, valid frame, performs the command: commits a WRITE payload to the registered variable, answers a READ, or processes an acknowledgement. |
| `bava_tick` | `void bava_tick(bava_handle_t *handle, uint32_t system_tick_ms)` | Advances the timeout logic using a millisecond timestamp. Expires pending acknowledgements and releases the transmit lock. |
| `bava_send_write` | `void bava_send_write(bava_handle_t *handle, uint8_t id)` | Reads the registered variable, builds and COBS-encodes a WRITE frame, and passes it to the transmit callback. |
| `bava_send_raw_write` | `void bava_send_raw_write(bava_handle_t *handle, uint8_t id, const uint8_t *ptr, uint8_t len)` | Sends a WRITE frame for a remote identifier from a caller-supplied buffer. No local registration is required. |
| `bava_send_read` | `void bava_send_read(bava_handle_t *handle, uint8_t id)` | Sends a READ frame requesting the remote value of an identifier. The response is committed to the local variable registered under the same identifier. |
| `bava_var_updated` | `bool bava_var_updated(bava_handle_t *handle, uint8_t id)` | Returns `true` if the variable received validated data from the remote node since the flag was last cleared. |
| `bava_var_clear_update_status` | `void bava_var_clear_update_status(bava_handle_t *handle, uint8_t id)` | Clears the update flag for an identifier. |

### Error codes

| Code | Meaning |
| :---: | :--- |
| `0x11` | Acknowledgement timeout. A WRITE was not acknowledged within the timeout. |
| `0x12` | Invalid payload. |

---

## Wire Format

### Frame layout

Each frame is assembled in the following decoded form, COBS-encoded, and followed by a `0x00` delimiter.

| Offset | Field | Size | Description |
| :---: | :--- | :---: | :--- |
| 0 | CMD | 1 byte | `0x01` READ, `0x02` WRITE, `0x81` READ_RESP, `0x82` WRITE_ACK |
| 1 | ID | 1 byte | Variable identifier, `0x00` to `0x1F` |
| 2 | LEN | 1 byte | Payload length `N`, `0` to `32` |
| 3 | PAYLOAD | `N` bytes | Raw data |
| 3 + `N` | CRC_LOW | 1 byte | CRC-16-CCITT, low byte |
| 4 + `N` | CRC_HIGH | 1 byte | CRC-16-CCITT, high byte |

The CRC is computed over CMD, ID, LEN and PAYLOAD, using polynomial `0x1021` and initial value `0xFFFF`.

On the wire:

```text
COBS( CMD | ID | LEN | PAYLOAD | CRC_LOW | CRC_HIGH )  0x00
```

### Frame size

The decoded frame is `N + 5` bytes. COBS adds one code byte for any decoded length up to 254 bytes, and the delimiter adds one more byte.

| Quantity | Value |
| :--- | :--- |
| Decoded frame | `N + 5` bytes |
| Encoded frame (COBS) | `N + 6` bytes |
| Wire frame (with `0x00` delimiter) | `N + 7` bytes |
| Maximum wire frame (`N = 32`) | 39 bytes |

The wire size depends only on `N`. It does not depend on the byte values in the payload.

### Worked examples

WRITE of identifier `0x01` with the one-byte payload `0x2A`:

```text
Decoded:  02 01 01 2A 81 E8
Wire:     07 02 01 01 2A 81 E8 00
```

WRITE of identifier `0x02` with the payload `00 10 00 FF` (contains zero bytes):

```text
Decoded:  02 02 04 00 10 00 FF F8 CE
Wire:     04 02 02 04 02 10 04 FF F8 CE 00
```

READ of identifier `0x01` (no payload):

```text
Decoded:  01 01 00 9D C8
Wire:     03 01 01 03 9D C8 00
```

The CRC values are transmitted low byte first.

### Receive rules

The receiver accumulates bytes until it sees `0x00`, then decodes and validates the frame. A frame is discarded, and no memory is modified, if any of the following holds:

- The accumulated length exceeds the maximum encoded frame length.
- The COBS structure is invalid, for example a code byte that points past the end of the frame.
- The decoded length is not `LEN + 5`, or `LEN` exceeds `BAVA_MAX_PAYLOAD`.
- The CRC does not match.
- The identifier is not registered.

After a discarded frame, the parser resumes with the first byte following the delimiter.

---

## Protocol Behavior

### Commands

| Command | Code | Direction | Behavior |
| :--- | :---: | :--- | :--- |
| READ | `0x01` | Requester to responder | Requests the current value of an identifier. |
| WRITE | `0x02` | Sender to receiver | Carries a value for an identifier. |
| READ_RESP | `0x81` | Responder to requester | Carries the requested value. |
| WRITE_ACK | `0x82` | Receiver to sender | Confirms a WRITE. |

### Transactions

- A WRITE expects a WRITE_ACK. While a transaction is pending, the transmit lock is held. The lock is released when the acknowledgement arrives or when `bava_tick()` detects the timeout, in which case the error callback is invoked with code `0x11`.
- A received WRITE is applied to the registered variable only after the frame passes all receive checks. The variable's update flag is then set.
- The update flag is a boolean. If two writes arrive before the application clears it, the flag does not record that two updates occurred and the variable holds the most recent value.

### Concurrency

The dictionary commit (`memcpy()` plus flag update) executes inside a platform critical section so that an interrupt-context parser and an application-context reader do not observe a partially written value.

---

## Platform Integration

The critical section is implemented per platform:

| Platform | Mechanism |
| :--- | :--- |
| ESP-IDF | `portENTER_CRITICAL_ISR()` / `portEXIT_CRITICAL_ISR()` with a spinlock |
| Zephyr | `irq_lock()` / `irq_unlock()` |
| STM32 with FreeRTOS | `taskENTER_CRITICAL_FROM_ISR()` / `taskEXIT_CRITICAL_FROM_ISR()` |
| STM32 bare metal | `__disable_irq()` / `__enable_irq()` |
| Arduino | `noInterrupts()` / `interrupts()` |
| Other bare metal | User-supplied critical-section callbacks |

Transmission uses the callback passed to `bava_init()`, so any byte-oriented transport that preserves byte order can carry BAVA frames.

---

## Examples

| Platform | Directory | Demonstrates |
| :--- | :--- | :--- |
| ESP-IDF | [`examples/esp_demo/`](examples/esp_demo/) | FreeRTOS UART RX task, spinlock critical sections, `bava_tick()` with `esp_log_timestamp()`, transmission with `uart_write_bytes()` |
| Zephyr | [`examples/zephyr_demo/`](examples/zephyr_demo/) | `uart_poll_in()` / `uart_poll_out()`, `k_uptime_get_32()`, synchronization of a packed struct |
| Arduino | [`examples/arduino_demo/`](examples/arduino_demo/) | `HardwareSerial`, `millis()`-based non-blocking timing with `bava_tick()` |

---

## Directory Structure

```text
bavaprotocol/
├── CMakeLists.txt        Build configuration (Zephyr, ESP-IDF, bare metal)
├── Kconfig               Zephyr configuration options
├── idf_component.yml     Espressif Component Registry manifest
├── library.properties    Arduino Library Manager metadata
├── include/
│   └── bava.h            Public API, types and constants
├── src/
│   ├── bava_crc.c        CRC-16-CCITT
│   ├── bava_cobs.c       COBS encoder and decoder
│   ├── bava_dict.c       Object dictionary: registration and updates
│   ├── bava_state.c      Receive state machine
│   └── bava_tx.c         Frame construction and transmission
├── examples/
│   ├── arduino_demo/
│   ├── esp_demo/
│   └── zephyr_demo/
└── LICENSE               GNU General Public License v3
```

---

## Migrating from v1

v2 changes the framing and is **not wire-compatible** with v1. Both nodes on a link must run the same major version.

| Item | v1 | v2 |
| :--- | :--- | :--- |
| Frame start | `0xAA 0x55` preamble | None |
| Frame end | Length field | `0x00` delimiter |
| Escaping | `0x7D` stuffing, `0xAA` and `0x7D` escaped | COBS |
| Worst-case encoding overhead | Up to one extra byte per frame byte | 1 code byte for `N` up to 254 |
| Resynchronization | Search for `0xAA 0x55` | Search for `0x00` |
| Frame fields, CRC, command codes, API | Unchanged | Unchanged |

The public API is unchanged. Applications recompile against v2 without source changes.

---

## License

Licensed under the GNU General Public License v3.0. See [LICENSE](LICENSE).

Use in products that cannot comply with GPL-3.0 requires separate commercial terms. Contact the maintainer through the [repository](https://github.com/NishanthRaj707/bavaprotocol).