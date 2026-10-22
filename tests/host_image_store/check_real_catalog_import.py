#!/usr/bin/env python3
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
TMP = Path(tempfile.mkdtemp(prefix="ki", dir="/tmp"))
STUBS = TMP / "stubs"
STUBS.mkdir(mode=0o700)
(STUBS / "freertos").mkdir()
(STUBS / "lvgl.h").write_text("typedef struct lv_obj_t lv_obj_t;\n")
(STUBS / "esp_err.h").write_text("""#ifndef ESP_ERR_H
#define ESP_ERR_H
typedef int esp_err_t;
#define ESP_OK 0
const char *esp_err_to_name(esp_err_t);
#endif
""".replace("\n+", "\n"))
(STUBS / "esp_littlefs.h").write_text("""#include <stddef.h>
#include "esp_err.h"
typedef struct { const char *base_path; const char *partition_label; int format_if_mount_failed; int read_only; int dont_mount; int grow_on_mount; } esp_vfs_littlefs_conf_t;
esp_err_t esp_vfs_littlefs_register(const esp_vfs_littlefs_conf_t *);
esp_err_t esp_vfs_littlefs_unregister(const char *);
esp_err_t esp_littlefs_info(const char *, size_t *, size_t *);
""".replace("\n+", "\n"))
(STUBS / "esp_log.h").write_text("#define ESP_LOGW(tag, ...) ((void)(tag))\n")
(STUBS / "esp_partition.h").write_text("""#include <stdint.h>
#include <stddef.h>
typedef struct { size_t size; } esp_partition_t;
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_LITTLEFS 0x83
const esp_partition_t *esp_partition_find_first(int,int,const char *);
""")
(STUBS / "esp_random.h").write_text("#include <stdint.h>\nuint32_t esp_random(void);\n")
(STUBS / "freertos/FreeRTOS.h").write_text("#define pdTRUE 1\n#define portMAX_DELAY 0x7fffffff\n")
(STUBS / "freertos/semphr.h").write_text("typedef void *SemaphoreHandle_t;\nSemaphoreHandle_t xSemaphoreCreateMutex(void);\nint xSemaphoreTake(SemaphoreHandle_t, int);\nint xSemaphoreGive(SemaphoreHandle_t);\n")

source = "#define _POSIX_C_SOURCE 200809L\n" + (ROOT / "firmware/components/katmed_images/katmed_image_store_idf.c").read_text()
source = source.replace('#define STORE_MOUNT_POINT "/katmed"', f'#define STORE_MOUNT_POINT "{TMP}/ke"')
assert source != (ROOT / "firmware/components/katmed_images/katmed_image_store_idf.c").read_text()
(TMP / "store.c").write_text(source)

image_dir = ROOT / "assets/generated/c3_240x240"
images = sorted(image_dir.glob("*.jpg"))[:2]
if len(images) != 2:
    raise SystemExit("need two checked-in generated C3 JPEG fixtures")

(TMP / "test.c").write_text((ROOT / "tests/host_image_store/test_real_catalog_import.c").read_text().replace("/tmp/ke", str(TMP / "ke")))

try:
    binary = TMP / "check"
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
        f"-I{STUBS}",
        f"-I{ROOT / 'firmware/components/katmed_security/include'}",
        f"-I{ROOT / 'firmware/components/katmed_images'}",
        f"-I{ROOT / 'firmware/components/katmed_images/include'}",
        f"-I{ROOT / 'firmware/components/katmed_core/include'}",
        str(TMP / "store.c"),
        str(ROOT / "firmware/components/katmed_images/katmed_images_idf.c"),
        str(ROOT / "firmware/components/katmed_images/katmed_image_store_cleanup.c"),
        str(TMP / "test.c"),
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), str(images[0]), str(images[1])], check=True)
finally:
    shutil.rmtree(TMP)
