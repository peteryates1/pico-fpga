#!/bin/bash
# Pin connectivity verification: Pico GP0-15 <-> FPGA JP1[0:15]
#
# Requires:
#   - Pico running pico-fpga on $PICO_DEV
#   - FPGA running pin_test design, UART on $FPGA_DEV (DB_FPGA CP2102N)
#   - Pico GP0-15 wired to JP1 header pins 3-18
#
# Physical wiring (connected in order of wiring convenience):
#   Pico GP0  -> JP1 pin 17 -> FPGA jp1[14]
#   Pico GP1  -> JP1 pin 18 -> FPGA jp1[15]
#   Pico GP2  -> JP1 pin 15 -> FPGA jp1[12]
#   Pico GP3  -> JP1 pin 16 -> FPGA jp1[13]
#   ...pattern continues: pairs reversed...
#   Pico GP14 -> JP1 pin 3  -> FPGA jp1[0]
#   Pico GP15 -> JP1 pin 4  -> FPGA jp1[1]
#
# Phase 1: Pico drives each pin HIGH, FPGA reads (verifies Pico->FPGA path)
# Phase 2: FPGA drives each pin LOW, Pico reads (verifies FPGA->Pico path)

set -uo pipefail

PICO_DEV="${1:-/dev/ttyACM1}"
FPGA_DEV="${2:-/dev/ttyUSB0}"
PASS=0
FAIL=0
ERRORS=""

# Mapping: GP[n] -> FPGA jp1 bit number
# Wiring connects GP0 to JP1 pin17 (jp1[14]), GP1 to pin18 (jp1[15]), etc.
# Pattern: pairs are reversed from top to bottom of the header
GP_TO_JP1=(14 15 12 13 10 11 8 9 6 7 4 5 2 3 0 1)

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

# --- Pico helpers ---
stty -F "$PICO_DEV" 115200 raw -echo -hupcl 2>/dev/null
exec 3<>"$PICO_DEV"
while read -r -t 0.3 _line <&3; do :; done

pico_cmd() {
    echo -ne "$1\n" >&3
    sleep 0.05
    local line=""
    if read -r -t 2 line <&3; then
        echo "$line" | tr -d '\r'
    else
        echo "TIMEOUT"
    fi
}

# --- FPGA helpers (uses python serial for reliable CP2102N comms) ---
fpga_cmd() {
    python3 -c "
import serial, sys, time
s = serial.Serial('$FPGA_DEV', 115200, timeout=2)
s.reset_input_buffer()
s.write(sys.argv[1].encode())
time.sleep(0.15)
data = s.readline()
s.close()
if data:
    print(data.decode().strip())
else:
    print('TIMEOUT')
" "$1"
}

assert_eq() {
    local actual=$(echo "$3" | sed 's/[[:space:]]*$//')
    local expected=$(echo "$2" | sed 's/[[:space:]]*$//')
    if [ "$actual" = "$expected" ]; then
        echo -e "  ${GREEN}PASS${NC} $1"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC} $1"
        echo -e "       expected: '$expected'"
        echo -e "       actual:   '$actual'"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: $1"
    fi
}

echo "============================================"
echo " Pin connectivity verification"
echo " Pico GP0-15 <-> FPGA JP1[0:15]"
echo " pico: $PICO_DEV"
echo " fpga: $FPGA_DEV"
echo "============================================"
echo ""

# -----------------------------------------------------------
echo -e "${YELLOW}[setup]${NC} Reset Pico, set FPGA to input mode..."
# -----------------------------------------------------------
pico_cmd "RESET" > /dev/null
fpga_cmd "I" > /dev/null  # all FPGA JP1 pins to input

# Verify FPGA UART is alive
result=$(fpga_cmd "R")
if [ "$result" = "TIMEOUT" ]; then
    echo -e "  ${RED}ERROR${NC} FPGA UART not responding on $FPGA_DEV"
    exec 3>&-
    exit 1
fi
echo -e "  ${CYAN}INFO${NC} FPGA baseline read: $result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[1] Pico -> FPGA: walk each pin${NC}"
# -----------------------------------------------------------
# Set all Pico GP0-15 as outputs, all LOW
for pin in $(seq 0 15); do
    pico_cmd "PIN $pin FUNC OUTPUT" > /dev/null
    pico_cmd "GPIO WRITE $pin 0" > /dev/null
done

sleep 0.1

# Verify FPGA sees all LOW
result=$(fpga_cmd "R")
assert_eq "FPGA reads all LOW" "0000" "$result"

# Walk each pin HIGH one at a time
for pin in $(seq 0 15); do
    jp1_bit=${GP_TO_JP1[$pin]}
    pico_cmd "GPIO WRITE $pin 1" > /dev/null
    sleep 0.05
    result=$(fpga_cmd "R")
    expected=$(printf "%04X" $((1 << jp1_bit)))
    assert_eq "Pico GP$pin -> FPGA jp1[$jp1_bit]" "$expected" "$result"
    pico_cmd "GPIO WRITE $pin 0" > /dev/null
done

# Set all HIGH
for pin in $(seq 0 15); do
    pico_cmd "GPIO WRITE $pin 1" > /dev/null
done
sleep 0.05
result=$(fpga_cmd "R")
assert_eq "FPGA reads all HIGH" "FFFF" "$result"

# Release all Pico pins
for pin in $(seq 0 15); do
    pico_cmd "PIN $pin RELEASE" > /dev/null
done

# -----------------------------------------------------------
echo -e "\n${YELLOW}[2] FPGA -> Pico: walk each pin${NC}"
# -----------------------------------------------------------
# Set all Pico GP0-15 as inputs with pull-down (so undriven pins read LOW)
for pin in $(seq 0 15); do
    pico_cmd "PIN $pin FUNC INPUT" > /dev/null
    pico_cmd "PIN $pin PULL DOWN" > /dev/null
