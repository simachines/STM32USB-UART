#!/usr/bin/env bash
# ============================================================
# STM32F401 USB-UART Bridge - USB via DFU Flash Script (Linux/macOS)
# ============================================================
# Prerequisites:
#   1. STM32CubeProgrammer installed (provides STM32_Programmer_CLI)
#      OR dfu-util (apt install dfu-util / brew install dfu-util)
#   2. Board in DFU mode: BOOT0=1 + press RESET
# ============================================================
set -euo pipefail

# ---- Config ----
HEX="../EWARM/STM32F401 USB-UART Bridge PAssthrough/Exe/STM32F401 USB-UART Bridge PAssthrough.hex"
# Try to locate STM32_Programmer_CLI, else fall back to dfu-util
CUBEPROG="$(command -v STM32_Programmer_CLI || true)"
DFUUTIL="$(command -v dfu-util || true)"
# ----------------

if [[ ! -f "$HEX" ]]; then
  echo "[ERROR] Hex file not found: $HEX"
  echo "        Build the project in IAR first."
  exit 1
fi

if [[ -n "$CUBEPROG" ]]; then
  echo "[1/2] Listing USB DFU devices..."
  "$CUBEPROG" -list port=usb
  echo "[2/2] Flashing over USB DFU..."
  "$CUBEPROG" -c port=usb -w "$HEX" -v
elif [[ -n "$DFUUTIL" ]]; then
  echo "[1/2] Flashing over USB DFU with dfu-util..."
  "$DFUUTIL" -a 0 -s 0x08000000:leave -D "$HEX"
else
  echo "[ERROR] Neither STM32_Programmer_CLI nor dfu-util found."
  echo "        Install STM32CubeProgrammer or dfu-util."
  exit 1
fi

echo "[OK] Flashed. Set BOOT0=0 and press RESET to run."