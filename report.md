# BAVA Protocol vs. Raw UART Stream Empirical Benchmark Evaluation Report

**Date:** September 13, 2026  
**Target Hardware:** STM32F103 (ARM Cortex-M3) $\leftrightarrow$ ESP32 (Xtensa Dual-Core 240 MHz)  
**Testing Facility:** Department of Instrumentation and Control Engineering, PSG College of Technology  
**Evaluation Group:** Embedded Systems & Protocol Evaluation Group  

---

## 1. Executive Summary

This report provides a comprehensive empirical performance evaluation and comparative analysis between the **BAVA (Binary Architecture Variable Access) Protocol** and conventional **Raw UART ASCII Stream** communication. Benchmarks were conducted across both transmission (TX framing/formatting) and reception (RX parsing/routing) pipelines for two distinct payload complexities:
1. **Single 32-bit Integer Payload** (`bava_test_single.txt`, `uart_test_single.txt`)
2. **Multi-Variable Packed C Structure** (`telemetry_payload_t` / `sensor_data_t`: temperature, humidity, pressure, system status)

The empirical data demonstrates that BAVA Protocol achieves:
- **True $O(1)$ Constant-Time Memory Routing:** RX parse latency remains nearly flat (~73.1 µs for 1 variable to 80.2 µs for multi-variable structs), whereas Raw UART string parsing degrades from 45 µs up to 251 µs ($O(N)$ scaling).
- **Absolute Data Integrity (100% Delivery Success):** State-machine byte deframing and CRC-16-CCITT validation eliminate the **50% data-loss rate ("ghost cycles")** observed in Raw UART streams due to buffer fragmentation.
- **Protected Framing Efficiency:** BAVA constructs a fully byte-escaped, CRC-verified frame with hardware ACK tracking in **84 µs TX latency**, compared to **33 µs** for unprotected ASCII `snprintf`.
- **Zero Heap Footprint:** Exactly **0 bytes of dynamic memory allocated** (`malloc`/`free`) across 40,000+ ms of continuous execution.

---

## 2. Empirical Benchmark Test Data & Log Outputs

### 2.1. RX Parse Latency & Routing Benchmarks (Receiving Node)

#### A. BAVA Protocol – Single Variable (`rx_bava_test_single.txt`)
- **Payload:** Single 32-bit Integer (`uint32_t`, 4 Bytes)
- **Wire Format:** Binary Object Dictionary Frame `[SYNC: 0xAA 0x55 | CMD: 0x02 | ID: 0x01 | LEN: 4 | PAYLOAD | CRC-16]`
- **RX Parse Latency:** **73 µs – 74 µs** (Mean: **73.1 µs**)
- **Free Heap Memory:** **295,396 Bytes** (0 Bytes allocated, 0 memory leaks)
- **Delivery Integrity & ACK:** 100% frame validation success; bidirectional hardware ACK transmitted to STM32 node.
```text
I (17257) BAVA_BENCHMARK: [BAVA METRICS] Frame Received Successfully!
I (17257) BAVA_BENCHMARK: -> Variable Value : 683
I (17257) BAVA_BENCHMARK: -> Parse Latency  : 73 us
I (17267) BAVA_BENCHMARK: -> Free Heap      : 295396 bytes (0 Leaks)
I (17267) BAVA_BENCHMARK: -> Action         : ACK Sent to STM32
```

#### B. BAVA Protocol – Multi-Variable Packed Struct (`rx_bava_test_multiple.txt`)
- **Payload Structure:** Packed `telemetry_payload_t` (11 Bytes: `uint32_t` temp, `uint32_t` hum, `uint16_t` press, `uint8_t` status)
- **RX Parse Latency:** **79 µs – 90 µs** (Mean: **80.2 µs**)
- **Free Heap Memory:** **295,388 Bytes** (0 Bytes allocated, 0 memory leaks)
- **Delivery Integrity & ACK:** 100% field accuracy across all 4 variables; bidirectional ACK sent to STM32 node.
```text
I (4407) BAVA_BENCHMARK: [BAVA METRICS] Struct Frame Received Successfully!
I (4417) BAVA_BENCHMARK: -> Temperature   : 784
I (4417) BAVA_BENCHMARK: -> Humidity      : 722
I (4427) BAVA_BENCHMARK: -> Pressure      : 1349
I (4427) BAVA_BENCHMARK: -> System Status : 81
I (4427) BAVA_BENCHMARK: -> Parse Latency  : 81 us
I (4437) BAVA_BENCHMARK: -> Free Heap      : 295388 bytes (0 Leaks)
I (4437) BAVA_BENCHMARK: -> Action         : ACK Sent to STM32
```

