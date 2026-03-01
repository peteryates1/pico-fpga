#!/bin/bash
# Regression test suite for pico-fpga
# Usage: ./test/run_tests.sh [/dev/pico-la]

set -uo pipefail

DEV="${1:-/dev/pico-la}"
BAUD=115200
PASS=0
FAIL=0
ERRORS=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Configure serial port
stty -F "$DEV" "$BAUD" raw -echo -hupcl 2>/dev/null

# Use persistent file descriptors to avoid open/close races
exec 3<>"$DEV"

# Drain stale data
while read -r -t 0.3 _line <&3; do :; done

send_cmd() {
    echo -ne "$1\n" >&3
}

read_line() {
    local timeout_sec="${1:-2}"
    local line=""
    if read -r -t "$timeout_sec" line <&3; then
        echo "$line" | tr -d '\r'
    else
        echo "TIMEOUT"
    fi
}

# Send command and read one response line
cmd() {
    send_cmd "$1"
    sleep 0.05
    read_line 2
}

assert_eq() {
    local test_name="$1"
    local expected="$2"
    local actual="$3"
    actual=$(echo "$actual" | sed 's/[[:space:]]*$//')
    expected=$(echo "$expected" | sed 's/[[:space:]]*$//')
    if [ "$actual" = "$expected" ]; then
        echo -e "  ${GREEN}PASS${NC} $test_name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC} $test_name"
        echo -e "       expected: '$expected'"
        echo -e "       actual:   '$actual'"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: $test_name"
    fi
}

assert_prefix() {
    local test_name="$1"
    local prefix="$2"
    local actual="$3"
    actual=$(echo "$actual" | sed 's/[[:space:]]*$//')
    if [[ "$actual" == ${prefix}* ]]; then
        echo -e "  ${GREEN}PASS${NC} $test_name"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC} $test_name"
        echo -e "       expected prefix: '$prefix'"
        echo -e "       actual:          '$actual'"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: $test_name"
    fi
}

echo "============================================"
echo " pico-fpga regression tests"
echo " device: $DEV"
echo "============================================"
echo ""

# -----------------------------------------------------------
echo -e "${YELLOW}[setup]${NC} Resetting device..."
result=$(cmd "RESET")
assert_eq "RESET returns OK" "OK" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[1] System commands${NC}"
# -----------------------------------------------------------

result=$(cmd "PING")
assert_eq "PING" "OK pico-fpga v1.0" "$result"

result=$(cmd "STATUS")
assert_prefix "STATUS format" "OK pio0=" "$result"

result=$(cmd "BADCMD")
assert_prefix "Unknown command" "ERROR:" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[2] Pin management${NC}"
# -----------------------------------------------------------

result=$(cmd "PIN 0 FUNC INPUT")
assert_eq "PIN 0 INPUT" "OK" "$result"

result=$(cmd "PIN 1 FUNC OUTPUT")
assert_eq "PIN 1 OUTPUT" "OK" "$result"

result=$(cmd "PIN 2 FUNC INPUT PULLUP")
assert_eq "PIN 2 INPUT PULLUP" "OK" "$result"

# Double-assign should fail
result=$(cmd "PIN 0 FUNC OUTPUT")
assert_prefix "PIN double-assign" "ERROR:" "$result"

# Reserved pins
result=$(cmd "PIN 23 FUNC INPUT")
assert_prefix "PIN 23 reserved" "ERROR:" "$result"

result=$(cmd "PIN 24 FUNC INPUT")
assert_prefix "PIN 24 reserved" "ERROR:" "$result"

result=$(cmd "PIN 25 FUNC INPUT")
assert_prefix "PIN 25 reserved" "ERROR:" "$result"

# Query
result=$(cmd "PIN QUERY")
assert_prefix "PIN QUERY all" "OK 0:INPUT" "$result"

result=$(cmd "PIN QUERY 1")
assert_eq "PIN QUERY 1" "OK 1:OUTPUT" "$result"

# Release
result=$(cmd "PIN 0 RELEASE")
assert_eq "PIN 0 RELEASE" "OK" "$result"

result=$(cmd "PIN 1 RELEASE")
assert_eq "PIN 1 RELEASE" "OK" "$result"

result=$(cmd "PIN 2 RELEASE")
assert_eq "PIN 2 RELEASE" "OK" "$result"

