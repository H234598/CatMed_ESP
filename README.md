# CatMed ESP

ESP-IDF 5.5.5 firmware for the round ESP32-C3 240×240 GC9A01 display and the
ESP32-C6 172×320 display. It provides medication reminders, shared authenticated
Mesh state, background images, analog/digital clocks and a three-row interface:

| | Left | Centre | Right |
|---|---|---|---|
| Top | Alarm placeholder | Alarm placeholder | Alarm placeholder |
| Middle | Clock | Home | Settings |
| Bottom | Status | Status | Status |

Vertical swipes cycle through rows while retaining the column. Swipe from the
upper/lower edge to navigate from a scrolling list; scroll inside the list.
The red Alarm button is deliberately inactive. Medication alarms retain their
separate confirmation flow and always take display priority.

Settings offer **Morsecode**, a transient half-duplex light interface coordinated
by the existing dynamic Mesh root, and **Seltsam**, the unchanged local repeating
Morse display. Reception works outside the communication menu. A confirmed new
Mesh member triggers a locally dismissible, non-blinking welcome lasting at most
30 minutes. History is bounded to 256 unique device identities; this does not
mean 256 simultaneous connections. You can do much more.
A custom image of the device.

## Build and tests

Use ESP-IDF **5.5.5** and its Python environment. Component versions are pinned in
`firmware/main/idf_component.yml`. Build each target in a separate directory:

```sh
cd firmware
idf.py -B build-c3 -D IDF_TARGET=esp32c3 \
  -D SDKCONFIG=build-c3/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/c3.defaults' build
```

Before a local build, generate the required build-only WLAN header with
`tools/private_wifi_defaults.py --config /your/private/config.json --output
firmware/build-c3/generated/private_wifi_defaults.h`. The JSON contains `ssid`
and `password`, lives in an owned mode-0700 directory and has mode 0600. Keep it
outside Git. Stored device WLAN settings take precedence over these defaults.
Use `c6`/`esp32c6` for the other target. The hash-bound SDK compatibility helper
`tools/prepare_idf_compat.py` documents the small ESP-IDF/IoT-Bridge adjustment;
only apply it to a dedicated owned SDK, as enforced by the helper.

`tools/ci.sh` is the reproducible CI entry point. GitHub Actions checks host
contracts and both firmware builds in the official ESP-IDF container using only
synthetic temporary WLAN values. It uploads no firmware, credentials or device
backups. Local host UI tests use `firmware/components/katmed_ui/host` with
`KATMED_UI_HOST_MEM34=ON` to exercise the 34 KiB LVGL pool. Root CMake/CTest also
checks the reducer and the checked-in image packs.

## Flashing

No flashing or
network provisioning runs in CI. Device backups, private firmware, local network
configuration and project working reports are excluded from this repository.

## Pics
Public host CI checks the reducer, prepared JPEG/pack checksums and crop geometry,
the three UI formats with the 34 KiB pool, main contacts, authenticated Mesh,
transient Morse, admission persistence and Store contracts. Original image files
are external: generation requires `tools/prepare_images.py --source /your/archive`;
`--verify-only` validates checked-in prepared assets without that archive.
Historical local diagnosis fixtures that execute private working documents or
require private machine paths are excluded from this public source snapshot.
