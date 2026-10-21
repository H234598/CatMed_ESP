#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "katmed_reset_acme_files.h"

static void create_file(const char *directory, const char *name)
{
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fputc('x', file) == 'x');
    assert(fclose(file) == 0);
}

static bool exists(const char *directory, const char *name)
{
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    struct stat info;
    return lstat(path, &info) == 0;
}

static void remove_file(const char *directory, const char *name)
{
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
    assert(length > 0 && (size_t)length < sizeof(path));
    assert(unlink(path) == 0);
}

int main(void)
{
    char root[] = "/tmp/katmed-reset-acme-XXXXXX";
    assert(mkdtemp(root));
    char acme[512], images[512], missing[512];
    assert(snprintf(acme, sizeof(acme), "%s/acme", root) > 0);
    assert(snprintf(images, sizeof(images), "%s/user-images", root) > 0);
    assert(snprintf(missing, sizeof(missing), "%s/missing", root) > 0);
    assert(mkdir(acme, 0700) == 0 && mkdir(images, 0700) == 0);
    assert(katmed_reset_acme_files_at(missing));
    assert(katmed_reset_acme_files_at(acme));
    assert(!katmed_reset_acme_files_at(NULL));
    create_file(images, "img-00000001.jpg");
    const char *names[] = {"account.json", "directory.json", "account.pem",
        "order.json", "certkey.pem", "certificate.pem", "renewal.json",
        "token", "account.pem.katmed-0", "renewal.json.katmed-4294967295",
        "token.katmed-1"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        create_file(acme, names[i]);
    create_file(acme, "future-client-state.json");
    assert(!katmed_reset_acme_files_at(acme));
    assert(exists(acme, "account.pem")); /* unknown blocks before deletion */
    assert(exists(acme, "future-client-state.json"));
    remove_file(acme, "future-client-state.json");
    assert(katmed_reset_acme_files_at(acme));
    assert(katmed_reset_acme_files_at(acme)); /* interrupted stage is repeatable */
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        assert(!exists(acme, names[i]));
    assert(exists(images, "img-00000001.jpg"));

    /* A familiar name on a non-regular entry must never be followed. */
    char link_path[512], image_path[512];
    assert(snprintf(link_path, sizeof(link_path), "%s/account.pem", acme) > 0);
    assert(snprintf(image_path, sizeof(image_path), "%s/img-00000001.jpg", images) > 0);
    assert(symlink(image_path, link_path) == 0);
    assert(!katmed_reset_acme_files_at(acme));
    assert(exists(images, "img-00000001.jpg"));
    remove_file(acme, "account.pem");

    /* No suffix confusion, traversal or recursive deletion. */
    create_file(acme, "account.pem.katmed-1.tmp");
    assert(!katmed_reset_acme_files_at(acme));
    remove_file(acme, "account.pem.katmed-1.tmp");
    create_file(acme, "account.json.bak");
    assert(!katmed_reset_acme_files_at(acme));
    remove_file(acme, "account.json.bak");
    assert(symlink(images, link_path) == 0);
    assert(!katmed_reset_acme_files_at(link_path));
    remove_file(acme, "account.pem");
    assert(katmed_reset_acme_files_at(acme));
    remove_file(images, "img-00000001.jpg");
    assert(rmdir(images) == 0 && rmdir(acme) == 0 && rmdir(root) == 0);
    return 0;
}
