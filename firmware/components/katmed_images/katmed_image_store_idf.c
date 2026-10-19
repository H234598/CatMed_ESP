#include "katmed_image_store.h"
#include "katmed_images_internal.h"
#include "katmed_image_store_cleanup.h"

#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_PARTITION "user_images"
#define STORE_MOUNT_POINT "/katmed"
#define STORE_DIRECTORY STORE_MOUNT_POINT "/user-images"
#define STORE_MAX_UPLOADS 2u
#define STORE_MAX_READERS KATMED_IMAGE_MAX_OPEN_FILES
#define STORE_FREE_RESERVE_BYTES (8u * 1024u)
#define CATALOG_CACHE_ID_FLAG UINT32_C(0x80000000)

typedef struct {
    FILE *stream;
    uint32_t token;
    uint32_t expected_bytes;
    uint32_t written_bytes;
    char temporary_path[56];
} UploadSlot;

typedef struct {
    FILE *stream;
    uint32_t token;
    uint32_t id;
    uint32_t size;
    uint32_t position;
} ReaderSlot;

static const char *tag = "katmed_image_store";
static SemaphoreHandle_t store_lock;
static UploadSlot uploads[STORE_MAX_UPLOADS];
static ReaderSlot readers[STORE_MAX_READERS];
static uint32_t next_token = 1u;
static bool mounted;

static bool enter_store(void)
{
    return mounted && store_lock != NULL &&
           xSemaphoreTake(store_lock, portMAX_DELAY) == pdTRUE;
}

static void leave_store(void)
{
    (void)xSemaphoreGive(store_lock);
}

static bool catalog_image_path(uint32_t index, uint32_t profile,
                               char *path, size_t capacity)
{
    if (index >= KATMED_IMAGE_COUNT || profile > 1u) return false;
    int n = snprintf(path, capacity, STORE_DIRECTORY "/cat-%02" PRIx32 "-%u.jpg",
                     index, (unsigned)profile);
    return n > 0 && (size_t)n < capacity;
}

static bool image_path(uint32_t id, char *path, size_t capacity)
{
    if (id == 0u) return false;
    if ((id & CATALOG_CACHE_ID_FLAG) != 0u) {
        uint32_t encoded = id & ~CATALOG_CACHE_ID_FLAG;
        if ((encoded >> 1u) < KATMED_IMAGE_COUNT) {
            char cached[56];
            struct stat info;
            if (!catalog_image_path(encoded >> 1u, encoded & 1u,
                                    cached, sizeof(cached))) return false;
            if (stat(cached, &info) == 0) {
                int n = snprintf(path, capacity, "%s", cached);
                return n > 0 && (size_t)n < capacity;
            }
            if (errno != ENOENT) return false;
        }
    }
    int n = snprintf(path, capacity, STORE_DIRECTORY "/img-%08" PRIx32 ".jpg", id);
    return n > 0 && (size_t)n < capacity;
}

static bool temporary_path(uint32_t token, char *path, size_t capacity)
{
    int n = snprintf(path, capacity, STORE_DIRECTORY "/.upload-%08" PRIx32 ".tmp", token);
    return n > 0 && (size_t)n < capacity;
}

