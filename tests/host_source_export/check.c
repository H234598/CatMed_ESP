#include "katmed_source_export.h"
#include "nvs.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { const char *ns; const char *key; const unsigned char *data; size_t size; int type; } Entry;
static const unsigned char state[] = { 'K', 'M', 'S', 'T', 10, 0 };
static const unsigned char history[] = { 'K', 'M', 'H', '2' };
static const unsigned char auth[] = { 'K', 'W', 'A', '1' };
static const unsigned char identity[] = { 'K', 'W', 'I', '1' };
static const unsigned char journal[] = { 0, 0 };
static const unsigned char bad_journal[] = { 1, 2 };
static Entry entries[] = {
    { "katmed", "state", state, sizeof(state), NVS_TYPE_BLOB },
    { "katmed", "mesh_history", history, sizeof(history), NVS_TYPE_BLOB },
    { "katmed_web", "auth", auth, sizeof(auth), NVS_TYPE_BLOB },
    { "katmed_web", "identity", identity, sizeof(identity), NVS_TYPE_BLOB },
    { "katmed_reset", "journal", journal, sizeof(journal), NVS_TYPE_BLOB }
};
static unsigned entry_count = 5;
struct MockIterator { unsigned index; };
static struct MockIterator iterator;

esp_err_t nvs_entry_find(const char *partition, const char *ns, int type, nvs_iterator_t *out)
{
    assert(strcmp(partition, "nvs") == 0 && ns == NULL && type == NVS_TYPE_ANY);
    iterator.index = 0;
    *out = entry_count ? &iterator : NULL;
    return entry_count ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_entry_next(nvs_iterator_t *it)
{
    assert(*it == &iterator);
    if (++iterator.index == entry_count) { *it = NULL; return ESP_ERR_NVS_NOT_FOUND; }
    return ESP_OK;
}
esp_err_t nvs_entry_info(nvs_iterator_t it, nvs_entry_info_t *info)
{
    assert(it == &iterator);
    memset(info, 0, sizeof(*info));
    snprintf(info->namespace_name, sizeof(info->namespace_name), "%s", entries[it->index].ns);
    snprintf(info->key, sizeof(info->key), "%s", entries[it->index].key);
    info->type = entries[it->index].type;
    return ESP_OK;
}
void nvs_release_iterator(nvs_iterator_t it) { assert(it == NULL || it == &iterator); }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle)
{
    assert(mode == NVS_READONLY);
    for (unsigned i = 0; i < entry_count; ++i)
        if (strcmp(ns, entries[i].ns) == 0) { *handle = (int)i + 1; return ESP_OK; }
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *dst, size_t *length)
{
    const char *ns = entries[handle - 1].ns;
    for (unsigned i = 0; i < entry_count; ++i)
        if (strcmp(ns, entries[i].ns) == 0 && strcmp(key, entries[i].key) == 0) {
            if (dst) { assert(*length >= entries[i].size); memcpy(dst, entries[i].data, entries[i].size); }
            *length = entries[i].size;
            return ESP_OK;
        }
    return ESP_ERR_NVS_NOT_FOUND;
}
void nvs_close(nvs_handle_t handle) { assert(handle > 0); }

