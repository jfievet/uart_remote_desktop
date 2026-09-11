# Project: Real-Time Screen Transmission over IP (UART Simulation)

## Objective

Develop a C application capable of capturing a computer screen and transmitting it to a remote receiver.

The final target transport layer will be UART. However, since UART hardware is not currently available, the first implementation will use TCP/IP communication between two separate programs:

- A **Sender** application
- A **Receiver** application

The applications shall communicate over configurable network ports.

---

# System Architecture

## Sender

Responsibilities:

1. Capture the screen periodically.
2. Compare the current frame with the previous frame.
3. Detect regions that have changed.
4. Compress each modified region using JPEG at 80% quality.
5. Send only the modified regions to the receiver.
6. Display transmission statistics in the console.

## Receiver

Responsibilities:

1. Receive the initial full-screen image.
2. Display the image in a window.
3. Receive compressed update regions.
4. Decompress the received data.
5. Update the display as regions arrive.

---

# Functional Requirements

## FR1 - Screen Capture

The sender shall capture the entire screen at regular intervals.

### Constraints

- The capture rate shall be configurable.
- The first captured frame shall be treated as a full reference frame.

---

## FR2 - Change Detection

The sender shall compare the current frame against the previous frame.

### Objective

Detect only the regions that have changed.

### Constraints

- Unchanged regions shall not be transmitted.
- The algorithm should minimize the amount of transmitted data.

---

## FR3 - Region Compression

Each modified region shall be compressed independently.

### Parameters

- Format: JPEG
- Quality: 80%

---

## FR4 - Data Transmission

Each compressed region shall be transmitted together with a header that allows reconstruction on the receiver side.

### Minimum Header Information

- Region X coordinate
- Region Y coordinate
- Region width
- Region height
- Compressed data size
- Message type

### Message Types

- Full frame
- Update region

---

## FR5 - Transmission Statistics

The sender shall display runtime transmission statistics in the console.

### Instantaneous Statistics

- Current throughput in bytes per second
- Current throughput in bits per second

### Cumulative Statistics

- Total bytes transmitted since session start
- Session duration

### Example Output

```text
Current throughput : 1.25 MB/s
Total transmitted  : 512.4 MB
Session duration   : 00:12:34
```

---

## FR6 - Initial Frame Reception

The receiver shall be able to receive a complete frame when a connection is established.

### Behavior

- Decompress the received image.
- Create a local framebuffer.
- Display the image immediately.

---

## FR7 - Update Reception

The receiver shall receive modified regions transmitted by the sender.

### Processing Sequence

For each received region:

1. Read and parse the header.
2. Decompress the JPEG image.
3. Copy the region into the local framebuffer.
4. Refresh the display.

---

## FR8 - Real-Time Display Updates

The receiver shall update the display as soon as data is received and decoded.

### Objective

Provide a near real-time representation of the remote screen with minimal latency.

---

## FR9 - Command-Line Help

Both the sender and receiver applications shall support a command-line help option.

### Constraints

- Invoking the application with `--help` or `-h` shall print usage information and exit successfully (exit code 0).
- Invoking the application with an unrecognized argument shall still print usage information, but exit with a failure code.

---

# Communication Requirements

## CR1 - Network Transport

For this implementation phase:

- Communication shall use TCP/IP.
- The sender and receiver shall be separate applications.
- IP address shall be configurable.
- Port number shall be configurable.

---

## CR2 - Future UART Compatibility

The transport layer shall be abstracted to allow replacement of TCP/IP with UART in the future without significant modifications to the application logic.

---

# Performance Requirements

## PR1 - Bandwidth Reduction

The system shall transmit only modified screen regions.

---

## PR2 - Compression

All transmitted regions shall be compressed before transmission.

---

## PR3 - Low Latency

Update regions shall be displayed immediately after reception and decompression.

---

# Deliverables

## Application 1: Sender

Core capabilities:

- Screen capture
- Change detection
- JPEG compression
- Network transmission
- Throughput statistics

## Application 2: Receiver

Core capabilities:

- Network reception
- JPEG decompression
- Frame reconstruction
- Real-time display

---

# Acceptance Criteria

The implementation shall be considered successful if:

1. The initial full frame is correctly displayed.
2. Screen changes are detected accurately.
3. Only modified regions are transmitted.
4. Regions are correctly reconstructed on the receiver side.
5. The displayed image matches the source screen.
6. Throughput statistics are displayed in real time.
7. The architecture allows future migration from TCP/IP to UART.