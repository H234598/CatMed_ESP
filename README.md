# CatMed ESP

ESP-IDF 6.1 firmware for the round ESP32-C3 240×240 GC9A01 display and the
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
mean 256 simultaneous connections. You can do much more, including displaying a
custom image of the device.

## Build and tests

Use ESP-IDF **6.1** and its Python environment. Component versions are pinned in
`firmware/main/idf_component.yml`. The standard build is neutral and does not
need a private profile. Generate its build-only headers without `--config`, then
build each target in a separate directory:

```sh
python tools/private_wifi_defaults.py \
  --output firmware/build-c3/generated/private_wifi_defaults.h \
  --ui-profile-output firmware/build-c3/generated/katmed_private_ui_profile.h
cd firmware
idf.py -B build-c3 -D IDF_TARGET=esp32c3 \
  -D SDKCONFIG=build-c3/sdkconfig \
  -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;boards/c3.defaults' build
```

For a deliberately selected private build, pass an external profile to the same
generator command with `--config /path/to/private-profile.json`; keep that file
outside the repository. The profile is optional and does not create a second
firmware path. Stored device WLAN settings take precedence over generated
defaults. For the other target, use `c6`, `esp32c6` and the matching `build-c6`
output paths.
The hash-bound SDK compatibility helper `tools/prepare_idf_compat.py` documents
the small ESP-IDF/IoT-Bridge adjustment; only apply it to a dedicated owned SDK,
as enforced by the helper.

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
