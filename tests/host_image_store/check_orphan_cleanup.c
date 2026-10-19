#define _POSIX_C_SOURCE 200809L
#include "katmed_image_store_cleanup.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void create_file(const char *path)
{
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite("fixture", 1, 7, file) == 7);
    assert(fclose(file) == 0);
}

static bool exists(const char *path)
{
    struct stat info;
    return lstat(path, &info) == 0;
}

int main(void)
{
    char root[] = "/tmp/katmed-image-cleanup-XXXXXX";
    assert(mkdtemp(root));
    char orphan[256], image[256], unrelated[256], link_path[256], target[256];
    assert(snprintf(orphan, sizeof(orphan), "%s/.upload-0123abcd.tmp", root) > 0);
    assert(snprintf(image, sizeof(image), "%s/img-0123abcd.jpg", root) > 0);
    assert(snprintf(unrelated, sizeof(unrelated), "%s/.upload-note.tmp", root) > 0);
    assert(snprintf(link_path, sizeof(link_path), "%s/.upload-87654321.tmp", root) > 0);
    assert(snprintf(target, sizeof(target), "%s/target", root) > 0);
    create_file(orphan);
    create_file(image);
    create_file(unrelated);

    assert(katmed_image_store_temporary_name(".upload-0123abcd.tmp"));
    assert(!katmed_image_store_temporary_name(".upload-note.tmp"));
    assert(katmed_image_store_cleanup_orphan_uploads(root));
    assert(!exists(orphan) && exists(image) && exists(unrelated));
    assert(katmed_image_store_cleanup_orphan_uploads(root)); /* Idempotent. */

    create_file(target);
    assert(symlink(target, link_path) == 0);
    assert(!katmed_image_store_cleanup_orphan_uploads(root));
    assert(exists(link_path) && exists(target));
    assert(unlink(link_path) == 0);

    char directory_path[256];
    assert(snprintf(directory_path, sizeof(directory_path),
                    "%s/.upload-abcdef01.tmp", root) > 0);
    assert(mkdir(directory_path, 0700) == 0);
    assert(!katmed_image_store_cleanup_orphan_uploads(root));
    assert(exists(directory_path) && exists(image) && exists(unrelated));

    assert(unlink(image) == 0);
    assert(unlink(unrelated) == 0);
    assert(unlink(target) == 0);
    assert(rmdir(directory_path) == 0);
    assert(rmdir(root) == 0);
    puts("orphan upload cleanup passed");
    return 0;
}
