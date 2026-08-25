@echo off
REM ============================================================
REM  STM32F401 USB-UART Bridge - USB DFU Flash Script (Windows)
REM ============================================================
REM  Prerequisites:
REM    1. STM32CubeProgrammer installed (provides STM32_Programmer_CLI)
REM    2. Board in DFU mode: BOOT0=HIGH + press RESET
REM       -> PC should show a "STM32 BOOTLOADER" / DFU device
REM ============================================================

setlocal

REM ---- Adjust these to match your setup ----
set "CUBEPROG=C:\Program Files\STMicroelectronics\STM32Cube\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe"
set "HEX=..\EWARM\STM32F401 USB-UART Bridge PAssthrough\Exe\STM32F401 USB-UART Bridge PAssthrough.hex"
REM ------------------------------------------

if not exist "%CUBEPROG%" (
  echo [ERROR] STM32_Programmer_CLI not found at:
  echo         %CUBEPROG%
  echo         Edit CUBEPROG in this script to point to your install.
  exit /b 1
)

if not exist "%HEX%" (
  echo [ERROR] Firmware not found at:
  echo         %HEX%
  echo         Build the project in IAR first.
  exit /b 1
)

echo.
echo [1/2] Checking for DFU device on USB...
"%CUBEPROG%" -c port=usb1

echo.
echo [2/2] Flashing firmware over USB DFU...
"%CUBEPROG%" -c port=usb -w "%HEX%" -v -rst

if %errorlevel% neq 0 (
  echo.
  echo [FAIL] Flash failed. Make sure the board is in DFU mode:
  echo        BOOT0=1 + press RESET, then re-run this script.
  exit /b 1
)

echo.
echo [OK] Flashed successfully. Set BOOT0=0 and press RESET to run.
endlocal