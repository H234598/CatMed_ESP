#include "katmed_import_files_idf.h"
#include "katmed_security_layout.h"

#include "esp_littlefs.h"
#include "esp_rom_crc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MOUNT "/katmed"
#define DIRECTORY MOUNT "/user-images"
#define ACME_DIRECTORY MOUNT "/acme"
#define PARTITION KATMED_USER_IMAGE_PARTITION_LABEL
#define FS_BYTES (576u * 1024u)
/* LittleFS allocates whole 4 KiB blocks and copy-on-write metadata. A full
 * 127-file source inventory needs a larger target than its byte sum suggests;
 * actual write, sync and readback remain authoritative. */
#define BASE_RESERVE (32u * 1024u)
#define FILE_RESERVE 256u
#define RUN_RESERVE (8u * 1024u)

static bool owned_mount;

static bool mount_files(void)
{
    if (owned_mount) return true;
    const esp_vfs_littlefs_conf_t config = {
        .base_path = MOUNT,
        .partition_label = PARTITION,
        .format_if_mount_failed = false,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    if (esp_vfs_littlefs_register(&config) != ESP_OK) return false;
    owned_mount = true;
    return true;
}

bool katmed_import_files_unmount(void)
{
    if (owned_mount) {
        if (esp_vfs_littlefs_unregister(PARTITION) != ESP_OK) return false;
        owned_mount = false;
    }
    return true;
}

static bool path_for(const KatmedImportFile *file, unsigned ordinal,
                     char *final, size_t final_size,
                     char *temporary, size_t temporary_size)
{
    if (!file || ordinal >= 128) return false;
    int n;
    if (file->kind == 1 && file->id > 0 && file->id < UINT32_C(0x80000000))
        n = snprintf(final, final_size, DIRECTORY "/img-%08" PRIx32 ".jpg", file->id);
    else if (file->kind == 2 && (file->id >> 1) < 72)
        n = snprintf(final, final_size, DIRECTORY "/cat-%02" PRIx32 "-%u.jpg",
                     file->id >> 1, (unsigned)(file->id & 1));
    else if (file->kind == 3 && file->id < 7) {
        static const char *const names[] = {
            "account.json", "directory.json", "account.pem", "order.json",
            "certkey.pem", "certificate.pem", "renewal.json"
        };
        n = snprintf(final, final_size, ACME_DIRECTORY "/%s", names[file->id]);
    } else return false;
    if (n <= 0 || (size_t)n >= final_size) return false;
    n = snprintf(temporary, temporary_size, "%s/.migration-%03u.tmp",
                 file->kind == 3 ? ACME_DIRECTORY : DIRECTORY, ordinal);
    return n > 0 && (size_t)n < temporary_size;
}

/* Return -1 on error, 0 for absence, 1 only for a regular file. */
static int file_presence(const char *path)
{
    struct stat info;
    if (stat(path, &info) == 0) return S_ISREG(info.st_mode) ? 1 : -1;
    return errno == ENOENT ? 0 : -1;
}

static bool matches(const char *path, const KatmedImportFile *file)
{
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_size != (off_t)file->length) return false;
    FILE *stream = fopen(path, "rb");
    if (!stream) return false;
    uint8_t buffer[512];
    uint32_t crc = 0;
    size_t total = 0;
    bool ok = true;
    while (total < file->length) {
        size_t wanted = file->length - total;
        if (wanted > sizeof(buffer)) wanted = sizeof(buffer);
        if (fread(buffer, 1, wanted, stream) != wanted) { ok = false; break; }
        crc = esp_rom_crc32_le(crc, buffer, wanted);
        total += wanted;
    }
    if (ok && fgetc(stream) != EOF) ok = false;
    if (fclose(stream) != 0) ok = false;
    memset(buffer, 0, sizeof(buffer));
    return ok && total == file->length && crc == file->crc;
}

static bool empty_directory(const char *path)
{
    DIR *directory = opendir(path);
    if (!directory) return errno == ENOENT;
    struct dirent *entry;
    bool ok = true;
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            ok = false; break;
        }
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(directory) != 0) ok = false;
    return ok;
}