static bool parse_image_name(const char *name, uint32_t *id)
{
    if (name == NULL || id == NULL) return false;
    if (strlen(name) == 12u && memcmp(name, "cat-", 4u) == 0 &&
        name[6] == '-' && (name[7] == '0' || name[7] == '1') &&
        memcmp(name + 8u, ".jpg", 4u) == 0) {
        uint32_t index = 0u;
        for (size_t i = 4u; i < 6u; ++i) {
            unsigned char c = (unsigned char)name[i];
            uint32_t digit;
            if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
            else return false;
            index = (index << 4u) | digit;
        }
        if (index >= KATMED_IMAGE_COUNT) return false;
        *id = CATALOG_CACHE_ID_FLAG | (index << 1u) | (uint32_t)(name[7] - '0');
        return true;
    }
    if (strlen(name) != 16u || memcmp(name, "img-", 4u) != 0 ||
        memcmp(name + 12u, ".jpg", 4u) != 0) return false;
    uint32_t value = 0;
    for (size_t i = 4u; i < 12u; ++i) {
        unsigned char c = (unsigned char)name[i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
        else return false;
        value = (value << 4u) | digit;
    }
    if (value == 0u) return false;
    *id = value;
    return true;
}

static KatmedImageStatus file_info_locked(uint32_t id, KatmedUserImageInfo *out)
{
    char path[56];
    if (!image_path(id, path, sizeof(path))) return KATMED_IMAGE_BAD_ID;
    FILE *stream = fopen(path, "rb");
    if (stream == NULL) return errno == ENOENT ? KATMED_IMAGE_NOT_FOUND : KATMED_IMAGE_IO_ERROR;
    if (fseek(stream, 0, SEEK_END) != 0) {
        fclose(stream);
        return KATMED_IMAGE_IO_ERROR;
    }
    long length = ftell(stream);
    if (length < 4 || (unsigned long)length > KATMED_IMAGE_UPLOAD_MAX_BYTES ||
        fseek(stream, 0, SEEK_SET) != 0) {
        fclose(stream);
        return KATMED_IMAGE_BAD_RANGE;
    }

    uint16_t width = 0, height = 0;
    int first = fgetc(stream), second = fgetc(stream);
    bool found_frame = first == 0xff && second == 0xd8;
    found_frame = false;
    while (first == 0xff && second == 0xd8 && !found_frame) {
        int marker_prefix;
        do { marker_prefix = fgetc(stream); } while (marker_prefix == 0xff);
        if (marker_prefix == EOF || marker_prefix == 0x00 || marker_prefix == 0xd8 ||
            marker_prefix == 0xd9 || (marker_prefix >= 0xd0 && marker_prefix <= 0xd7)) break;
        int high = fgetc(stream), low = fgetc(stream);
        if (high == EOF || low == EOF) break;
        uint16_t segment_length = (uint16_t)(((unsigned)high << 8u) | (unsigned)low);
        if (segment_length < 2u) break;
        if (marker_prefix == 0xc0) {
            int precision = fgetc(stream);
            int hh = fgetc(stream), hl = fgetc(stream), wh = fgetc(stream), wl = fgetc(stream);
            int components = fgetc(stream);
            if (precision != 8 || hh == EOF || hl == EOF || wh == EOF || wl == EOF ||
                components == EOF || segment_length < 8u) break;
            height = (uint16_t)(((unsigned)hh << 8u) | (unsigned)hl);
            width = (uint16_t)(((unsigned)wh << 8u) | (unsigned)wl);
            found_frame = (components == 1 || components == 3) && width > 0u && height > 0u &&
                          width <= KATMED_IMAGE_UPLOAD_MAX_WIDTH &&
                          height <= KATMED_IMAGE_UPLOAD_MAX_HEIGHT;
            break;
        }
        if (marker_prefix == 0xc2 || marker_prefix == 0xda ||
            fseek(stream, (long)segment_length - 2L, SEEK_CUR) != 0) break;
    }
    fclose(stream);
    if (!found_frame) return KATMED_IMAGE_BAD_CATALOG;
    *out = (KatmedUserImageInfo){.id=id, .bytes=(uint32_t)length,
                                 .width=width, .height=height};
    return KATMED_IMAGE_OK;
}

static UploadSlot *upload_slot_locked(KatmedImageUpload upload)
{
    for (size_t i = 0; i < STORE_MAX_UPLOADS; ++i)
        if (uploads[i].stream != NULL && uploads[i].token == upload.token) return &uploads[i];
    return NULL;
}

static ReaderSlot *reader_slot_locked(KatmedImageReader reader)
{
    for (size_t i = 0; i < STORE_MAX_READERS; ++i)
        if (readers[i].stream != NULL && readers[i].token == reader.token) return &readers[i];
    return NULL;
}

static uint32_t new_token_locked(void)
{
    if (++next_token == 0u) ++next_token;
    return next_token;
}

static uint32_t new_image_id_locked(void)
{
    for (unsigned attempt = 0; attempt < 32u; ++attempt) {
        uint32_t id = esp_random();
        char path[56];
        struct stat info;
        if (id != 0u && (id & CATALOG_CACHE_ID_FLAG) == 0u &&
            image_path(id, path, sizeof(path)) && stat(path, &info) != 0 && errno == ENOENT)
            return id;
    }
    return 0u;
}

KatmedImageStatus katmed_image_store_mount(void)
{
    if (mounted) return KATMED_IMAGE_OK;
    if (store_lock == NULL) store_lock = xSemaphoreCreateMutex();
    if (store_lock == NULL) return KATMED_IMAGE_UNAVAILABLE;

    esp_vfs_littlefs_conf_t config = {
        .base_path = STORE_MOUNT_POINT,
        .partition_label = STORE_PARTITION,
        .format_if_mount_failed = false,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    esp_err_t error = esp_vfs_littlefs_register(&config);
    if (error != ESP_OK) {
        ESP_LOGW(tag, "user image filesystem unavailable (%s); partition left unchanged",
                 esp_err_to_name(error));
        return KATMED_IMAGE_UNAVAILABLE;
    }

    struct stat info;
    if (stat(STORE_DIRECTORY, &info) != 0) {
        if (errno != ENOENT || mkdir(STORE_DIRECTORY, 0700) != 0) {
            (void)esp_vfs_littlefs_unregister(STORE_PARTITION);
            return KATMED_IMAGE_IO_ERROR;
        }
    } else if (!S_ISDIR(info.st_mode)) {
        (void)esp_vfs_littlefs_unregister(STORE_PARTITION);
        return KATMED_IMAGE_BAD_CATALOG;
    }
    if (!katmed_image_store_cleanup_orphan_uploads(STORE_DIRECTORY)) {
        (void)esp_vfs_littlefs_unregister(STORE_PARTITION);
        return KATMED_IMAGE_IO_ERROR;
    }
    mounted = true;
    return KATMED_IMAGE_OK;
}

void katmed_image_store_unmount(void)
{
    if (!mounted || !enter_store()) return;
    for (size_t i = 0; i < STORE_MAX_UPLOADS; ++i) {
        if (uploads[i].stream != NULL) {
            fclose(uploads[i].stream);
            (void)unlink(uploads[i].temporary_path);
            memset(&uploads[i], 0, sizeof(uploads[i]));
        }
    }
    for (size_t i = 0; i < STORE_MAX_READERS; ++i) {
        if (readers[i].stream != NULL) fclose(readers[i].stream);
        memset(&readers[i], 0, sizeof(readers[i]));
    }
    mounted = false;
    leave_store();
    (void)esp_vfs_littlefs_unregister(STORE_PARTITION);
}

KatmedImageStatus katmed_image_store_upload_begin(uint32_t expected_bytes,
                                                   KatmedImageUpload *out)
{
    if (out == NULL || expected_bytes < 16u || expected_bytes > KATMED_IMAGE_UPLOAD_MAX_BYTES)
        return KATMED_IMAGE_BAD_RANGE;
    if (!enter_store()) return KATMED_IMAGE_UNAVAILABLE;
    UploadSlot *slot = NULL;
    for (size_t i = 0; i < STORE_MAX_UPLOADS; ++i)
        if (uploads[i].stream == NULL) { slot = &uploads[i]; break; }
    if (slot == NULL) { leave_store(); return KATMED_IMAGE_BUSY; }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(STORE_PARTITION, &total, &used) != ESP_OK || used > total ||
        total - used < (size_t)expected_bytes + STORE_FREE_RESERVE_BYTES) {
        leave_store();
        return KATMED_IMAGE_NO_SPACE;
    }
    uint32_t token = new_token_locked();
    char path[sizeof(slot->temporary_path)];
    if (!temporary_path(token, path, sizeof(path))) {
        leave_store();
        return KATMED_IMAGE_BAD_ID;
    }
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) {
        KatmedImageStatus result = errno == ENOSPC ? KATMED_IMAGE_NO_SPACE : KATMED_IMAGE_IO_ERROR;
        leave_store();
        return result;
    }
    *slot = (UploadSlot){.stream=stream, .token=token, .expected_bytes=expected_bytes};
    memcpy(slot->temporary_path, path, strlen(path) + 1u);
    *out = (KatmedImageUpload){.token=token};
    leave_store();
    return KATMED_IMAGE_OK;
}

