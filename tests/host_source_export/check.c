#include "katmed_source_export.h"
#include "nvs.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

typedef struct { unsigned files; size_t bytes; bool finished; bool success; bool reject; unsigned reject_at; char paths[8][80]; const char *output; FILE *stream; } Result;
static bool begin(void *ctx, const char *path, size_t size)
{
    Result *r = ctx;
    assert(r->files < 8 && size <= 128 * 1024);
    snprintf(r->paths[r->files], sizeof(r->paths[0]), "%s", path);
    ++r->files;
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
static Result run(bool expected)
{
    Result r = {0};
    KatmedSourceExportSink sink = { &r, begin, bytes, end, finish };
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

    /* ARI can persist this seventh ACME file. Keep the conservative abort
     * until source, host, import, and reset contracts all cover it. */
    extra = fopen(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/renewal.json", "wb");
    assert(extra);
    assert(fclose(extra) == 0);
    r = run(false);
    assert(r.files == 0);
    assert(remove(KATMED_SOURCE_EXPORT_HOST_TEST "/acme/renewal.json") == 0);

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
    KatmedSourceExportSink sink = { &refused, begin, bytes, end, finish };
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
        KatmedSourceExportSink incomplete = { &invalid, begin, bytes, end, finish };
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
    free(fixture_bytes);
    return 0;
}
