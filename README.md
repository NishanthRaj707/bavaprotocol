# BAVA Protocol

A small, static-memory C99 validation library that validates every frame on a UART link, and every write it carries, before anything is committed to application memory.

BAVA enables variable exchange between microcontrollers over asynchronous serial byte streams, ensuring no byte reaches application memory unless five sequential validation gates pass. Rather than introducing a new wire format, BAVA integrates established primitives—Consistent Overhead Byte Stuffing (COBS), CRC-16/CCITT-FALSE, and an in-memory ID-to-pointer dictionary—into a deterministic, fail-closed commit pipeline.

[![Espressif Component Registry](https://img.shields.io/badge/Espressif-Component%20Registry-E7352C.svg)](https://components.espressif.com/components/nishanthraj707/bava)
[![Arduino Library Reference](https://img.shields.io/badge/Arduino-Library%20Reference-00979D.svg)](https://www.arduino.cc/reference/en/libraries/bava-protocol/)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Standard: C99](https://img.shields.io/badge/Standard-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)

---

## Contents

1. [Overview](#overview)
2. [The Five Validation Gates](#the-five-validation-gates)
3. [Core Invariants](#core-invariants)
4. [Scope & Explicit Non-Goals](#scope--explicit-non-goals)
5. [Hardware Verification](#hardware-verification)
6. [Specifications](#specifications)
7. [Installation](#installation)
8. [Quick Start](#quick-start)
9. [Public API](#public-api)
10. [Wire Format](#wire-format)
11. [Protocol Behavior](#protocol-behavior)
12. [Platform Integration](#platform-integration)
13. [Examples](#examples)
14. [Directory Structure](#directory-structure)
15. [Migrating from v1](#migrating-from-v1)
16. [License](#license)

---

## Overview

BAVA binds an 8-bit identifier to an application variable in RAM. A node writes a registered variable to its peer, or requests the peer's value, over any byte-oriented serial transport.

Unlike raw framing scripts or unvalidated serial parsers, BAVA operates as a corruption-rejecting commit gate: incoming byte streams are parsed on-the-fly and checked through structural and dictionary validations. A validated payload is committed to its destination via an atomic memory copy inside a platform critical section only when all gates pass.

Design properties:

- **Static memory (zero-heap).** All state, dictionary tables, and transmit buffers are statically allocated inside `bava_handle_t`. The library performs zero dynamic memory allocation (`malloc`, `calloc`, or `free`).
- **Fail-closed dictionary commit.** Each identifier maps to a pointer, size, and permission mask (`BAVA_PERM_READ_ONLY` vs. `BAVA_PERM_READ_WRITE`). Writes targeting unregistered IDs, mismatched payload lengths, or read-only variables are rejected before modifying target memory.
- **COBS framing.** Frames are delimited by `0x00` and encoded with Consistent Overhead Byte Stuffing. Wire encoding overhead is fixed at 2 bytes (one COBS code byte plus the delimiter) for all payloads of 32 bytes or less, regardless of payload byte contents.
- **Delimiter resynchronization.** If an in-flight byte is corrupted, truncated, or dropped, the parser safely discards data until the next `0x00` delimiter and re-arms cleanly for subsequent frames.
- **Streaming non-blocking parser.** `bava_process_byte()` accepts one byte per call without blocking, making it suitable for direct execution from UART RX interrupts or DMA/FIFO poll loops.
- **Deterministic transaction state.** `bava_tick()` expires unacknowledged transactions and releases the hardware transmit lock without blocking.

---

## The Five Validation Gates

Every incoming frame must sequentially pass five validation gates before any write is committed to application memory:

| Gate | Check Name | Validation Criteria | Rejection Behavior |
| :---: | :--- | :--- | :--- |
| **Gate 1** | **Frame Bound** | Checks that accumulated frame size is within `[min_frame_size, BAVA_COBS_FRAME_MAX_SIZE]`. | Rejects empty frames and frames exceeding static buffer ceiling; resets parser state to wait for `0x00`. |
| **Gate 2** | **COBS Structure** | Verifies on-the-fly COBS code byte distance and boundary containment against the delimiter. | Rejects frames where a COBS code byte points past the received delimiter boundary; discards corrupt frame. |
| **Gate 3** | **Header Sanity** | Validates command byte (`READ`, `WRITE`, `READ_RESP`, `WRITE_ACK`) and declared length (`LEN <= BAVA_MAX_PAYLOAD`). | Rejects unknown commands and payload lengths inconsistent with the decoded frame size. |
| **Gate 4** | **Integrity** | Verifies CRC-16/CCITT-FALSE (`0x1021`, init `0xFFFF`) across CMD, ID, LEN, and PAYLOAD bytes. | Rejects frames with bit errors or byte corruptions; zero application memory is modified. |
| **Gate 5** | **Dictionary & Commit Gate** | Verifies registered ID exists, payload size exactly matches variable size, and `BAVA_PERM_READ_WRITE` is permitted. | Rejects unregistered IDs (`ERR_NOT_FOUND`), read-only writes (`ERR_RO`), or size mismatches (`ERR_SIZE`). On pass, executes atomic copy in critical section. |

---

## Core Invariants

The protocol implementation enforces five operational invariants:

- **I1 (Fail-Closed Commit):** No byte is written to a registered application variable until all five validation gates pass.
- **I2 (Commit-Before-ACK):** An acknowledgment (`BAVA_STATUS_OK`) is queued only after the variable has been safely committed to memory.
- **I3 (Side-Effect-Free Rejection):** Any frame failing a gate modifies no application state and invokes the user error callback with the corresponding status code.
- **I4 (Static & Bounded Footprint):** All buffers, state trackers, and dictionary slots are statically allocated within `bava_handle_t` (`0` heap allocations, 656 B static RAM).
- **I5 (Delimiter Resynchronization):** Following any wire-level corruption or framing error, the parser resumes cleanly at the next `0x00` frame delimiter.

---

## Scope & Explicit Non-Goals

BAVA is intentionally focused on predictable, deterministic point-to-point microcontroller links:

- **Topology:** Point-to-point byte streams between two microcontrollers (e.g., UART, RS-232, RS-422, or serial wireless bridge).
- **Dictionary Capacity:** Compile-time bounded up to 32 registered variables (`BAVA_MAX_VARIABLES = 32`).
- **Payload Capacity:** Compile-time bounded up to 32 bytes per variable payload (`BAVA_MAX_PAYLOAD = 32`).
- **Transaction Model:** Single outstanding transaction with stop-and-wait ACK semantics; transmit lock is held until peer acknowledgment or timeout.
- **Wire Overhead:** Deterministic `N + 7` bytes on the wire for an $N$-byte payload.
- **Explicit Non-Goals:**
  - No multi-drop bus arbitration or multi-master collision handling.
  - No dynamic memory allocation or unbounded variable-length streaming.
  - No floating-point or text-based schema serialization (transfers raw native memory layouts).
  - No packet fragmentation or multi-hop network routing.
  - No cryptographic encryption or authentication (intended for direct board-to-board PCB/inter-chassis UART connections).

---

## Hardware Verification

The protocol's zero-heap guarantee and fault-rejection characteristics have been empirically verified on physical microcontroller hardware:

- **Hardware Test Setup:** ESP32 DevKit V1 (Sender) $\leftrightarrow$ STM32F103 Blue Pill (Receiver) over physical UART at 921,600 baud.
- **Active Fault Injection:** Byte noise injected into 1 out of every 75 transmitted frames at wire level.
- **Benchmark Run:** Full hardware run log available in [`bava_report.txt`](./bava_report.txt).
  - **Processed Frames:** > 106,000 frames under continuous bidirectional traffic.
  - **Memory Stability:** Exactly `0 B` heap delta throughout the entire benchmark (`Heap_Delta = 0`).
  - **Memory Footprint:** Exact static RAM cost of `sizeof(bava_handle_t) = 656 bytes`.
  - **Round-Trip Latency:** ~799 µs median round-trip time (`p50`) at 921,600 baud.
  - **Integrity Proof:** `0` silent corruptions committed to memory across all 106,000 frames under active fault injection.

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
static void bava_error_handler(uint8_t id, bava_status_t error_code)
{
    (void)id;
    (void)error_code;
}

void setup(void)
{
    Serial.begin(115200);

    bava_init(&bava_handle, hardware_uart_tx);
    bava_set_error_callback(&bava_handle, bava_error_handler);

    if (!bava_register_var(&bava_handle, TELEMETRY_VAR_ID,
                           &sys_telemetry, sizeof(sys_telemetry), BAVA_PERM_READ_WRITE)) {
        /* Registration failed: duplicate ID, size limit exceeded, or dictionary full. */
    }
}

void loop(void)
{
    /* 1. Feed received bytes to the parser (can be called from UART RX ISR). */
    while (Serial.available() > 0) {
        bava_process_byte(&bava_handle, (uint8_t)Serial.read());
    }

    /* 2. Dispatch pending ACKs / read responses and proactive timeouts outside ISR. Required. */
    bava_poll(&bava_handle);

    /* 3. Service acknowledgement timeouts and advance tick. Required. */
    bava_tick(&bava_handle, millis());

    /* 4. Consume a value written by the remote node. */
    if (bava_var_updated(&bava_handle, TELEMETRY_VAR_ID)) {
        bava_var_clear_update_status(&bava_handle, TELEMETRY_VAR_ID);
        /* sys_telemetry now holds the validated remote value. */
    }

    /* 5. Periodic, non-blocking transmission with re-entrancy protection. */
    uint32_t now = millis();
    if (now - last_tx_ms >= 50) {
        last_tx_ms = now;
        sys_telemetry++;
        bava_status_t st = bava_send_write(&bava_handle, TELEMETRY_VAR_ID);
        if (st == BAVA_STATUS_ERR_BUSY) {
            /* Outbound TX deferred: previous transaction is still awaiting an ACK. */
        }
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
| `bava_error_cb_t` | Error callback: `void (*)(uint8_t id, bava_status_t error)`. |
| `BAVA_MAX_VARIABLES` | Dictionary capacity. Default 32. |
| `BAVA_MAX_PAYLOAD` | Maximum payload length in bytes. Default 32. |

### Functions

| Function | Signature | Description |
| :--- | :--- | :--- |
| `bava_init` | `void bava_init(bava_handle_t *handle, bava_tx_cb_t tx_cb)` | Initializes the handle, resets the receive state machine and registers the transmit callback. |
| `bava_set_error_callback` | `void bava_set_error_callback(bava_handle_t *handle, bava_error_cb_t error_cb)` | Registers the user error callback for protocol exceptions, timeouts, and negative acknowledgements. |
| `bava_register_var` | `bool bava_register_var(bava_handle_t *handle, uint8_t id, void *ptr, uint8_t size, uint8_t permissions)` | Binds an identifier to a variable pointer, size, and permissions (`BAVA_PERM_READ_WRITE` or `BAVA_PERM_READ_ONLY`). Returns `true` on success, `false` on failure. |
| `bava_process_byte` | `void bava_process_byte(bava_handle_t *handle, uint8_t byte)` | Passes one received byte to the parser. Decoupled from transmission; sets pending flags for deferred processing. Safe for UART RX ISR. |
| `bava_poll` | `void bava_poll(bava_handle_t *handle)` | Dispatches pending ACKs, read responses, and proactive RX inactivity timeout checks outside ISR context. Required in main loop. |
| `bava_read_var` | `bool bava_read_var(bava_handle_t *handle, uint8_t id, void *dest_buf, uint8_t size)` | Atomically copies a registered variable into user buffer under an interrupt-disabled critical section, preventing multi-byte data tearing. |
| `bava_tick` | `void bava_tick(bava_handle_t *handle, uint32_t system_tick_ms)` | Advances the timeout logic using a millisecond timestamp. Expires pending acknowledgements and checks proactive timeouts. |
| `bava_send_write` | `bava_status_t bava_send_write(bava_handle_t *handle, uint8_t id)` | Reads the registered variable, builds and COBS-encodes a WRITE frame, and passes it to the transmit callback. Returns `BAVA_STATUS_ERR_BUSY` if waiting for previous ACK. |
| `bava_send_raw_write` | `bava_status_t bava_send_raw_write(bava_handle_t *handle, uint8_t id, const uint8_t *ptr, uint8_t len)` | Sends a WRITE frame for an identifier from a caller-supplied buffer. Returns `BAVA_STATUS_ERR_BUSY` if waiting for previous ACK. |
| `bava_send_read` | `bava_status_t bava_send_read(bava_handle_t *handle, uint8_t id)` | Sends a READ frame requesting the remote value of an identifier. Returns `BAVA_STATUS_ERR_BUSY` if waiting for previous ACK. |
| `bava_var_updated` | `bool bava_var_updated(bava_handle_t *handle, uint8_t id)` | Returns `true` if the variable received validated data from the remote node since the flag was last cleared. |
| `bava_var_clear_update_status` | `void bava_var_clear_update_status(bava_handle_t *handle, uint8_t id)` | Clears the update flag for an identifier. |

### Status & Error codes (`bava_status_t`)

| Code | Enumeration | Meaning |
| :---: | :--- | :--- |
| `0x00` | `BAVA_STATUS_OK` | Success / OK. |
| `0x01` | `BAVA_STATUS_ERR_RO` | Remote WRITE rejected because variable is registered as Read-Only. |
| `0x02` | `BAVA_STATUS_ERR_SIZE` | Payload length mismatch with registered dictionary variable size. |
| `0x03` | `BAVA_STATUS_ERR_NOT_FOUND` | Variable identifier not registered in Object Dictionary. |
| `0x04` | `BAVA_STATUS_ERR_BUSY` | Outbound request rejected: previous transaction is still awaiting ACK. |
| `0x11` | `BAVA_STATUS_ERR_TIMEOUT` | Acknowledgement timeout or RX frame inactivity timeout elapsed. |
| `0x12` | `BAVA_STATUS_ERR_INVALID_PAYLOAD_LENGTH` | Incoming frame payload exceeds `BAVA_MAX_PAYLOAD`. |

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