KatmedImageStatus katmed_image_store_upload_write(KatmedImageUpload upload,
                                                    const uint8_t *bytes,
                                                    size_t length)
{
    if (bytes == NULL || length == 0u) return KATMED_IMAGE_BAD_RANGE;
    if (!enter_store()) return KATMED_IMAGE_UNAVAILABLE;
    UploadSlot *slot = upload_slot_locked(upload);
    if (slot == NULL || length > slot->expected_bytes - slot->written_bytes) {
        leave_store();
        return KATMED_IMAGE_BAD_STATE;
    }
    size_t written = fwrite(bytes, 1u, length, slot->stream);
    slot->written_bytes += (uint32_t)written;
    if (written != length) {
        leave_store();
        return errno == ENOSPC ? KATMED_IMAGE_NO_SPACE : KATMED_IMAGE_IO_ERROR;
    }
    leave_store();
    return KATMED_IMAGE_OK;
}

KatmedImageStatus katmed_image_store_upload_commit(KatmedImageUpload upload,
                                                     KatmedUserImageInfo *out)
{
    if (out == NULL || !enter_store()) return KATMED_IMAGE_UNAVAILABLE;
    UploadSlot *slot = upload_slot_locked(upload);
    if (slot == NULL) { leave_store(); return KATMED_IMAGE_BAD_STATE; }
    if (slot->written_bytes != slot->expected_bytes) {
        leave_store();
        return KATMED_IMAGE_BAD_RANGE;
    }
    if (fflush(slot->stream) != 0 || fsync(fileno(slot->stream)) != 0) {
        leave_store();
        return errno == ENOSPC ? KATMED_IMAGE_NO_SPACE : KATMED_IMAGE_IO_ERROR;
    }
    if (fclose(slot->stream) != 0) {
        slot->stream = NULL;
        (void)unlink(slot->temporary_path);
        memset(slot, 0, sizeof(*slot));
        leave_store();
        return KATMED_IMAGE_IO_ERROR;
    }
    slot->stream = NULL;

    KatmedImageStatus result = KATMED_IMAGE_OK;
    uint16_t width = 0, height = 0;
    /* Validate the temporary JPEG before it receives a public image name. */
    FILE *temporary = fopen(slot->temporary_path, "rb");
    if (temporary == NULL) result = KATMED_IMAGE_IO_ERROR;
    if (temporary != NULL) {
        if (fseek(temporary, 0, SEEK_END) != 0) result = KATMED_IMAGE_IO_ERROR;
        long length = ftell(temporary);
        if (length < 4 || (uint32_t)length != slot->expected_bytes ||
            fseek(temporary, 0, SEEK_SET) != 0) result = KATMED_IMAGE_BAD_RANGE;
        int first = fgetc(temporary), second = fgetc(temporary);
        bool found_frame = false;
        if (first == 0xff && second == 0xd8) {
            while (!found_frame) {
                int marker_prefix;
                do { marker_prefix = fgetc(temporary); } while (marker_prefix == 0xff);
                if (marker_prefix == EOF || marker_prefix == 0x00 || marker_prefix == 0xd8 ||
                    marker_prefix == 0xd9 || (marker_prefix >= 0xd0 && marker_prefix <= 0xd7)) break;
                int high = fgetc(temporary), low = fgetc(temporary);
                if (high == EOF || low == EOF) break;
                uint16_t segment_length = (uint16_t)(((unsigned)high << 8u) | (unsigned)low);
                if (segment_length < 2u) break;
                if (marker_prefix == 0xc0) {
                    int precision = fgetc(temporary);
                    int hh = fgetc(temporary), hl = fgetc(temporary);
                    int wh = fgetc(temporary), wl = fgetc(temporary), components = fgetc(temporary);
                    if (precision != 8 || hh == EOF || hl == EOF || wh == EOF || wl == EOF ||
                        components == EOF || segment_length < 8u) break;
                    height = (uint16_t)(((unsigned)hh << 8u) | (unsigned)hl);
                    width = (uint16_t)(((unsigned)wh << 8u) | (unsigned)wl);
                    if ((components != 1 && components != 3) || width == 0u || height == 0u ||
                        width > KATMED_IMAGE_UPLOAD_MAX_WIDTH ||
                        height > KATMED_IMAGE_UPLOAD_MAX_HEIGHT) break;
                    found_frame = true;
                    break;
                }
                if (marker_prefix == 0xc2 || marker_prefix == 0xda ||
                    fseek(temporary, (long)segment_length - 2L, SEEK_CUR) != 0) break;
            }
        }
        fclose(temporary);
        if (!found_frame) result = KATMED_IMAGE_BAD_CATALOG;
        else result = KATMED_IMAGE_OK;
    }
    if (result != KATMED_IMAGE_OK) {
        (void)unlink(slot->temporary_path);
        memset(slot, 0, sizeof(*slot));
        leave_store();
        return result;
    }

    uint32_t id = new_image_id_locked();
    char final_path[56];
    if (id == 0u || !image_path(id, final_path, sizeof(final_path))) {
        (void)unlink(slot->temporary_path);
        memset(slot, 0, sizeof(*slot));
        leave_store();
        return KATMED_IMAGE_BUSY;
    }
    if (rename(slot->temporary_path, final_path) != 0) {
        KatmedImageStatus rename_result = errno == ENOSPC ? KATMED_IMAGE_NO_SPACE : KATMED_IMAGE_IO_ERROR;
        (void)unlink(slot->temporary_path);
        memset(slot, 0, sizeof(*slot));
        leave_store();
        return rename_result;
    }
    *out = (KatmedUserImageInfo){.id=id, .bytes=slot->written_bytes,
                                 .width=width, .height=height};
    memset(slot, 0, sizeof(*slot));
    leave_store();
    return KATMED_IMAGE_OK;
}

