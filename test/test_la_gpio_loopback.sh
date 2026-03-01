#!/bin/bash
# LA + GPIO loopback test: GPIO outputs captured by LA
#
# Simulates real use case: GPIO controls FPGA (reset, step, etc.)
# while LA monitors signals.
#
# Wiring required:
#   GPIO 0 (output) -> GPIO 6 (LA input)
#   GPIO 1 (output) -> GPIO 7 (LA input)

set -uo pipefail

DEV="${1:-/dev/ttyACM1}"
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

# Read N LA samples starting at offset, extract a specific GPIO bit
# Usage: la_read_bit <offset> <count> <gpio_bit>
# Prints space-separated bit values
la_read_bit() {
    local la_offset=$1
    local la_count=$2
    local gpio_bit=$3

    send_cmd "LA DATA $la_offset $la_count"
    sleep 0.1
    local hdr
    read -r -t 2 hdr <&3
    hdr=$(echo "$hdr" | tr -d '\r')

    local bits=""
    local remaining=$la_count
    while [ $remaining -gt 0 ]; do
        local line
        read -r -t 2 line <&3
        line=$(echo "$line" | tr -d '\r')
        for sample in $line; do
            local val=$((16#$sample))
            local bit=$(( (val >> gpio_bit) & 1 ))
            bits="${bits}${bit}"
            remaining=$((remaining - 1))
        done
    done
    echo "$bits"
}

echo "============================================"
echo " LA + GPIO loopback test"
echo " GPIO outputs -> LA capture"
echo " Wiring: GPIO0(OUT)->GPIO6(LA),"
echo "         GPIO1(OUT)->GPIO7(LA)"
echo " device: $DEV"
echo "============================================"
echo ""

# -----------------------------------------------------------
echo -e "${YELLOW}[setup]${NC} Reset and configure pins..."
# -----------------------------------------------------------
cmd "RESET" > /dev/null

result=$(cmd "PIN 0 FUNC OUTPUT")
assert_eq "PIN 0 OUTPUT" "OK" "$result"

result=$(cmd "PIN 1 FUNC OUTPUT")
assert_eq "PIN 1 OUTPUT" "OK" "$result"

result=$(cmd "PIN 6 FUNC LA")
assert_eq "PIN 6 LA" "OK" "$result"

result=$(cmd "PIN 7 FUNC LA")
assert_eq "PIN 7 LA" "OK" "$result"

result=$(cmd "LA INIT")
assert_eq "LA INIT" "OK" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[1] Static LOW capture${NC}"
# -----------------------------------------------------------
# Both outputs LOW, capture and verify LA sees 0 on bits 6,7

result=$(cmd "GPIO WRITE 0 0")
assert_eq "GPIO 0 = LOW" "OK" "$result"

result=$(cmd "GPIO WRITE 1 0")
assert_eq "GPIO 1 = LOW" "OK" "$result"

sleep 0.05

result=$(cmd "LA CAPTURE 100 125")
assert_eq "LA CAPTURE (both low)" "OK capturing" "$result"
sleep 0.1

result=$(cmd "LA STATUS")
assert_prefix "LA done" "OK done" "$result"

# Read all 100 samples, check bit 6
bits6=$(la_read_bit 0 100 6)
bits7=$(la_read_bit 0 100 7) 2>/dev/null  # DATA already consumed, need re-capture

# Actually we need to re-read. Let me capture again and read both bits in one pass.
# Re-capture for clean read
result=$(cmd "LA CAPTURE 100 125")
sleep 0.1
cmd "LA STATUS" > /dev/null

send_cmd "LA DATA 0 100"
sleep 0.1
read -r -t 2 hdr <&3

all_low=true
while read -r -t 1 line <&3; do
    line=$(echo "$line" | tr -d '\r')
    [ -z "$line" ] && break
    for sample in $line; do
        val=$((16#$sample))
        bit6=$(( (val >> 6) & 1 ))
        bit7=$(( (val >> 7) & 1 ))
        if [ $bit6 -ne 0 ] || [ $bit7 -ne 0 ]; then
            all_low=false
        fi
    done
done

if $all_low; then
    echo -e "  ${GREEN}PASS${NC} LA sees both pins LOW"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC} LA sees both pins LOW"
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: LA sees both pins LOW"
fi

# -----------------------------------------------------------
echo -e "\n${YELLOW}[2] Static HIGH capture${NC}"
# -----------------------------------------------------------

result=$(cmd "GPIO WRITE 0 1")
assert_eq "GPIO 0 = HIGH" "OK" "$result"

result=$(cmd "GPIO WRITE 1 1")
assert_eq "GPIO 1 = HIGH" "OK" "$result"

sleep 0.05

result=$(cmd "LA CAPTURE 100 125")
assert_eq "LA CAPTURE (both high)" "OK capturing" "$result"
sleep 0.1
cmd "LA STATUS" > /dev/null

send_cmd "LA DATA 0 100"
sleep 0.1
read -r -t 2 hdr <&3

all_high=true
while read -r -t 1 line <&3; do
    line=$(echo "$line" | tr -d '\r')
    [ -z "$line" ] && break
    for sample in $line; do
        val=$((16#$sample))
        bit6=$(( (val >> 6) & 1 ))
        bit7=$(( (val >> 7) & 1 ))
        if [ $bit6 -ne 1 ] || [ $bit7 -ne 1 ]; then
            all_high=false
        fi
    done
done

if $all_high; then
    echo -e "  ${GREEN}PASS${NC} LA sees both pins HIGH"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC} LA sees both pins HIGH"
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: LA sees both pins HIGH"
fi

# -----------------------------------------------------------
echo -e "\n${YELLOW}[3] Independent pin control${NC}"
# -----------------------------------------------------------
# GPIO0=HIGH, GPIO1=LOW -> bit6=1, bit7=0

result=$(cmd "GPIO WRITE 0 1")
assert_eq "GPIO 0 = HIGH" "OK" "$result"

result=$(cmd "GPIO WRITE 1 0")
assert_eq "GPIO 1 = LOW" "OK" "$result"

sleep 0.05

result=$(cmd "LA CAPTURE 100 125")
assert_eq "LA CAPTURE (0=H,1=L)" "OK capturing" "$result"
sleep 0.1
cmd "LA STATUS" > /dev/null

send_cmd "LA DATA 0 100"
sleep 0.1
read -r -t 2 hdr <&3

pin_correct=true
while read -r -t 1 line <&3; do
    line=$(echo "$line" | tr -d '\r')
    [ -z "$line" ] && break
    for sample in $line; do
        val=$((16#$sample))
        bit6=$(( (val >> 6) & 1 ))
        bit7=$(( (val >> 7) & 1 ))
        if [ $bit6 -ne 1 ] || [ $bit7 -ne 0 ]; then
            pin_correct=false
        fi
    done
done

if $pin_correct; then
    echo -e "  ${GREEN}PASS${NC} LA sees GPIO0=H, GPIO1=L independently"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC} LA sees GPIO0=H, GPIO1=L independently"
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: LA sees GPIO0=H, GPIO1=L independently"
fi

# Swap: GPIO0=LOW, GPIO1=HIGH -> bit6=0, bit7=1
result=$(cmd "GPIO WRITE 0 0")
assert_eq "GPIO 0 = LOW" "OK" "$result"

result=$(cmd "GPIO WRITE 1 1")
assert_eq "GPIO 1 = HIGH" "OK" "$result"

sleep 0.05

result=$(cmd "LA CAPTURE 100 125")
assert_eq "LA CAPTURE (0=L,1=H)" "OK capturing" "$result"
sleep 0.1
cmd "LA STATUS" > /dev/null

send_cmd "LA DATA 0 100"
sleep 0.1
read -r -t 2 hdr <&3

pin_correct=true
while read -r -t 1 line <&3; do
    line=$(echo "$line" | tr -d '\r')
    [ -z "$line" ] && break
    for sample in $line; do
        val=$((16#$sample))
        bit6=$(( (val >> 6) & 1 ))
        bit7=$(( (val >> 7) & 1 ))
        if [ $bit6 -ne 0 ] || [ $bit7 -ne 1 ]; then
            pin_correct=false
        fi
    done
done

if $pin_correct; then
    echo -e "  ${GREEN}PASS${NC} LA sees GPIO0=L, GPIO1=H independently"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC} LA sees GPIO0=L, GPIO1=H independently"
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: LA sees GPIO0=L, GPIO1=H independently"
fi

# -----------------------------------------------------------
echo -e "\n${YELLOW}[4] Toggle during capture (simulated step/reset)${NC}"
# -----------------------------------------------------------
# Start with both LOW, start LA, toggle GPIO0 (simulating a reset pulse),
# then verify LA captured the transition.
#
# At divider 125 = 1MHz (1us/sample), 1000 samples = 1ms capture.
# We toggle after a short delay so the transition lands mid-capture.

result=$(cmd "GPIO WRITE 0 0")
assert_eq "GPIO 0 = LOW (pre-toggle)" "OK" "$result"

result=$(cmd "GPIO WRITE 1 0")
assert_eq "GPIO 1 = LOW (pre-toggle)" "OK" "$result"

sleep 0.05

# Start capture (1000 samples at 1MHz = 1ms)
send_cmd "LA CAPTURE 1000 125"
# Immediately toggle GPIO 0 HIGH (simulating reset assert)
send_cmd "GPIO WRITE 0 1"
sleep 0.05
# Read both responses
read -r -t 2 resp1 <&3
resp1=$(echo "$resp1" | tr -d '\r')
read -r -t 2 resp2 <&3
resp2=$(echo "$resp2" | tr -d '\r')
assert_eq "LA CAPTURE (toggle)" "OK capturing" "$resp1"
assert_eq "GPIO WRITE during capture" "OK" "$resp2"

sleep 0.2
result=$(cmd "LA STATUS")
assert_prefix "LA done (toggle)" "OK done" "$result"

# Read all 1000 samples, look for transition on bit 6 (GPIO 0 via loopback to GPIO 6)
send_cmd "LA DATA 0 1000"
sleep 0.2
read -r -t 2 hdr <&3

bit6_low=0
bit6_high=0
bit6_transitions=0
prev_bit6=-1
first_high=-1
sample_idx=0

while read -r -t 1 line <&3; do
    line=$(echo "$line" | tr -d '\r')
    [ -z "$line" ] && break
    for sample in $line; do
        val=$((16#$sample))
        bit6=$(( (val >> 6) & 1 ))
        if [ $bit6 -eq 0 ]; then
            bit6_low=$((bit6_low + 1))
        else
            bit6_high=$((bit6_high + 1))
            if [ $first_high -lt 0 ]; then first_high=$sample_idx; fi
        fi
        if [ $prev_bit6 -ge 0 ] && [ $bit6 -ne $prev_bit6 ]; then
            bit6_transitions=$((bit6_transitions + 1))
        fi
        prev_bit6=$bit6
        sample_idx=$((sample_idx + 1))
    done
done

echo -e "  ${CYAN}INFO${NC} bit6: low=$bit6_low high=$bit6_high transitions=$bit6_transitions first_high@sample=$first_high"

# We expect: starts LOW, transitions to HIGH exactly once
if [ $bit6_low -gt 0 ] && [ $bit6_high -gt 0 ] && [ $bit6_transitions -ge 1 ]; then
    echo -e "  ${GREEN}PASS${NC} LA captured GPIO toggle (rising edge)"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC} LA did not capture GPIO toggle"
    echo -e "       low=$bit6_low high=$bit6_high transitions=$bit6_transitions"
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: LA did not capture GPIO toggle"
fi

# Verify bit 7 (GPIO 1) stayed LOW the whole time
send_cmd "LA DATA 0 1000"
sleep 0.2
read -r -t 2 hdr <&3

bit7_all_low=true
while read -r -t 1 line <&3; do
    line=$(echo "$line" | tr -d '\r')
    [ -z "$line" ] && break
    for sample in $line; do
        val=$((16#$sample))
        bit7=$(( (val >> 7) & 1 ))
        if [ $bit7 -ne 0 ]; then
            bit7_all_low=false
        fi
    done
done

if $bit7_all_low; then
    echo -e "  ${GREEN}PASS${NC} GPIO 1 stayed LOW during toggle (no crosstalk)"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC} GPIO 1 did not stay LOW (crosstalk?)"
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: GPIO 1 crosstalk during toggle"
fi

# -----------------------------------------------------------
echo -e "\n${YELLOW}[5] Cleanup${NC}"
# -----------------------------------------------------------

result=$(cmd "LA DEINIT")
assert_eq "LA DEINIT" "OK" "$result"

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
