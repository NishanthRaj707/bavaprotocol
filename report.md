# BAVA Protocol Technical Specification and Evaluation Report

Document Version: 2.1.0  
Target Hardware: STM32F103C8T6 (ARM Cortex-M3, 72 MHz) <-> ESP32-WROOM-32 (Xtensa Dual-Core, 240 MHz)  
Evaluation Facility: Department of Instrumentation and Control Engineering, PSG College of Technology  
License: GNU General Public License Version 3  

---

## 1. Problem Statement and Engineering Objectives

### 1.1 The Asynchronous Stream Desynchronization Problem
In microcontroller telemetry, inter-MCU links (UART, RS-485) typically transmit data using ASCII-encoded streams (e.g., JSON, CSV, or formatted text strings via printf). This approach introduces two critical failure modes in constrained embedded systems:

1. FIFO Boundary Fragmentation: Hardware UART FIFOs generate interrupts or transfer buffers in irregular chunk sizes. Delimiter-dependent parsers (relying on \n or ,) suffer desynchronization when messages are split across buffer boundaries. This leads to delimiter truncation, invalid tokens, and up to a 50% drop in valid packet delivery under high baud rates or heavy CPU load.
2. O(N) Parsing Scaling and Heap Exhaustion: Decoding ASCII text requires conversion routines such as strtod(), sscanf(), or JSON DOM parsers. These algorithms scale linearly or quadratically with payload complexity, consuming 130 us to 250+ us of CPU time per packet. Implementations that rely on dynamic memory allocation (malloc) cause memory fragmentation, non-deterministic latency spikes, and eventual stack-heap collisions on bare-metal systems.

### 1.2 Design Objectives
The BAVA Protocol was engineered to meet the following criteria:
- Zero Dynamic Memory Allocation: Eliminate all calls to malloc() and free(). Memory footprint must be fully deterministic at compile time.
- Constant-Time O(1) Memory Synchronization: Map incoming payloads directly to target memory pointers without string searching or schema traversal.
- Deterministic Stream Deframing: Provide a non-blocking byte-level finite state machine capable of resynchronizing following communication drops or line noise.
- Data Integrity Verification: Provide 16-bit CRC coverage across command, identifier, length, and payload fields.

---

## 2. Protocol State Machine (FSM)

The receiver implements a non-blocking finite state machine that processes one byte at a time via bava_process_byte(). It requires no underlying OS task and can execute safely inside an interrupt service routine (ISR) or a polling loop.

```text
[WAIT_SYNC1] --(0xAA)--> [WAIT_SYNC2] --(0x55)--> [READ_CMD]
     ^                        |                        |
     |                     (!0x55)                  (Valid CMD)
     |                        v                        v
     +------------------------+                    [READ_ID]
                                                       |
                                                       v
                                                   [READ_LEN]
                                                       |
                                                       v
                                                   [PAYLOAD]
                                                       |
                                                       v
                                                  [READ_CRC1]
                                                       |
                                                       v
[WAIT_SYNC1] <--- (CRC Valid: Memory Commit) <--- [READ_CRC2]
```

### 2.1 State Definitions
1. BAVA_STATE_WAIT_SYNC1: Awaits primary preamble byte 0xAA. All other bytes are discarded.
2. BAVA_STATE_WAIT_SYNC2: Awaits secondary preamble byte 0x55. If matched, advances to READ_CMD. If mismatched, resets to WAIT_SYNC1.
3. BAVA_STATE_READ_CMD: Reads command opcode (0x01: READ, 0x02: WRITE, 0x81: READ_RESP, 0x82: WRITE_ACK). Invalid or unknown opcodes reset the state machine to WAIT_SYNC1.
4. BAVA_STATE_READ_ID: Reads the target Object Dictionary variable index (0 to BAVA_MAX_VARIABLES - 1).
5. BAVA_STATE_READ_LEN: Reads the payload length byte. If len > BAVA_MAX_PAYLOAD, resets to WAIT_SYNC1 and triggers bava_error_cb_t. If len == 0, skips directly to READ_CRC1.
6. BAVA_STATE_READ_PAYLOAD: Ingests unescaped bytes into the local payload buffer until the counter reaches len.
7. BAVA_STATE_READ_CRC1: Ingests low byte of the incoming 16-bit checksum.
8. BAVA_STATE_READ_CRC2: Ingests high byte of the incoming checksum, computes the expected CRC-16 over the received frame, and verifies data integrity.

### 2.2 Byte-Stuffing Mechanism
To prevent payload bytes from accidentally mimicking the frame synchronization sequence (0xAA 0x55), the transmitter applies byte-escaping:
- Escape Character: 0x7D
- Mask: 0x20
- Any occurrence of 0xAA or 0x7D in the command, identifier, length, payload, or CRC fields is transmitted as 0x7D, followed by the original byte XORed with 0x20:
  - 0xAA becomes 0x7D 0x8A
  - 0x7D becomes 0x7D 0x5D

The receiver reverses this transformation before passing bytes to the state machine or CRC engine.

---

## 3. Object Dictionary Architecture

