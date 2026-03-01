# Pico FPGA Debug Tool

Multi-function FPGA debug tool on RP2040, combining logic analyzer, UART
bridge, and GPIO control over a single USB CDC text protocol.

Designed for host automation — every interaction is a line of ASCII text,
making it straightforward to script from Python, shell, or an AI agent.

**Capabilities:**
- 4-channel PIO UART + 2 hardware UART instances
- Logic analyzer: up to 125 Msps, 50K-sample capture buffer, all 32 GPIO pins
- GPIO read/write with pin conflict detection
- Safe boot: all pins start as floating inputs

## Hardware

**Board:** Raspberry Pi Pico (RP2040). Any RP2040 board with USB should work.

**Usable GPIOs:** 0–22 and 26–28 (26 pins total).
GPIOs 23–25 are reserved (SMPS control, VBUS sense, LED on some boards).

**Pin constraints:**

| Constraint | Detail |
|---|---|
| HW UART0 TX | GPIO 0, 12, or 16 |
| HW UART0 RX | GPIO 1, 13, or 17 |
| HW UART1 TX | GPIO 4, 8, or 20 |
| HW UART1 RX | GPIO 5, 9, or 21 |
| PIO UART | Any valid GPIO for TX and RX |
| Logic analyzer | Captures all 32 GPIO bits; pin assignments select which to observe |

## Build & Flash