typedef struct { unsigned files; size_t bytes; bool finished; bool success; bool reject; unsigned reject_at; char paths[128][80]; const char *output; FILE *stream; unsigned checks, abort_after, denied_writes; bool test_writer; } Result;
static bool begin(void *ctx, const char *path, size_t size)
{
    Result *r = ctx;
    assert(r->files < 128 && size <= 128 * 1024);
    snprintf(r->paths[r->files], sizeof(r->paths[0]), "%s", path);
    ++r->files;
    if (r->test_writer && strcmp(path, "littlefs/acme/account.json") == 0) {
        /* A same-length ACME replacement after inventory must be refused. */
        bool granted = katmed_source_writer_enter();
        if (granted) {
            FILE *replacement = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/account.json", "wb");
            assert(replacement && fwrite("XX", 1, 2, replacement) == 2 &&
                   fclose(replacement) == 0);
            katmed_source_writer_leave();
        }
        assert(!granted);
        ++r->denied_writes;
    }
    if (r->reject || (r->reject_at && r->files >= r->reject_at)) return false;
    if (r->output) {
        char target[256];
        int n = snprintf(target, sizeof(target), "%s/%s", r->output, path);
        assert(n > 0 && (size_t)n < sizeof(target));
        r->stream = fopen(target, "wb");
        assert(r->stream);
    }
    return true;
}
static bool bytes(void *ctx, const uint8_t *data, size_t length)
{
    Result *r = ctx;
    assert(data && length);
    if (r->test_writer) assert(!katmed_source_writer_enter());
    if (r->stream) assert(fwrite(data, 1, length, r->stream) == length);
    r->bytes += length;
    return true;
}
static bool end(void *ctx)
{
    Result *r = ctx;
    if (r->stream) { assert(fclose(r->stream) == 0); r->stream = NULL; }
    return true;
}
static void finish(void *ctx, bool success)
{
    Result *r = ctx;
    assert(!r->finished);
    r->finished = true;
    r->success = success;
}
static bool keep_going(void *ctx)
{
    Result *r = ctx;
    return r->abort_after == 0 || ++r->checks < r->abort_after;
}
static bool idle(void) { return true; }
static bool busy(void) { return false; }

