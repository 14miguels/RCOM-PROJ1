# RCOM Lab 1 — Project Guide

A study guide for the FEUP RCOM 2026/2027 Lab 1 base code (the data link protocol). It explains
what the project is, how the code you were given fits together, and what the protocol needs to
do. **It does not contain the solution.** Every "how" here stays at the level of concepts,
diagrams and byte examples. You write the C.

**Sources** (in the parent folder `../`), in order of authority:

| Short name used below | File |
|---|---|
| **[DLG]** Data-Link Protocol Guide | `Lab 1 - Data-Link Protocol Guide 2026-2027 (EN).pdf` (32 slides) |
| **[FCG]** First Lab Class Guide | `Lab 1 - First Lab Class Guide 2026-2027.pdf` |
| **[SPD]** Serial Port Implementation Details | `Lab 1 - Serial Port Implementation Details 2026-2027 (EN).pdf` |
| **[PRES]** Additional-details presentation | `rcom2025-Lab1Presentation-additionalDetails.pdf` (older year, uses older function names, see §17) |

When two sources disagree, this guide follows **[DLG] 2026/2027** together with the actual
header files, because those are the most recent. Every disagreement I found is listed in §17.

---

## Table of contents

1. [Big Picture](#1-big-picture)
2. [Existing Codebase Map](#2-existing-codebase-map)
3. [Program Execution Flow](#3-program-execution-flow)
4. [Serial Port Layer](#4-serial-port-layer)
5. [Data Link Layer](#5-data-link-layer)
6. [Connection Establishment](#6-connection-establishment)
7. [Information Transfer / Stop-and-Wait](#7-information-transfer--stop-and-wait)
8. [Error Detection](#8-error-detection)
9. [Byte Stuffing / Transparency](#9-byte-stuffing--transparency)
10. [Timeouts and Retransmissions](#10-timeouts-and-retransmissions)
11. [Application Layer](#11-application-layer)
12. [Full End-to-End Example](#12-full-end-to-end-example)
13. [Already Implemented vs. What I Must Implement](#13-already-implemented-vs-what-i-must-implement)
14. [Recommended Implementation Order](#14-recommended-implementation-order)
15. [Function Implementation Checklist](#15-function-implementation-checklist)
16. [Glossary](#16-glossary)
17. [Discrepancies & Gotchas Found](#17-discrepancies--gotchas-found)

---

## 1. Big Picture

### 1.1 What the project is for

Two computers are joined by a **serial cable (RS-232)**. The cable moves bytes and nothing else.
It gives you:

- no way to know where a "message" starts or ends,
- no guarantee a byte arrives correctly (noise can flip bits),
- no guarantee a byte arrives at all (the cable can be unplugged mid-transfer),
- no acknowledgement that the other side got anything.

Your job is to build a **reliable** channel on top of that unreliable byte pipe, then use it to
send a file (`penguin.gif`) from one PC to the other. The received file must be **byte-for-byte
identical** to the original, even when you add noise or unplug the cable for a while.

That breaks down into two protocols you must implement ([DLG] slide 2):

1. A **Data Link Layer (DLL) protocol**: framing, error detection, acknowledgements and
   retransmission. This is most of the work.
2. A small **Application Layer protocol**: cutting the file into pieces, labelling them, and
   rebuilding the file on the other side.

### 1.2 The layered architecture

```
   PC "Tx" (sends the file)                     PC "Rx" (receives the file)
 ┌─────────────────────────────┐              ┌─────────────────────────────┐
 │  Application Layer          │              │  Application Layer          │
 │  application_layer.c        │              │  application_layer.c        │
 │  file ⇄ packets             │              │  packets ⇄ file             │
 └──────────────┬──────────────┘              └──────────────▲──────────────┘
                │ llOpenTx / llSend / llCloseTx               │ llOpenRx / llReceive / llCloseRx
 ┌──────────────▼──────────────┐              ┌──────────────┴──────────────┐
 │  Data Link Layer            │              │  Data Link Layer            │
 │  link_layer.c               │              │  link_layer.c               │
 │  packets ⇄ frames, ACKs,    │              │  frames ⇄ packets, ACKs,    │
 │  retransmission, stuffing   │              │  duplicate detection        │
 └──────────────┬──────────────┘              └──────────────▲──────────────┘
                │ openSerialPort / writeBytesSerialPort / readByteSerialPort / closeSerialPort
 ┌──────────────▼──────────────┐              ┌──────────────┴──────────────┐
 │  Serial Port API            │              │  Serial Port API            │
 │  serial_port.c (GIVEN)      │              │  serial_port.c (GIVEN)      │
 └──────────────┬──────────────┘              └──────────────▲──────────────┘
                │ open / read / write / tcsetattr (POSIX)     │
 ┌──────────────▼──────────────┐              ┌──────────────┴──────────────┐
 │  OS kernel serial driver    │              │  OS kernel serial driver    │
 │  /dev/ttyS0  (or /tmp/ttyS10│              │  /dev/ttyS0 (or /tmp/ttyS11 │
 │  with the virtual cable)    │              │  with the virtual cable)    │
 └──────────────┬──────────────┘              └──────────────▲──────────────┘
                │                 RS-232 cable                │
                └─────────────────────────────────────────────┘
                   (or cable.c + socat, emulating it on one PC)
```

Each layer only talks to the one directly below it through a small API. Layer independence is
graded ([DLG] slide 29):

- The **link layer** must treat every application packet as opaque bytes. It must not look
  inside, and must not treat START/DATA/END packets differently.
- The **application layer** must know nothing about frames, flags, stuffing, BCCs, sequence
  numbers or retransmissions. It only calls `llSend`/`llReceive` and trusts them.

### 1.3 Tx and Rx: who they are and what they do

- **Tx (Transmitter)** is the program started with `tx`. It **owns the file and sends it**.
- **Rx (Receiver)** is the program started with `rx`. It **receives the file and writes it to disk**.

These names describe the *file transfer* roles only. At the frame level, **both sides send and
receive frames** ([DLG] slide 12):

| Frame | Sent by | Received by |
|---|---|---|
| SET | Tx | Rx |
| UA | Rx (answering SET) **and** Tx (answering Rx's DISC) | the other side |
| I (information) | **only Tx** | Rx |
| RR / REJ | **only Rx** | Tx |
| DISC | **both** (Tx first, then Rx replies with its own DISC) | the other side |

So Tx both writes and reads the serial port (it has to read UA, RR and REJ), and Rx does too (it
has to write UA, RR, REJ and DISC). Only data (I frames) flows in one direction.

### 1.4 What the finished project looks like

With the virtual cable running:

```
terminal 1: make run_cable      (virtual cable; you can type "off", "on", "ber 0.0001", ...)
terminal 2: make run_rx         (./bin/main /tmp/ttyS11 9600 rx penguin-received.gif)
terminal 3: make run_tx         (./bin/main /tmp/ttyS10 9600 tx penguin.gif)
then:       make check_files    → "Files penguin.gif and penguin-received.gif are identical"
```

This should still work if you type `off` in the cable console for a few seconds (shorter than
the total retry budget) and then `on`, or if you set a small BER. When closing, each side prints
**protocol statistics** (required by `link_layer.h` and [DLG] slide 30).

In the lab it is the same program on two real PCs using `/dev/ttyS0` and `/dev/ttyUSB0`
([FCG] step 5).

---

## 2. Existing Codebase Map

```
lab1-code-2627/
├── Makefile                 build/run targets          — DO NOT CHANGE
├── README.txt               how to run                 — reference
├── penguin.gif              10 968-byte test file      — the payload
├── bin/                     build output (git-ignored)
├── cable/cable.c            virtual cable emulator     — DO NOT CHANGE
└── src/
    ├── main.c               entry point, arg parsing   — DO NOT CHANGE
    ├── application_layer.h  applicationLayer() proto   — DO NOT CHANGE
    ├── application_layer.c  file-transfer app          — YOU IMPLEMENT
    ├── link_layer.h         DLL API + LinkLayer struct — DO NOT CHANGE
    ├── link_layer.c         DLL protocol               — YOU IMPLEMENT (the core)
    ├── serial_port.h        serial API                 — DO NOT CHANGE
    └── serial_port.c        serial API implementation  — DO NOT CHANGE
```

The "DO NOT CHANGE" labels come from the file header comments and from [FCG] step 4.

### Call graph (as the files are declared)

```
main()                                         [main.c]
  └── applicationLayer(...)                    [application_layer.c]
        ├── llOpenTx(LinkLayer) / llOpenRx(LinkLayer)     [link_layer.c]
        ├── llSend(buf, size)   / llReceive(packet)       [link_layer.c]
        └── llCloseTx()         / llCloseRx()             [link_layer.c]
              └── openSerialPort, writeBytesSerialPort,
                  readByteSerialPort, closeSerialPort     [serial_port.c]
                        └── open, tcgetattr, tcsetattr, tcflush,
                            fcntl, read, write, close     [POSIX / kernel]
```

---

### 2.1 `src/main.c`: entry point (do not modify)

**Responsibility:** read the command line, validate it, pick the protocol constants, and hand
everything to the application layer. It contains no protocol logic.

**Important contents:**

| Item | Value / meaning |
|---|---|
| `#define N_TRIES 3` | maximum tries per frame, passed along as `nTries` |
| `#define TIMEOUT 4` | retransmission timeout **in seconds**, passed along as `timeout` |
| `argv[1]` | serial port path (`/tmp/ttyS10`, `/tmp/ttyS11`, `/dev/ttyS0`, ...) |
| `argv[2]` | baud rate, checked against 1200…115200 |
| `argv[3]` | role: must be exactly `"tx"` or `"rx"` |
| `argv[4]` | filename (file to **send** for Tx, file to **create** for Rx) |

**Calls:** only `applicationLayer(serialPort, role, baudrate, N_TRIES, TIMEOUT, filename);`

**Called by:** the shell, directly or through `make run_tx` / `make run_rx`.

**Interaction:** the retry and timeout values of the whole protocol live here, as compile-time
constants. They travel `main → applicationLayer → LinkLayer struct → llOpenTx/llOpenRx`. That
path is the only way the link layer learns them.

---

### 2.2 `src/application_layer.h`: application API (do not modify)

Declares a single function:

```c
void applicationLayer(const char *serialPort, const char *role, int baudRate,
                      int nTries, int timeout, const char *filename);
```

This is the contract between `main.c` and your application code. (Its doc comment says
"@return 0 on success or -1" but the function is `void`. See §17.)

---

### 2.3 `src/application_layer.c`: file-transfer application (you implement)

**Responsibility (final):** on Tx, open the file, send START, send DATA packets, send END and
close the link. On Rx, receive packets, create the output file, write the fragments into it,
stop at END and close the link.

**What is there now:** a skeleton that only does two things.

1. It builds the `LinkLayer` parameter struct from its arguments:
   ```c
   LinkLayer llParameters = {
       .baudRate = baudRate,
       .nRetransmissions = nTries,
       .timeout = timeout,
   };
   strcpy(llParameters.serialPort, serialPort);
   ```
2. It dispatches on `role`: `"tx"` → `llOpenTx(llParameters)`, `"rx"` → `llOpenRx(llParameters)`.

It never uses `filename`, never calls `llSend`/`llReceive`/`llClose*`, and ignores the return
value of `llOpen*`. It has a `// TODO: Adapt and extend this code`.

**Calls:** `llOpenTx`, `llOpenRx` (later also `llSend`, `llReceive`, `llCloseTx`, `llCloseRx`,
and C standard library file I/O such as `fopen`/`fread`/`fwrite`).

**Called by:** `main()`.

**Must not call:** anything in `serial_port.h`. It doesn't even include it, and that is
deliberate (layer independence).

---

### 2.4 `src/link_layer.h`: the DLL API (do not modify)

This header is the contract between your application layer and your link layer, and it matches
[DLG] slide 21 exactly.

**Struct:**

```c
typedef struct {
    char serialPort[50];  // device path
    int baudRate;
    int nRetransmissions; // "Number of retries in case of failure"  (= N_TRIES = 3)
    int timeout;          // seconds                                   (= TIMEOUT = 4)
} LinkLayer;
```

**Constants:**

| Name | Value | Meaning |
|---|---|---|
| `MAX_PAYLOAD_SIZE` | 1000 | Largest buffer the **application** may pass to `llSend`. This is a whole application packet (header included), not just file bytes. |
| `FALSE` / `TRUE` | 0 / 1 | Convenience booleans |

**Functions** (return conventions from the header and [DLG] slides 22–24):

| Function | Role | Returns |
|---|---|---|
| `int llOpenTx(LinkLayer)` | Tx: open port, send SET, wait for UA | 0 ok, negative on error |
| `int llOpenRx(LinkLayer)` | Rx: open port, wait for SET, send UA | 0 ok, negative on error |
| `int llSend(const unsigned char *buf, int bufSize)` | Tx: deliver one packet reliably | bytes written, or -1 |
| `int llReceive(unsigned char *packet)` | Rx: get the next new packet | packet length, or -1 |
| `int llCloseTx()` | Tx: DISC handshake, close port, **print stats** | 0 ok / -1 |
| `int llCloseRx()` | Rx: DISC handshake, close port, **print stats** | 0 ok / -1 |

**Something the signatures tell you about your design:** only the `llOpen*` functions receive
`LinkLayer`. `llSend`, `llReceive` and `llClose*` take no connection handle and no timeout or
retry count. So anything they need that is decided at open time or carried between calls (the
configured timeout and retry count, the current sequence number, the statistics counters) has to
live somewhere that outlives a single call, which in practice means state private to
`link_layer.c`. How you organise that state is up to you.

---

### 2.5 `src/link_layer.c`: the DLL protocol (you implement, the core)

**What is there now:**

| Function | Current content |
|---|---|
| `llOpenTx` | **Milestone-1 demo.** Opens the port, writes 256 bytes `"abcde\nfgh…"` (with a `'\n'` at index 5 to show non-canonical mode sends it like any other byte), `sleep(1)`, closes the port. |
| `llOpenRx` | **Milestone-1 demo.** Opens the port, loops `readByteSerialPort` printing each byte until it sees `'z'`, prints the count, closes the port. |
| `llSend` | `// TODO: Implement this function` then `return 0;` |
| `llReceive` | `// TODO` then `return 0;` |
| `llCloseTx` | `// TODO` then `return 0;` |
| `llCloseRx` | `// TODO` then `return 0;` |

It also defines `BUF_SIZE 256` (used only by the demo) and `_POSIX_SOURCE` (defined *after* the
includes, so it has no effect. Harmless.)

**Notes on the demo code:**

- The demo `llOpenTx`/`llOpenRx` **close the serial port before returning**. In the real
  protocol the port has to stay open after `llOpen*`, because `llSend`/`llReceive` need it.
  It should only close in `llClose*`.
- The demo `llOpenRx` ignores `readByteSerialPort`'s return value and uses `byte` anyway. The
  comment itself warns: *"You must check how many bytes were actually read by reading the return
  value."* Keep that habit.
- The demo is still useful for **Milestone 1**. Run it before changing anything, to prove the
  cable and ports work.

**Calls:** `openSerialPort`, `writeBytesSerialPort`, `readByteSerialPort`, `closeSerialPort`
(later also signal/alarm functions for the timer, see §10).

**Called by:** `applicationLayer` only.

---

### 2.6 `src/serial_port.h` / `src/serial_port.c`: serial port library (do not modify)

**Responsibility:** hide all POSIX/termios details and give the link layer four simple calls.
[PRES] slide 8 says of this file: "NOTHING TO DO here." Details in §4.

**Important internals:**

```c
int fd = -1;           // File descriptor for open serial port
struct termios oldtio; // Serial port settings to restore on closing
```

The **file descriptor is a global inside `serial_port.c`**. The link layer never sees it, so it
works purely through the four functions. One consequence: only one serial port can be open per
process at a time, which is all this project needs.

**Called by:** `link_layer.c` only.
**Calls:** `open`, `tcgetattr`, `tcflush`, `tcsetattr`, `fcntl`, `read`, `write`, `close`.

---

### 2.7 `cable/cable.c`: virtual cable (do not modify, not part of your program)

A **separate program** (`bin/cable`) that lets you develop on one computer without a physical
serial cable.

**How it works:**

```
 your Tx program                cable program                      your Rx program
 opens /tmp/ttyS10              (bin/cable)                        opens /tmp/ttyS11
      │                                                                   ▲
      │  socat PTY pair #1                           socat PTY pair #2    │
      ▼                                                                   │
 /tmp/ttyS10 ⇄ /tmp/emulatorTx ──► [ring buffer, delay, ─► /tmp/emulatorRx ⇄ /tmp/ttyS11
                                    noise, on/off]
             /tmp/emulatorTx ◄── [same, other direction] ◄── /tmp/emulatorRx
```

1. `main()` launches two `socat` processes. Each creates a **pseudo-terminal (PTY) pair**, two
   linked fake serial devices: `/tmp/ttyS10`↔`/tmp/emulatorTx` and
   `/tmp/ttyS11`↔`/tmp/emulatorRx`.
2. It opens the two emulator ends in **non-blocking** mode (`VMIN=0`, `VTIME=0`).
3. Its main loop runs once per **byte time** (`10 / baud` seconds: 8 data bits + start + stop).
   Each iteration it reads at most one byte from each direction, pushes it through a ring buffer
   (to simulate propagation delay) and writes it out the other side. This way the cable really
   behaves like a 9600-baud line.
4. It reads commands from stdin:

| Command | Effect | Why you care |
|---|---|---|
| `off` / `on` | drop all bytes / reconnect | tests timeouts and retransmission |
| `ber <x>` | flip bits with bit error rate *x* (at most one bit per byte) | tests BCC1/BCC2, REJ, duplicates |
| `baud <rate>` | change the emulated line speed (default **9600**) | must match what you pass to `main` |
| `prop <usec>` | add propagation delay | efficiency study ([DLG] slide 31) |
| `log <file>` / `endlog` | log every byte in hex, both directions | **very good for debugging frames** |
| `quit` | stop and `killall socat` | |

**How it interacts with your code:** it doesn't, directly. Your program just opens
`/tmp/ttyS10` or `/tmp/ttyS11` as if they were real serial ports. `serial_port.c` can't tell the
difference.

`addNoiseToBuffer()` is defined but never called. Noise is applied inline in the main loop.

---

### 2.8 `Makefile` (do not modify)

| Target | What it does |
|---|---|
| `make` / `make all` | builds `bin/main` (all `src/*.c`) and `bin/cable` |
| `make run_cable` | checks for `socat`, runs `bin/cable` |
| `make run_rx` | `./bin/main /tmp/ttyS11 9600 rx penguin-received.gif` |
| `make run_tx` | `./bin/main /tmp/ttyS10 9600 tx penguin.gif` |
| `make check_files` | `diff -s penguin.gif penguin-received.gif` |
| `make clean` | removes binaries and the received file |

Since `main` is built from `src/*.c`, any **new** `.c` file you add under `src/` is compiled in
automatically.

### 2.9 Other files

- `README.txt`: run instructions (note: it says `./bin/cable_app`, but the binary is `bin/cable`. See §17).
- `penguin.gif`: 10 968 bytes. This is your test payload.
- `.gitignore`: ignores `penguin-received.gif` and `*.o`. `bin/.gitignore` ignores everything in `bin/`.

---

## 3. Program Execution Flow

### 3.1 How the command line chooses Tx or Rx

```
$ ./bin/main /tmp/ttyS10 9600 tx penguin.gif
              argv[1]    argv[2] argv[3] argv[4]
                              │
main.c:  role = argv[3]  ──── validated: "tx" or "rx" (else exit(3))
                              │
applicationLayer(serialPort, role, baudrate, N_TRIES, TIMEOUT, filename)
                              │
application_layer.c:  strcmp(role, "tx") == 0 ?  ──yes──► Tx branch (llOpenTx ...)
                      strcmp(role, "rx") == 0 ?  ──yes──► Rx branch (llOpenRx ...)
```

`role` is the only thing that decides which half of the protocol runs. Both roles are the same
binary.

### 3.2 TX execution: what happens today

```
main()                                  [main.c]
  │ parse + validate args, print banner
  ▼
applicationLayer(port, "tx", 9600, 3, 4, "penguin.gif")   [application_layer.c]
  │ fill LinkLayer {serialPort, baudRate, nRetransmissions=3, timeout=4}
  ▼
llOpenTx(llParameters)                  [link_layer.c]  (demo code)
  ├─► openSerialPort("/tmp/ttyS10", 9600)          [serial_port.c] → open/tcsetattr/fcntl
  ├─► writeBytesSerialPort("abcde\nfgh…", 256)      [serial_port.c] → write(fd, …)
  ├─► sleep(1)
  └─► closeSerialPort()                             [serial_port.c] → tcsetattr(old), close
```

### 3.3 TX execution: target flow once finished

```
main()
  ▼
applicationLayer(..., "tx", ..., filename)
  ├─► llOpenTx(params)
  │     ├─► openSerialPort(...)
  │     └─► [send SET] writeBytesSerialPort ──► [wait UA] readByteSerialPort × n  (timer, retries)
  │
  ├─► open file, find its size
  ├─► build START packet ──► llSend(start, len)
  │                            ├─► BCC2, stuffing, build I(Ns) frame
  │                            ├─► writeBytesSerialPort(frame)
  │                            └─► readByteSerialPort × n → RR / REJ / timeout → maybe resend
  ├─► loop until EOF:
  │     read fragment ──► build DATA packet ──► llSend(data, len)   (same as above)
  ├─► build END packet ──► llSend(end, len)
  │
  └─► llCloseTx()
        ├─► [send DISC] writeBytesSerialPort ──► [wait DISC] readByteSerialPort × n (timer, retries)
        ├─► [send UA]   writeBytesSerialPort
        ├─► print statistics
        └─► closeSerialPort()
```

### 3.4 RX execution: what happens today

```
main()
  ▼
applicationLayer(port, "rx", 9600, 3, 4, "penguin-received.gif")
  ▼
llOpenRx(llParameters)                  [link_layer.c]  (demo code)
  ├─► openSerialPort("/tmp/ttyS11", 9600)
  ├─► loop: readByteSerialPort(&byte) → printf → stop at 'z'
  └─► closeSerialPort()
```

### 3.5 RX execution: target flow once finished

```
main()
  ▼
applicationLayer(..., "rx", ..., filename)
  ├─► llOpenRx(params)
  │     ├─► openSerialPort(...)
  │     ├─► [wait SET] readByteSerialPort × n  (state machine)
  │     └─► [send UA]  writeBytesSerialPort
  │
  ├─► loop until END packet:
  │     llReceive(packet)
  │       ├─► readByteSerialPort × n  (I-frame state machine)
  │       ├─► check BCC1, check Ns (new or duplicate?), destuff, check BCC2
  │       ├─► writeBytesSerialPort(RR or REJ)
  │       └─► return only when a NEW, VALID packet arrives
  │     inspect packet: START → create file / DATA → append bytes / END → stop
  │
  └─► llCloseRx()
        ├─► [wait DISC] readByteSerialPort × n
        ├─► [send DISC] writeBytesSerialPort ──► [wait UA] readByteSerialPort × n
        ├─► print statistics
        └─► closeSerialPort()
```

### 3.6 Start-up order matters

Start **cable first**, then **Rx**, then **Tx** (README step 4). The cable must exist before
either side opens `/tmp/ttyS1x`. Rx should already be listening when Tx sends SET, although the
SET timer/retry mechanism covers small delays.

---

## 4. Serial Port Layer

### 4.1 The four functions you are given

| Function | What it does | Returns |
|---|---|---|
| `openSerialPort(path, baud)` | `open()`s the device, saves its old settings, sets up 8N1 raw non-canonical mode at `baud`, flushes stale data, makes reads blocking | fd (>0) or -1 |
| `readByteSerialPort(&b)` | `read(fd, b, 1)`: gets **one** byte | 1 = got a byte, 0 = none, -1 = error/interrupted |
| `writeBytesSerialPort(buf, n)` | `write(fd, buf, n)` | bytes written, or -1 |
| `closeSerialPort()` | restores the saved settings and `close()`s | 0 or -1 |

### 4.2 What this layer already does for you (don't reimplement it)

- Opening the device and getting a file descriptor.
- Baud-rate translation (`9600` → `B9600`).
- **Raw 8-bit mode** (`CS8`, no parity (`IGNPAR`), `c_oflag = 0`, `c_lflag = 0`), so every
  byte value 0x00–0xFF passes unmodified. No `\n` translation, no echo, no line buffering.
- Flushing garbage left in the driver buffers when opening (`tcflush(fd, TCIOFLUSH)`).
- Restoring the port's original configuration when closing.

What it **doesn't** do: delimit frames, detect errors, acknowledge, time out or retry. All of
that is the link layer's job.

### 4.3 The minimum OS background

- **File descriptor (fd):** in Unix a device is accessed like a file. `open("/dev/ttyS0")` returns
  a small integer, the fd, which you then pass to `read`/`write`/`close`. Here the fd is hidden in
  a global inside `serial_port.c`.
- **termios:** the `struct termios` that configures a terminal/serial device (speed, character
  size, parity, input processing). Read with `tcgetattr`, applied with `tcsetattr` ([SPD] slide 7).
- **Canonical vs non-canonical** ([SPD] slide 8):
  - *Canonical* mode is line-oriented (think of a shell). `read()` returns only complete lines ending in `\n`.
  - *Non-canonical* mode is byte-oriented. `read()` returns as soon as a byte-count/time condition
    set by `VMIN`/`VTIME` is met. Your frames are binary and may contain `0x0A`, so canonical mode
    would break them. That is why the demo deliberately puts `'\n'` at `buf[5]`.

### 4.4 The configuration that actually matters to you: `VMIN = 1`, `VTIME = 0`

```c
newtio.c_cc[VTIME] = 0; // Block reading
newtio.c_cc[VMIN] = 1;  // Byte by byte
...
oflags ^= O_NONBLOCK;   // Clear O_NONBLOCK flag to ensure blocking reads
```

With this configuration, **`readByteSerialPort` blocks until at least one byte has arrived.**
It does not time out by itself. If nothing ever arrives, it waits forever, **unless a signal
interrupts it** (then `read` returns -1 with `errno == EINTR`).

That fact decides how you build timeouts (§10). The `serial_port.h` comment ("Wait up to 0.1
second (VTIME)… 0 if no byte was received") describes a different configuration from the one the
code actually uses (see §17). Still, write your code so it copes with all three return values
(1, 0, -1). That is correct in either case.

---

## 5. Data Link Layer

### 5.1 Responsibilities mapped to functions

[DLG] slides 5–8 and 30 list the DLL's jobs. Here's where each one lives in this repo:

| DLL responsibility | Where it happens |
|---|---|
| Opening the physical port | `llOpenTx` / `llOpenRx` (via `openSerialPort`) |
| **Connection establishment** (SET/UA) | `llOpenTx` / `llOpenRx` |
| **Framing** (FLAG delimiting, header) | building: `llSend` (I), all functions (S/U). Parsing: every function that reads |
| **Transparency** (byte stuffing) | stuff in `llSend`, destuff in `llReceive` |
| **Error detection** (BCC1, BCC2) | compute on send; check on every receive |
| **Frame numbering** (Ns / Nr, modulo 2) | `llSend` (Ns), `llReceive` (expected Ns, Nr in RR/REJ) |
| **Positive/negative ACK** (RR/REJ) | sent in `llReceive`, handled in `llSend` |
| **Timer + retransmission** | `llOpenTx` (SET), `llSend` (I), `llCloseTx` (DISC), `llCloseRx` (DISC) |
| **Duplicate detection** | `llReceive` |
| **Connection termination** (DISC/DISC/UA) | `llCloseTx` / `llCloseRx` |
| **Statistics** | counted everywhere, printed in `llCloseTx` / `llCloseRx` |

You'll likely want private helper functions in `link_layer.c` (e.g. for building a supervision
frame, for the receive state machine, for stuffing, for the timer). The header only fixes the
**public** API. How you split things up internally is your design decision.

### 5.2 What each public function will eventually have to achieve

**Opening a connection**
- `llOpenTx`: open the port and store the parameters. Send **SET**. Wait for a valid **UA**.
  If none arrives within `timeout`, resend SET, up to the retry limit. Return 0 once UA arrives,
  negative if every try fails.
- `llOpenRx`: open the port and store the parameters. Read bytes until a valid **SET** frame is
  recognised. Reply with **UA**. Return 0.

**Sending information**
- `llSend(buf, bufSize)`: treat `buf` (≤ `MAX_PAYLOAD_SIZE`) as opaque data. Compute **BCC2**
  over it, **stuff** data+BCC2, wrap it in an **I frame** with the current **Ns**, and send it.
  Then wait for the reply: **RR** with the next number → success, flip Ns, return. **REJ** →
  resend now. **Nothing** within `timeout` → resend. Give up after the retry limit and return -1.

**Receiving information**
- `llReceive(packet)`: read bytes until a complete I frame is delimited. Check **BCC1**; if
  wrong, ignore the frame silently. Look at **Ns** to see whether it's new or a duplicate.
  **Destuff**, check **BCC2**, reply **RR** or **REJ** per the rules in §7, and **return only
  once a new, error-free packet** has been copied into `packet` (return its length).

**Closing a connection**
- `llCloseTx`: send **DISC**, wait for Rx's **DISC** (timer + retries), send **UA**, print
  statistics, close the port.
- `llCloseRx`: wait for **DISC**, reply with **DISC** (timer + retries, since DISC is a command),
  wait for **UA**, print statistics, close the port.

### 5.3 Frame formats

There are two shapes ([DLG] slides 9–11):

```
Supervision (S) and Unnumbered (U) frames: always exactly 5 bytes
┌──────┬─────┬─────┬──────┬──────┐
│ FLAG │  A  │  C  │ BCC1 │ FLAG │
└──────┴─────┴─────┴──────┴──────┘

Information (I) frames: variable length
┌──────┬─────┬─────┬──────┬────────────────────┬──────┬──────┐
│ FLAG │  A  │  C  │ BCC1 │ D1 D2 … DN         │ BCC2 │ FLAG │
└──────┴─────┴─────┴──────┴────────────────────┴──────┴──────┘
   ◄────── header ──────►   ◄─ data field ─►    trailer
                            (= one whole application packet, stuffed together with BCC2)
```

#### The fields and why each exists

| Field | Size | Value(s) | Why it exists |
|---|---|---|---|
| **FLAG** | 1 | `0x7E` (01111110) | **Frame delimiting.** The byte stream has no boundaries of its own. The flag marks start and end, so a receiver that joins mid-stream or loses bytes can resynchronise at the next flag. A frame may be preceded by several flags ([DLG] slide 14). |
| **A** (address) | 1 | `0x03` or `0x01` | Tells **commands** from **replies** and which side sent them (rule below). Lets a receiver reject frames that make no sense in context. |
| **C** (control) | 1 | see table | **Frame type**, plus the **sequence number** (Ns in I frames, Nr in RR/REJ). |
| **BCC1** | 1 | `A XOR C` | **Header error detection.** Without it, a flipped bit could turn an RR into a REJ, or I0 into I1. |
| **D1…DN** | N | application packet | The payload. The DLL does not interpret it. |
| **BCC2** | 1 | `D1 XOR … XOR DN` | **Data error detection**, independent from BCC1, so a frame with a good header but corrupted data can still be identified (and REJected) ([DLG] slide 8). |

#### The address (A) rule ([DLG] slide 14)

- **Commands** = I, SET, DISC. **Replies** = UA, RR, REJ.
- `A = 0x03`: **commands sent by Tx**, and **replies sent by Rx**.
- `A = 0x01`: **commands sent by Rx**, and **replies sent by Tx**.

So during closing, Rx's DISC is a *command from Rx* (`A = 0x01`), and Tx's final UA is a *reply
from Tx* (`A = 0x01`). Most frames use `0x03`; getting these two cases wrong is a common bug.

#### Control (C) values ([DLG] slide 10–11)

| Frame | C (binary) | C (hex) | Notes |
|---|---|---|---|
| SET | 00000011 | `0x03` | |
| UA | 00000111 | `0x07` | |
| DISC | 00001011 | `0x0B` | |
| RR0 | 10101010 | `0xAA` | pattern `1010101R`, R = Nr |
| RR1 | 10101011 | `0xAB` | |
| REJ0 | 01010100 | `0x54` | pattern `0101010R`, R = Nr |
| REJ1 | 01010101 | `0x55` | |
| I, Ns=0 | 00000000 | `0x00` | pattern `S0000000`, S = Ns |
| I, Ns=1 | 10000000 | `0x80` | |

### 5.4 Each frame type in detail

| Frame | Sender → Receiver | Why it exists | When |
|---|---|---|---|
| **SET** (set up) | Tx → Rx | Asks "are you there and ready?", starting the logical connection | `llOpenTx` (repeated on timeout) |
| **UA** (unnumbered acknowledgement) | Rx → Tx (reply to SET); Tx → Rx (reply to Rx's DISC) | Confirms a valid SET or DISC was received | `llOpenRx`; end of `llCloseTx` |
| **I** (information) | Tx → Rx | Carries one application packet, numbered Ns ∈ {0,1} | `llSend` (repeated on timeout/REJ) |
| **RR** (receiver ready) | Rx → Tx | Positive ACK: "got it, send me Nr next" | `llReceive` |
| **REJ** (reject) | Rx → Tx | Negative ACK: "frame Nr had a data error, resend it now". Saves waiting for the timeout | `llReceive` |
| **DISC** (disconnect) | Tx → Rx, then Rx → Tx | Asks to end the connection | `llCloseTx`, `llCloseRx` |

**Conceptual byte layouts** (BCC1 computed as A⊕C; try these by hand before checking):

| Frame | Bytes |
|---|---|
| SET (from Tx) | `7E 03 03 00 7E` (03⊕03 = 00) |
| UA (from Rx) | `7E 03 07 04 7E` (03⊕07 = 04) |

<details>
<summary>Exercise: work out the rest yourself, then open this to check</summary>

| Frame | A | C | BCC1 |
|---|---|---|---|
| DISC from Tx (command by Tx) | 03 | 0B | 08 |
| DISC from Rx (command by Rx) | 01 | 0B | 0A |
| UA from Tx (reply by Tx) | 01 | 07 | 06 |
| RR0 | 03 | AA | A9 |
| RR1 | 03 | AB | A8 |
| REJ0 | 03 | 54 | 57 |
| REJ1 | 03 | 55 | 56 |
| I header Ns=0 | 03 | 00 | 03 |
| I header Ns=1 | 03 | 80 | 83 |

Note that none of the header bytes (A, C, BCC1) ever equals `0x7E` or `0x7D`. That's why, in
practice, stuffing only affects the data field and BCC2 (§9).
</details>

---

## 6. Connection Establishment

### 6.1 The exchange

```
      Tx                                              Rx
 llOpenTx()                                      llOpenRx()
 openSerialPort()                                openSerialPort()
      │                                               │ (already reading bytes,
      │  SET  7E 03 03 00 7E                          │  waiting for SET)
      ├──────────────────────────────────────────────►│
      │  start timer                                  │ state machine recognises SET
      │                                               │ BCC1 ok
      │                    UA  7E 03 07 04 7E         │
      │◄──────────────────────────────────────────────┤
      │  state machine recognises UA                  │ return 0
      │  stop timer                                   │
      │  return 0                                     │
```

If the SET or the UA is lost or corrupted, Tx's timer fires after `timeout` seconds and Tx sends
SET again, up to the configured limit ([DLG] slide 15: *"I, SET and DISC frames are protected by
a timer"*). A corrupted frame (bad BCC1) is **ignored without any action** ([DLG] slide 15), so
from Tx's point of view it looks exactly like a lost one.

[FCG] step 6 defines the goal of Milestone 2: *"The establishment phase is complete once the
transmitter sends a SET frame, the receiver answers with an UA frame after checking that the
received frame was correct, and the transmitter correctly detects the UA frame."*

### 6.2 Why reception has to be byte-by-byte

1. **The line gives you a byte stream, not messages.** `read` doesn't know where frames begin or
   end. You find the boundaries yourself by recognising FLAGs.
2. **Each header byte means something depending on its position.** You need to know "this is the
   byte right after the opening flag" to treat it as A, and so on.
3. **Noise and loss.** Bytes can be missing or wrong. By checking one byte at a time you can
   notice "this isn't a valid A" and immediately go back to hunting for a flag, instead of
   misinterpreting a whole buffer.
4. **Timing.** Bytes arrive at ~960 bytes/s at 9600 baud. A frame dribbles in over several
   milliseconds, and a single bulk `read` could easily return half a frame.

That is why the API offers `readByteSerialPort` (one byte) but `writeBytesSerialPort` (many):
sending a whole frame at once is fine, but receiving must be incremental.

### 6.3 The receiver state machine: the idea

A **state machine** here is just a variable remembering "how far into a frame am I?", updated
once per byte. Each incoming byte either moves you forward, sends you back to the start, or keeps
you where you are.

For **supervision frames** ([PRES] slides 19–20, the variant that handles repeated flags):

```
                 any byte ≠ FLAG
                  ┌─────┐
                  │     ▼
               ┌──────────┐
     ┌────────►│  START   │◄──────────────────────────────────────────┐
     │         └────┬─────┘                                           │
     │              │ FLAG                                            │
     │              ▼                                                 │
     │         ┌──────────┐ ◄─┐ FLAG (repeated flags: stay here)      │
     │         │ FLAG_RCV │───┘                                       │
     │         └────┬─────┘                                           │
     │  other       │ byte == expected A                              │
     ├──────────────┤                                                 │
     │              ▼                                                 │
     │         ┌──────────┐  FLAG → back to FLAG_RCV                  │
     │         │  A_RCV   │                                           │
     │         └────┬─────┘                                           │
     │  other       │ byte == expected C                              │
     ├──────────────┤                                                 │
     │              ▼                                                 │
     │         ┌──────────┐  FLAG → back to FLAG_RCV                  │
     │         │  C_RCV   │                                           │
     │         └────┬─────┘                                           │
     │  other       │ byte == A ⊕ C  (BCC1 OK)                        │
     │  (BCC1 bad)  │                                                 │
     ├──────────────┤                                                 │
     │              ▼                                                 │
     │         ┌──────────┐                                           │
     │         │ BCC1_OK  │── other ──────────────────────────────────┘
     │         └────┬─────┘
     │              │ FLAG
     │              ▼
     │         ┌──────────┐
     │         │   STOP   │  → frame accepted
     │         └──────────┘
```

Questions to answer while designing yours (don't skip them):

- In `FLAG_RCV`, why is receiving *another* FLAG not an error? (Hint: [DLG] slide 14.)
- In `A_RCV`/`C_RCV`, if the byte is a FLAG, why is `FLAG_RCV` a better place to go than `START`?
- Should the machine accept *one specific* C value, or several (e.g. Tx in `llSend` can get
  RR0, RR1, REJ0 or REJ1)? If several, the machine needs to **remember which one** it saw.
- What does the machine do when `readByteSerialPort` returns 0 or -1 instead of 1?
- How does the timer (§10) get the machine out of an endless wait?

For **I frames** ([PRES] slide 21) the first part is identical up to BCC1, but after BCC1 comes a
**data-collection state**: keep storing bytes until the closing FLAG. That FLAG is unambiguous
because stuffing guarantees `0x7E` never appears inside the frame. Only **then** do you destuff
and check BCC2, since the BCC2 byte is simply the last byte before the flag.

```
 START ─FLAG─► FLAG_RCV ─A─► A_RCV ─C(0x00|0x80)─► C_RCV ─BCC1 ok─► DATA ─┐
                                                                          │ any byte ≠ FLAG:
                                                                          │ store it, stay
                                                                          ▼
                                                              FLAG ─► STOP (destuff, check BCC2)
```

---

## 7. Information Transfer / Stop-and-Wait

### 7.1 The core idea

**Stop-and-Wait** ([DLG] slide 8: *"unit window and modulo 2 numbering"*):

> Send one frame, **stop**, and **wait** until it's acknowledged before sending the next.

At most **one I frame is outstanding** (unacknowledged) at any time. It is simple and robust.
The downside is that the line sits idle while waiting for the ACK, which is what the efficiency
study in [DLG] slide 31 measures.

### 7.2 Sequence numbers

With only one outstanding frame, one bit is enough to tell "this frame" from "the previous
frame". Frames alternate 0, 1, 0, 1, …

- **Ns** (send sequence number), in the I frame's C field: "this is frame #Ns".
- **Nr** (receive sequence number), in the RR/REJ C field: "I am now **expecting** frame #Nr".

So `RR(Nr)` means "everything before Nr is fine, send Nr". After I0 is accepted the receiver
sends **RR1**. After I1, **RR0**.

```
      Tx                                        Rx  (expects 0)
      │ I0 [packet A] ─────────────────────────►│ new, OK → deliver A, expect 1
      │◄───────────────────────────────── RR1   │
      │ I1 [packet B] ─────────────────────────►│ new, OK → deliver B, expect 0
      │◄───────────────────────────────── RR0   │
      │ I0 [packet C] ─────────────────────────►│ new, OK → deliver C, expect 1
      │◄───────────────────────────────── RR1   │
```

### 7.3 Receiver rules (from [DLG] slide 15 and [PRES] slides 14, 22)

Let `expected` be the Ns the receiver is waiting for.

| Situation | Data | Reply | Change `expected`? |
|---|---|---|---|
| **Header bad** (BCC1 wrong) | discard | **none**, ignore completely | no |
| Header OK, **Ns == expected**, BCC2 **OK** (*new frame*) | **deliver** to application | **RR(expected ⊕ 1)** | **yes**, flip |
| Header OK, **Ns == expected**, BCC2 **bad** (*new frame, corrupted*) | discard | **REJ(expected)** | no |
| Header OK, **Ns ≠ expected**, BCC2 OK (*duplicate*) | discard | **RR(expected)** | no |
| Header OK, **Ns ≠ expected**, BCC2 bad (*duplicate, corrupted*) | discard | **RR(expected)** | no |

### 7.4 Transmitter rules

After sending I(Ns), Tx waits for one of these:

| Tx receives | Meaning | Tx action |
|---|---|---|
| **RR(Ns ⊕ 1)** | frame accepted | success. Flip Ns, return from `llSend` |
| **REJ(Ns)** | frame had data errors | **retransmit immediately** (counts toward retries) |
| RR(Ns) or REJ(Ns ⊕ 1) | stale/unexpected reply | not an ACK for the current frame. Think about what's safe (usually: keep waiting) |
| frame with bad BCC1 | corrupted reply | ignore (as if nothing arrived) |
| **nothing before timeout** | frame or reply lost | **retransmit**, up to the limit |
| retry limit exceeded | link is dead | give up: `llSend` returns -1 |

### 7.5 Why duplicates happen and why they must be ACKed

```
      Tx                                        Rx  (expects 1)
      │ I1 [packet B] ─────────────────────────►│ new, OK → deliver B, expect 0
      │         ╳◄─────────────────────── RR0   │   (RR0 lost!)
      │ …timeout…                               │
      │ I1 [packet B] ─────────────────────────►│ Ns=1 ≠ expected 0 → DUPLICATE
      │                                         │ discard (don't deliver B twice!)
      │◄───────────────────────────────── RR0   │ but re-ACK with RR0
      │ I0 [packet C] ─────────────────────────►│
```

- Without **duplicate detection**, packet B would be written to the file twice, which corrupts it.
- Without **re-ACKing the duplicate**, Tx would never get its RR0 and would retry until it gives
  up, even though Rx actually has the data.

### 7.6 REJ in action

```
      Tx                                        Rx  (expects 0)
      │ I0 [packet A] ──── noise flips a data bit ──►│ BCC1 ok, BCC2 WRONG, Ns == expected
      │◄──────────────────────────────────────  REJ0 │ discard data
      │ I0 [packet A] (resent at once, no timeout) ──►│ OK → deliver A
      │◄──────────────────────────────────────  RR1  │
```

REJ is an optimisation. Tx could just wait for the timeout, but REJ saves `timeout` seconds
(4 s here!) per corrupted frame.

### 7.7 Why only one frame is outstanding

- A 1-bit sequence number can only tell "current" from "previous". With two unacknowledged frames
  it couldn't tell them apart.
- The receiver needs no buffer for out-of-order frames. It accepts exactly the expected one.
- The API enforces it anyway: `llSend` is synchronous. It returns only once its frame is
  acknowledged (or it gives up), so the application can't hand over the next packet earlier.

---

## 8. Error Detection

### 8.1 How BCC works

BCC = **Block Check Character**, an XOR over bytes. [DLG] slide 14 describes it as "even parity"
per bit position: the BCC is chosen so that, **counting the protected bytes plus the BCC itself,
every bit position has an even number of 1s**. XOR-ing all the bytes gives exactly that.

The check therefore amounts to: XOR the protected bytes, compare with the received BCC. (Or,
equivalently, XOR everything *including* the BCC and expect `0x00`.)

### 8.2 BCC1: protects the header

- **Protects:** A and C.
- **Computed:** by the sender of *every* frame (S, U, and I), as `A ⊕ C`.
- **Checked:** by the receiver, as soon as the BCC1 byte arrives (inside the state machine), and
  before trusting C.

Example (SET):

```
A    = 0x03 = 0000 0011
C    = 0x03 = 0000 0011
             ─────────── XOR
BCC1 = 0x00 = 0000 0000
```

Example (RR1):

```
A    = 0x03 = 0000 0011
C    = 0xAB = 1010 1011
             ─────────── XOR
BCC1 = 0xA8 = 1010 1000
```

If noise flips bit 0 of C in transit (0xAB → 0xAA, i.e. RR1 → RR0), the receiver computes
03 ⊕ AA = A9 ≠ A8, and the frame is rejected. That flip would otherwise have caused a wrong
protocol decision.

### 8.3 BCC2: protects the data field

- **Protects:** D1 … DN, the whole application packet in an I frame.
- **Computed:** by Tx in `llSend`, over the **original (unstuffed)** data, **before** stuffing.
- **Checked:** by Rx in `llReceive`, over the data **after destuffing**.

Example: data = `01 02 03`

```
01 = 0000 0001
02 = 0000 0010   01⊕02 = 0000 0011 = 03
03 = 0000 0011   03⊕03 = 0000 0000 = 00   → BCC2 = 0x00
```

Example: data = `41 7E 10`

```
41 ⊕ 7E = 0100 0001 ⊕ 0111 1110 = 0011 1111 = 3F
3F ⊕ 10 = 0011 1111 ⊕ 0001 0000 = 0010 1111 = 2F   → BCC2 = 0x2F
```

### 8.4 What to do when corruption is detected

| Detected | Where | Reaction |
|---|---|---|
| Bad BCC1 on any frame | Tx or Rx | **Ignore the frame completely, no reply** ([DLG] slide 15). Go back to hunting for a flag. The sender's timer handles recovery. |
| Bad BCC2, new I frame | Rx | Discard data, send **REJ(Ns)** |
| Bad BCC2, duplicate I frame | Rx | Discard data, send **RR(expected)** |

Why ignore a bad header entirely? Because once the header is untrustworthy you don't know what
the frame *was*, not even its sequence number, so any reply could be wrong.

### 8.5 Limitations (worth knowing for the quiz/report)

XOR parity detects any **odd** number of bit errors in a given bit position. It **misses** two
flips in the same bit position of different bytes (they cancel out). The cable's `ber` mode flips
at most one bit per byte, but in a long data field two bytes can each get a flipped bit in the
same position. Real protocols use CRCs for this reason. For this lab, XOR is what's specified.

---

## 9. Byte Stuffing / Transparency

### 9.1 Why it's needed

The data field carries **arbitrary bytes** (a GIF can contain any of the 256 values). If
`0x7E` shows up inside the data, the receiver would take it for the closing FLAG and cut the
frame short. You need a way to send any byte value without it being mistaken for a flag. This is
called **transparency** ([DLG] slide 16).

### 9.2 The two special bytes

| Byte | Name | Meaning |
|---|---|---|
| `0x7E` (01111110) | **FLAG** | frame boundary. May appear *only* as a delimiter. |
| `0x7D` (01111101) | **ESCAPE** | "the next byte is an escaped (XOR 0x20) version of a special byte" |

ESCAPE itself has to be escaped too, otherwise a real `0x7D` in the data would be taken for the
start of an escape sequence.

### 9.3 The transformation (PPP-style, [DLG] slide 17)

**Stuffing (Tx), applied to data bytes and BCC2:**

| Original byte | Sent as | Why |
|---|---|---|
| `0x7E` | `0x7D 0x5E` | `0x7E ⊕ 0x20 = 0x5E` |
| `0x7D` | `0x7D 0x5D` | `0x7D ⊕ 0x20 = 0x5D` |
| anything else | unchanged | |

**Destuffing (Rx):**

| Received | Becomes |
|---|---|
| `0x7D 0x5E` | `0x7E` |
| `0x7D 0x5D` | `0x7D` |
| anything else | unchanged |

After stuffing, **the only `0x7E`s on the wire are real flags**. That's what makes the receiver's
"collect until FLAG" state work.

### 9.4 Order of operations: stuffing and BCC2

From [DLG] slide 17 and [PRES] slide 17 note:

```
TX:  packet ──► compute BCC2 over ORIGINAL bytes ──► stuff (data + BCC2) ──► add F A C BCC1 … F
RX:  … F ──► strip F A C BCC1 … F ──► destuff (data + BCC2) ──► compute BCC2 over RESULT, compare
```

- BCC2 is computed **before** stuffing and checked **after** destuffing, so it always covers the
  *original* bytes.
- **BCC2 itself can be `0x7E` or `0x7D`**, so it must be stuffed too. ([DLG] slide 17: *"even
  if some octet (including the BCC itself) has to be replaced"*.)
- The header (A, C, BCC1) never takes those values (see §5.4), and the flags are never stuffed.

### 9.5 Worked examples

**Example A: a flag inside the data**

```
packet (original):     41 7E 10
BCC2:                  41⊕7E⊕10 = 2F
data+BCC2 (original):  41 7E 10 2F
after stuffing:        41 7D 5E 10 2F
full I0 frame on wire: 7E 03 00 03 41 7D 5E 10 2F 7E
                       F  A  C  B1 └──stuffed──┘  F
```

Rx collects `41 7D 5E 10 2F` between the header and the closing flag, destuffs it to
`41 7E 10 2F`, takes the last byte `2F` as BCC2, XORs `41 7E 10` → `2F`. It matches.

**Example B: the BCC2 itself needs stuffing**

```
packet (original):     7D 03
BCC2:                  7D⊕03 = 0111 1101 ⊕ 0000 0011 = 0111 1110 = 7E   ← a flag value!
data+BCC2 (original):  7D 03 7E
after stuffing:        7D 5D 03 7D 5E
full I1 frame on wire: 7E 03 80 83 7D 5D 03 7D 5E 7E
```

If you forgot to stuff BCC2, Rx would see `… 03 7E 7E` and end the frame one byte early.

### 9.6 Buffer sizing (something to think about, not code)

In the worst case every byte gets doubled. If `llSend` receives up to `MAX_PAYLOAD_SIZE` = 1000
bytes, the data+BCC2 portion can grow to 2 × 1001 bytes on the wire, plus the 5 header/trailer
bytes. Size your frame buffers (on both sides) with that in mind.

---

## 10. Timeouts and Retransmissions

### 10.1 Why Tx needs a timer

Tx sends a frame and waits for a reply. If the frame **or** the reply is lost (cable off) or
corrupted (bad BCC1 → silently ignored), the reply never comes. Without a timer, Tx would wait
forever. The timer turns "no reply" into an event Tx can act on.

```
            send frame (SET / I / DISC)
                     │
                     ▼
               start timer (timeout s)
                     │
                     ▼
        ┌──── wait for reply bytes ◄────────────┐
        │            │                          │
   valid reply    timer fires             invalid/other bytes
        │            │                          │
        ▼            ▼                          │
   stop timer   tries left? ──no──► give up     │
   continue          │ yes          (return -1) │
                     ▼                          │
               retransmit same frame ───────────┘
               (restart timer)
```

Which frames are timer-protected? **I, SET, DISC**, i.e. all the *commands* ([DLG] slides 15, 19).
Replies (UA, RR, REJ) are never retransmitted on their own. They're only re-sent in *response* to
a retransmitted command.

(So Rx also needs the timer for the **DISC it sends** in `llCloseRx`.)

### 10.2 The configured values

| Where | Name | Value | Meaning |
|---|---|---|---|
| `main.c` | `TIMEOUT` → `LinkLayer.timeout` | **4** | seconds to wait for a reply before retransmitting |
| `main.c` | `N_TRIES` → `LinkLayer.nRetransmissions` | **3** | limit on attempts |

**Ambiguity to settle:** `main.c` calls it "Number of *tries*" (3 transmissions in total?), while
`link_layer.h` calls it "Number of *retries*"/`nRetransmissions` (1 original + 3 retransmissions
= 4 total?). [DLG] slide 15 says "*a maximum number of retransmission attempts… (for example,
three)*". Either reading is defensible. Pick one, apply it **consistently**, and state it in your
report or ask your lab teacher. Also decide whether a REJ-triggered resend counts toward the
same limit.

With timeout = 4 s and ~3–4 attempts, a cable that is `off` for up to ~12 s should be survivable.

### 10.3 How `alarm()` and signals fit in

The course materials refer to an `alarm_sigaction.c` example ([DLG] slide 31). The concept:

- `alarm(n)` asks the kernel to send your process **SIGALRM** after *n* seconds. `alarm(0)`
  cancels a pending alarm.
- A **signal handler** is a function you register (via `sigaction`) that the kernel calls when the
  signal arrives. It should do very little, typically just set a flag your main loop checks.
- **Interaction with the blocking read (important):** as explained in §4.4,
  `readByteSerialPort` **blocks** until a byte arrives. When SIGALRM is delivered during that
  block, what happens depends on how the handler was installed:
  - installed **without** `SA_RESTART` → the blocked `read` **returns -1** (`errno == EINTR`),
    your loop wakes up, sees the flag, and can retransmit.
  - installed **with** restart semantics (e.g. the old `signal()` call on Linux/macOS behaves
    this way) → the `read` is **silently restarted** and keeps blocking forever. Your timeout
    appears to "never fire".

  Understanding this is the key to Milestone 4. Read the course's `alarm_sigaction.c` and check
  which flags it uses.

Other things to reason about (no answers here, on purpose):

- When exactly should the alarm be **cancelled**? (What if the ACK arrives 0.1 s before it fires?)
- The flag set by the handler has to be reset at the right moment. When?
- Should the timer restart after each byte, or only after each (re)transmission?

### 10.4 Timeline examples

**Lost I frame (cable off briefly):**

```
 t=0   Tx: I0 ──────────╳ (lost)
 t=4   Tx: timeout #1 → I0 ──────────╳
 t=8   Tx: timeout #2 → I0 ────────────────► Rx: new → deliver, RR1
       Tx ◄──────────────────────────────── RR1
       Tx: llSend returns success
```

**Link dead (cable off for good):**

```
 t=0  I0   t=4 I0   t=8 I0   t=12 (I0?)  → attempts exhausted → llSend returns -1
                                            → application should abort cleanly
```

---

## 11. Application Layer

### 11.1 Its job, once the link layer works

The application treats `llSend`/`llReceive` as a reliable, in-order packet pipe, and on top of it
implements a very small **file transfer protocol** ([DLG] slides 26–28):

- **Tx:** announce the file (START), send its contents in pieces (DATA), announce the end (END).
- **Rx:** create the file on START, append each DATA piece, finish on END.

### 11.2 Packet vs. frame: two different envelopes

```
Application layer builds:     ┌─ PACKET ──────────────────────────────┐
                              │ C │ … packet header … │ file fragment │
                              └───────────────────────────────────────┘
                                         │ llSend(packet, len)
                                         ▼
Link layer wraps it:  ┌─ FRAME ─────────────────────────────────────────────────────┐
                      │ F │ A │ C │ BCC1 │  stuffed( PACKET … + BCC2 )  │ F │
                      └─────────────────────────────────────────────────────────────┘
```

| | Application **packet** | Data link **frame** |
|---|---|---|
| Built by | `application_layer.c` | `link_layer.c` |
| Contents | control info or a file fragment | a whole packet, or nothing (S/U frames) |
| "C" field means | packet type: 1 START, 2 DATA, 3 END | frame type + sequence number |
| Integrity | L2/L1 length field for a sanity check | BCC1, BCC2 |
| Knows about the other layer? | **No.** Never sees flags, stuffing, Ns | **No.** Never parses the packet |

Both have a field called "C", but they are **completely unrelated**. The link layer must not
look at the packet's C.

### 11.3 Packet formats ([DLG] slide 28)

**Data packet:**

```
┌─────┬─────┬─────┬────┬────┬─────┬────┐
│  C  │ L2  │ L1  │ P1 │ P2 │ ... │ PK │
└─────┴─────┴─────┴────┴────┴─────┴────┘
  C = 2 (data)
  K = 256 × L2 + L1     (number of data bytes; L2 is the high byte, L1 the low byte)
  P1…PK = file fragment
```

**Control packet (START / END):**

```
┌─────┬────┬────┬──────────┬────┬────┬──────────┬─────┐
│  C  │ T1 │ L1 │ V1 …     │ T2 │ L2 │ V2 …     │ ... │
└─────┴────┴────┴──────────┴────┴────┴──────────┴─────┘
  C = 1 (START) or 3 (END)
  Each parameter is TLV:
    T (1 byte): 0 = file size, 1 = file name, others = your own extension
    L (1 byte): length of V in bytes
    V (L bytes): the value
```

Rules from [DLG] slide 27:

- START **must** contain the file size and **may** contain the file name.
- END **repeats** the information from START (useful as a final consistency check).
- DATA packets carry the 2-byte length field "to allow for additional checks regarding data
  integrity".

**Design decisions left to you** (both ends must agree):

- How many bytes represent the file size in V, and in which byte order?
- Fragment size: the whole packet must fit in `MAX_PAYLOAD_SIZE` (1000). A DATA packet has
  3 header bytes, so a fragment can be up to 997 bytes.
- What should Rx do if START and END disagree, or if the bytes received don't add up to the
  announced size?

The 2026/2027 data packet has **no sequence-number field** (only C, L2, L1). Older years' specs
you find online may include one, but follow this year's format.

### 11.4 From file to serial bytes, and back

```
TX side                                                    RX side
───────                                                    ───────
penguin.gif (10 968 bytes)                                 penguin-received.gif
   │ read in fragments (≤ 997 B)                                ▲ write/append fragments
   ▼                                                            │
fragments                                                  fragments
   │ add C=2, L2, L1                                            ▲ strip C, L2, L1 (check K)
   ▼                                                            │
application packets (START, DATA×n, END)                   application packets
   │ llSend(packet) one at a time                               ▲ llReceive(packet) returns one
   ▼                                                            │
BCC2 → stuffing → header/flags → I frame (Ns 0/1)          check BCC1, Ns, destuff, BCC2
   │ writeBytesSerialPort                                       ▲ readByteSerialPort (byte by byte)
   ▼                                                            │
serial bytes ═════════════ RS-232 / virtual cable ══════════════╝
```

For penguin.gif with 997-byte fragments: 10 968 = 11 × 997 + 1, so there would be
**12 DATA packets** (the last one carries a single byte), plus START and END, which makes
**14 I frames**. At 9600 baud (≈960 bytes/s), the transfer takes at least ~12 s even without
errors.

---

## 12. Full End-to-End Example

Imaginary file `hi.txt` with 5 bytes, `48 45 4C 4C 4F` ("HELLO"). For illustration the fragment
size is set to 3 bytes, and the file size is encoded in 1 byte (your real design may use more;
see §11.3). One cable glitch is included.

| # | Layer | Tx | Wire | Rx |
|---|---|---|---|---|
| **Connection establishment** |
| 1 | App | calls `llOpenTx` | | calls `llOpenRx` |
| 2 | DLL | `openSerialPort`, send SET, start timer | `7E 03 03 00 7E` → | state machine: SET ok |
| 3 | DLL | UA ok, stop timer, return 0 | ← `7E 03 07 04 7E` | send UA, return 0 |
| **START packet** |
| 4 | App | builds START: `01 00 01 05 01 06 68 69 2E 74 78 74` (C=1; T=0 L=1 V=5; T=1 L=6 V="hi.txt") → `llSend` | | waiting in `llReceive` |
| 5 | DLL | BCC2 over the 12 bytes, stuff (no special bytes here), I frame Ns=0 | `7E 03 00 03 <12 B> <BCC2> 7E` → | BCC1 ok, Ns=0 = expected, BCC2 ok → new |
| 6 | DLL | RR1 ok → Ns := 1, `llSend` returns | ← `7E 03 AB A8 7E` | send RR1, expected := 1, return 12 |
| 7 | App | | | sees C=1: reads size 5 and name, creates output file |
| **DATA packet 1** |
| 8 | App | reads "HEL", builds `02 00 03 48 45 4C` → `llSend` | | `llReceive` |
| 9 | DLL | I frame Ns=1 | `7E 03 80 83 02 00 03 48 45 4C <BCC2> 7E` → noise flips one data bit | BCC1 ok, Ns=1 = expected, **BCC2 bad** |
| 10 | DLL | REJ1 → retransmit now | ← `7E 03 55 56 7E` | discard, send REJ1 |
| 11 | DLL | same I1 again | `7E 03 80 83 … 7E` → | BCC2 ok → new |
| 12 | DLL | RR0 ok → Ns := 0, return | ← `7E 03 AA A9 7E` | send RR0, expected := 0, return 6 |
| 13 | App | | | C=2, K=3, appends "HEL" |
| **DATA packet 2** |
| 14 | App | reads "LO", builds `02 00 02 4C 4F` → `llSend` | | |
| 15 | DLL | I frame Ns=0 | `7E 03 00 03 02 00 02 4C 4F <BCC2> 7E` → | new, ok → send RR1 |
| 16 | DLL | **RR1 lost** (cable glitch) … 4 s … timeout | ← ~~RR1~~ | (already returned packet, appended "LO") |
| 17 | DLL | retransmit I0 | `7E 03 00 03 … 7E` → | Ns=0 ≠ expected 1 → **duplicate**: discard, **re-send RR1**, keep waiting |
| 18 | DLL | RR1 ok → Ns := 1, return | ← `7E 03 AB A8 7E` | |
| **END packet** |
| 19 | App | builds END: `03 00 01 05 01 06 68 69 2E 74 78 74` → `llSend` | | |
| 20 | DLL | I frame Ns=1 → RR0 | ⇄ | new → RR0, return |
| 21 | App | | | C=3: stop loop, (optionally) check size 5 == bytes written, close file |
| **Connection termination** |
| 22 | App | calls `llCloseTx` | | calls `llCloseRx` |
| 23 | DLL | send DISC (A=03), start timer | `7E 03 0B 08 7E` → | DISC ok |
| 24 | DLL | DISC ok, stop timer | ← `7E 01 0B 0A 7E` | send DISC (A=**01**), start timer |
| 25 | DLL | send UA (A=**01**) | `7E 01 07 06 7E` → | UA ok, stop timer |
| 26 | DLL | print stats, `closeSerialPort` | | print stats, `closeSerialPort` |

Throughout, **the application layer never sees** rows 9–12 or 16–18. From its point of view
`llSend` just took a bit longer.

---

## 13. Already Implemented vs. What I Must Implement

| Component | Already implemented? | My responsibility | File |
|---|---|---|---|
| Argument parsing, validation, constants | ✅ Yes | none (don't modify) | `src/main.c` |
| Application-layer entry prototype | ✅ Yes | none (don't modify) | `src/application_layer.h` |
| `LinkLayer` struct, DLL API, `MAX_PAYLOAD_SIZE` | ✅ Yes | none (don't modify) | `src/link_layer.h` |
| Serial port open/config/read/write/close | ✅ Yes | none (don't modify) | `src/serial_port.c/.h` |
| Virtual cable (socat, noise, on/off, logging) | ✅ Yes | use it for testing | `cable/cable.c` |
| Build/run targets | ✅ Yes | use them | `Makefile` |
| Filling the `LinkLayer` struct, tx/rx dispatch | ✅ Partially (skeleton) | keep and extend | `src/application_layer.c` |
| Milestone-1 string exchange | ✅ Demo exists | run and understand it, then replace | `src/link_layer.c` (`llOpenTx`/`llOpenRx`) |
| SET/UA connection establishment + state machine | ❌ No | **implement** | `src/link_layer.c` |
| Supervision frame build/parse | ❌ No | **implement** | `src/link_layer.c` |
| I-frame construction, BCC1/BCC2 | ❌ No | **implement** | `src/link_layer.c` |
| Byte stuffing / destuffing | ❌ No | **implement** | `src/link_layer.c` |
| Stop-and-Wait (Ns/Nr, RR/REJ, duplicates) | ❌ No (`llSend`/`llReceive` return 0) | **implement** | `src/link_layer.c` |
| Timer (alarm/signal) + retransmission | ❌ No | **implement** | `src/link_layer.c` |
| DISC/DISC/UA termination | ❌ No (`llCloseTx`/`llCloseRx` return 0) | **implement** | `src/link_layer.c` |
| Protocol statistics | ❌ No | **implement** (choose what to count) | `src/link_layer.c` |
| START/DATA/END packets, TLV | ❌ No | **implement** | `src/application_layer.c` |
| File reading/fragmenting and writing | ❌ No | **implement** | `src/application_layer.c` |
| Efficiency measurements (FER, T_prop, C, frame size) | ❌ No | **measure for report** | outside code / cable |

---

## 14. Recommended Implementation Order

This follows the official milestones M1–M5 ([DLG] slide 3). The last phase covers the report.

### Phase 0: Understand the existing code (before M1)
- **Understand:** this guide, §2–§4. Why is the fd hidden? Why non-canonical mode?
- **Work on:** nothing. Just read `main.c`, `application_layer.c`, `link_layer.c`, `serial_port.c`.
- **Observable proof:** you can explain, without looking, what happens when you run `make run_tx`.
- **Test:** `make` compiles cleanly (`-Wall`: read every warning).

### Phase 1: Basic serial communication (**M1**: exchange strings)
- **Understand:** byte streams, blocking reads, why the return value of `readByteSerialPort` matters.
- **Work on:** run the existing demo in `llOpenTx`/`llOpenRx` without changes.
  Then (still as a learning step) make Rx store the bytes and print them, as its TODO says.
- **Observable proof:** Rx prints the 256 bytes Tx sent, including the `\n`.
- **Test before moving on:** works through the virtual cable (cable → rx → tx) and, in the lab,
  between the two PCs. Try `log bytes.txt` in the cable and read the hex log.

### Phase 2: Connection establishment (**M2**: SET/UA + state machine in `llOpenTx`/`llOpenRx`)
- **Understand:** §5.3–§5.4, §6. The A/C/BCC1 values for SET and UA. The state machine idea.
- **Work on:** `link_layer.c`: `llOpenTx`, `llOpenRx`, plus private helpers you design.
  The port must now **stay open** after `llOpen*` returns.
- **Observable proof:** Tx prints "UA received", Rx prints "SET received". The cable log shows
  `7E 03 03 00 7E` one way and `7E 03 07 04 7E` the other.
- **Test:** start Rx *after* Tx (does SET get lost? That's Phase 4's job). Send junk before the
  SET, or send the flag twice. Does the state machine still sync?

### Phase 3: Information frames (**M3**: Stop-and-Wait in `llSend`/`llReceive`)
- **Understand:** §7, §8, §9. Ns/Nr, BCC2, stuffing order.
- **Work on:** `llSend`, `llReceive`, stuffing/destuffing and BCC2 helpers.
  A tiny temporary test in `applicationLayer` that sends a few hand-made buffers is fine.
- **Observable proof:** buffers sent arrive identical, alternating I0/I1 ↔ RR1/RR0 in the log.
- **Test before moving on:** **send a buffer containing `0x7E` and `0x7D`**. Send a buffer
  whose BCC2 comes out as `0x7E`. Send the maximum size (1000 bytes). Check the stuffed bytes in
  the cable log.

### Phase 4: Timer and retransmission (**M4**)
- **Understand:** §10. `alarm`, `sigaction`, EINTR, the retry limit. Duplicates (§7.5).
- **Work on:** timer mechanism in `link_layer.c`, integrated into `llOpenTx` and `llSend`
  (and later the DISC exchanges). REJ handling. Duplicate handling in `llReceive`.
- **Observable proof:** with `off` for a few seconds mid-transfer, Tx prints timeouts and
  retransmissions, then continues after `on`. With `off` permanently, Tx gives up cleanly after
  the limit.
- **Test:** `ber 0.0001`, `ber 0.001`: you should see REJs and duplicate handling, and data still
  arrives intact. Start Tx before Rx: SET retries should cover it.

### Phase 5: Connection termination (part of **M5**)
- **Understand:** DISC/DISC/UA, the A field rule for Rx's DISC and Tx's UA (§5.3).
- **Work on:** `llCloseTx`, `llCloseRx`, statistics printing.
- **Observable proof:** both programs exit by themselves with a clean close and print stats.
- **Test:** lose the final UA or a DISC (`off` at the right moment). What does each side do?
  Neither should hang forever.

### Phase 6: Application layer / file transfer (**M5**)
- **Understand:** §11. Packet vs. frame. TLV. The design decisions in §11.3.
- **Work on:** `application_layer.c`, Tx branch and Rx branch.
- **Observable proof:** `make check_files` → *"Files … are identical"*.
- **Test:** small file, empty file (size 0?), file whose size is an exact multiple of the
  fragment size, `penguin.gif`.

### Phase 7: Integration and robustness testing
- Full `penguin.gif` transfer with: `off`/`on` during the transfer, `ber` > 0, different baud rates
  (remember to set `baud` in the cable **and** pass the same baud to both programs).
- In the lab: real cable between tux2 and the desk PC (`/dev/ttyS0` ↔ `/dev/ttyUSB0`), with
  GTKTerm **closed** ([FCG] step 3).

### Phase 8: Efficiency study and report ([DLG] slides 31–32)
- Vary FER (e.g. random error injection at the receiver), T_prop (`prop` in cable), C (`baud`),
  and frame size. Measure S = R/C and compare with the theoretical Stop-and-Wait efficiency.

---

## 15. Function Implementation Checklist

> Specifications only. How each function works internally is for you to design.

### `llOpenTx`, `src/link_layer.c`
- **Purpose:** open the port and establish the logical connection as the transmitter.
- **Inputs:** `LinkLayer llParameters` (port, baud, retries, timeout).
- **Returns:** `0` once a valid UA arrives; negative if the port can't be opened or every SET
  attempt goes unanswered.
- **Interacts with:** `openSerialPort`, `writeBytesSerialPort`, `readByteSerialPort`, your timer
  mechanism, your supervision-frame state machine. Leaves the port **open**.
- **Concepts first:** SET/UA formats, A/C/BCC1, byte-by-byte state machine, timer/retries.
- **Depends on:** M1 working (bytes flow through the cable). A receive state machine.

### `llOpenRx`, `src/link_layer.c`
- **Purpose:** open the port and wait for the transmitter to establish the connection.
- **Inputs:** `LinkLayer llParameters`.
- **Returns:** `0` after receiving a valid SET and sending UA; negative on port error.
- **Interacts with:** `openSerialPort`, `readByteSerialPort`, `writeBytesSerialPort`, state machine.
- **Concepts first:** same as `llOpenTx`. Also: what if the SET is retransmitted because the UA got lost?
- **Depends on:** M1; the state machine.

### `llSend`, `src/link_layer.c`
- **Purpose:** deliver one application packet reliably (Stop-and-Wait).
- **Inputs:** `buf` (packet bytes, opaque), `bufSize` (≤ `MAX_PAYLOAD_SIZE`).
- **Returns:** number of bytes written (per [DLG] slide 23; decide whether that means the packet
  size or the stuffed frame size, and be consistent); `-1` when retries run out or on error.
- **Interacts with:** `writeBytesSerialPort`, `readByteSerialPort`, timer, BCC2 and stuffing
  helpers, reply state machine, the stored Ns and retry/timeout parameters, statistics.
- **Concepts first:** I-frame format, Ns, BCC2 before stuffing, stuffing rules, RR/REJ meaning,
  timer, retry limit.
- **Depends on:** `llOpenTx` working; supervision-frame parsing (to read RR/REJ); the timer.

### `llReceive`, `src/link_layer.c`
- **Purpose:** return the next **new**, **error-free** packet, handling duplicates and errors
  internally.
- **Inputs:** `packet`, a caller-provided buffer (should hold `MAX_PAYLOAD_SIZE` bytes).
- **Returns:** packet length (> 0) on success; `-1` on error.
- **Interacts with:** `readByteSerialPort`, `writeBytesSerialPort`, I-frame state machine,
  destuffing, BCC2 check, the stored expected-Ns, statistics.
- **Concepts first:** §7.3 receiver rules table, destuffing, BCC2 after destuffing, why duplicates
  get RR, why bad headers get nothing.
- **Depends on:** `llOpenRx`; a state machine that can collect a variable-length data field.

### `llCloseTx`, `src/link_layer.c`
- **Purpose:** terminate the connection from the transmitter side, report statistics, release the port.
- **Inputs:** none (uses stored state).
- **Returns:** `0` on success; `-1` on failure (e.g. no DISC reply after all retries).
- **Interacts with:** `writeBytesSerialPort`, `readByteSerialPort`, timer, state machine,
  `closeSerialPort`, statistics.
- **Concepts first:** DISC → DISC → UA sequence, A field values (Rx's DISC and Tx's UA use `0x01`).
- **Depends on:** timer + supervision-frame parsing already solid.

### `llCloseRx`, `src/link_layer.c`
- **Purpose:** terminate from the receiver side, report statistics, release the port.
- **Inputs:** none.
- **Returns:** `0` on success; `-1` on failure.
- **Interacts with:** same set as `llCloseTx`.
- **Concepts first:** Rx's DISC is a **command**, so it is timer-protected. What if a
  retransmitted I frame (last RR lost) shows up while Rx is waiting for DISC?
- **Depends on:** `llReceive` and the timer.

### `applicationLayer`, Tx branch, `src/application_layer.c`
- **Purpose:** send `filename` over the link.
- **Inputs:** `serialPort`, `role == "tx"`, `baudRate`, `nTries`, `timeout`, `filename`.
- **Returns:** nothing (void). It should report failures and stop cleanly, e.g. not keep calling
  `llSend` after it failed.
- **Interacts with:** `llOpenTx`, `llSend`, `llCloseTx`; C file I/O.
- **Concepts first:** packet formats, TLV, fragment size vs `MAX_PAYLOAD_SIZE`, L2/L1 encoding.
- **Depends on:** a fully working link layer.

### `applicationLayer`, Rx branch, `src/application_layer.c`
- **Purpose:** receive a file and save it as `filename`.
- **Inputs:** same as above with `role == "rx"`.
- **Returns:** nothing.
- **Interacts with:** `llOpenRx`, `llReceive`, `llCloseRx`; C file I/O.
- **Concepts first:** parsing packets by C, TLV decoding, verifying K, START/END consistency.
- **Depends on:** a fully working link layer; the Tx branch (to have something to receive).

### Supporting pieces you'll likely create (private to `link_layer.c`, your design)
- A **supervision-frame sender** (A, C → 5 bytes).
- A **frame-reception state machine** (supervision and I-frame variants, or one that handles both).
- **BCC2** computation, **stuffing** and **destuffing** routines.
- A **timer** mechanism (signal handler + alarm management).
- **Link-layer state** (parameters, current Ns/expected Ns, statistics counters).

---

## 16. Glossary

| Term | Meaning |
|---|---|
| **Tx** | Transmitter: the program run with `tx`. Owns and sends the file, sends I frames, SET and the first DISC. |
| **Rx** | Receiver: the program run with `rx`. Receives and saves the file, sends UA, RR, REJ and a DISC. |
| **frame** | Link-layer unit on the wire, delimited by FLAGs, with a header and (for I frames) a data field. |
| **packet** | Application-layer unit (START/DATA/END). Carried inside the data field of one I frame. |
| **FLAG** | `0x7E`. Marks the start and end of every frame. |
| **A** | Address byte: `0x03` (commands from Tx / replies from Rx) or `0x01` (commands from Rx / replies from Tx). |
| **C** | Control byte: frame type and, for I/RR/REJ, the sequence number. |
| **BCC1** | `A ⊕ C`. Detects header errors. |
| **BCC2** | XOR of all (unstuffed) data bytes. Detects data-field errors in I frames. |
| **SET** | U frame (C=`0x03`) that Tx sends to open the connection. |
| **UA** | U frame (C=`0x07`) that acknowledges SET or DISC. |
| **DISC** | U frame (C=`0x0B`) requesting disconnection. Sent by both sides when closing. |
| **I frame** | Information frame (C=`0x00`/`0x80`) carrying one application packet. |
| **RR** | Receiver Ready (C=`0xAA`/`0xAB`). Positive ACK: "send frame Nr next". |
| **REJ** | Reject (C=`0x54`/`0x55`). Negative ACK: "frame Nr was corrupted, resend it". |
| **byte stuffing** | Replacing `0x7E`→`7D 5E` and `0x7D`→`7D 5D` inside a frame so the flag stays unique. Reversed on reception (destuffing). |
| **timeout** | Time (`TIMEOUT` = 4 s) Tx waits for a reply before assuming loss. |
| **retransmission** | Sending the same frame again after a timeout or a REJ, up to a configured limit (`N_TRIES` = 3). |
| **sequence number** | 1-bit counter (0/1): Ns in I frames, Nr in RR/REJ. Detects duplicates and tells new frames from old. |
| **Stop-and-Wait** | ARQ protocol with window 1: send one frame, wait for its ACK, then send the next. |
| **serial port** | Hardware interface that sends bytes one bit at a time. Accessed in Unix as `/dev/ttyS*` (or the virtual `/tmp/ttyS1*`). |
| **RS-232** | Physical-layer standard for serial communication (voltages, DB9 pins, TD/RD/GND…). Asynchronous: each byte is framed by start/stop bits. |
| **file descriptor** | Integer handle the kernel gives you from `open()`, used by `read`/`write`/`close`. Here it's the global `fd` in `serial_port.c`. |
| **termios** | POSIX struct and API (`tcgetattr`/`tcsetattr`) for configuring a serial/terminal device: speed, bits, parity, canonical mode, `VMIN`/`VTIME`. |

---

## 17. Discrepancies & Gotchas Found

Differences between the repository and the specification documents, plus traps worth knowing
about.

1. **Old function names in [PRES].** The additional-details presentation (dated 2024/2025, file
   named `rcom2025`) uses `llopen`, `llwrite`, `llread`, `llclose` with a single role-aware
   `llopen`. This year's code and [DLG] use **`llOpenTx`/`llOpenRx`, `llSend`, `llReceive`,
   `llCloseTx`/`llCloseRx`**. Follow `link_layer.h`. [PRES] also mentions "slides 40, 42, 46",
   which don't exist in the PDF provided. The state machine diagrams it refers to are its slides
   19–21.

2. **`serial_port.h` says reads time out, but they block.** The header says
   `readByteSerialPort` "wait[s] up to 0.1 second (VTIME)… 0 if no byte was received". But
   `serial_port.c` sets `VTIME = 0`, `VMIN = 1` and clears `O_NONBLOCK`, so **`read` blocks
   until a byte arrives**. [PRES] slide 18 likewise says to use `read` "in non-blocking mode".
   In practice your timeout mechanism must be able to interrupt a blocking read (signal without
   `SA_RESTART`, see §10.3). Handling the 0 return anyway costs nothing.

3. **`application_layer.h` documents a return value for a `void` function.** The doc comment
   says "@return 0 on success or -1 on error", but `applicationLayer` returns `void`. You can't
   change the header, so report errors by printing them and returning early.

4. **"Tries" vs "retransmissions".** `main.c` prints `N_TRIES` as "Number of tries".
   `link_layer.h` names the field `nRetransmissions` ("Number of retries"). [DLG] says
   "maximum number of retransmission attempts". Whether 3 means 3 total sends or 1 + 3 is
   ambiguous. Choose one and document it.

5. **[PRES] slide 14 vs. slide 22 / [DLG] slide 15 on duplicates.** Slide 14 says "else, ignore
   or send RR(Ns)". Slide 22 and [DLG] say a duplicate **must** be confirmed with RR indicating
   the frame the receiver **expects** (i.e. RR(Ns ⊕ 1) = RR(expected)). "Ignore" would cause Tx
   to exhaust its retries. Follow [DLG].

6. **[PRES] slide 14's `llclose` (Rx) stops at "send DISC".** [DLG] slide 24 shows the full
   DISC → DISC → UA sequence, with Rx receiving the final UA, and slide 15 says DISC is
   timer-protected. Follow [DLG].

7. **README binary name.** `README.txt` says `./bin/cable_app`, but the Makefile builds
   `bin/cable`. Use `make run_cable` or `./bin/cable`.

8. **Cable baud rate is separate from your program's.** `cable.c` always starts at **9600**
   whatever you pass to `main`. To test another speed, type `baud <rate>` in the cable console
   **and** pass the same rate to both programs.

9. **The demo `llOpen*` close the serial port.** Fine for M1, wrong for the real protocol: the
   port has to stay open until `llClose*`.

10. **`_POSIX_SOURCE` is defined after the includes** in `link_layer.c` and `serial_port.c`,
    so it has no effect. Harmless, and worth knowing if you ever wonder why some POSIX feature
    macro "isn't working".

11. **Running on macOS.** The cable needs `socat` (installed here at `/opt/homebrew/bin/socat`).
    `cable.c` already has a macOS branch for scheduling priority. Signal/`read` interrupt
    semantics are the same as Linux for `sigaction` without `SA_RESTART`, but the final demo
    runs on the lab's Linux PCs, so test there too.
