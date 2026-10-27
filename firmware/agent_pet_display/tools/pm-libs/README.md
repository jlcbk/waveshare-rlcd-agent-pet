# PM-enabled Arduino-ESP32 2.0.17 SDK libs (round 3)

The stock Arduino-ESP32 2.0.17 precompiled SDK (`tools/sdk/esp32s3`) has no
power management: `esp_pm_configure` is a stub that immediately returns
`ESP_ERR_NOT_SUPPORTED` (verified by disassembly: 3-instruction body returning
0x106), and `libfreertos.a` carries no tickless symbols. Auto light sleep
therefore requires rebuilding the SDK libs from source.

## What this build changes

1. `CONFIG_PM_ENABLE=y` and `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` appended to
   `esp32-arduino-lib-builder/configs/defconfig.esp32s3`. Both are required:
   `pm_impl.c` returns `ESP_ERR_NOT_SUPPORTED` for `light_sleep_enable=true`
   unless `CONFIG_FREERTOS_USE_TICKLESS_IDLE` is set.
2. `idf-v4.4.7-light-sleep-count.patch` on `esp-idf` v4.4.7
   (`components/esp_hw_support/sleep_modes.c`): counts completed light sleeps
   in an RTC-data counter, incremented only where `s_light_sleep_wakeup` is
   set (`err == ESP_OK`), exposed as
   `uint32_t esp_sleep_get_light_sleep_count(void)`. The firmware references
   it as a weak symbol, so the same firmware also links against the stock SDK
   (reporting "n/a").
3. `lib-builder-pin-components.patch` records the Arduino, esp32-camera,
   esp_littlefs and tinyusb revisions used by the local build. Its component
   update branches assume those repositories already exist: on a fresh clone,
   check the resulting component revisions explicitly before building.

## SDK rebuild notes

The measured board used `esp32-arduino-lib-builder` commit
`3eb9cb06afb534d37f47e98bca9f45e99126f81a` and ESP-IDF v4.4.7 commit
`38eeba213aa695aabfd6d89aa9f5078dbe5a94c3`. The rebuilt SDK and its full
builder checkout are **not** in this repository. A clean-machine rebuild has
not been independently verified; these are notes, not a one-command recipe.

In a separate lib-builder checkout, apply `lib-builder-pin-components.patch`
there and `idf-v4.4.7-light-sleep-count.patch` inside its ESP-IDF checkout.
Add `CONFIG_PM_ENABLE=y` and `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y` to
`configs/defconfig.esp32s3`, ensure the relevant components are at the commits
listed in the patch, then run `./build.sh -t esp32s3 -b idf_libs qio 80m qio_ram`
from the builder directory. The builder installs or updates dependencies and
replaces its own `build` and `sdkconfig` files; keep it separate from other work.
See the builder's prerequisites; on macOS it requires `gsed` and `gawk`.

The resulting `out/tools/sdk/esp32s3/` contains `lib`, `include`, `ld`,
`qio_qspi` and `sdkconfig`.

## Using a prepared PM framework with PlatformIO

First make a separate copy of the stock PlatformIO `framework-arduinoespressif32`
2.0.17 package and overlay the rebuilt `out/tools/sdk/esp32s3/` onto the copy's
`tools/sdk/esp32s3/`. Do not overwrite the global package. With all ordinary
PlatformIO packages already installed, use an isolated package directory:

```bash
cd firmware/agent_pet_display
export RLCD_PM_FRAMEWORK_DIR="/absolute/path/to/the/separate/pm-framework"
export RLCD_PIO_PACKAGES="$(mktemp -d "${TMPDIR:-/tmp}/rlcd-pio-packages.XXXXXX")"

for package in "$HOME"/.platformio/packages/*; do
  [ -d "$package" ] || continue
  name="$(basename "$package")"
  if [ "$name" = framework-arduinoespressif32 ]; then
    ln -s "$RLCD_PM_FRAMEWORK_DIR" "$RLCD_PIO_PACKAGES/$name"
  else
    ln -s "$package" "$RLCD_PIO_PACKAGES/$name"
  fi
done

PLATFORMIO_PACKAGES_DIR="$RLCD_PIO_PACKAGES" pio run -e waveshare_rlcd_pm
```

Keep `RLCD_PIO_PACKAGES` in the same shell when using the top-level upload
command. The isolated directory links other tool packages to the existing
PlatformIO installation; it does not contain a copy of the custom SDK. Without
the PM SDK, the firmware can still compile but falls back to a fixed CPU clock
without automatic light sleep. The older `use-pm-sdk.sh` is a machine-specific
global swap script; do not run it on another machine or use it as a general
installation method.

## Verification of the rebuilt libs

- `xtensa-esp32s3-elf-objdump -d libesp_pm.a`: `esp_pm_configure` is a real
  implementation (writes PM config, no immediate `0x106` return).
- `xtensa-esp32s3-elf-nm` on the rebuilt `libesp_hw_support.a` (or
  `libesp_pm.a` link set) shows `esp_sleep_get_light_sleep_count`.
- The effective `sdkconfig` in the lib-builder build dir shows both CONFIG
  lines set.
- This proves what was linked or configured, not the board's actual sleep time
  or average current. The cycle-based duty percentage is inaccurate when CPU
  frequency changes.