KatmedImageStatus katmed_image_store_upload_abort(KatmedImageUpload upload)
{
    if (!enter_store()) return KATMED_IMAGE_UNAVAILABLE;
    UploadSlot *slot = upload_slot_locked(upload);
    if (slot == NULL) { leave_store(); return KATMED_IMAGE_BAD_STATE; }
    bool failed = fclose(slot->stream) != 0;
    if (unlink(slot->temporary_path) != 0 && errno != ENOENT) failed = true;
    memset(slot, 0, sizeof(*slot));
    leave_store();
    return failed ? KATMED_IMAGE_IO_ERROR : KATMED_IMAGE_OK;
}

static bool catalog_cache_matches_locked(uint32_t id, uint32_t index,
                                         uint32_t profile,
                                         const KatmedImageVariant *variant)
{
    char path[56];
    if (!image_path(id, path, sizeof(path))) return false;
    FILE *cached = fopen(path, "rb");
    if (cached == NULL) return false;
    uint8_t expected[KATMED_IMAGE_READ_CHUNK];
    uint8_t actual_bytes[KATMED_IMAGE_READ_CHUNK];
    bool matches = true;
    for (uint32_t position = 0; position < variant->length;) {
        uint32_t remaining = variant->length - position;
        size_t wanted = remaining < sizeof(expected) ? remaining : sizeof(expected);
        size_t actual = 0;
        if (katmed_images_read_variant(index, profile, position, expected,
                                       wanted, &actual) != KATMED_IMAGE_OK ||
            actual != wanted || fread(actual_bytes, 1u, wanted, cached) != wanted ||
            memcmp(expected, actual_bytes, wanted) != 0) {
            matches = false;
            break;
        }
        position += (uint32_t)wanted;
    }
    if (matches && fgetc(cached) != EOF) matches = false;
    if (fclose(cached) != 0) matches = false;
    return matches;
}

