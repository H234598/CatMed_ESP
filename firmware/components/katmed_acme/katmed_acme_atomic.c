#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "katmed_source_export.h"

#define ATOMIC_SLOTS 8
#define ATOMIC_PATH_MAX 256

typedef struct {
    FILE *stream;
    char target[ATOMIC_PATH_MAX];
    char temporary[ATOMIC_PATH_MAX];
} AtomicSlot;

static const char *TAG = "katmed_acme_fs";
static AtomicSlot slots[ATOMIC_SLOTS];
static portMUX_TYPE slots_lock = portMUX_INITIALIZER_UNLOCKED;
static unsigned sequence;

static bool allowed_path(const char *path)
{
    return path != NULL && path[0] == '/' && strstr(path, "/../") == NULL &&
           strcmp(path, "/..") != 0 && strlen(path) < ATOMIC_PATH_MAX - 32;
}

FILE *katmed_acme_atomic_open(const char *target)
{
    if (!allowed_path(target)) return NULL;
    if (!katmed_source_writer_enter()) return NULL;
    char temporary[ATOMIC_PATH_MAX];
    portENTER_CRITICAL(&slots_lock);
    unsigned serial = sequence++;
    portEXIT_CRITICAL(&slots_lock);
    int length = snprintf(temporary, sizeof(temporary), "%s.katmed-%u",
                          target, serial);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        katmed_source_writer_leave(); return NULL;
    }
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { katmed_source_writer_leave(); return NULL; }
    FILE *stream = fdopen(fd, "w");
    if (stream == NULL) {
        close(fd);
        unlink(temporary);
        katmed_source_writer_leave();
        return NULL;
    }
    bool registered = false;
    portENTER_CRITICAL(&slots_lock);
    for (size_t index = 0; index < ATOMIC_SLOTS; ++index) {
        if (slots[index].stream == NULL) {
            slots[index].stream = stream;
            strlcpy(slots[index].target, target, sizeof(slots[index].target));
            strlcpy(slots[index].temporary, temporary, sizeof(slots[index].temporary));
            registered = true;
            break;
        }
    }
    portEXIT_CRITICAL(&slots_lock);
    if (!registered) {
        fclose(stream);
        unlink(temporary);
        katmed_source_writer_leave();
        return NULL;
    }
    return stream;
}

static int atomic_finish(FILE *stream, bool commit)
{
    if (stream == NULL) return EOF;
    AtomicSlot slot = {0};
    bool found = false;
    portENTER_CRITICAL(&slots_lock);
    for (size_t index = 0; index < ATOMIC_SLOTS; ++index) {
        if (slots[index].stream == stream) {
            slot = slots[index];
            slots[index] = (AtomicSlot){0};
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&slots_lock);
    if (!found) return EOF;
    int result = 0;
    if (commit) {
        result = fflush(stream);
        if (result == 0) result = fsync(fileno(stream));
    }
    if (fclose(stream) != 0) result = EOF;
    if (commit && result == 0 && rename(slot.temporary, slot.target) != 0) result = -1;
    if (!commit || result != 0) {
        unlink(slot.temporary);
        if (commit) ESP_LOGE(TAG, "atomic ACME state commit failed: %s", strerror(errno));
        katmed_source_writer_leave();
        return commit ? EOF : 0;
    }
    char parent[ATOMIC_PATH_MAX];
    strlcpy(parent, slot.target, sizeof(parent));
    char *slash = strrchr(parent, '/');
    if (slash != NULL && slash != parent) {
        *slash = '\0';
        int directory = open(parent, O_RDONLY);
        if (directory >= 0) {
            (void)fsync(directory);
            close(directory);
        }
    }
    katmed_source_writer_leave();
    return 0;
}

int katmed_acme_atomic_close(FILE *stream)
{
    return atomic_finish(stream, true);
}

int katmed_acme_atomic_abort(FILE *stream)
{
    return atomic_finish(stream, false);
}

bool katmed_acme_atomic_idle(void)
{
    bool idle = true;
    portENTER_CRITICAL(&slots_lock);
    for (size_t i = 0; i < ATOMIC_SLOTS; ++i)
        if (slots[i].stream != NULL) idle = false;
    portEXIT_CRITICAL(&slots_lock);
    return idle;
}