**Prerequisites:** [Pico SDK](https://github.com/raspberrypi/pico-sdk) 2.2.0+
with `PICO_SDK_PATH` set.

```
mkdir -p build && cd build
PICO_SDK_PATH=~/pico-sdk cmake ..
make -j$(nproc)
```

Output: `build/pico_fpga.uf2` (drag-and-drop) or `build/pico_fpga.elf` (SWD).

**Flash via OpenOCD (CMSIS-DAP probe):**

```
sudo openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg \
  -c "adapter speed 5000; program build/pico_fpga.elf verify reset exit"
```

## USB Setup

The Pico enumerates as a USB CDC serial device with product name
"Pico FPGA Debug Tool". The serial number is the board's unique flash ID.

**Connecting:**

```
stty -F /dev/ttyACM0 115200 raw -echo
cat /dev/ttyACM0 &
echo "PING" > /dev/ttyACM0
```

**udev rule (optional):** Create `/etc/udev/rules.d/99-pico-fpga.rules`:

```
SUBSYSTEM=="tty", ATTRS{idVendor}=="2e8a", ATTRS{serial}=="YOUR_SERIAL_HERE", \
  SYMLINK+="pico-la", MODE="0666"
```

Then `udevadm control --reload-rules && udevadm trigger`. Find your serial
with `udevadm info /dev/ttyACM0 | grep SERIAL`.

## Command Protocol

Line-based ASCII. Send a command terminated by `\n`. Responses are `\n`-terminated.

- Success: `OK` or `OK <data>`
- Error: `ERROR: <reason>`

Commands are case-insensitive. Maximum line length: 2048 bytes, up to 16 tokens.

---

### System Commands

| Command | Response |
|---|---|
| `PING` | `OK pico-fpga v1.0` |
| `STATUS` | `OK pio0=<sm>/<insn> pio1=<sm>/<insn> dma=<n> pins=<list>` |
| `RESET` | `OK` — deinits all modules, releases all pins |

STATUS pin list format: `<gpio>:<func>` separated by spaces, or `no pins assigned`.
UART pins include the instance: `10:UART_TX(pio0)`.

---

### PIN — Pin Assignment

All pins start as floating inputs. Assign a function before using a pin.
A pin must be released before reassigning.

```
PIN <gpio> FUNC <function> [options]
PIN <gpio> PULL <UP|DOWN|NONE>
PIN <gpio> RELEASE
PIN QUERY [gpio]
```

**Functions:**

| Function | Options | Notes |
|---|---|---|
| `INPUT` | `PULLUP`, `PULLDOWN` (optional) | Configures as GPIO input |
| `OUTPUT` | | Configures as GPIO output |
| `LA` | | Marks pin for logic analyzer observation |
| `UART_TX` | `<id>` (required) | `id`: pio0–pio3, hw0–hw1 |
| `UART_RX` | `<id>` (required) | `id`: pio0–pio3, hw0–hw1 |

**Examples:**

```
PIN 10 FUNC INPUT PULLUP  → OK
PIN 11 FUNC OUTPUT        → OK
PIN QUERY                 → OK 10:INPUT 11:OUTPUT
PIN QUERY 23              → OK 23:RESERVED
PIN 10 RELEASE            → OK
```

---

### GPIO — Read and Write

```
GPIO READ <gpio>           → OK 0  or  OK 1
GPIO WRITE <gpio> <0|1>    → OK
GPIO READ_ALL              → OK <8-hex-digit>
```

Pin must be assigned as INPUT (for READ) or OUTPUT (for WRITE).
READ_ALL returns a 32-bit hex snapshot of all GPIOs (bit 0 = GPIO 0).

---

### LA — Logic Analyzer

PIO-driven capture at up to 125 Msps. Each sample is a 32-bit snapshot of all
GPIO pins. Buffer holds 50,000 samples (200 KB).

```
LA INIT                              → OK
LA CAPTURE [samples] [divider]       → OK capturing
LA STATUS                            → OK idle | OK capturing | OK done <count>
LA DATA [offset] [count]             → OK <count>\n<hex data>
LA DEINIT                            → OK
```

**Sample rate:** `125 MHz / divider`. Divider must be >= 1.0 (integer or float).

| Divider | Sample Rate | Sample Period |
|---|---|---|
| 1 | 125 MHz | 8 ns |
| 125 | 1 MHz | 1 us |
| 12500 | 10 kHz | 100 us |

**CAPTURE defaults:** samples = 50000, divider = 1.0.

**DATA output format:** First line is `OK <count>`, followed by lines of up to
8 space-separated 32-bit hex values:

```
LA DATA 0 16
OK 16
00000400 00000400 00000400 00000400 00000400 00000400 00000400 00000400
00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
```

Each hex value is a GPIO snapshot: bit N = GPIO N.

**Resources:** 1 PIO SM, 1 instruction word, 1 DMA channel.

---

### UART — Serial Bridge

Up to 6 independent UART instances: 4 PIO-based (pio0–pio3) and 2 hardware
(hw0–hw1). All are 8N1 format.

**Setup:** Assign TX and RX pins with `PIN` before `INIT`.

```
UART <id> INIT <baud>            → OK
UART <id> DEINIT                 → OK
UART <id> BAUD <baud>            → OK
UART <id> SEND <hex>             → OK <bytes_sent>
UART <id> RECV [max] [timeout]   → OK <hex>
```

**Instance IDs:** `pio0`, `pio1`, `pio2`, `pio3`, `hw0`, `hw1`

**SEND:** Hex byte pairs, no spaces. Up to 256 bytes.
Example: `UART pio0 SEND 48656C6C6F` sends "Hello".

**RECV:** Returns received data as hex. Defaults: max = 256 bytes,
timeout = 100 ms. Returns immediately once data arrives, or empty on timeout.

**PIO UART resources:** 2 SMs per instance. TX program (4 insn) and RX program
(7 insn) are shared (reference-counted) within each PIO block.

**Example session:**

```
PIN 10 FUNC UART_TX pio0     → OK
PIN 11 FUNC UART_RX pio0     → OK
UART pio0 INIT 115200        → OK
UART pio0 SEND 48454C4C4F    → OK 5
UART pio0 RECV 256 500       → OK 574f524c44
UART pio0 DEINIT             → OK
```

---

## Architecture

### PIO Resource Budget

Each RP2040 PIO block has 4 state machines and 32 instruction slots.
The allocator (`pio_alloc`) tracks usage across both blocks.

| Module | SMs | Instructions | DMA | Notes |
|---|---|---|---|---|
| Logic analyzer | 1 | 1 | 1 | |
| PIO UART (each) | 2 | 11 (shared) | 0 | TX (4 insn) + RX (7 insn), ref-counted per PIO block |

A single PIO block can host: LA + 1 PIO UART (1+2 = 3 SMs, 1+11 = 12 insn).
The RP2040 has 12 DMA channels total.

### Memory Layout

- **Capture buffer:** 50,000 × 4 bytes = 200 KB.
- **UART RX buffers:** 512 bytes per instance (6 instances max = 3 KB).
- **Code + data:** ~75 KB.
- **Total RP2040 SRAM:** 264 KB.

### Safe Boot

All GPIOs initialize as floating inputs. No module claims resources until
explicitly told to via `INIT`. `RESET` returns to this safe state. Safe to
have FPGA connected on power-up.

### USB

Single USB CDC interface using the SDK's `stdio_usb`. Product name:
"Pico FPGA Debug Tool". Serial number: board unique flash ID. The main loop
polls for input with `getchar_timeout_us(100)` and polls all UART RX buffers
each iteration.

## Testing

Four test suites in `test/`. All use bash and communicate via serial.

### `test/run_tests.sh` — Regression (no wiring)

Exercises all command paths, error conditions, and state transitions without
requiring any external wiring.

```
./test/run_tests.sh [/dev/ttyACM1]
```

Covers: system commands, pin management (including reserved pin rejection and
conflict detection), GPIO read/write, LA init/capture/data cycle, UART error
paths, and RESET cleanup verification.

### `test/test_uart_loopback.sh` — UART Loopback

Tests real data transfer between PIO and hardware UART instances, including
LA capture of UART waveforms.

**Wiring:**

```
Pin 14 (GP10, PIO pio0 TX)  ──────  Pin 12 (GP9, HW UART1 RX)
Pin 15 (GP11, PIO pio0 RX)  ──────  Pin 11 (GP8, HW UART1 TX)
```

```
./test/test_uart_loopback.sh [/dev/ttyACM1]
```

### `test/test_la_gpio_loopback.sh` — LA + GPIO Loopback

Tests LA capture of GPIO output transitions. Verifies static levels,
independent pin control, and mid-capture toggling.

**Wiring:**

```
Pin 1 (GP0, output)  ──────  Pin 9 (GP6, LA input)
Pin 2 (GP1, output)  ──────  Pin 10 (GP7, LA input)
```

```
./test/test_la_gpio_loopback.sh [/dev/ttyACM1]
```

### `test/test_pinout_verify.sh` — FPGA Pin Connectivity

Verifies all 16 Pico GP0–15 to FPGA JP1 header connections bidirectionally.
Requires FPGA running `fpga/pin_test` design with UART on CP2102N.

**Wiring:** Pico GP0–15 to JP1 header pins 3–18. Physical wiring is reversed
(GP0 connects to JP1 pin 17, GP15 to pin 4). The test uses a mapping table.

```
./test/test_pinout_verify.sh [/dev/ttyACM1] [/dev/ttyUSB0]
```

## FPGA Test Designs

### `fpga/pin_test` — JP1 Header Pin Test (EP4CGX150)

Verilog design for QMTech EP4CGX150DF27 + DB_FPGA daughter board. UART-controlled
GPIO test for verifying Pico-to-FPGA pin connectivity.

**FPGA UART commands (115200 8N1 on CP2102N):**

| Command | Response | Notes |
|---|---|---|
| `R` | `XXXX` (hex) | Read all 16 JP1 pins, bit0 = jp1[0] |
| `Oxxxx` | `XXXX` (readback) | Set output enable mask |
| `Dxxxx` | `XXXX` (readback) | Set output data |
| `I` | `OK` | All pins back to input (hi-Z) |
| `W` | `NN:XXXX` per pin | Walk mode: drive each pin HIGH, 100ms each |

**Build:** Requires Quartus 25.1 (Lite Edition).

```
cd fpga/pin_test
quartus_sh --flow compile pin_test
quartus_cpf -c output_files/pin_test.sof output_files/pin_test.rbf
openFPGALoader -c usb-blaster output_files/pin_test.rbf
```

## Project Structure

```
pico-fpga/
├── CMakeLists.txt             Build config, SDK libraries, PIO programs
├── pico_sdk_import.cmake      SDK bootstrap (copied from Pico SDK)
├── main.c                     Main loop, command dispatch, PING/STATUS/RESET
├── cmd.h/c                    Line-based command parser (2048-byte buffer, 16 tokens)
├── pin_manager.h/c            Pin assignment, validation, conflict detection
├── pio_alloc.h/c              PIO SM/instruction/DMA resource tracker
├── gpio_cmd.h/c               GPIO READ, WRITE, READ_ALL
├── la.h/c                     Logic analyzer (PIO+DMA capture, hex output)
├── logic_analyzer.pio         PIO capture program (1 instruction)
├── uart_tx.pio                PIO UART TX program (4 instructions, 8N1)
├── uart_rx.pio                PIO UART RX program (7 instructions, 8N1)
├── uart/
│   ├── pio_uart.h/c           PIO UART instances (pio0–pio3), command dispatch
│   └── hw_uart.h/c            Hardware UART instances (hw0–hw1)
├── fpga/
│   └── pin_test/              EP4CGX150 JP1 header pin connectivity test
│       ├── pin_test.v         Verilog design (UART + tristate GPIO)
│       ├── pin_test.qsf       Quartus pin assignments
│       ├── pin_test.qpf       Quartus project file
│       └── pin_test.sdc       Timing constraints (50 MHz clock)
└── test/
    ├── run_tests.sh           Regression suite (no wiring required)
    ├── test_uart_loopback.sh  UART loopback with LA capture
    ├── test_la_gpio_loopback.sh  LA + GPIO loopback test
    └── test_pinout_verify.sh  FPGA pin connectivity verification
```