static pthread_mutex_t writer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t writer_cond = PTHREAD_COND_INITIALIZER;
static bool writer_ready, writer_release;
static void *competing_writer(void *unused)
{
    (void)unused;
    assert(katmed_source_writer_enter());
    assert(pthread_mutex_lock(&writer_mutex) == 0);
    writer_ready = true;
    assert(pthread_cond_signal(&writer_cond) == 0);
    while (!writer_release) assert(pthread_cond_wait(&writer_cond, &writer_mutex) == 0);
    assert(pthread_mutex_unlock(&writer_mutex) == 0);
    katmed_source_writer_leave();
    return NULL;
}
static Result run(bool expected)
{
    Result r = {0};
    KatmedSourceExportSink sink = { &r, begin, bytes, end, finish, NULL };
    assert(katmed_source_export(&sink) == expected);
    assert(r.finished && r.success == expected);
    return r;
}
static unsigned read_le32(const unsigned char *p)
{
    return (unsigned)p[0] | ((unsigned)p[1] << 8) |
        ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    Result r = run(true);
    assert(r.files == 7);
    assert(strcmp(r.paths[0], "nvs/katmed/state") == 0);
    assert(strcmp(r.paths[5], "littlefs/user-images/img-00000001.jpg") == 0);
    assert(strcmp(r.paths[6], "littlefs/acme/account.json") == 0);
    assert(r.bytes == sizeof(state) + sizeof(history) + sizeof(auth) +
                      sizeof(identity) + sizeof(journal) + 4 + 2);

    entry_count = 1;
    r = run(true);
    assert(r.files == 3 && strcmp(r.paths[0], "nvs/katmed/state") == 0);
    entry_count = 3;
    r = run(false);
    assert(r.files == 0); /* Auth without its server identity is incomplete. */
    entry_count = 5;

    entries[4].data = bad_journal;
    run(false);
    entries[4].data = journal;
    entries[4].key = "unknown";
    r = run(false);
    assert(r.files == 0);
    entries[4].key = "journal";
    entries[0].key = "missing";
    r = run(false);
    assert(r.files == 0);
    entries[0].key = "state";
    entries[0].type = 42;
    r = run(false);
    assert(r.files == 0);
    entries[0].type = NVS_TYPE_BLOB;
    entries[0].size = 4097;
    r = run(false);
    assert(r.files == 0);
    entries[0].size = sizeof(state);

    FILE *extra = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/.upload-1.tmp", "wb");
    assert(extra);
    assert(fclose(extra) == 0);
    r = run(false);
    assert(r.files == 0);
    assert(remove(KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/.upload-1.tmp") == 0);

    extra = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/unknown.pem", "wb");
    assert(extra);
    assert(fclose(extra) == 0);
    r = run(false);
    assert(r.files == 0);
    assert(remove(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/unknown.pem") == 0);

    extra = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/token", "wb");
    assert(extra);
    assert(fclose(extra) == 0);
    r = run(false);
    assert(r.files == 0);
    assert(remove(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/token") == 0);

    /* ARI persists this seventh state file; it must be exported. */
    extra = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/renewal.json", "wb");
    assert(extra);
    assert(fwrite("{}", 1, 2, extra) == 2);
    assert(fclose(extra) == 0);
    r = run(true);
    bool saw_renewal = false;
    for (unsigned i = 0; i < r.files; ++i)
        if (strcmp(r.paths[i], "littlefs/acme/renewal.json") == 0)
            saw_renewal = true;
    assert(r.files == 8 && saw_renewal);
    assert(remove(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/renewal.json") == 0);

    extra = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/renewal.json", "wb");
    assert(extra);
    assert(fclose(extra) == 0);
    r = run(false);
    assert(r.files == 0);
    assert(remove(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/renewal.json") == 0);

    extra = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/renewal.json.katmed-1", "wb");
    assert(extra);
    assert(fclose(extra) == 0);
    r = run(false);
    assert(r.files == 0);
    assert(remove(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/renewal.json.katmed-1") == 0);

    assert(symlink(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/account.json",
                   KATMED_SOURCE_EXPORT_HOST_TEST "/acme/renewal.json") == 0);
    r = run(false);
    assert(r.files == 0);
    assert(unlink(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/renewal.json") == 0);

    assert(symlink(KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-00000001.jpg",
                   KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-00000003.jpg") == 0);
    r = run(false);
    assert(r.files == 0);
    assert(unlink(KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-00000003.jpg") == 0);

    extra = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-00000002.jpg", "wb");
    assert(extra);
    for (unsigned i = 0; i < 128u * 1024u + 1u; ++i) assert(fputc(0, extra) != EOF);
    assert(fclose(extra) == 0);
    r = run(false);
    assert(r.files == 0);
    assert(remove(KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-00000002.jpg") == 0);

    r = run(true);
    assert(r.files == 7);
    Result refused = { .reject = true };
    KatmedSourceExportSink sink = { &refused, begin, bytes, end, finish, NULL };
    assert(!katmed_source_export(&sink));
    assert(refused.finished && !refused.success);
    Result partial = { .reject_at = 4 };
    sink.context = &partial;
    assert(!katmed_source_export(&sink));
    assert(partial.finished && !partial.success && partial.files == 4);

    assert(!katmed_source_export(NULL));
    Result no_finish = {0};
    KatmedSourceExportSink missing_finish = { .context = &no_finish, .begin = begin,
        .bytes = bytes, .end = end };
    assert(!katmed_source_export(&missing_finish) && !no_finish.finished);
    for (unsigned omitted = 0; omitted < 3; ++omitted) {
        Result invalid = {0};
        KatmedSourceExportSink incomplete = { &invalid, begin, bytes, end, finish, NULL };
        if (omitted == 0) incomplete.begin = NULL;
        if (omitted == 1) incomplete.bytes = NULL;
        if (omitted == 2) incomplete.end = NULL;
        assert(!katmed_source_export(&incomplete));
        assert(invalid.finished && !invalid.success && invalid.files == 0);
    }

    /* Feed the same source adapter with synthetic bytes from the real product
     * codec, then let the host package validate the emitted logical tree. */
    FILE *fixture = fopen(argv[1], "rb");
    assert(fixture);
    assert(fseek(fixture, 0, SEEK_END) == 0);
    long fixture_size = ftell(fixture);
    assert(fixture_size > 8 && fseek(fixture, 0, SEEK_SET) == 0);
    unsigned char *fixture_bytes = malloc((size_t)fixture_size);
    assert(fixture_bytes);
    assert(fread(fixture_bytes, 1, (size_t)fixture_size, fixture) == (size_t)fixture_size);
    assert(fclose(fixture) == 0);
    unsigned state_size = read_le32(fixture_bytes);
    assert(state_size <= 4096 && 4u + state_size + 4u < (unsigned)fixture_size);
    unsigned history_size = read_le32(fixture_bytes + 4u + state_size);
    assert(8u + state_size + history_size == (unsigned)fixture_size);
    entries[0].data = fixture_bytes + 4;
    entries[0].size = state_size;
    entries[1].data = fixture_bytes + 8u + state_size;
    entries[1].size = history_size;
    unsigned char auth_valid[1112] = {0};
    auth_valid[0] = '1'; auth_valid[1] = 'A'; auth_valid[2] = 'W'; auth_valid[3] = 'K';
    auth_valid[4] = 1;
    entries[2].data = auth_valid; entries[2].size = sizeof(auth_valid);
    unsigned char identity_valid[1288] = {0};
    identity_valid[0] = '1'; identity_valid[1] = 'I'; identity_valid[2] = 'W'; identity_valid[3] = 'K';
    identity_valid[4] = 32; identity_valid[6] = 4;
    entries[3].data = identity_valid; entries[3].size = sizeof(identity_valid);
    Result emitted = { .output = argv[2] };
    sink.context = &emitted;
    assert(katmed_source_export(&sink));
    assert(emitted.finished && emitted.success && emitted.files == 7);

    /* Full read-only source inventory: 127 files and exactly 248 KiB of
     * synthetic content remain intact; one more path fails before emission. */
    assert(remove(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/account.json") == 0);
    entry_count = 1;
    for (unsigned id = 1; id <= 127; ++id) {
        char path[160];
        int n = snprintf(path, sizeof(path),
                         KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-%08x.jpg", id);
        assert(n > 0 && (size_t)n < sizeof(path));
        FILE *image = fopen(path, "wb");
        assert(image);
        const size_t length = 253952u / 127u + (id <= 253952u % 127u);
        for (size_t i = 0; i < length; ++i) assert(fputc('S', image) != EOF);
        assert(fclose(image) == 0);
    }
    r = run(true);
    assert(r.files == 128 && r.bytes == state_size + 253952u);
    extra = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-00000080.jpg", "wb");
    assert(extra);
    assert(fclose(extra) == 0);
    r = run(false);
    assert(r.files == 0);
    for (unsigned id = 1; id <= 128; ++id) {
        char path[160];
        int n = snprintf(path, sizeof(path),
                         KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-%08x.jpg", id);
        assert(n > 0 && (size_t)n < sizeof(path) && remove(path) == 0);
    }
    FILE *original = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-00000001.jpg", "wb");
    assert(original && fwrite("jpeg", 1, 4, original) == 4 && fclose(original) == 0);
    original = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/account.json", "wb");
    assert(original && fwrite("{}", 1, 2, original) == 2 && fclose(original) == 0);

    /* Full bounded boot snapshot: all five NVS classes, image and seven ACME
     * files, with deterministic paths and a writer denied inside a callback. */
    const char *const acme_extra[] = {"directory.json", "account.pem", "order.json",
        "certkey.pem", "certificate.pem", "renewal.json"};
    for (unsigned i = 0; i < sizeof(acme_extra) / sizeof(acme_extra[0]); ++i) {
        char path[160];
        int n = snprintf(path, sizeof(path), KATMED_SOURCE_EXPORT_HOST_TEST "/acme/%s",
                         acme_extra[i]);
        assert(n > 0 && (size_t)n < sizeof(path));
        FILE *file = fopen(path, "wb");
        assert(file && fwrite("{}", 1, 2, file) == 2 && fclose(file) == 0);
    }
    entry_count = 5;
    katmed_source_snapshot_boot_window_open();
    Result denied = {0};
    KatmedSourceExportSink bounded = { &denied, begin, bytes, end, finish, keep_going };
    assert(!katmed_source_snapshot_boot(&bounded, busy));
    assert(denied.finished && !denied.success && denied.files == 0);

    pthread_t worker;
    assert(pthread_create(&worker, NULL, competing_writer, NULL) == 0);
    assert(pthread_mutex_lock(&writer_mutex) == 0);
    while (!writer_ready) assert(pthread_cond_wait(&writer_cond, &writer_mutex) == 0);
    assert(pthread_mutex_unlock(&writer_mutex) == 0);
    denied = (Result){0};
    assert(!katmed_source_snapshot_boot(&bounded, idle));
    assert(denied.finished && !denied.success && denied.files == 0);
    assert(pthread_mutex_lock(&writer_mutex) == 0);
    writer_release = true;
    assert(pthread_cond_signal(&writer_cond) == 0);
    assert(pthread_mutex_unlock(&writer_mutex) == 0);
    assert(pthread_join(worker, NULL) == 0);

    Result cancelled = { .abort_after = 12 };
    bounded.context = &cancelled;
    assert(!katmed_source_snapshot_boot(&bounded, idle));
    assert(cancelled.finished && !cancelled.success);
    assert(katmed_source_writer_enter());
    katmed_source_writer_leave();
    Result sink_failure = { .reject_at = 4 };
    bounded.context = &sink_failure;
    assert(!katmed_source_snapshot_boot(&bounded, idle));
    assert(sink_failure.finished && !sink_failure.success && sink_failure.files == 4);
    assert(katmed_source_writer_enter());
    katmed_source_writer_leave();

    /* The independently maximal image/NVS/ACME populations exceed the
     * shared 128-record limit; none may be silently omitted. */
    for (unsigned id = 2; id <= 123; ++id) {
        char path[160];
        int n = snprintf(path, sizeof(path),
                         KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-%08x.jpg", id);
        assert(n > 0 && (size_t)n < sizeof(path));
        FILE *file = fopen(path, "wb");
        assert(file && fwrite("jpeg", 1, 4, file) == 4 && fclose(file) == 0);
    }
    Result overfull = {0};
    bounded.context = &overfull;
    assert(!katmed_source_snapshot_boot(&bounded, idle));
    assert(overfull.finished && !overfull.success && overfull.files == 0);
    for (unsigned id = 2; id <= 123; ++id) {
        char path[160];
        int n = snprintf(path, sizeof(path),
                         KATMED_SOURCE_EXPORT_HOST_TEST "/user-images/img-%08x.jpg", id);
        assert(n > 0 && (size_t)n < sizeof(path) && remove(path) == 0);
    }
    Result complete = { .test_writer = true };
    bounded.context = &complete;
    assert(katmed_source_snapshot_boot(&bounded, idle));
    assert(complete.finished && complete.success && complete.files == 13);
    assert(complete.denied_writes == 1);
    assert(strcmp(complete.paths[5], "littlefs/user-images/img-00000001.jpg") == 0);
    assert(strcmp(complete.paths[6], "littlefs/acme/account.json") == 0);
    assert(strcmp(complete.paths[12], "littlefs/acme/renewal.json") == 0);
    katmed_source_snapshot_boot_window_close();
    Result replay = {0};
    bounded.context = &replay;
    assert(!katmed_source_snapshot_boot(&bounded, idle));
    assert(replay.finished && !replay.success && replay.files == 0);
    for (unsigned i = 0; i < sizeof(acme_extra) / sizeof(acme_extra[0]); ++i) {
        char path[160];
        int n = snprintf(path, sizeof(path), KATMED_SOURCE_EXPORT_HOST_TEST "/acme/%s",
                         acme_extra[i]);
        assert(n > 0 && (size_t)n < sizeof(path) && remove(path) == 0);
    }
    free(fixture_bytes);
    return 0;
}