KatmedImageStatus katmed_image_store_import_catalog(uint32_t index,
                                                      uint32_t profile,
                                                      KatmedUserImageInfo *out)
{
    if (out == NULL) return KATMED_IMAGE_BAD_ID;
    const KatmedImageVariant *variant = katmed_images_variant(index, profile);
    if (variant == NULL || variant->length < 16u ||
        variant->length > KATMED_IMAGE_UPLOAD_MAX_BYTES)
        return KATMED_IMAGE_BAD_RANGE;
    KatmedImageStatus status = katmed_images_validate_variant(index, profile);
    if (status != KATMED_IMAGE_OK) return status;
    if (!enter_store()) return KATMED_IMAGE_UNAVAILABLE;

    const uint32_t id = CATALOG_CACHE_ID_FLAG | (index << 1u) | profile;
    for (size_t i = 0; i < STORE_MAX_READERS; ++i) {
        if (readers[i].stream != NULL && readers[i].id == id) {
            leave_store();
            return KATMED_IMAGE_BUSY;
        }
    }
    KatmedUserImageInfo existing = {0};
    if (file_info_locked(id, &existing) == KATMED_IMAGE_OK &&
        existing.bytes == variant->length && existing.width == variant->width &&
        existing.height == variant->height &&
        catalog_cache_matches_locked(id, index, profile, variant)) {
        *out = existing;
        leave_store();
        return KATMED_IMAGE_OK;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(STORE_PARTITION, &total, &used) != ESP_OK || used > total ||
        total - used < (size_t)variant->length + STORE_FREE_RESERVE_BYTES) {
        leave_store();
        return KATMED_IMAGE_NO_SPACE;
    }
    const uint32_t token = new_token_locked();
    char temporary[56], destination[56], legacy_upload_path[56];
    if (!temporary_path(token, temporary, sizeof(temporary)) ||
        !catalog_image_path(index, profile, destination, sizeof(destination))) {
        leave_store();
        return KATMED_IMAGE_BAD_ID;
    }
    int legacy_length = snprintf(legacy_upload_path, sizeof(legacy_upload_path),
                                STORE_DIRECTORY "/img-%08" PRIx32 ".jpg", id);
    struct stat legacy_info;
    if (legacy_length <= 0 || (size_t)legacy_length >= sizeof(legacy_upload_path)) {
        leave_store();
        return KATMED_IMAGE_BAD_ID;
    }
    if (stat(legacy_upload_path, &legacy_info) == 0) {
        leave_store();
        return KATMED_IMAGE_BUSY;
    }
    if (errno != ENOENT) {
        leave_store();
        return KATMED_IMAGE_IO_ERROR;
    }
    FILE *stream = fopen(temporary, "wb");
    if (stream == NULL) {
        status = errno == ENOSPC ? KATMED_IMAGE_NO_SPACE : KATMED_IMAGE_IO_ERROR;
        leave_store();
        return status;
    }
    uint8_t buffer[KATMED_IMAGE_READ_CHUNK];
    for (uint32_t position = 0; position < variant->length;) {
        const uint32_t remaining = variant->length - position;
        const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        size_t actual = 0;
        status = katmed_images_read_variant(index, profile, position,
                                             buffer, wanted, &actual);
        if (status != KATMED_IMAGE_OK || actual != wanted ||
            fwrite(buffer, 1u, wanted, stream) != wanted) {
            if (status == KATMED_IMAGE_OK)
                status = errno == ENOSPC ? KATMED_IMAGE_NO_SPACE : KATMED_IMAGE_IO_ERROR;
            break;
        }
        position += (uint32_t)wanted;
    }
    if (status == KATMED_IMAGE_OK &&
        (fflush(stream) != 0 || fsync(fileno(stream)) != 0))
        status = errno == ENOSPC ? KATMED_IMAGE_NO_SPACE : KATMED_IMAGE_IO_ERROR;
    if (fclose(stream) != 0 && status == KATMED_IMAGE_OK)
        status = KATMED_IMAGE_IO_ERROR;
    if (status == KATMED_IMAGE_OK && rename(temporary, destination) != 0)
        status = errno == ENOSPC ? KATMED_IMAGE_NO_SPACE : KATMED_IMAGE_IO_ERROR;
    if (status != KATMED_IMAGE_OK) {
        (void)unlink(temporary);
        leave_store();
        return status;
    }
    *out = (KatmedUserImageInfo){.id=id, .bytes=variant->length,
                                 .width=variant->width, .height=variant->height};
    leave_store();
    return KATMED_IMAGE_OK;
}