done

sleep 0.1

# FPGA drives all pins HIGH, then releases one at a time to verify each path
# First: drive all HIGH via FPGA
fpga_cmd "Offff" > /dev/null
fpga_cmd "Dffff" > /dev/null
sleep 0.05

# Verify Pico sees all HIGH
for pin in $(seq 0 15); do
    result=$(pico_cmd "GPIO READ $pin")
    if [ "$result" != "OK 1" ]; then
        echo -e "  ${CYAN}WARN${NC} GP$pin reads '$result' with all FPGA pins driven HIGH"
    fi
done

# Now walk: drive only one FPGA pin HIGH at a time, verify correct Pico GP
fpga_cmd "I" > /dev/null  # all back to input
sleep 0.05

for pin in $(seq 0 15); do
    jp1_bit=${GP_TO_JP1[$pin]}
    # FPGA drives only jp1[jp1_bit] HIGH
    oe_val=$(printf "%04X" $((1 << jp1_bit)))
    fpga_cmd "O${oe_val}" > /dev/null
    fpga_cmd "D${oe_val}" > /dev/null
    sleep 0.05

    # Read from Pico — this GP pin should be HIGH
    result=$(pico_cmd "GPIO READ $pin")
    assert_eq "FPGA jp1[$jp1_bit] -> Pico GP$pin HIGH" "OK 1" "$result"

    # Check that an adjacent pin is NOT driven HIGH
    if [ $pin -gt 0 ]; then
        prev=$((pin - 1))
        result=$(pico_cmd "GPIO READ $prev")
        if [ "$result" != "OK 0" ]; then
            echo -e "  ${CYAN}WARN${NC} GP$prev reads '$result' while driving jp1[$jp1_bit] (crosstalk?)"
        fi
    fi

    # Return FPGA to input
    fpga_cmd "I" > /dev/null
    sleep 0.05
done

# Release all Pico pins
for pin in $(seq 0 15); do
    pico_cmd "PIN $pin RELEASE" > /dev/null
done

# -----------------------------------------------------------
echo -e "\n${YELLOW}[3] Bidirectional: Pico output + FPGA readback + LA capture${NC}"
# -----------------------------------------------------------
# GP0-3 as outputs -> FPGA reads on jp1[14,15,12,13]
# GP4-7 as LA inputs <- FPGA drives on jp1[10,11,8,9]
#
# This tests the real use case: Pico controls FPGA, LA monitors response

# Set up Pico pins
for pin in 0 1 2 3; do
    pico_cmd "PIN $pin FUNC OUTPUT" > /dev/null
    pico_cmd "GPIO WRITE $pin 0" > /dev/null
done
for pin in 4 5 6 7; do
    pico_cmd "PIN $pin FUNC LA" > /dev/null
done

# FPGA: set output enable for jp1 bits that map to GP4-7
# GP4->jp1[10], GP5->jp1[11], GP6->jp1[8], GP7->jp1[9]
# OE mask = (1<<10)|(1<<11)|(1<<8)|(1<<9) = 0x0F00
fpga_cmd "O0f00" > /dev/null
fpga_cmd "D0000" > /dev/null  # all low initially

# Init LA
result=$(pico_cmd "LA INIT")
assert_eq "LA INIT" "OK" "$result"

# Set pattern on GP0-3: 0b0101 = GP0=1, GP1=0, GP2=1, GP3=0
pico_cmd "GPIO WRITE 0 1" > /dev/null
pico_cmd "GPIO WRITE 1 0" > /dev/null
pico_cmd "GPIO WRITE 2 1" > /dev/null
pico_cmd "GPIO WRITE 3 0" > /dev/null
sleep 0.05

# Read FPGA — GP0->jp1[14], GP2->jp1[12] should be set
# Expected: bits 14 and 12 set = 0x5000
result=$(fpga_cmd "R")
echo -e "  ${CYAN}INFO${NC} FPGA reads: $result (expect 5xxx with bits 14,12 set)"

# Have FPGA drive pattern on GP4-7 via jp1[10,11,8,9]
# Want GP4=0,GP5=1,GP6=0,GP7=1 -> jp1[10]=0,jp1[11]=1,jp1[8]=0,jp1[9]=1
# Data mask = (1<<11)|(1<<9) = 0x0A00
fpga_cmd "D0a00" > /dev/null
sleep 0.05

# Capture with LA
result=$(pico_cmd "LA CAPTURE 100 125")
assert_eq "LA CAPTURE" "OK capturing" "$result"
sleep 0.2
pico_cmd "LA STATUS" > /dev/null

# Read a few samples — just verify LA is working
pico_cmd "LA DATA 0 10" > /dev/null
result=$(pico_cmd "LA STATUS")
echo -e "  ${CYAN}INFO${NC} LA status: $result"

# Clean up
pico_cmd "LA DEINIT" > /dev/null
fpga_cmd "I" > /dev/null
for pin in $(seq 0 7); do
    pico_cmd "PIN $pin RELEASE" > /dev/null
done

# -----------------------------------------------------------
echo -e "\n${YELLOW}[4] Cleanup${NC}"
# -----------------------------------------------------------

fpga_cmd "I" > /dev/null
result=$(pico_cmd "RESET")
assert_eq "RESET" "OK" "$result"

# -----------------------------------------------------------
exec 3>&-

echo ""
echo "============================================"
echo -e " Results: ${GREEN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC}"
if [ $FAIL -gt 0 ]; then
    echo -e "\n Failures:${ERRORS}"
fi
echo "============================================"

exit $FAIL
