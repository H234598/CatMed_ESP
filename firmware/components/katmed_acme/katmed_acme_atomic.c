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
    char temporary[ATOMIC_PATH_MAX];
    int length = snprintf(temporary, sizeof(temporary), "%s.katmed-%u",
                          target, sequence++);
    if (length < 0 || (size_t)length >= sizeof(temporary)) return NULL;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return NULL;
    FILE *stream = fdopen(fd, "w");
    if (stream == NULL) {
        close(fd);
        unlink(temporary);
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
        return NULL;
    }
    return stream;
}

int katmed_acme_atomic_close(FILE *stream)
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
    if (!found) return fclose(stream);
    int result = fflush(stream);
    if (result == 0) result = fsync(fileno(stream));
    if (fclose(stream) != 0) result = EOF;
    if (result == 0 && rename(slot.temporary, slot.target) != 0) result = -1;
    if (result != 0) {
        unlink(slot.temporary);
        ESP_LOGE(TAG, "atomic ACME state commit failed: %s", strerror(errno));
        return EOF;
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
    return 0;
}
