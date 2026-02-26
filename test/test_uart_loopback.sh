#!/bin/bash
# UART loopback test: PIO pio0 <-> HW UART1 cross-connect + LA capture
# Optional: HW UART0 <-> debug probe
#
# Wiring required:
#   GPIO 10 (PIO pio0 TX) -> GPIO 9  (HW UART1 RX)
#   GPIO 8  (HW UART1 TX) -> GPIO 11 (PIO pio0 RX)
# Optional (for debug probe test):
#   GPIO 0  (HW UART0 TX) -> debug probe RX
#   GPIO 1  (HW UART0 RX) <- debug probe TX

set -uo pipefail

DEV="${1:-/dev/pico-la}"
DEBUG_DEV="${2:-}"
BAUD=115200
PASS=0
FAIL=0
ERRORS=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

stty -F "$DEV" "$BAUD" raw -echo -hupcl 2>/dev/null
exec 3<>"$DEV"
while read -r -t 0.3 _line <&3; do :; done

send_cmd() { echo -ne "$1\n" >&3; }

read_line() {
    local line=""
    if read -r -t "${1:-2}" line <&3; then
        echo "$line" | tr -d '\r'
    else
        echo "TIMEOUT"
    fi
}

cmd() {
    send_cmd "$1"
    sleep 0.05
    read_line 2
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

assert_prefix() {
    local actual=$(echo "$3" | sed 's/[[:space:]]*$//')
    if [[ "$actual" == $2* ]]; then
        echo -e "  ${GREEN}PASS${NC} $1"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC} $1"
        echo -e "       expected prefix: '$2'"
        echo -e "       actual:          '$actual'"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: $1"
    fi
}