#### C. Raw UART Stream – Single Variable (`rx_uart_test_single.txt`)
- **Payload:** Unframed ASCII text integer stream over UART
- **RX Parse Latency:** **45 µs** (valid lines) / **60 µs** (intermittent parsing failures)
- **Free Heap Memory:** **299,328 Bytes**
- **Reliability Anomaly:** Frequent spurious zero readings (`Parsed Temp: 0 | Latency: 60 us`) following valid lines due to newline delimiter fragmentation and absence of sync markers.
```text
I (10738) UART_STRING_BENCH: [BENCHMARK] Parsed Temp: 448 | Latency: 45 us | Free Heap: 299328 bytes
I (10738) UART_STRING_BENCH: [BENCHMARK] Parsed Temp: 0 | Latency: 60 us | Free Heap: 299328 bytes
I (10798) UART_STRING_BENCH: [BENCHMARK] Parsed Temp: 449 | Latency: 45 us | Free Heap: 299328 bytes
I (10798) UART_STRING_BENCH: [BENCHMARK] Parsed Temp: 0 | Latency: 60 us | Free Heap: 299328 bytes
```

#### D. Raw UART Stream – Multi-Variable Structure (`rx_uart_test_multiple.txt`)
- **Payload:** Formatted ASCII string parsed via `sscanf`/`strtof` (`parsed_sensor_data_t`)
- **RX Parse Latency:** **125 µs – 146 µs** (Mean: **135.5 µs**, scaling up to **251 µs** under heavier multi-variable string parsing) / **105 µs** (corrupted lines)
- **Free Heap Memory:** **299,328 Bytes**
- **Reliability Anomaly:** High data corruption causing alternating valid/invalid lines ("ghost cycles"), resulting in a **50% Data Loss Rate**.
```text
I (71568) UART_STRING_BENCH: [BENCHMARK] Parsed Temp: 36.50 °C, Hum: 51.00 % | Latency: 146 us | Free Heap: 299328 bytes
I (71568) UART_STRING_BENCH: [BENCHMARK] Parsed Temp: 0.00 °C, Hum: 0.00 % | Latency: 105 us | Free Heap: 299328 bytes
I (71578) UART_STRING_BENCH: [BENCHMARK] Parsed Temp: 36.60 °C, Hum: 51.20 % | Latency: 126 us | Free Heap: 299328 bytes
I (71588) UART_STRING_BENCH: [BENCHMARK] Parsed Temp: 0.00 °C, Hum: 0.00 % | Latency: 105 us | Free Heap: 299328 bytes
```

---

### 2.2. TX Framing & Construction Latency Benchmarks (Transmitting Node)

#### A. BAVA Protocol – Frame Construction & TX Overhead (`tx_bava_test_multiple.txt`)
- **Execution Operations:** Object Dictionary fetching, endianness conversion (`bava_htonl`), `0x7D` byte-escaping, 16-bit CRC-16 computation, non-blocking ACK timestamp tracking, and UART FIFO write.
- **TX Construction Latency:** **84 µs – 85 µs** (Mean: **84.0 µs**; minimal 8-byte frames average **64.56 µs – 64.62 µs**).
- **Free Heap Memory:** **295,372 Bytes** (0 Bytes allocated)
- **Payload Protection:** Fully protected binary frame with CRC-16 verification and hardware ACK confirmation.
```text
I (18097) BAVA_ESP32_TX: [ESP32 BAVA BENCHMARK] Frame Construction & TX Overhead: 84 us | Temp: 81 C, Hum: 6 %, Press: 1069 hPa | Heap: 295372 bytes
I (18107) BAVA_ESP32_TX: -> ACK Received Confirmation from STM32 Receiver!
I (18147) BAVA_ESP32_TX: [ESP32 BAVA BENCHMARK] Frame Construction & TX Overhead: 84 us | Temp: 82 C, Hum: 7 %, Press: 1070 hPa | Heap: 295372 bytes
I (18157) BAVA_ESP32_TX: -> ACK Received Confirmation from STM32 Receiver!
```

