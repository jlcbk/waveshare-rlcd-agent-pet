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
3. `lib-builder-pin-components.patch`: pins esp32-camera, esp_littlefs and
   tinyusb to the commits recorded in the stock package
   `tools/sdk/versions.txt` (the official 2.0.17 build), so the lib-builder's
   update-components.sh cannot drift them to master (newer manifests require
   IDF >= 5.0).

## Reproducing

```
git clone --depth 1 -b release/v4.4 https://github.com/espressif/esp32-arduino-lib-builder.git
git clone --depth 1 -b v4.4.7 --recurse-submodules --shallow-submodules \
    https://github.com/espressif/esp-idf.git esp32-arduino-lib-builder/esp-idf
# pin the three loose components (see lib-builder-pin-components.patch), then:
#   apply idf-v4.4.7-light-sleep-count.patch inside esp-idf
#   append the two CONFIG_ lines to configs/defconfig.esp32s3
# (macOS needs gsed/gawk available; shims that forward to BSD sed/awk suffice)
./build.sh -t esp32s3 -b idf_libs qio 80m qio_ram
```

The result lands in `out/tools/sdk/esp32s3/{lib,include,ld}`.

## Using it with PlatformIO

Copy the stock `framework-arduinoespressif32` package (APFS `cp -c`, near-zero
extra space), replace `tools/sdk/esp32s3` with the rebuilt one, and point the
`waveshare_rlcd_pm*` environments at the copy via `platform_packages` (see
platformio.ini). The stock `waveshare_rlcd` environment is unchanged so the
baseline build and rollback stay on official binaries.

## Verification of the rebuilt libs

- `xtensa-esp32s3-elf-objdump -d libesp_pm.a`: `esp_pm_configure` is a real
  implementation (writes PM config, no immediate `0x106` return).
- `xtensa-esp32s3-elf-nm` on the rebuilt `libesp_hw_support.a` (or
  `libesp_pm.a` link set) shows `esp_sleep_get_light_sleep_count`.
- The effective `sdkconfig` in the lib-builder build dir shows both CONFIG
  lines set.