bool katmed_import_files_empty(void *context, size_t content_bytes, size_t count)
{
    (void)context;
    if (!count || count > 128 || content_bytes > 248u * 1024u ||
        !mount_files()) return false;
    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION, &total, &used) != ESP_OK ||
        total != FS_BYTES || used > total ||
        content_bytes > total - used ||
        BASE_RESERVE + count * FILE_RESERVE > total - used - content_bytes)
        return false;
    DIR *root = opendir(MOUNT);
    if (!root) return false;
    bool ok = true;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(root)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..") &&
            strcmp(entry->d_name, "user-images") &&
            strcmp(entry->d_name, "acme")) { ok = false; break; }
        errno = 0;
    }
    if (errno != 0) ok = false;
    if (closedir(root) != 0) ok = false;
    if (!ok) return false;
    return empty_directory(DIRECTORY) && empty_directory(ACME_DIRECTORY);
}

bool katmed_import_file_stage(void *context, unsigned ordinal,
                              const KatmedImportFile *file,
                              const KatmedImportInput *input)
{
    (void)context;
    char final[64], temporary[64];
    if (!mount_files() || !input || !input->read ||
        !path_for(file, ordinal, final, sizeof(final),
                  temporary, sizeof(temporary)) ||
        file_presence(final) != 0) return false;
    const char *folder = file->kind == 3 ? ACME_DIRECTORY : DIRECTORY;
    struct stat directory;
    if (stat(folder, &directory) != 0) {
        if (errno != ENOENT || mkdir(folder, 0700) != 0) return false;
    } else if (!S_ISDIR(directory.st_mode)) return false;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    uint8_t buffer[512];
    uint32_t crc = 0;
    bool ok = true;
    for (size_t at = 0; at < file->length && ok;) {
        size_t wanted = file->length - at;
        if (wanted > sizeof(buffer)) wanted = sizeof(buffer);
        if (!input->read(input->context, file->input_offset + at, buffer, wanted) ||
            write(fd, buffer, wanted) != (ssize_t)wanted) ok = false;
        else { crc = esp_rom_crc32_le(crc, buffer, wanted); at += wanted; }
    }
    if (ok && crc != file->crc) ok = false;
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    memset(buffer, 0, sizeof(buffer));
    size_t total = 0, used = 0;
    return ok && matches(temporary, file) &&
        esp_littlefs_info(PARTITION, &total, &used) == ESP_OK &&
        used <= total && total - used >= RUN_RESERVE;
}

bool katmed_import_file_final(void *context, unsigned ordinal,
                              const KatmedImportFile *file)
{
    (void)context;
    char final[64], temporary[64];
    return mount_files() &&
        path_for(file, ordinal, final, sizeof(final), temporary, sizeof(temporary)) &&
        file_presence(temporary) == 0 && matches(final, file);
}

bool katmed_import_file_publish(void *context, unsigned ordinal,
                                const KatmedImportFile *file)
{
    (void)context;
    char final[64], temporary[64];
    if (!mount_files() || !path_for(file, ordinal, final, sizeof(final),
                                    temporary, sizeof(temporary))) return false;
    const int present = file_presence(final);
    if (present < 0) return false;
    if (present == 1) {
        if (!matches(final, file)) return false;
        const int staged = file_presence(temporary);
        if (staged < 0 || (staged == 1 &&
            (!matches(temporary, file) || unlink(temporary) != 0))) return false;
        return katmed_import_file_final(context, ordinal, file);
    }
    if (!matches(temporary, file) || file_presence(final) != 0 ||
        rename(temporary, final) != 0) return false;
    return katmed_import_file_final(context, ordinal, file);
}

bool katmed_import_file_abort(void *context, unsigned ordinal,
                              const KatmedImportFile *file)
{
    (void)context;
    char final[64], temporary[64];
    if (!mount_files() || !path_for(file, ordinal, final, sizeof(final),
                                    temporary, sizeof(temporary)) ||
        file_presence(final) != 0) return false;
    const int present = file_presence(temporary);
    return present == 0 || (present == 1 && unlink(temporary) == 0);
}
