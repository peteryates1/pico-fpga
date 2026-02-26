#!/bin/bash
# JTAG loopback test with LA monitoring
# Requires: GPIO 4 (TDI) wired to GPIO 5 (TDO)
# JTAG: TCK=2, TMS=3, TDI=4, TDO=5

set -uo pipefail

DEV="${1:-/dev/pico-la}"
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

# Decode LA sample: extract JTAG signals from 32-bit GPIO snapshot
decode_jtag() {
    local sample=$1
    local val=$((16#$sample))
    local tck=$(( (val >> 2) & 1 ))
    local tms=$(( (val >> 3) & 1 ))
    local tdi=$(( (val >> 4) & 1 ))
    local tdo=$(( (val >> 5) & 1 ))
    echo "TCK=$tck TMS=$tms TDI=$tdi TDO=$tdo"
}

echo "============================================"
echo " JTAG loopback test with LA monitoring"
echo " Wiring: GPIO4 (TDI) -> GPIO5 (TDO)"
echo " JTAG: TCK=2 TMS=3 TDI=4 TDO=5"
echo " device: $DEV"
echo "============================================"
echo ""

# -----------------------------------------------------------
echo -e "${YELLOW}[setup]${NC} Reset and configure pins..."
# -----------------------------------------------------------
cmd "RESET" > /dev/null

result=$(cmd "PIN 2 FUNC JTAG_TCK")
assert_eq "PIN 2 JTAG_TCK" "OK" "$result"

result=$(cmd "PIN 3 FUNC JTAG_TMS")
assert_eq "PIN 3 JTAG_TMS" "OK" "$result"

result=$(cmd "PIN 4 FUNC JTAG_TDI")
assert_eq "PIN 4 JTAG_TDI" "OK" "$result"

result=$(cmd "PIN 5 FUNC JTAG_TDO")
assert_eq "PIN 5 JTAG_TDO" "OK" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[1] JTAG INIT${NC}"
# -----------------------------------------------------------

result=$(cmd "JTAG INIT")
assert_eq "JTAG INIT" "OK" "$result"

result=$(cmd "STATUS")
echo -e "  ${CYAN}INFO${NC} $result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[2] LA capture of JTAG RESET${NC}"
# -----------------------------------------------------------

# Slow JTAG to 100kHz so individual clocks are visible to LA
# At 100kHz: clock period=10us, TCK high/low=5us each
result=$(cmd "JTAG FREQ 100000")
assert_eq "JTAG FREQ 100kHz" "OK" "$result"

result=$(cmd "LA INIT")
assert_eq "LA INIT" "OK" "$result"

# Pipeline: send LA CAPTURE and JTAG RESET back-to-back so the pico
# processes them sequentially with minimal gap. LA captures during JTAG.
# 5000 samples at divider 625 = 25ms capture, 5us/sample.
# JTAG RESET: 5 clocks at 100kHz = 50us = ~10 samples.
# Use direct read (not subshell read_line) to avoid bash buffer loss.
send_cmd "LA CAPTURE 5000 625"
send_cmd "JTAG RESET"
sleep 0.2
read -r -t 2 la_resp <&3
la_resp=$(echo "$la_resp" | tr -d '\r')
assert_eq "LA CAPTURE" "OK capturing" "$la_resp"
read -r -t 2 jtag_resp <&3
jtag_resp=$(echo "$jtag_resp" | tr -d '\r')
assert_eq "JTAG RESET" "OK" "$jtag_resp"

sleep 1

result=$(cmd "LA STATUS")
assert_eq "LA STATUS done" "OK done 5000" "$result"

# Read first 200 samples to see RESET waveform
send_cmd "LA DATA 0 200"
sleep 0.1
read -r -t 2 la_hdr <&3
la_hdr=$(echo "$la_hdr" | tr -d '\r')
assert_eq "LA DATA header" "OK 200" "$la_hdr"

echo -e "\n  ${CYAN}LA capture (JTAG RESET) - first 64 samples:${NC}"
echo -e "  ${CYAN}  Sample  Raw        TCK TMS TDI TDO${NC}"
sample_num=0
tck_high_count=0
for i in $(seq 1 25); do
    read -r -t 2 dataline <&3
    dataline=$(echo "$dataline" | tr -d '\r')
    for sample in $dataline; do
        decoded=$(decode_jtag "$sample")
        if [ $sample_num -lt 64 ]; then
            printf "  %4d    %s  %s\n" "$sample_num" "$sample" "$decoded"
        fi
        val=$((16#$sample))
        tck=$(( (val >> 2) & 1 ))
        if [ $tck -eq 1 ]; then
            tck_high_count=$((tck_high_count + 1))
        fi
        sample_num=$((sample_num + 1))
    done
done

echo -e "\n  ${CYAN}INFO${NC} Samples with TCK=1: $tck_high_count (of 200)"
if [ $tck_high_count -gt 0 ]; then
    echo -e "  ${GREEN}PASS${NC} JTAG RESET: TCK activity observed in LA"
    PASS=$((PASS + 1))
else
    echo -e "  ${RED}FAIL${NC} JTAG RESET: no TCK activity in LA"
    FAIL=$((FAIL + 1))
    ERRORS="${ERRORS}\n  FAIL: JTAG RESET: no TCK activity in LA"
fi

result=$(cmd "LA DEINIT")
assert_eq "LA DEINIT" "OK" "$result"

# Restore default JTAG frequency
result=$(cmd "JTAG FREQ 6250000")
assert_eq "JTAG FREQ restore" "OK" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[3] JTAG SCAN_DR loopback${NC}"
# -----------------------------------------------------------

# Scan 8-bit DR with pattern 0xA5, expect loopback on TDO
result=$(cmd "JTAG SCAN_DR 8 A5")
assert_prefix "JTAG SCAN_DR response" "OK TDO=" "$result"
tdo_val=$(echo "$result" | sed 's/OK TDO=//')
echo -e "  ${CYAN}INFO${NC} SCAN_DR 8 bits, TDI=0xA5, TDO=$tdo_val"

# With direct TDI->TDO wire, TDO should match TDI (0xA5)
assert_eq "SCAN_DR loopback TDO=A5" "OK TDO=A5" "$result"

# Try 16-bit pattern
result=$(cmd "JTAG SCAN_DR 16 BEEF")
tdo_val=$(echo "$result" | sed 's/OK TDO=//')
echo -e "  ${CYAN}INFO${NC} SCAN_DR 16 bits, TDI=0xBEEF, TDO=$tdo_val"
assert_eq "SCAN_DR loopback TDO=BEEF" "OK TDO=BEEF" "$result"

# Try 32-bit pattern
result=$(cmd "JTAG SCAN_DR 32 DEADBEEF")
tdo_val=$(echo "$result" | sed 's/OK TDO=//')
echo -e "  ${CYAN}INFO${NC} SCAN_DR 32 bits, TDI=0xDEADBEEF, TDO=$tdo_val"
assert_eq "SCAN_DR loopback TDO=DEADBEEF" "OK TDO=DEADBEEF" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[4] JTAG SCAN_IR loopback${NC}"
# -----------------------------------------------------------

result=$(cmd "JTAG SCAN_IR 8 3F")
tdo_val=$(echo "$result" | sed 's/OK TDO=//')
echo -e "  ${CYAN}INFO${NC} SCAN_IR 8 bits, TDI=0x3F, TDO=$tdo_val"
assert_eq "SCAN_IR loopback TDO=3F" "OK TDO=3F" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[5] LA capture of SCAN_DR waveform${NC}"
# -----------------------------------------------------------

# Slow JTAG for LA visibility
result=$(cmd "JTAG FREQ 100000")
assert_eq "JTAG FREQ 100kHz" "OK" "$result"

result=$(cmd "LA INIT")
assert_eq "LA INIT" "OK" "$result"

# Pipeline LA + SCAN_DR: 5000 samples at divider 625 = 25ms capture
# Use direct read (not subshell read_line) to avoid bash buffer loss.
send_cmd "LA CAPTURE 5000 625"
send_cmd "JTAG SCAN_DR 8 A5"
sleep 0.2
read -r -t 2 la_resp <&3
la_resp=$(echo "$la_resp" | tr -d '\r')
assert_eq "LA CAPTURE" "OK capturing" "$la_resp"
read -r -t 2 dr_resp <&3
dr_resp=$(echo "$dr_resp" | tr -d '\r')
assert_prefix "JTAG SCAN_DR during LA" "OK TDO=" "$dr_resp"

sleep 1

result=$(cmd "LA STATUS")
assert_eq "LA STATUS done" "OK done 5000" "$result"

# Read first 200 samples and look for TCK activity
send_cmd "LA DATA 0 200"
sleep 0.1
read -r -t 2 la_hdr <&3
la_hdr=$(echo "$la_hdr" | tr -d '\r')

echo -e "\n  ${CYAN}LA capture (SCAN_DR 0xA5) - first 64 samples:${NC}"
echo -e "  ${CYAN}  Sample  Raw        TCK TMS TDI TDO${NC}"
sample_num=0
prev_tck=0
tck_edges=0
for i in $(seq 1 25); do
    read -r -t 2 dataline <&3
    dataline=$(echo "$dataline" | tr -d '\r')
    for sample in $dataline; do
        decoded=$(decode_jtag "$sample")
        if [ $sample_num -lt 64 ]; then
            printf "  %4d    %s  %s\n" "$sample_num" "$sample" "$decoded"
        fi
        val=$((16#$sample))
        tck=$(( (val >> 2) & 1 ))
        if [ $tck -eq 1 ] && [ $prev_tck -eq 0 ]; then
            tck_edges=$((tck_edges + 1))
        fi
        prev_tck=$tck
        sample_num=$((sample_num + 1))
    done
done

echo -e "\n  ${CYAN}INFO${NC} TCK rising edges captured: $tck_edges"

result=$(cmd "LA DEINIT")
assert_eq "LA DEINIT" "OK" "$result"

# Restore default JTAG frequency
result=$(cmd "JTAG FREQ 6250000")
assert_eq "JTAG FREQ restore" "OK" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[6] JTAG DETECT (loopback)${NC}"
# -----------------------------------------------------------

# With TDI->TDO direct wire, DETECT shifts all-1s and reads back all-1s
# which looks like IDCODE=0xFFFFFFFF, ending the chain scan
result=$(cmd "JTAG DETECT")
echo -e "  ${CYAN}INFO${NC} $result"
assert_prefix "JTAG DETECT response" "OK 0 devices" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[7] JTAG RUNTEST${NC}"
# -----------------------------------------------------------

result=$(cmd "JTAG RUNTEST 100")
assert_eq "JTAG RUNTEST 100" "OK" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[8] JTAG FREQ change${NC}"
# -----------------------------------------------------------

result=$(cmd "JTAG FREQ 1000000")
assert_eq "JTAG FREQ 1MHz" "OK" "$result"

# Scan should still work after freq change
result=$(cmd "JTAG SCAN_DR 8 55")
assert_eq "SCAN_DR after freq change" "OK TDO=55" "$result"

result=$(cmd "JTAG FREQ 6250000")
assert_eq "JTAG FREQ restore 6.25MHz" "OK" "$result"

# -----------------------------------------------------------
echo -e "\n${YELLOW}[9] Cleanup${NC}"
# -----------------------------------------------------------

result=$(cmd "JTAG DEINIT")
assert_eq "JTAG DEINIT" "OK" "$result"

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
