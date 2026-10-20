/* Exercise the product importer against the pinned LittleFS block engine.
 * Only the IDF VFS glue is replaced; flash contents survive every remount. */
#define main nvs_contract_main
#include "check.cpp"
#undef main
extern "C" {
#include "lfs.h"
}

#include <cassert>

namespace {
constexpr size_t block_size = 4096, block_count = 64, cache_size = 256;
uint8_t flash[block_size * block_count];
uint8_t read_cache[cache_size], prog_cache[cache_size], lookahead[cache_size];
lfs_t fs;
int fail_publish_at = -1, fail_stage_at = -1;

int read_block(const lfs_config *, lfs_block_t block, lfs_off_t off,
               void *out, lfs_size_t n) {
    if (block >= block_count || off + n > block_size) return LFS_ERR_IO;
    std::memcpy(out, flash + block * block_size + off, n);
    return 0;
}
int prog_block(const lfs_config *, lfs_block_t block, lfs_off_t off,
               const void *in, lfs_size_t n) {
    if (block >= block_count || off + n > block_size) return LFS_ERR_IO;
    const auto *src = static_cast<const uint8_t *>(in);
    auto *dst = flash + block * block_size + off;
    for (size_t i = 0; i < n; ++i) {
        if ((dst[i] & src[i]) != src[i]) return LFS_ERR_IO;
        dst[i] &= src[i];
    }
    return 0;
}
int erase_block(const lfs_config *, lfs_block_t block) {
    if (block >= block_count) return LFS_ERR_IO;
    std::memset(flash + block * block_size, 0xff, block_size);
    return 0;
}
int sync_block(const lfs_config *) { return 0; }

lfs_config config = [] {
    lfs_config c{};
    c.read = read_block; c.prog = prog_block;
    c.erase = erase_block; c.sync = sync_block;
    c.read_size = 16; c.prog_size = 16; c.block_size = block_size;
    c.block_count = block_count; c.block_cycles = 500;
    c.cache_size = cache_size; c.lookahead_size = cache_size;
    c.read_buffer = read_cache; c.prog_buffer = prog_cache;
    c.lookahead_buffer = lookahead;
    return c;
}();
void fresh_fs() {
    std::memset(flash, 0xff, sizeof(flash));
    assert(lfs_format(&fs, &config) == 0);
    assert(lfs_mount(&fs, &config) == 0);
    fail_publish_at = fail_stage_at = -1;
}
void remount() {
    assert(lfs_unmount(&fs) == 0);
    assert(lfs_mount(&fs, &config) == 0);
}
std::string name_for(const KatmedImportFile *f, unsigned ordinal, bool temp) {
    char name[64];
    if (temp) std::snprintf(name, sizeof(name), "/user-images/.migration-%03u.tmp", ordinal);
    else if (f->kind == 1)
        std::snprintf(name, sizeof(name), "/user-images/img-%08x.jpg", unsigned(f->id));
    else std::snprintf(name, sizeof(name), "/user-images/cat-%02x-%u.jpg",
                       unsigned(f->id >> 1), unsigned(f->id & 1));
    return name;
}
int presence(const std::string &path) {
    lfs_info info;
    int result = lfs_stat(&fs, path.c_str(), &info);
    if (result == LFS_ERR_NOENT) return 0;
    return result == 0 && info.type == LFS_TYPE_REG ? 1 : -1;
}
bool matches_file(const std::string &path, const KatmedImportFile *f) {
    lfs_file_t file;
    if (lfs_file_open(&fs, &file, path.c_str(), LFS_O_RDONLY) != 0) return false;
    bool ok = lfs_file_size(&fs, &file) == lfs_soff_t(f->length);
    uint8_t buffer[512];
    uint32_t crc = 0;
    size_t at = 0;
    while (ok && at < f->length) {
        size_t n = std::min(sizeof(buffer), size_t(f->length) - at);
        if (lfs_file_read(&fs, &file, buffer, n) != lfs_ssize_t(n)) ok = false;
        else { crc = uint32_t(crc32(crc, buffer, n)); at += n; }
    }
    if (lfs_file_close(&fs, &file) != 0) ok = false;
    return ok && at == f->length && crc == f->crc;
}
bool lfs_empty(void *, size_t bytes, size_t count) {
    lfs_ssize_t used_blocks = lfs_fs_size(&fs);
    if (used_blocks < 0 || size_t(used_blocks) * block_size > sizeof(flash)) return false;
    size_t free = sizeof(flash) - size_t(used_blocks) * block_size;
    if (free < bytes + 32768 + count * 256) return false;
    lfs_dir_t root;
    if (lfs_dir_open(&fs, &root, "/") != 0) return false;
    lfs_info info;
    bool ok = true;
    for (int r; (r = lfs_dir_read(&fs, &root, &info)) > 0;)
        if (std::strcmp(info.name, ".") && std::strcmp(info.name, "..") &&
            std::strcmp(info.name, "user-images")) ok = false;
    if (lfs_dir_close(&fs, &root) != 0) ok = false;
    if (!ok) return false;
    int r = lfs_dir_open(&fs, &root, "/user-images");
    if (r == LFS_ERR_NOENT) return true;
    if (r != 0) return false;
    for (; (r = lfs_dir_read(&fs, &root, &info)) > 0;)
        if (std::strcmp(info.name, ".") && std::strcmp(info.name, "..")) ok = false;
    if (r < 0 || lfs_dir_close(&fs, &root) != 0) ok = false;
    return ok;
}
bool lfs_stage(void *, unsigned ordinal, const KatmedImportFile *f,
               const KatmedImportInput *in) {
    const bool partial = int(ordinal) == fail_stage_at;
    if (partial) fail_stage_at = -1;
    int mk = lfs_mkdir(&fs, "/user-images");
    if (mk != 0 && mk != LFS_ERR_EXIST) return false;
    const auto path = name_for(f, ordinal, true);
    lfs_file_t file;
    if (lfs_file_open(&fs, &file, path.c_str(),
                      LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL) != 0) return false;
    uint8_t chunk[512];
    uint32_t crc = 0;
    bool ok = true;
    for (size_t at = 0; at < f->length && ok;) {
        size_t n = std::min(sizeof(chunk), size_t(f->length) - at);
        if (!in->read(in->context, f->input_offset + at, chunk, n) ||
            lfs_file_write(&fs, &file, chunk, n) != lfs_ssize_t(n)) ok = false;
        else { crc = uint32_t(crc32(crc, chunk, n)); at += n;
               if (partial) ok = false; }
    }
    if (crc != f->crc || lfs_file_sync(&fs, &file) != 0) ok = false;
    if (lfs_file_close(&fs, &file) != 0) ok = false;
    return ok && matches_file(path, f);
}
bool lfs_final(void *, unsigned ordinal, const KatmedImportFile *f) {
    return presence(name_for(f, ordinal, true)) == 0 &&
        matches_file(name_for(f, ordinal, false), f);
}
bool lfs_publish(void *ctx, unsigned ordinal, const KatmedImportFile *f) {
    if (int(ordinal) == fail_publish_at) { fail_publish_at = -1; return false; }
    auto final = name_for(f, ordinal, false), temp = name_for(f, ordinal, true);
    int present = presence(final);
    if (present < 0) return false;
    if (present == 1) {
        if (!matches_file(final, f)) return false;
        int staged = presence(temp);
        if (staged < 0 || (staged == 1 &&
            (!matches_file(temp, f) || lfs_remove(&fs, temp.c_str()) != 0))) return false;
    } else if (!matches_file(temp, f) ||
               lfs_rename(&fs, temp.c_str(), final.c_str()) != 0) return false;
    return lfs_final(ctx, ordinal, f);
}
bool lfs_abort(void *, unsigned ordinal, const KatmedImportFile *f) {
    if (presence(name_for(f, ordinal, false)) != 0) return false;
    auto temp = name_for(f, ordinal, true);
    int present = presence(temp);
    return present == 0 || (present == 1 && lfs_remove(&fs, temp.c_str()) == 0);
}
KatmedImportBackend real_backend(Memory &m) {
    auto b = backend(m);
    b.files_empty = lfs_empty; b.file_stage = lfs_stage;
    b.file_publish = lfs_publish; b.file_final = lfs_final;
    b.file_abort = lfs_abort;
    return b;
}
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    std::ifstream stream(argv[1], std::ios::binary);
    if (!stream) return 2;
    std::vector<uint8_t> capsule((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());
    fresh_fs();
    Memory m;
    m.capsule = capsule;
    auto b = real_backend(m);
    auto in = input(m);
    assert(katmed_import_apply(&in, &b));
    remount();
    assert(katmed_import_resume(&b, 1));
    assert(lfs_empty(&m, 0, 0) == false); /* Images remain after remount. */
    assert(m.values.count("katmed/state") == 1);
    assert(!katmed_import_apply(&in, &b)); /* Never overwrite live images. */
    assert(lfs_unmount(&fs) == 0);

    fresh_fs();
    Memory partial;
    partial.capsule = capsule;
    auto partial_backend = real_backend(partial);
    auto partial_input = input(partial);
    fail_stage_at = 0;
    assert(!katmed_import_apply(&partial_input, &partial_backend));
    assert(partial.values.count("katmed_imp/journal") == 0);
    assert(partial.values.count("katmed_imp/stage") == 0);
    assert(partial.values.count("katmed/state") == 0);
    assert(presence("/user-images/.migration-000.tmp") == 0);
    remount();
    assert(katmed_import_resume(&partial_backend, 1));
    assert(lfs_unmount(&fs) == 0);

    fresh_fs();
    Memory interrupted;
    interrupted.capsule = capsule;
    auto interrupted_backend = real_backend(interrupted);
    auto interrupted_input = input(interrupted);
    fail_publish_at = 1;
    assert(!katmed_import_apply(&interrupted_input, &interrupted_backend));
    assert(interrupted.values.count("katmed_imp/journal") == 1);
    assert(interrupted.values.count("katmed/state") == 0);
    remount();
    assert(!katmed_import_resume(&interrupted_backend, 2));
    assert(katmed_import_resume(&interrupted_backend, 1));
    assert(interrupted.values.count("katmed/state") == 1);
    assert(interrupted.values.count("katmed_imp/journal") == 0);
    assert(lfs_unmount(&fs) == 0);

    fresh_fs();
    lfs_file_t filler;
    assert(lfs_file_open(&fs, &filler, "/filler",
                         LFS_O_WRONLY | LFS_O_CREAT) == 0);
    uint8_t fill[512] = {0x5a};
    for (size_t at = 0; at < 224 * 1024; at += sizeof(fill))
        assert(lfs_file_write(&fs, &filler, fill, sizeof(fill)) == sizeof(fill));
    assert(lfs_file_close(&fs, &filler) == 0);
    const lfs_ssize_t used_blocks = lfs_fs_size(&fs);
    assert(used_blocks >= 0 &&
           (block_count - size_t(used_blocks)) * block_size < 32768 + 2 * 256);
    Memory full_fs;
    full_fs.capsule = capsule;
    auto full_backend = real_backend(full_fs);
    auto full_input = input(full_fs);
    assert(!katmed_import_apply(&full_input, &full_backend));
    assert(full_fs.writes == 0 && full_fs.erases == 0);
    assert(presence("/filler") == 1);
    assert(lfs_unmount(&fs) == 0);

    fresh_fs();
    Memory missing;
    missing.capsule = capsule;
    auto missing_backend = real_backend(missing);
    auto missing_input = input(missing);
    fail_publish_at = 1;
    assert(!katmed_import_apply(&missing_input, &missing_backend));
    assert(lfs_remove(&fs, "/user-images/.migration-001.tmp") == 0);
    remount();
    assert(!katmed_import_resume(&missing_backend, 1));
    assert(missing.values.count("katmed_imp/journal") == 1);
    assert(missing.values.count("katmed/state") == 0);
    assert(lfs_unmount(&fs) == 0);

    fresh_fs();
    Memory conflicting;
    conflicting.capsule = capsule;
    auto conflicting_backend = real_backend(conflicting);
    auto conflicting_input = input(conflicting);
    fail_publish_at = 1;
    assert(!katmed_import_apply(&conflicting_input, &conflicting_backend));
    lfs_file_t changed;
    assert(lfs_file_open(&fs, &changed, "/user-images/cat-00-0.jpg",
                         LFS_O_WRONLY | LFS_O_TRUNC) == 0);
    assert(lfs_file_write(&fs, &changed, "other", 5) == 5);
    assert(lfs_file_close(&fs, &changed) == 0);
    remount();
    assert(!katmed_import_resume(&conflicting_backend, 1));
    assert(conflicting.values.count("katmed_imp/journal") == 1);
    assert(conflicting.values.count("katmed/state") == 0);
    assert(lfs_unmount(&fs) == 0);

    fresh_fs();
    assert(lfs_mkdir(&fs, "/user-images") == 0);
    lfs_file_t existing;
    assert(lfs_file_open(&fs, &existing, "/user-images/img-00000007.jpg",
                         LFS_O_WRONLY | LFS_O_CREAT) == 0);
    assert(lfs_file_write(&fs, &existing, "owned", 5) == 5);
    assert(lfs_file_close(&fs, &existing) == 0);
    Memory occupied;
    occupied.capsule = capsule;
    auto occupied_backend = real_backend(occupied);
    auto occupied_input = input(occupied);
    assert(!katmed_import_apply(&occupied_input, &occupied_backend));
    assert(occupied.writes == 0 && occupied.erases == 0);
    assert(presence("/user-images/img-00000007.jpg") == 1);
    assert(lfs_unmount(&fs) == 0);
    puts("actual LittleFS KMI1 import/remount/replay: PASS");
}
