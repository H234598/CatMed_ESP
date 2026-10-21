#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

FILE *katmed_acme_atomic_open(const char *target);
int katmed_acme_atomic_close(FILE *stream);
int katmed_acme_atomic_abort(FILE *stream);

static bool fail_sync;
static bool fail_rename;
static unsigned leases;

bool katmed_source_writer_enter(void) { ++leases; return true; }
void katmed_source_writer_leave(void) { assert(leases > 0); --leases; }

int fixture_fsync(int fd)
{
    if (fail_sync) { errno = EIO; return -1; }
    return fsync(fd);
}

int fixture_rename(const char *old_name, const char *new_name)
{
    if (fail_rename) { errno = EIO; return -1; }
    return rename(old_name, new_name);
}

static void verify(const char *path, const char *expected)
{
    char buffer[64] = {0};
    FILE *f = fopen(path, "r");
    assert(f != NULL);
    assert(fgets(buffer, sizeof(buffer), f) != NULL);
    assert(fclose(f) == 0);
    assert(strcmp(buffer, expected) == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *path = argv[1];
    FILE *old = fopen(path, "w");
    assert(old != NULL);
    assert(fputs("old", old) >= 0 && fclose(old) == 0);

    FILE *f = katmed_acme_atomic_open(path);
    assert(f && fputs("new", f) >= 0);
    fail_sync = true;
    assert(katmed_acme_atomic_close(f) == EOF);
    fail_sync = false;
    verify(path, "old");
    assert(leases == 0);

    f = katmed_acme_atomic_open(path);
    assert(f && fputs("new", f) >= 0);
    fail_rename = true;
    assert(katmed_acme_atomic_close(f) == EOF);
    fail_rename = false;
    verify(path, "old");
    assert(leases == 0);

    f = katmed_acme_atomic_open(path);
    assert(f && fputs("partial", f) >= 0);
    assert(katmed_acme_atomic_abort(f) == 0);
    verify(path, "old");
    assert(leases == 0);

    f = katmed_acme_atomic_open(path);
    assert(f && fputs("new", f) >= 0);
    assert(katmed_acme_atomic_close(f) == 0);
    verify(path, "new");
    assert(leases == 0);
    return 0;
}