KatmedImageStatus katmed_image_store_get(uint32_t id, KatmedUserImageInfo *out)
{
    if (id == 0u || out == NULL || !enter_store()) return KATMED_IMAGE_BAD_ID;
    KatmedImageStatus result = file_info_locked(id, out);
    leave_store();
    return result;
}

KatmedImageStatus katmed_image_store_list(uint32_t index, KatmedUserImageInfo *out)
{
    if (out == NULL || !enter_store()) return KATMED_IMAGE_UNAVAILABLE;
    uint32_t previous = 0u;
    for (uint32_t position = 0; position <= index; ++position) {
        DIR *directory = opendir(STORE_DIRECTORY);
        if (directory == NULL) { leave_store(); return KATMED_IMAGE_IO_ERROR; }
        uint32_t candidate = UINT32_MAX;
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            uint32_t id;
            if (parse_image_name(entry->d_name, &id) && id > previous && id < candidate)
                candidate = id;
        }
        closedir(directory);
        if (candidate == UINT32_MAX) { leave_store(); return KATMED_IMAGE_NOT_FOUND; }
        previous = candidate;
    }
    KatmedImageStatus result = file_info_locked(previous, out);
    leave_store();
    return result;
}

KatmedImageStatus katmed_image_store_remove(uint32_t id)
{
    if (id == 0u || !enter_store()) return KATMED_IMAGE_BAD_ID;
    for (size_t i = 0; i < STORE_MAX_READERS; ++i) {
        if (readers[i].stream != NULL && readers[i].id == id) {
            leave_store();
            return KATMED_IMAGE_BUSY;
        }
    }
    char path[56];
    if (!image_path(id, path, sizeof(path))) { leave_store(); return KATMED_IMAGE_BAD_ID; }
    KatmedImageStatus result = KATMED_IMAGE_OK;
    if (unlink(path) != 0)
        result = errno == ENOENT ? KATMED_IMAGE_NOT_FOUND : KATMED_IMAGE_IO_ERROR;
    leave_store();
    return result;
}

