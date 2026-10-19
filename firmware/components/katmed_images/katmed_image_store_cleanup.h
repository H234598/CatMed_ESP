#ifndef KATMED_IMAGE_STORE_CLEANUP_H
#define KATMED_IMAGE_STORE_CLEANUP_H

#include <stdbool.h>
#include <stddef.h>

/* Internal filesystem helper shared by boot recovery and the host test. */
bool katmed_image_store_temporary_name(const char *name);
bool katmed_image_store_cleanup_orphan_uploads(const char *directory_path);

typedef bool (*KatmedImageCleanupNext)(void *context, char *name, size_t capacity,
                                       bool *is_regular, bool *at_end);
typedef bool (*KatmedImageCleanupRemove)(void *context, const char *name);
bool katmed_image_store_cleanup_entries(void *context,
                                        KatmedImageCleanupNext next,
                                        KatmedImageCleanupRemove remove);

#endif