The Object Dictionary provides direct pointer-based variable binding. During system initialization, applications register local variables into a static lookup table (bava_var_t):

```c
typedef struct {
    uint8_t id;
    void* var_ptr;
    uint8_t size;
    volatile bool updated;
} bava_var_t;
```

### 3.1 Memory Synchronization Flow
1. Registration: bava_register_var(&handle, id, &my_variable, sizeof(my_variable)) records the memory address, length, and identifier.
2. Transmission: bava_send_write(&handle, id) extracts the current variable value from var_ptr, normalizes endianness via bava_htonl() / bava_htons(), computes the CRC, and dispatches the frame via the user's bava_tx_cb_t.
3. Reception: When a valid frame arrives matching id, bava_internal_update() acquires a platform-specific critical section (FreeRTOS spinlock, CMSIS IRQ disable, Zephyr mutex, or atomic flag) and copies the payload directly into var_ptr via memcpy().
4. Notification: The updated flag is asserted, enabling non-blocking status polling via bava_var_updated().

Because variable routing is a direct array index traversal over a fixed table (<= 32 entries), memory access is strictly O(1) constant time.

---

## 4. Packet Structure

All BAVA Point-to-Point frames enforce byte-stuffing (0x7D escape character, XOR 0x20) across command, identifier, length, payload, and CRC bytes.

| Field | Offset | Size (Bytes) | Description |
| :--- | :---: | :---: | :--- |
| SYNC1 | 0 | 1 | Primary synchronization byte 0xAA |
| SYNC2 | 1 | 1 | Secondary synchronization byte 0x55 |
| CMD | 2 | 1 | Command opcode (0x01: READ, 0x02: WRITE, 0x81: READ_RESP, 0x82: WRITE_ACK) |
| ID | 3 | 1 | Object Dictionary variable identifier (0x00 - 0x1F) |
| LEN | 4 | 1 | Payload length (0 <= N <= 32) |
| PAYLOAD | 5 | N | Raw binary data payload |
| CRC1 | 5+N | 1 | CRC-16-CCITT low byte |
| CRC2 | 6+N | 1 | CRC-16-CCITT high byte |

Total unescaped frame size: 7 + N bytes.

---

## 5. Empirical Benchmark Evaluation

### 5.1 Testbed Setup
- Node A (Transmitter): ESP32-WROOM-32 (240 MHz, FreeRTOS, hardware timer tracking)
- Node B (Receiver): STM32F103C8T6 (72 MHz ARM Cortex-M3, Bare-metal)
- Physical Link: Hardware UART at 115,200 baud, 8 data bits, 1 stop bit, no parity (8-N-1)
- Evaluation Mode: Polling execution on transmission and reception to isolate protocol logic from RTOS scheduler jitter

### 5.2 Comparative Results

| Performance Parameter | Raw ASCII UART Stream | BAVA Protocol Frame | Technical Trade-off |
| :--- | :--- | :--- | :--- |
| RX Parse Latency (Single Variable) | 45.0 us (nominal) / 60.0 us (error) | 73.1 us | Constant-time execution; immune to token length |
| RX Parse Latency (Multi-Variable Struct) | 135.5 us (mean) / 251.0 us (max) | 80.2 us | Up to 68% faster; O(1) scaling vs. O(N) ASCII scanning |
| TX Framing Latency | 33.0 us (snprintf) | 84.0 us | +51.0 us trade-off for byte-stuffing and CRC-16 calculation |
| Delivery Success Rate | ~50.0% (Buffer desync) | 100.00% (0 errors) | Sync preambles prevent delimiter fragmentation |
| Dynamic Memory Consumption | 0 Bytes | 0 Bytes | Zero dynamic heap utilization |
| Stack High-Water Mark | Undefined | 96 Bytes | Bounded local stack consumption |

### 5.3 Long-Duration Stability Test
Across an extended continuous run exceeding 40,000 ms (800+ sample transactions), free heap remained strictly constant at 296,088 bytes with zero byte variance, confirming absence of memory leaks.

---

## 6. Known Limitations and Trade-offs

Engineering transparency requires explicit identification of protocol constraints:

1. Worst-Case Byte-Stuffing Overhead: In the statistical worst case where every byte in the payload matches an escape boundary (0xAA or 0x7D), byte-stuffing doubles the payload size over the wire. (Roadmap item: Consistent Overhead Byte Stuffing).
2. Compile-Time Table Sizing: The Object Dictionary size (BAVA_MAX_VARIABLES = 32) and payload capacity (BAVA_MAX_PAYLOAD = 32) are fixed at compile time. Variable-length payloads beyond the configured maximum are rejected.
3. Single Transaction Window: The built-in acknowledgment engine supports a window size of 1. It tracks one outstanding unacknowledged transaction at a time per handle.
4. Point-to-Point Focus: The primary protocol framing is optimized for 1-to-1 links. Multi-drop bus addressing and transceiver half-duplex direction management require secondary handle configuration.
5. Bandwidth Efficiency vs. Small Payloads: Transmitting 1-byte payloads incurs 7 bytes of framing overhead in P2P mode (12.5% payload efficiency). BAVA prioritizes data integrity and determinism over raw transmission density.