result=$(cmd "PIN QUERY")
assert_eq "PIN QUERY empty" "OK no pins assigned" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[3] GPIO commands${NC}"
# -----------------------------------------------------------

cmd "PIN 10 FUNC OUTPUT" > /dev/null
cmd "PIN 11 FUNC INPUT" > /dev/null

result=$(cmd "GPIO WRITE 10 1")
assert_eq "GPIO WRITE 1" "OK" "$result"

result=$(cmd "GPIO READ 10")
assert_eq "GPIO READ output=1" "OK 1" "$result"

result=$(cmd "GPIO WRITE 10 0")
assert_eq "GPIO WRITE 0" "OK" "$result"

result=$(cmd "GPIO READ 10")
assert_eq "GPIO READ output=0" "OK 0" "$result"

result=$(cmd "GPIO READ_ALL")
assert_prefix "GPIO READ_ALL" "OK " "$result"

# Clean up
cmd "PIN 10 RELEASE" > /dev/null
cmd "PIN 11 RELEASE" > /dev/null

# Error: read unassigned pin
result=$(cmd "GPIO READ 10")
assert_prefix "GPIO READ unassigned" "ERROR:" "$result"

result=$(cmd "GPIO WRITE 10 1")
assert_prefix "GPIO WRITE unassigned" "ERROR:" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[4] Logic analyzer${NC}"
# -----------------------------------------------------------

cmd "PIN 0 FUNC LA" > /dev/null
cmd "PIN 1 FUNC LA" > /dev/null

result=$(cmd "LA INIT")
assert_eq "LA INIT" "OK" "$result"

result=$(cmd "LA STATUS")
assert_eq "LA STATUS idle" "OK idle" "$result"

result=$(cmd "LA CAPTURE 16 125")
assert_eq "LA CAPTURE" "OK capturing" "$result"

sleep 1

result=$(cmd "LA STATUS")
assert_eq "LA STATUS done" "OK done 16" "$result"

# DATA: line 1 = "OK <count>", line 2 = hex data
send_cmd "LA DATA 0 8"
sleep 0.1
read -r -t 2 la_line1 <&3
read -r -t 2 la_line2 <&3
la_line1=$(echo "$la_line1" | tr -d '\r')
la_line2=$(echo "$la_line2" | tr -d '\r')
assert_eq "LA DATA header" "OK 8" "$la_line1"
word_count=$(echo "$la_line2" | wc -w | tr -d ' ')
assert_eq "LA DATA 8 samples" "8" "$word_count"

result=$(cmd "LA DEINIT")
assert_eq "LA DEINIT" "OK" "$result"

result=$(cmd "LA DEINIT")
assert_eq "LA DEINIT idempotent" "OK" "$result"

result=$(cmd "LA CAPTURE 16 1")
assert_prefix "LA CAPTURE without init" "ERROR:" "$result"

cmd "PIN 0 RELEASE" > /dev/null
cmd "PIN 1 RELEASE" > /dev/null

# -----------------------------------------------------------
echo -e "\n${YELLOW}[5] UART (error paths)${NC}"
# -----------------------------------------------------------

result=$(cmd "UART pio0 INIT 9600")
assert_prefix "UART INIT no pins" "ERROR:" "$result"

result=$(cmd "UART pio9 INIT 9600")
assert_prefix "UART invalid id" "ERROR:" "$result"

# HW UART with wrong pins
cmd "PIN 2 FUNC UART_TX hw0" > /dev/null
cmd "PIN 3 FUNC UART_RX hw0" > /dev/null
result=$(cmd "UART hw0 INIT 9600")
assert_prefix "HW UART wrong pins" "ERROR:" "$result"
cmd "PIN 2 RELEASE" > /dev/null
cmd "PIN 3 RELEASE" > /dev/null

# -----------------------------------------------------------
echo -e "\n${YELLOW}[6] RESET cleans everything${NC}"
# -----------------------------------------------------------

cmd "PIN 0 FUNC LA" > /dev/null
cmd "PIN 1 FUNC LA" > /dev/null
cmd "LA INIT" > /dev/null

result=$(cmd "RESET")
assert_eq "RESET" "OK" "$result"

result=$(cmd "STATUS")
assert_prefix "STATUS after RESET" "OK pio0=0/0 pio1=0/0 dma=0" "$result"

result=$(cmd "PIN QUERY")
assert_eq "PIN QUERY after RESET" "OK no pins assigned" "$result"

# -----------------------------------------------------------
# Cleanup
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
