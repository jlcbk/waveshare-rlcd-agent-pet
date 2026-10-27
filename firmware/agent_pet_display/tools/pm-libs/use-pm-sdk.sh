#!/usr/bin/env bash
# Swap the global PlatformIO Arduino-ESP32 framework package between the
# stock 2.0.17 build and the round-3 PM-enabled rebuild (see README.md).
#
#   tools/pm-libs/use-pm-sdk.sh pm      # install the PM libs globally
#   tools/pm-libs/use-pm-sdk.sh stock   # restore the stock libs
#   tools/pm-libs/use-pm-sdk.sh status  # report what is installed
#
# PlatformIO's platform_packages file:// mechanism overwrites the global
# package in place, so we manage the swap explicitly and leave the tree in a
# known state instead.
set -euo pipefail

STOCK_SLOT="$HOME/.platformio/packages/framework-arduinoespressif32"
STOCK_KEEP="/Users/cui/Documents/Projects/RLCD_firm1/round3-build/framework-stock-backup"
PM_SRC="/Users/cui/Documents/Projects/RLCD_firm1/round3-build/framework-arduinoespressif32-pm"
HWLIB="tools/sdk/esp32s3/qio_qspi/libesp_hw_support.a"
NMCMD="$HOME/.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-nm"

is_pm() {  # PM libs carry the light-sleep counter symbol
  [ -f "$1/$HWLIB" ] || return 1
  local out
  out="$($NMCMD "$1/$HWLIB" 2>/dev/null)"
  [[ "$out" == *esp_sleep_get_light_sleep_count* ]]
}

mode="${1:-status}"
case "$mode" in
  status)
    if [ ! -d "$STOCK_SLOT" ]; then echo "slot: empty"; fi
    if is_pm "$STOCK_SLOT"; then echo "slot: PM"; else echo "slot: stock"; fi
    [ -d "$STOCK_KEEP" ] && echo "backup: stock kept" || echo "backup: none"
    ;;
  pm)
    if is_pm "$STOCK_SLOT"; then echo "already PM"; exit 0; fi
    if [ ! -d "$STOCK_KEEP" ] && [ -d "$STOCK_SLOT" ]; then
      mv "$STOCK_SLOT" "$STOCK_KEEP"
      echo "stock moved to backup"
    fi
    rm -rf "$STOCK_SLOT"
    cp -cR "$PM_SRC" "$STOCK_SLOT"   # APFS clone: near-zero extra space
    is_pm "$STOCK_SLOT" && echo "PM installed"
    ;;
  stock)
    if ! is_pm "$STOCK_SLOT"; then echo "already stock"; exit 0; fi
    rm -rf "$STOCK_SLOT"
    if [ -d "$STOCK_KEEP" ]; then
      mv "$STOCK_KEEP" "$STOCK_SLOT"
      echo "stock restored from backup"
    else
      echo "stock restored slot is empty; run 'pio run -e waveshare_rlcd' to reinstall"
    fi
    ;;
  *) echo "usage: $0 {pm|stock|status}" >&2; exit 2 ;;
esac