KatmedImageStatus katmed_image_store_stats(KatmedImageStoreStats *out)
{
    if (out == NULL || !enter_store()) return KATMED_IMAGE_UNAVAILABLE;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(STORE_PARTITION, &total, &used) != ESP_OK || used > total ||
        total > UINT32_MAX || used > UINT32_MAX) {
        leave_store();
        return KATMED_IMAGE_IO_ERROR;
    }
    uint32_t count = 0;
    DIR *directory = opendir(STORE_DIRECTORY);
    if (directory == NULL) { leave_store(); return KATMED_IMAGE_IO_ERROR; }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        uint32_t id;
        if (parse_image_name(entry->d_name, &id) && count < UINT32_MAX) ++count;
    }
    closedir(directory);
    *out = (KatmedImageStoreStats){.total_bytes=(uint32_t)total,
                                  .used_bytes=(uint32_t)used,
                                  .free_bytes=(uint32_t)(total-used),
                                  .image_count=count};
    leave_store();
    return KATMED_IMAGE_OK;
}

KatmedImageStatus katmed_image_store_clear_user_data(void)
{
    if (!enter_store()) return KATMED_IMAGE_UNAVAILABLE;
    for (size_t i=0;i<STORE_MAX_UPLOADS;++i) {
        if (uploads[i].stream) fclose(uploads[i].stream);
        uploads[i]=(UploadSlot){0};
    }
    for (size_t i=0;i<STORE_MAX_READERS;++i) {
        if (readers[i].stream) fclose(readers[i].stream);
        readers[i]=(ReaderSlot){0};
    }
    DIR *directory=opendir(STORE_DIRECTORY);
    if (!directory) { leave_store(); return KATMED_IMAGE_IO_ERROR; }
    KatmedImageStatus result=KATMED_IMAGE_OK;
    struct dirent *entry;
    while ((entry=readdir(directory))!=NULL) {
        if (strcmp(entry->d_name,".")==0 || strcmp(entry->d_name,"..")==0) continue;
        uint32_t id=0;
        if (!parse_image_name(entry->d_name,&id) &&
            !katmed_image_store_temporary_name(entry->d_name)) {
            result=KATMED_IMAGE_IO_ERROR; break;
        }
        char path[56];
        int length=snprintf(path,sizeof(path),STORE_DIRECTORY "/%s",entry->d_name);
        if (length<=0 || (size_t)length>=sizeof(path) || unlink(path)!=0) {
            result=KATMED_IMAGE_IO_ERROR; break;
        }
    }
    closedir(directory);
    leave_store();
    return result;
}

