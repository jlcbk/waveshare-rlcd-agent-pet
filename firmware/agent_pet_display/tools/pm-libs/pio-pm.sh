#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPO_ROOT="$(cd "$FIRMWARE_DIR/../.." && pwd)"

if [[ -z "${RLCD_PM_FRAMEWORK_DIR:-}" ]]; then
  for candidate in \
    "$HOME/.platformio/framework-arduinoespressif32-pm" \
    "$REPO_ROOT/../round3-build/framework-arduinoespressif32-pm"; do
    if [[ -d "$candidate" ]]; then
      RLCD_PM_FRAMEWORK_DIR="$(cd "$candidate" && pwd)"
      break
    fi
  done
fi

if [[ -z "${RLCD_PM_FRAMEWORK_DIR:-}" || ! -d "$RLCD_PM_FRAMEWORK_DIR" ]]; then
  cat >&2 <<'EOF'
error: PM-enabled Arduino-ESP32 framework not found.
Set RLCD_PM_FRAMEWORK_DIR=/absolute/path/to/framework-arduinoespressif32-pm
or place/symlink it at ~/.platformio/framework-arduinoespressif32-pm.
See firmware/agent_pet_display/tools/pm-libs/README.md for details.
EOF
  exit 1
fi

SDKCONFIG_HEADER="$RLCD_PM_FRAMEWORK_DIR/tools/sdk/esp32s3/qio_qspi/include/sdkconfig.h"
if [[ ! -f "$SDKCONFIG_HEADER" ]] \
  || ! grep -q '^#define CONFIG_PM_ENABLE 1' "$SDKCONFIG_HEADER" \
  || ! grep -q '^#define CONFIG_FREERTOS_USE_TICKLESS_IDLE 1' "$SDKCONFIG_HEADER"; then
  echo "error: $RLCD_PM_FRAMEWORK_DIR does not contain a PM + tickless ESP32-S3 SDK ($SDKCONFIG_HEADER)" >&2
  exit 1
fi

STOCK_PACKAGES_DIR="${RLCD_STOCK_PACKAGES_DIR:-$HOME/.platformio/packages}"
if [[ ! -d "$STOCK_PACKAGES_DIR" ]]; then
  echo "error: stock PlatformIO packages directory not found at $STOCK_PACKAGES_DIR" >&2
  exit 1
fi

PM_PACKAGES_DIR="${RLCD_PIO_PACKAGES_DIR:-$HOME/.platformio/packages-pm}"
mkdir -p "$PM_PACKAGES_DIR"

for existing in "$PM_PACKAGES_DIR"/*; do
  [[ -L "$existing" ]] && rm -f "$existing"
done

for pkg in "$STOCK_PACKAGES_DIR"/*; do
  [[ -d "$pkg" ]] || continue
  name="$(basename "$pkg")"
  if [[ "$name" == "framework-arduinoespressif32" ]]; then
    continue
  fi
  ln -sfn "$pkg" "$PM_PACKAGES_DIR/$name"
done
ln -sfn "$RLCD_PM_FRAMEWORK_DIR" "$PM_PACKAGES_DIR/framework-arduinoespressif32"

PIO_BIN="${PIO_BIN:-}"
if [[ -z "$PIO_BIN" ]]; then
  if command -v pio >/dev/null 2>&1; then
    PIO_BIN="$(command -v pio)"
  elif [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then
    PIO_BIN="$HOME/.platformio/penv/bin/pio"
  else
    echo "error: PlatformIO CLI (pio) not found in PATH or ~/.platformio/penv/bin/pio" >&2
    exit 1
  fi
fi

if [[ ! -f "platformio.ini" ]]; then
  cd "$FIRMWARE_DIR"
fi

exec env PLATFORMIO_PACKAGES_DIR="$PM_PACKAGES_DIR" "$PIO_BIN" "$@"
