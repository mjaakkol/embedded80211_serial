#!/usr/bin/env bash
# Dump the device RTT log buffer over J-Link SWD (Mac has no RTT CLI).
#
# The device logs to SEGGER RTT (LOG_BACKEND_RTT=y). The up-buffer lives in
# RAM; we read it with JLinkExe mem8 and decode the printed log lines.
#
# Control block layout (SEGGER_RTT_CB, see modules/debug/segger/SEGGER/SEGGER_RTT.h):
#   acID[16] @ 0x00, MaxNumUp/Down @ 0x10/0x14,
#   aUp[0]   @ 0x18: sName, pBuffer, SizeOfBuffer, WrOff, RdOff, Flags
# Addresses are from build/nrf7002dk (zephyr.elf):
#   _SEGGER_RTT (control block) = 0x20020040
#   aUp[0].pBuffer              = 0x20000040 (128 KB)
#
# Usage: ./rtt_dump.sh [bytes]   (default 8192: tail of the reconstructed ring)
set -uo pipefail

BYTES="${1:-8192}"
JLINK="/Applications/SEGGER/JLink_V876/JLinkExe"
CB=0x20020040

CMDFILE=$(mktemp /tmp/e80211-rtt-XXXX.cmd)
RAW=$(mktemp /tmp/e80211-rtt-XXXX.raw)
trap 'rm -f "$CMDFILE" "$RAW"' EXIT

# First pass: read the control block to learn buffer base/size/WrOff.
cat > "$CMDFILE" <<EOF
si SWD
speed 4000
device nRF5340
connect
halt
mem32 $CB 12
go
quit
EOF
OUT=$("$JLINK" -CommandFile "$CMDFILE" 2>&1)

# mem32 lines are 4 words (16 B) wide, addr-aligned. aUp[0] fields:
#   line CB+0x10: $3=MaxUp $4=MaxDown $5=sName $6=pBuffer
#   line CB+0x20: $3=SizeOfBuffer $4=WrOff $5=RdOff $6=Flags
PBUF=$(echo "$OUT" | awk -v c="$(printf '%X' $((CB + 0x10)))" '$1 == c && $2 == "=" {print $6}')
BSIZE=$(echo "$OUT" | awk -v c="$(printf '%X' $((CB + 0x20)))" '$1 == c && $2 == "=" {print $3}')
WR_OFF=$(echo "$OUT" | awk -v c="$(printf '%X' $((CB + 0x20)))" '$1 == c && $2 == "=" {print $4}')
if ! [[ "$PBUF" =~ ^[0-9A-Fa-f]{8}$ && "$BSIZE" =~ ^[0-9A-Fa-f]{8}$ && "$WR_OFF" =~ ^[0-9A-Fa-f]{8}$ ]]; then
    echo "rtt_dump: cannot parse control block (pbuf=$PBUF size=$BSIZE wroff=$WR_OFF)" >&2
    exit 1
fi
PBUF_DEC=$((PBUF))
BSIZE_DEC=$((BSIZE))
WR_OFF_DEC=$((WR_OFF))

# Second pass: dump the whole buffer, then reconstruct the ring (rotate at
# WrOff) and print its most recent $BYTES bytes.
cat > "$CMDFILE" <<EOF
si SWD
speed 4000
device nRF5340
connect
halt
mem8 $PBUF_DEC $BSIZE_DEC
go
quit
EOF
"$JLINK" -CommandFile "$CMDFILE" 2>/dev/null \
  | awk '/^[0-9A-Fa-f]{8} = / { sub(/^[0-9A-Fa-f]+ = /, ""); for (i = 1; i <= NF; i++) printf "%s", $i }' \
  | xxd -r -p > "$RAW"

if (( WR_OFF_DEC == 0 )); then
    echo "rtt_dump: RTT buffer empty (WrOff=0)" >&2
    exit 0
fi

{ tail -c "+$((WR_OFF_DEC + 1))" "$RAW"; head -c "$WR_OFF_DEC" "$RAW"; } \
  | tail -c "$BYTES" | tr -d '\000'
