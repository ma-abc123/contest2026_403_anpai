#!/bin/bash
# boottest.sh <binary> <label> [seconds]
# Flash a firmware, hard-reset the board, capture the console and report
# whether the BLE controller came up (advertising) within the window.

BIN="$1"
LABEL="$2"
SECS="${3:-30}"
LOG="/home/ma/huangshan-logs/boottest_${LABEL}_$(date +%H%M%S).log"

fuser -k /dev/ttyUSB0 2>/dev/null
sleep 1

OK=0
for i in 1 2 3; do
  sftool -c SF32LB52 -p /dev/ttyUSB0 -b 1000000 \
      --before default_reset --after soft_reset \
      write_flash "${BIN}@0x12010000" > /home/ma/tmp/boottest_flash.log 2>&1
  if [ $? -eq 0 ] && ! grep -q "Failed" /home/ma/tmp/boottest_flash.log; then
    OK=1
    break
  fi
  echo "  flash retry $i"
  sleep 3
done

if [ $OK -eq 0 ]; then
  echo "${LABEL}: FLASH-FAILED"
  exit 2
fi

sleep 8
cd /home/ma/tmp && /usr/bin/python3 cap_boot.py "$SECS" "$LOG" >/dev/null 2>&1

VER=$(grep -m1 "AI Watch version" "$LOG" | sed 's/.*version //')
ADV=$(grep -c "advertising as" "$LOG")
RST=$(grep -c "Sending HCI_RESET" "$LOG")
UNR=$(grep -c "Controller unresponsive" "$LOG")

if [ "$ADV" -gt 0 ]; then
  echo "${LABEL}: PASS  ver=${VER}  advertising=${ADV}  hci_reset=${RST}  unresponsive=${UNR}  log=$(basename "$LOG")"
else
  echo "${LABEL}: FAIL  ver=${VER}  advertising=0  hci_reset=${RST}  unresponsive=${UNR}  log=$(basename "$LOG")"
fi