KatmedImageStatus katmed_image_store_open_read(uint32_t id,
                                                KatmedImageReader *out,
                                                KatmedUserImageInfo *info)
{
    if (id == 0u || out == NULL || info == NULL || !enter_store())
        return KATMED_IMAGE_BAD_ID;
    ReaderSlot *slot = NULL;
    for (size_t i = 0; i < STORE_MAX_READERS; ++i)
        if (readers[i].stream == NULL) { slot = &readers[i]; break; }
    if (slot == NULL) { leave_store(); return KATMED_IMAGE_BUSY; }
    KatmedImageStatus result = file_info_locked(id, info);
    char path[56];
    if (result == KATMED_IMAGE_OK && image_path(id, path, sizeof(path))) {
        FILE *stream = fopen(path, "rb");
        if (stream == NULL) result = KATMED_IMAGE_IO_ERROR;
        else {
            *slot = (ReaderSlot){.stream=stream, .token=new_token_locked(), .id=id,
                                 .size=info->bytes};
            *out = (KatmedImageReader){.token=slot->token};
        }
    }
    leave_store();
    return result;
}

KatmedImageStatus katmed_image_store_read(KatmedImageReader reader,
                                          uint8_t *buffer, size_t wanted,
                                          size_t *actual)
{
    if (actual != NULL) *actual = 0u;
    if (actual == NULL || (wanted != 0u && buffer == NULL) || !enter_store())
        return KATMED_IMAGE_BAD_RANGE;
    ReaderSlot *slot = reader_slot_locked(reader);
    if (slot == NULL) { leave_store(); return KATMED_IMAGE_BAD_STATE; }
    size_t available = slot->size - slot->position;
    size_t take = wanted < available ? wanted : available;
    size_t read_count = fread(buffer, 1u, take, slot->stream);
    slot->position += (uint32_t)read_count;
    *actual = read_count;
    KatmedImageStatus result = read_count == take ? KATMED_IMAGE_OK : KATMED_IMAGE_IO_ERROR;
    leave_store();
    return result;
}

KatmedImageStatus katmed_image_store_seek(KatmedImageReader reader,
                                          uint32_t offset,
                                          KatmedImageStoreWhence whence)
{
    if (!enter_store()) return KATMED_IMAGE_UNAVAILABLE;
    ReaderSlot *slot = reader_slot_locked(reader);
    if (slot == NULL) { leave_store(); return KATMED_IMAGE_BAD_STATE; }
    uint64_t target = UINT64_MAX;
    if (whence == KATMED_IMAGE_STORE_SEEK_SET) target = offset;
    else if (whence == KATMED_IMAGE_STORE_SEEK_CUR) target = (uint64_t)slot->position + offset;
    else if (whence == KATMED_IMAGE_STORE_SEEK_END) target = (uint64_t)slot->size + offset;
    if (target > slot->size || fseek(slot->stream, (long)target, SEEK_SET) != 0) {
        leave_store();
        return KATMED_IMAGE_BAD_RANGE;
    }
    slot->position = (uint32_t)target;
    leave_store();
    return KATMED_IMAGE_OK;
}

KatmedImageStatus katmed_image_store_close_read(KatmedImageReader reader)
{
    if (!enter_store()) return KATMED_IMAGE_UNAVAILABLE;
    ReaderSlot *slot = reader_slot_locked(reader);
    if (slot == NULL) { leave_store(); return KATMED_IMAGE_BAD_STATE; }
    bool failed = fclose(slot->stream) != 0;
    memset(slot, 0, sizeof(*slot));
    leave_store();
    return failed ? KATMED_IMAGE_IO_ERROR : KATMED_IMAGE_OK;
}