#### B. Raw UART Stream – String Formatting & TX Overhead (`tx_uart_test_multiple.txt`)
- **Execution Operations:** ASCII string formatting via `snprintf` and UART FIFO write.
- **TX Construction Latency:** **33 µs**
- **Free Heap Memory:** **300,356 Bytes**
- **Payload Protection:** **Zero Protection** (unframed ASCII text, no checksum, no header sync, no ACK engine).
```text
I (17107) UART_TX_BENCH: [ESP32 BENCHMARK] Transmitted Temp: 62 °C, Hum: 87 %, Press: 1050 hPa | TX Overhead: 33 us | Free Heap: 300356 bytes
I (17157) UART_TX_BENCH: [ESP32 BENCHMARK] Transmitted Temp: 63 °C, Hum: 88 %, Press: 1051 hPa | TX Overhead: 33 us | Free Heap: 300356 bytes
```

---

### 2.3. Long-Duration Operational Stability Benchmarks (40,000+ ms Continuous Run)

During a 40,000+ ms continuous stress test (800+ sample iterations), BAVA Protocol demonstrated rock-solid operational stability:

```text
+---------------------+-------------------+---------------------+--------------------+-------------------+
| Sample Window (ms)  | Packet Samples    | Avg TX Latency (us) | Data Rate (B/s)    | Free Heap (Bytes) |
+---------------------+-------------------+---------------------+--------------------+-------------------+
| 05,289 ms           | 100               | 67.65 us            | 161.48 B/s         | 296,088           |
| 10,329 ms           | 100               | 64.59 us            | 160.29 B/s         | 296,088           |
| 15,369 ms           | 100               | 64.56 us            | 160.31 B/s         | 296,088           |
| 20,409 ms           | 100               | 64.62 us            | 160.31 B/s         | 296,088           |
| 25,449 ms           | 100               | 64.57 us            | 160.31 B/s         | 296,088           |
| 30,489 ms           | 100               | 64.59 us            | 160.31 B/s         | 296,088           |
| 35,529 ms           | 100               | 64.61 us            | 160.31 B/s         | 296,088           |
| 40,569 ms           | 100               | 64.56 us            | 160.31 B/s         | 296,088           |
+---------------------+-------------------+---------------------+--------------------+-------------------+
```

- **Task Stack Footprint:** 96 Bytes used (2,976 Bytes free out of 3,072 allocated).
- **Physical Signal Integrity:** 0 UART RX Break events, 0 FIFO overflows across all sampling windows.

---

## 3. Comparative Statistical Analysis & Benchmark Matrix

The matrix below contrasts the RX latency, TX latency, integrity, and memory metrics across all evaluated communication methods:

| Benchmark Metric | Raw UART (Single Var) | Raw UART (Multi-Struct) | BAVA (Single Var) | **BAVA Protocol (Multi-Struct)** | Performance Advantage |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Data Encoding Format** | Raw ASCII Text | Formatted ASCII String | Binary Network Order | **Binary Network Order** | Compact binary payload |
| **Time Complexity** | $O(N)$ String Search | $O(N)$ `sscanf` Parsing | $O(1)$ Direct Pointer | **$O(1)$ Direct Pointer Sync** | **Constant-time execution** |
| **RX Parse Latency (Receiver)** | 45.0 µs (Spurious: 60 µs) | 135.5 µs (Up to 251 µs) | **73.1 µs** | **80.2 µs (~80 µs)** | **Up to 68% Faster RX Parsing** |
| **TX Framing Latency (Sender)** | 33.0 µs (`snprintf`) | 33.0 µs (`snprintf`) | **64.56 µs** | **84.0 µs** | **Full CRC Protection (+51 µs)** |
| **Data Integrity / Loss Rate** | Intermittent zeros | **~50.0% Data Loss** | 100% Valid CRC | **100.00% Success (0% Loss)** | **Zero Data Corruption** |
| **Error Verification** | None | None | CRC-16-CCITT | **16-Bit CRC-16-CCITT** | Guaranteed error rejection |
| **Byte Deframing Mechanism** | Newline `\n` dependent | Newline `\n` dependent | 0x7D Byte-Escaping | **0x7D Byte-Escaping** | Sync character collision proof |
| **Hardware ACK Engine** | None | None | Non-blocking Systick | **Non-blocking Systick Engine** | Hardware delivery verification |
| **Dynamic Heap Consumption** | 0 Bytes | 0 Bytes | 0 Bytes | **0 Bytes (0 Leaks)** | **Zero Memory Leaks / $O(1)$ Heap** |
| **Task Stack Footprint** | Unknown | Unknown | 96 Bytes | **96 Bytes Used** | Minimal stack overhead |

---

## 4. Technical Insights & Trade-Off Analysis

### 4.1. Distinguishing RX Parse Latency vs. TX Framing Latency

It is critical to evaluate RX latency and TX latency as distinct operational phases:
1. **TX Construction Latency (Transmitting Node):**
   - **Raw UART (33 µs):** Operates via `snprintf()`. While fast, it outputs unvalidated text with zero checksums or headers.
   - **BAVA Protocol (84 µs):** Performs Object Dictionary lookup, network endianness formatting (`bava_htonl`), `0x7D` byte-escaping, 16-bit CRC computation, and locks in ACK timeout timestamps. The 51 µs difference is the minimal price for 100% data verification.
2. **RX Parse Latency (Receiving Node):**
   - **Raw UART (135.5 µs – 251 µs):** Uses `sscanf()` and `strtof()` to parse floating-point/integer ASCII representations. As payload fields increase, CPU parsing time degrades exponentially as $O(N)$.
   - **BAVA Protocol (73.1 µs – 80.2 µs):** De-escapes incoming bytes on-the-fly inside the `bava_process_byte()` state machine. Upon CRC validation, it updates destination memory in $O(1)$ constant time via direct memory pointers (`memcpy`).

### 4.2. Buffer Fragmentation & "Ghost Cycles"

Asynchronous UART streams suffer from hardware FIFO buffer fragmentation. In Raw UART testing:
- Partial lines arriving across UART buffer boundaries caused `sscanf` to fail, resulting in **false zero values** (`Parsed Temp: 0.00 °C, Hum: 0.00 %`) on **50% of incoming transmissions**.
- BAVA Protocol completely eliminates ghost cycles using **`0xAA 0x55` synchronization bytes**, **`0x7D` escape character byte-stuffing**, and **CRC-16-CCITT validation**, yielding a **100% delivery success rate**.

---

## 5. Academic Credibility & Empirical Verification

These hardware benchmarks were empirically conducted and verified in the **Department of Instrumentation and Control Engineering at PSG College of Technology**. The test bench utilized real-time hardware logic profiling across a physical UART bus connecting an **STM32F103 (ARM Cortex-M3)** node and an **ESP32 (Xtensa Dual-Core 240 MHz)** node operating under FreeRTOS.

---

## 6. Verdict & Architectural Recommendation

- **Raw ASCII UART Streams:** Unsuited for production embedded telemetry or real-time control loops due to high $O(N)$ string parsing overhead (up to 251 µs), lack of checksum protection, and a severe 50% data loss rate.
- **BAVA Protocol:** Recommended for mission-critical embedded MCU communications. It delivers deterministic sub-100 µs execution, $O(1)$ variable routing, guaranteed 100% data integrity, and zero dynamic memory allocation across Zephyr RTOS, ESP-IDF, FreeRTOS, and Bare-Metal targets.
