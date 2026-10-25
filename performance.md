# BAVA Protocol Empirical Benchmark and Performance Data

Evaluation Date: September 13, 2026  
Testing Facility: Department of Instrumentation and Control Engineering, PSG College of Technology  
Validation Target: STM32F103 (ARM Cortex-M3) <-> ESP32 (Xtensa Dual-Core)  

This document provides the raw empirical benchmark data, hardware timing profiles, memory utilization measurements, and reliability metrics gathered during comparative hardware testing of the BAVA Protocol against standard ASCII UART streams.

---

## 1. Testbed Specification

All measurements were obtained directly from physical microcontrollers connected via hardware serial interfaces.

| Parameter | Specification | Notes |
| :--- | :--- | :--- |
| Node A (Transmitter / Profiler) | Espressif ESP32-WROOM-32 | 240 MHz Xtensa Dual-Core, FreeRTOS |
| Node B (Receiver / Target) | STMicroelectronics STM32F103C8T6 | 72 MHz ARM Cortex-M3, Bare-metal |
| Physical Interface | Hardware UART | Asynchronous serial, 115,200 baud, 8-N-1 |
| Measurement Instrument | ESP32 Hardware High-Resolution Timer | Microsecond resolution (esp_timer_get_time) |
| Execution Architecture | Polling | Polled RX/TX used on both ASCII and BAVA to isolate protocol logic from RTOS task switching |
| Test Duration | 40,000+ ms | Continuous execution across 800+ sequential packet iterations |

---

## 2. Latency Benchmarks

Measurements compare two transmission scenarios:
1. Single Variable: A 32-bit integer (uint32_t, 4 bytes).
2. Multi-Variable Packed Structure: A structured payload containing 4 variables: temperature (uint32_t), humidity (uint32_t), pressure (uint16_t), and status (uint8_t) (11 bytes total).

### 2.1 Execution Latency Comparison Matrix

| Benchmark Metric | Raw ASCII UART (Single Var) | Raw ASCII UART (Multi-Struct) | BAVA Protocol (Single Var) | BAVA Protocol (Multi-Struct) |
| :--- | :--- | :--- | :--- | :--- |
| Data Format | Unframed ASCII Text | Formatted ASCII String | Binary Network Order | Binary Network Order |
| Algorithmic Complexity | O(N) string scan | O(N) sscanf parsing | O(1) direct pointer sync | O(1) direct pointer sync |
| RX Parse Latency (Receiver) | 45.0 us (nominal) / 60.0 us (error) | 135.5 us (mean) / 251.0 us (max) | 73.1 us | 80.2 us (~80 us) |
| TX Framing Latency (Sender) | 33.0 us (snprintf) | 33.0 us (snprintf) | 64.56 us | 84.0 us |
| Payload Integrity Protection | None (Raw characters) | None (Raw characters) | 16-Bit CRC-16-CCITT | 16-Bit CRC-16-CCITT |
| Hardware Acknowledgment | None | None | Automatic Systick ACK | Automatic Systick ACK |

### 2.2 Latency Analysis
- RX Parsing: ASCII UART string parsing scales linearly with the number of variables, requiring 135.5 us on average and peaking at 251.0 us due to repetitive tokenization and string-to-float conversions. BAVA parses binary frames in constant time (O(1)), averaging 73.1 us for single variables and 80.2 us for multi-variable structures (a 68% latency reduction on multi-variable payloads).
- TX Framing: ASCII snprintf executes in 33.0 us but provides zero error detection. BAVA requires 84.0 us for multi-variable frames (64.56 us for minimal frames). The additional 51.0 us of execution time covers byte-stuffing, endianness conversion, 16-bit CRC computation, and transmission queue tracking.

---

## 3. Memory Footprint and Allocation

Memory consumption was monitored across 40,000+ ms of continuous operation under FreeRTOS.

| Metric | Raw ASCII UART | BAVA Protocol | Target Requirement |
| :--- | :--- | :--- | :--- |
| Dynamic Heap Allocation (malloc) | 0 Bytes | 0 Bytes | 0 Bytes (Strict Zero-Allocation) |
| Free Heap Memory (Continuous) | 299,328 Bytes | 296,088 Bytes | Stable (Zero variance over time) |
| Heap Memory Leaks | 0 Bytes | 0 Bytes | Zero leaks |
| Stack High-Water Mark | Undefined | 96 Bytes Used | < 512 Bytes allocated |
| Free Task Stack Space | Undefined | 2,976 Bytes Free | Out of 3,072 bytes allocated |

BAVA does not call dynamic allocation functions (malloc, calloc, realloc, free). All state machines and buffer arrays are allocated statically inside bava_handle_t, ensuring zero heap fragmentation on bare-metal systems.

---

## 4. Stress Test Results and Data Integrity

Under standard ASCII UART streaming, receiver FIFOs read incomplete data chunks when packets cross UART buffer boundaries. This causes delimiter truncation and invalid parsing tokens.

### 4.1 Packet Delivery and Integrity Comparison

| Reliability Metric | Raw ASCII UART Stream | BAVA Protocol Binary Frame |
| :--- | :--- | :--- |
| Packets Evaluated | 800+ sequential frames | 800+ sequential frames |
| Delivery Success Rate | ~50.0% (Frequent 0.00 readouts) | 100.00% (0 errors) |
| Data Loss Mechanism | FIFO boundary delimiter truncation | None (CRC rejection on line noise) |
| UART Break / Framing Errors | Observed on buffer boundaries | 0 detected |
| FIFO Overrun Errors | Observed under CPU load spikes | 0 detected |

### 4.2 Multi-Window Sampling Log (Continuous Run)

The table below records execution stability across sequential 5,000 ms sampling windows extracted from the physical test run:

| Sample Window (ms) | Packet Samples | Average TX Latency (us) | Data Throughput (Bytes/sec) | Free Heap (Bytes) |
| :---: | :---: | :---: | :---: | :---: |
| 05,289 | 100 | 67.65 | 161.48 | 296,088 |
| 10,329 | 100 | 64.59 | 160.29 | 296,088 |
| 15,369 | 100 | 64.56 | 160.31 | 296,088 |
| 20,409 | 100 | 64.62 | 160.31 | 296,088 |
| 25,449 | 100 | 64.57 | 160.31 | 296,088 |
| 30,489 | 100 | 64.59 | 160.31 | 296,088 |
| 35,529 | 100 | 64.61 | 160.31 | 296,088 |
| 40,569 | 100 | 64.56 | 160.31 | 296,088 |

### 4.3 Observations
1. Timing Determinism: Average TX framing latency showed less than 0.1 us jitter after initialization (holding steady at 64.56 us to 64.62 us for 8-byte frames).
2. Memory Invariance: Free heap space remained locked at 296,088 bytes throughout all sampling windows without drifting.
3. Data Loss Elimination: The dual-byte preamble (0xAA 0x55) and payload length tracking allowed the receiver to cleanly assemble frames regardless of UART FIFO read sizes, eliminating the 50% data loss rate observed in delimiter-based ASCII parsing.