# Case-insensitive comparison (normalize to uppercase)
assert_eq_hex() {
    local actual=$(echo "$3" | sed 's/[[:space:]]*$//' | tr 'a-f' 'A-F')
    local expected=$(echo "$2" | sed 's/[[:space:]]*$//' | tr 'a-f' 'A-F')
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
echo " UART loopback test"
echo " PIO pio0 <-> HW UART1 cross-connect"
echo " Wiring: GPIO10(TX)->GPIO9(RX),"
echo "         GPIO8(TX)->GPIO11(RX)"
echo " device: $DEV"
if [ -n "$DEBUG_DEV" ]; then
    echo " debug:  $DEBUG_DEV"
fi
echo "============================================"
echo ""

# -----------------------------------------------------------
echo -e "${YELLOW}[setup]${NC} Reset and configure pins..."
# -----------------------------------------------------------
cmd "RESET" > /dev/null

# PIO pio0: TX=GPIO10, RX=GPIO11
result=$(cmd "PIN 10 FUNC UART_TX pio0")
assert_eq "PIN 10 UART_TX pio0" "OK" "$result"

result=$(cmd "PIN 11 FUNC UART_RX pio0")
assert_eq "PIN 11 UART_RX pio0" "OK" "$result"

# HW UART1: TX=GPIO8, RX=GPIO9
result=$(cmd "PIN 8 FUNC UART_TX hw1")
assert_eq "PIN 8 UART_TX hw1" "OK" "$result"

result=$(cmd "PIN 9 FUNC UART_RX hw1")
assert_eq "PIN 9 UART_RX hw1" "OK" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[1] PIO <-> HW1 at 9600 baud${NC}"
# -----------------------------------------------------------

result=$(cmd "UART pio0 INIT 9600")
assert_eq "PIO pio0 INIT 9600" "OK" "$result"

result=$(cmd "UART hw1 INIT 9600")
assert_eq "HW1 INIT 9600" "OK" "$result"

# --- PIO TX -> HW1 RX ---
# Send 1 byte from PIO, receive on HW1
result=$(cmd "UART pio0 SEND A5")
assert_prefix "PIO SEND A5" "OK " "$result"
echo -e "  ${CYAN}INFO${NC} SEND returned: $result"

sleep 0.1  # wait for byte to transmit at 9600 baud

result=$(cmd "UART hw1 RECV 1 500")
assert_eq_hex "HW1 RECV A5" "OK A5" "$result"

# Send 3 bytes from PIO, receive on HW1
result=$(cmd "UART pio0 SEND CAFE42")
assert_prefix "PIO SEND CAFE42" "OK " "$result"

sleep 0.1

result=$(cmd "UART hw1 RECV 3 500")
assert_eq_hex "HW1 RECV CAFE42" "OK CAFE42" "$result"

# --- HW1 TX -> PIO RX ---
# Send 1 byte from HW1, receive on PIO
result=$(cmd "UART hw1 SEND 5A")
assert_prefix "HW1 SEND 5A" "OK " "$result"

sleep 0.1

result=$(cmd "UART pio0 RECV 1 500")
assert_eq_hex "PIO RECV 5A" "OK 5A" "$result"

# Send 3 bytes from HW1, receive on PIO
result=$(cmd "UART hw1 SEND BEEF99")
assert_prefix "HW1 SEND BEEF99" "OK " "$result"

sleep 0.1

result=$(cmd "UART pio0 RECV 3 500")
assert_eq_hex "PIO RECV BEEF99" "OK BEEF99" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[2] PIO <-> HW1 at 115200 baud${NC}"
# -----------------------------------------------------------

result=$(cmd "UART pio0 BAUD 115200")
assert_eq "PIO BAUD 115200" "OK" "$result"

result=$(cmd "UART hw1 BAUD 115200")
assert_eq "HW1 BAUD 115200" "OK" "$result"

# PIO -> HW1
result=$(cmd "UART pio0 SEND FF")
assert_prefix "PIO SEND FF @115200" "OK " "$result"

sleep 0.05

result=$(cmd "UART hw1 RECV 1 500")
assert_eq_hex "HW1 RECV FF @115200" "OK FF" "$result"

# HW1 -> PIO
result=$(cmd "UART hw1 SEND 00")
assert_prefix "HW1 SEND 00 @115200" "OK " "$result"

sleep 0.05

result=$(cmd "UART pio0 RECV 1 500")
assert_eq_hex "PIO RECV 00 @115200" "OK 00" "$result"

# Bidirectional: send from both at same time
result=$(cmd "UART pio0 SEND AB")
result2=$(cmd "UART hw1 SEND CD")

sleep 0.1

result=$(cmd "UART hw1 RECV 1 500")
assert_eq_hex "HW1 RECV AB bidir" "OK AB" "$result"

result=$(cmd "UART pio0 RECV 1 500")
assert_eq_hex "PIO RECV CD bidir" "OK CD" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[3] LA capture of UART TX waveform${NC}"
# -----------------------------------------------------------

# Slow baud for clear LA waveform: 9600 baud
# At 9600 baud, 1 bit = ~104us. LA divider 125 = 1us/sample => ~104 samples/bit.
# 1 byte = 10 bits (start + 8 data + stop) = ~1040 samples.
result=$(cmd "UART pio0 BAUD 9600")
assert_eq "PIO BAUD 9600 (for LA)" "OK" "$result"

result=$(cmd "UART hw1 BAUD 9600")
assert_eq "HW1 BAUD 9600 (for LA)" "OK" "$result"

result=$(cmd "LA INIT")
assert_eq "LA INIT" "OK" "$result"

# Pipeline: start LA capture then immediately send UART data
# 5000 samples at divider 125 = 40ms capture at 1MHz
send_cmd "LA CAPTURE 5000 125"
send_cmd "UART pio0 SEND A5"
sleep 0.2
read -r -t 2 la_resp <&3
la_resp=$(echo "$la_resp" | tr -d '\r')
assert_eq "LA CAPTURE" "OK capturing" "$la_resp"
read -r -t 2 uart_resp <&3
uart_resp=$(echo "$uart_resp" | tr -d '\r')
assert_prefix "UART SEND during LA" "OK " "$uart_resp"

sleep 1

result=$(cmd "LA STATUS")
assert_prefix "LA STATUS done" "OK done" "$result"

# Drain the received byte on HW1
cmd "UART hw1 RECV 1 200" > /dev/null

# Read 2000 samples to cover the UART byte (starts ~200-500us after capture)
# At 1us/sample, 2000 samples = 2ms, well covering a 9600-baud byte (~1ms)
send_cmd "LA DATA 0 2000"
sleep 0.2
read -r -t 2 la_hdr <&3
la_hdr=$(echo "$la_hdr" | tr -d '\r')
assert_eq "LA DATA header" "OK 2000" "$la_hdr"

echo -e "\n  ${CYAN}LA capture (UART TX 0xA5 on GPIO 10):${NC}"
sample_num=0
gpio10_low=0
gpio10_high=0
gpio10_transitions=0
prev_gpio10=-1
first_low=-1
for i in $(seq 1 250); do
    read -r -t 2 dataline <&3
    dataline=$(echo "$dataline" | tr -d '\r')
    for sample in $dataline; do
        val=$((16#$sample))
        gpio10=$(( (val >> 10) & 1 ))
        if [ $gpio10 -eq 0 ]; then
            gpio10_low=$((gpio10_low + 1))
            if [ $first_low -lt 0 ]; then first_low=$sample_num; fi
        else
            gpio10_high=$((gpio10_high + 1))
        fi
        if [ $prev_gpio10 -ge 0 ] && [ $gpio10 -ne $prev_gpio10 ]; then
            gpio10_transitions=$((gpio10_transitions + 1))
        fi
        prev_gpio10=$gpio10
        sample_num=$((sample_num + 1))
    done
done

# Print summary around the waveform
if [ $first_low -ge 0 ]; then
    echo -e "  ${CYAN}INFO${NC} Start bit at sample $first_low (~${first_low}us into capture)"
fi

echo -e "\n  ${CYAN}INFO${NC} GPIO10 (PIO TX): low=$gpio10_low high=$gpio10_high transitions=$gpio10_transitions"

# UART idle is HIGH. A transmitted byte has start bit (LOW) + data + stop (HIGH).
# We expect both HIGH and LOW samples, and multiple transitions.
if [ $gpio10_low -gt 0 ] && [ $gpio10_high -gt 0 ] && [ $gpio10_transitions -ge 2 ]; then
    echo -e "  ${GREEN}PASS${NC} UART TX waveform visible in LA"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC} UART TX waveform not visible (expected transitions)"
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: UART TX waveform not visible"
fi

result=$(cmd "LA DEINIT")
assert_eq "LA DEINIT" "OK" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[4] HW UART0 <-> debug probe${NC}"
# -----------------------------------------------------------

if [ -n "$DEBUG_DEV" ]; then
    # Deinit cross-connect UARTs first
    cmd "UART pio0 DEINIT" > /dev/null
    cmd "UART hw1 DEINIT" > /dev/null
    cmd "PIN 8 RELEASE" > /dev/null
    cmd "PIN 9 RELEASE" > /dev/null
    cmd "PIN 10 RELEASE" > /dev/null
    cmd "PIN 11 RELEASE" > /dev/null

    # Configure HW UART0: TX=GPIO0, RX=GPIO1
    result=$(cmd "PIN 0 FUNC UART_TX hw0")
    assert_eq "PIN 0 UART_TX hw0" "OK" "$result"

    result=$(cmd "PIN 1 FUNC UART_RX hw0")
    assert_eq "PIN 1 UART_RX hw0" "OK" "$result"

    result=$(cmd "UART hw0 INIT 115200")
    assert_eq "HW0 INIT 115200" "OK" "$result"

    # Open debug probe serial port
    stty -F "$DEBUG_DEV" 115200 raw -echo -hupcl 2>/dev/null
    exec 4<>"$DEBUG_DEV"
    # Drain stale data
    while read -r -t 0.3 _line <&4; do :; done

    # --- Pico TX -> Debug probe RX ---
    result=$(cmd "UART hw0 SEND 48454C4C4F")  # "HELLO"
    assert_prefix "HW0 SEND HELLO" "OK " "$result"

    sleep 0.2

    debug_data=""
    while IFS= read -r -t 0.5 -d '' -n 5 chunk <&4; do
        debug_data="${debug_data}${chunk}"
    done
    # Convert to hex for comparison
    debug_hex=$(printf '%s' "$debug_data" | xxd -p | tr 'a-f' 'A-F')
    echo -e "  ${CYAN}INFO${NC} Debug probe received: '$debug_data' (hex: $debug_hex)"
    if [[ "$debug_hex" == "48454C4C4F" ]]; then
        echo -e "  ${GREEN}PASS${NC} Pico TX -> debug probe"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}FAIL${NC} Pico TX -> debug probe"
        echo -e "       expected hex: 48454C4C4F"
        echo -e "       actual hex:   $debug_hex"
        FAIL=$((FAIL + 1))
        ERRORS="${ERRORS}\n  FAIL: Pico TX -> debug probe"
    fi

    # --- Debug probe TX -> Pico RX ---
    printf 'WORLD' >&4

    sleep 0.2

    result=$(cmd "UART hw0 RECV 5 500")
    result_hex=$(echo "$result" | tr 'a-f' 'A-F')
    assert_eq_hex "HW0 RECV WORLD" "OK 574F524C44" "$result"

    # Cleanup
    exec 4>&-
    result=$(cmd "UART hw0 DEINIT")
    assert_eq "HW0 DEINIT" "OK" "$result"
    cmd "PIN 0 RELEASE" > /dev/null
    cmd "PIN 1 RELEASE" > /dev/null
else
    echo -e "  ${CYAN}SKIP${NC} No debug probe device specified (pass as 2nd arg)"
fi

# -----------------------------------------------------------
echo -e "\n${YELLOW}[5] Cleanup${NC}"
# -----------------------------------------------------------

# Deinit any remaining UARTs
cmd "UART pio0 DEINIT" > /dev/null
cmd "UART hw1 DEINIT" > /dev/null

result=$(cmd "RESET")
assert_eq "RESET" "OK" "$result"

result=$(cmd "STATUS")
assert_prefix "STATUS clean" "OK pio0=0/0" "$result"

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
