#include "katmed_images_internal.h"
#include "katmed_images_hash.h"
#include "katmed_core.h"
#include "src/image/lv_image_decoder_private.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
bool fail_next_image_path_copy = false;
unsigned failed_image_path_copies = 0;
}

extern "C" char *__real_lv_strdup(const char *value);
extern "C" char *__wrap_lv_strdup(const char *value)
{
    if(fail_next_image_path_copy && value != nullptr && std::strcmp(value, "K:/01_0.jpg") == 0) {
        fail_next_image_path_copy = false;
        ++failed_image_path_copies;
        return nullptr;
    }
    return __real_lv_strdup(value);
}

namespace {
struct Storage {
    FILE *file = nullptr;
    uint32_t bytes = 0;
    bool fail = false;
    bool short_read = false;
    bool corrupt = false;
    uint64_t reads = 0;
    size_t largest_read = 0;
};

bool read_pack(void *context, uint32_t offset, uint8_t *dst, size_t wanted, size_t *actual)
{
    auto &s = *static_cast<Storage *>(context);
    *actual = 0;
    ++s.reads;
    if(wanted > s.largest_read) s.largest_read = wanted;
    if(s.fail || offset > s.bytes || wanted > s.bytes - offset ||
       std::fseek(s.file, static_cast<long>(offset), SEEK_SET) != 0) return false;
    size_t length = s.short_read && wanted ? wanted - 1 : wanted;
    *actual = std::fread(dst, 1, length, s.file);
    if(s.corrupt && *actual) dst[0] ^= 0x01;
    return true;
}

void expect(bool condition, const char *why)
{
    if(!condition) throw std::runtime_error(why);
}

void hash_vectors()
{
    struct Vector { const char *input; const char *hex; };
    constexpr Vector vectors[] = {
        {"", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {"abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
    };
    for(const auto &v : vectors) {
        KatmedImageHash sha;
        expect(katmed_images_hash_start(&sha), "mbedTLS SHA-256 start failed");
        for(size_t i = 0; i < std::strlen(v.input); ++i)
            expect(katmed_images_hash_update(&sha, reinterpret_cast<const uint8_t *>(v.input + i), 1),
                   "mbedTLS SHA-256 update failed");
        uint8_t got[32];
        expect(katmed_images_hash_finish(&sha, got) && !sha.active, "mbedTLS SHA-256 finish/release failed");
        char hex[65];
        for(unsigned i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02x", got[i]);
        expect(std::strcmp(hex, v.hex) == 0, "SHA-256 standard vector mismatch");
    }
    KatmedImageHash released;
    expect(katmed_images_hash_start(&released), "mbedTLS lifecycle setup failed");
    expect(!katmed_images_hash_update(&released, nullptr, 1) && !released.active,
           "invalid hash input did not release its context");
    katmed_images_hash_free(&released);
    expect(katmed_images_hash_start(&released), "released hash context could not restart");
    expect(!katmed_images_hash_finish(&released, nullptr) && !released.active,
           "invalid hash output did not release its context");
    expect(katmed_images_hash_start(&released), "million-byte vector start failed");
    std::array<uint8_t, 1000> thousand{};
    thousand.fill('a');
    for(unsigned i = 0; i < 1000; ++i)
        expect(katmed_images_hash_update(&released, thousand.data(), thousand.size()),
               "million-byte vector update failed");
    uint8_t result[32];
    expect(katmed_images_hash_finish(&released, result), "million-byte vector finish failed");
    const uint8_t million_a[32] = {0xcd,0xc7,0x6e,0x5c,0x99,0x14,0xfb,0x92,
        0x81,0xa1,0xc7,0xe2,0x84,0xd7,0x3e,0x67,0xf1,0x80,0x9a,0x48,
        0xa4,0x97,0x20,0x0e,0x04,0x6d,0x39,0xcc,0xc7,0x11,0x2c,0xd0};
    expect(std::memcmp(result, million_a, sizeof(result)) == 0, "million-byte SHA-256 vector mismatch");
}

unsigned flushes = 0;
void flush(lv_display_t *display, const lv_area_t *, uint8_t *)
{
    ++flushes;
    lv_display_flush_ready(display);
}

void decode_tiles(const char *path, uint16_t width, uint16_t height)
{
    lv_image_decoder_dsc_t dsc;
    expect(lv_image_decoder_open(&dsc, path, nullptr) == LV_RESULT_OK, "TJpgD open failed");
    lv_area_t full = {0, 0, static_cast<lv_coord_t>(width - 1),
                      static_cast<lv_coord_t>(height - 1)};
    lv_area_t area = {LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN};
    bool complete = false;
    uint32_t pixels = 0;
    uint32_t tiles = 0;
    for(unsigned i = 0; i < 1280; ++i) {
        if(lv_image_decoder_get_area(&dsc, &full, &area) != LV_RESULT_OK) break;
        expect(area.x1 >= 0 && area.y1 >= 0 && area.x2 < width && area.y2 < height &&
               area.x1 <= area.x2 && area.y1 <= area.y2, "decoder emitted invalid tile bounds");
        expect(dsc.decoded != nullptr && dsc.decoded->data_size <= 16u * 16u * 3u,
               "decoder allocated a frame instead of a bounded RGB888 MCU");
        pixels += static_cast<uint32_t>(lv_area_get_width(&area) * lv_area_get_height(&area));
        ++tiles;
        if(area.x2 == width - 1 && area.y2 == height - 1) {
            complete = true;
            break;
        }
    }
    lv_image_decoder_close(&dsc);
    expect(complete, "TJpgD did not produce all image tiles");
    expect(pixels == uint32_t{width} * height && tiles == ((width + 15u) / 16u) * ((height + 15u) / 16u),
           "decoder did not cover the complete image with 4:2:0 MCU tiles");
    expect(katmed_images_open_file_count() == 0, "TJpgD leaked a file handle");
}
}

int main(int argc, char **argv)
{
    try {
        expect(argc == 2, "expected the target image pack path");
        hash_vectors();
        Storage backend;
        backend.file = std::fopen(argv[1], "rb");
        expect(backend.file != nullptr, "image pack unavailable");
        expect(std::fseek(backend.file, 0, SEEK_END) == 0, "pack seek failed");
        long size = std::ftell(backend.file);
        expect(size > 0 && size <= UINT32_MAX, "pack size invalid");
        backend.bytes = static_cast<uint32_t>(size);
        KatmedStorage storage{&backend, backend.bytes, read_pack};

        expect(katmed_images_mount_storage(storage) == KATMED_IMAGE_OK, "valid pack rejected");
        expect(backend.largest_read == KATMED_IMAGE_READ_CHUNK, "pack hash exceeded its 512-byte buffer");
        expect(katmed_images_count() == 72, "logical image count mismatch");
        KatmedGeneratedCatalog malformed = katmed_generated_catalog;
        malformed.images[0].variants[0].offset = UINT32_MAX;
        expect(katmed_images_validate_catalog(&malformed, backend.bytes) == KATMED_IMAGE_BAD_CATALOG,
               "overflowing catalog offset accepted");
        malformed = katmed_generated_catalog;
        malformed.images[0].variants[0].length = UINT32_MAX;
        expect(katmed_images_validate_catalog(&malformed, backend.bytes) == KATMED_IMAGE_BAD_CATALOG,
               "overflowing catalog length accepted");
        malformed = katmed_generated_catalog;
        malformed.images[1].logical_id = malformed.images[0].logical_id;
        expect(katmed_images_validate_catalog(&malformed, backend.bytes) == KATMED_IMAGE_BAD_CATALOG,
               "duplicate catalog identity accepted");
        malformed = katmed_generated_catalog;
        malformed.images[0].variants[0].width += 1;
        expect(katmed_images_validate_catalog(&malformed, backend.bytes) == KATMED_IMAGE_BAD_CATALOG,
               "wrong catalog dimension accepted");
        lv_init();
        katmed_images_register_lvgl_fs();

        unsigned decoded = 0;
        for(uint32_t id = 0; id < katmed_images_count(); ++id) {
            uint64_t logical_id = 0;
            for(unsigned angle = 0; angle < 4; ++angle) {
                KatmedImageSelection selection;
                auto orientation = static_cast<Orientation>(angle);
                expect(katmed_images_select(id, orientation, &selection) == KATMED_IMAGE_OK,
                       "catalogue selection rejected a valid image/orientation");
                expect(!selection.show_neutral_fallback, "valid image selected fallback");
                if(angle == 0) logical_id = selection.logical_id;
                expect(selection.logical_id == logical_id, "rotation changed logical ID");
                const bool landscape = angle == 1 || angle == 3;
                const uint16_t expected_width = katmed_generated_catalog.profile_count == 1 ? 240 :
                                                landscape ? 320 : 172;
                const uint16_t expected_height = katmed_generated_catalog.profile_count == 1 ? 240 :
                                                 landscape ? 172 : 320;
                expect(selection.width == expected_width && selection.height == expected_height,
                       "orientation dimensions differ from the explicit board contract");
                uint32_t p = katmed_images_profile_for_orientation(orientation);
                const auto *v = katmed_images_variant(id, p);
                expect(v != nullptr && selection.width == v->width && selection.height == v->height,
                       "orientation chose the wrong image profile");
                if(angle == 0 || (angle == 1 && katmed_generated_catalog.profile_count == 2)) {
                    decode_tiles(selection.lvgl_path, selection.width, selection.height);
                    ++decoded;
                }
            }
        }
        expect(decoded == (katmed_generated_catalog.profile_count == 1 ? 72u : 144u),
               "not every JPEG variant was decoded");

        uint8_t buffer[8];
        size_t actual = 0;
        auto *first = katmed_images_variant(0, 0);
        expect(katmed_images_read_variant(0, 0, first->length, buffer, 1, &actual) == KATMED_IMAGE_BAD_RANGE && actual == 0,
               "image EOF crossing was accepted");
        expect(katmed_images_read_variant(0, 0, UINT32_MAX, buffer, 1, &actual) == KATMED_IMAGE_BAD_RANGE,
               "overflowing image offset was accepted");
        expect(katmed_images_read_variant(0, 0, 0, buffer, SIZE_MAX, &actual) == KATMED_IMAGE_BAD_RANGE,
               "huge read length was accepted");
        expect(katmed_images_read_variant(72, 0, 0, buffer, 1, &actual) == KATMED_IMAGE_BAD_ID,
               "missing logical image was accepted");
        KatmedImageSelection missing;
        for(uint32_t old_index : {72u, 73u, 74u, 65535u, UINT32_MAX}) {
            expect(katmed_images_select(old_index, ORIENTATION_0, &missing) == KATMED_IMAGE_BAD_ID &&
                   missing.show_neutral_fallback, "old or invalid image did not request fallback");
            expect(katmed_images_read_variant(old_index, 0, 0, buffer, 1, &actual) == KATMED_IMAGE_BAD_ID,
                   "old or invalid image index was readable");
        }
        for(int invalid_angle : {-1, 4, 255})
            expect(katmed_images_select(0, static_cast<Orientation>(invalid_angle), &missing) ==
                   KATMED_IMAGE_BAD_ID && missing.show_neutral_fallback,
                   "invalid orientation was accepted");

        lv_fs_file_t file;
        expect(lv_fs_open(&file, "K:/00_0.jpg", LV_FS_MODE_RD) == LV_FS_RES_OK,
               "registered FS driver cannot open an image");
        expect(lv_fs_seek(&file, UINT32_MAX, LV_FS_SEEK_SET) != LV_FS_RES_OK,
               "overflowing FS seek was accepted");
        uint32_t got = 10;
        expect(lv_fs_read(&file, buffer, sizeof(buffer), &got) != LV_FS_RES_OK && got == 0,
               "seek failure was not sticky for decoder reads");
        expect(lv_fs_close(&file) == LV_FS_RES_OK && katmed_images_open_file_count() == 0,
               "seek-error file handle leaked");
        std::array<lv_fs_file_t, KATMED_IMAGE_MAX_OPEN_FILES> handles{};
        for(auto &handle : handles)
            expect(lv_fs_open(&handle, "K:/00_0.jpg", LV_FS_MODE_RD) == LV_FS_RES_OK,
                   "fixed file pool filled too early");
        expect(lv_fs_open(&file, "K:/00_0.jpg", LV_FS_MODE_RD) != LV_FS_RES_OK,
               "file pool exceeded its fixed bound");
        for(auto &handle : handles) expect(lv_fs_close(&handle) == LV_FS_RES_OK, "pool handle close failed");
        expect(katmed_images_open_file_count() == 0, "fixed file pool leaked slots");

        lv_display_t *display = lv_display_create(240, 240);
        expect(display != nullptr, "LVGL display creation failed");
        std::array<uint8_t, 240 * 16 * 2> draw_buffer{};
        lv_display_set_buffers(display, draw_buffer.data(), nullptr, draw_buffer.size(),
                               LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(display, flush);
        lv_obj_t *screen = lv_screen_active();
        lv_obj_t *image = lv_image_create(screen);
        lv_obj_t *fallback = lv_label_create(screen);
        ClockSample clock{1000, 1, 0, false, CLOCK_UNAVAILABLE};
        EventId boot{1, 1, 1};
        AppState state{};
        SharedSettings settings = app_default_settings();
        expect(app_init(&state, &settings, &clock, boot, katmed_images_count(),
                        katmed_images_catalog_sha256()) == APP_OK && state.alarm_active,
               "core did not establish boot alarm");
        state.start_reason = START_SETTINGS;
        state.deadline.regular_monotonic_ms = clock.monotonic_ms;
        state.deadline.effective_monotonic_ms = clock.monotonic_ms;
        AppEvent confirmation{};
        confirmation.kind = EVENT_CONFIRM;
        confirmation.id = {1, 1, 2};
        confirmation.sender = 1;
        confirmation.round = state.round;
        confirmation.parent_revision = state.revision;
        confirmation.occurred_at = clock;
        confirmation.coordinator_accepted = true;
        AppEffects effects{};
        expect(app_apply_event(&state, &confirmation, &clock, &effects) == APP_OK &&
               !state.alarm_active && state.deadline.effective_monotonic_ms > clock.monotonic_ms,
               "core did not establish a real nonzero medication deadline");
        ClockSample due = clock;
        due.monotonic_ms = state.deadline.effective_monotonic_ms;
        expect(app_tick(&state, &due, &effects) == APP_OK && state.alarm_active,
               "core alarm did not become due after a confirmed round");
        AppState before = state;
        expect(katmed_images_present(image, fallback, 0, ORIENTATION_0) == KATMED_IMAGE_OK,
               "valid image could not be presented");
        expect(!lv_obj_is_hidden(image) &&
               lv_obj_is_hidden(fallback), "valid image left neutral fallback visible");
        lv_refr_now(display);
        expect(flushes != 0 && !lv_obj_is_hidden(image),
               "real successful LVGL refresh failed");
        expect(std::strcmp(static_cast<const char *>(lv_image_get_src(image)), "K:/00_0.jpg") == 0,
               "source-copy regression needs a previously selected image");
        // Fail only the source-string allocation while changing from image 0
        // to image 1. LVGL's existing old source must not count as success.
        fail_next_image_path_copy = true;
        expect(katmed_images_present(image, fallback, 1, ORIENTATION_0) == KATMED_IMAGE_IO_ERROR,
               "failed source-string allocation was reported as a successful image change");
        expect(!fail_next_image_path_copy && failed_image_path_copies == 1,
               "source-string allocation fault was not injected exactly once");
        expect(lv_image_get_src(image) == nullptr && lv_obj_is_hidden(image) &&
               !lv_obj_is_hidden(fallback) &&
               katmed_images_last_error() == KATMED_IMAGE_IO_ERROR,
               "source-string allocation failure did not replace the stale image with fallback");
        expect(katmed_images_open_file_count() == 0 &&
               std::memcmp(&state, &before, sizeof(state)) == 0,
               "source-string allocation failure leaked a handle or changed the active alarm");
        lv_refr_now(display);
        expect(katmed_images_present(image, fallback, 1, ORIENTATION_0) == KATMED_IMAGE_OK &&
               std::strcmp(static_cast<const char *>(lv_image_get_src(image)), "K:/01_0.jpg") == 0 &&
               !lv_obj_is_hidden(image) && lv_obj_is_hidden(fallback),
               "image path did not recover to the requested image after an allocation failure");
        lv_refr_now(display);
        // Select and preflight successfully, then fail only the later real
        // draw. The fallback must be switched after LVGL ends rendering.
        backend.fail = true;
        lv_obj_invalidate(image);
        lv_refr_now(display);
        expect(lv_obj_is_hidden(image) &&
               !lv_obj_is_hidden(fallback),
               "late draw-time I/O failure did not show neutral fallback");
        expect(katmed_images_present(image, fallback, 0, ORIENTATION_0) != KATMED_IMAGE_OK,
               "injected image read fault went unnoticed");
        expect(lv_obj_is_hidden(image) &&
               !lv_obj_is_hidden(fallback), "failed image did not show neutral fallback");
        expect(std::strcmp(lv_label_get_text(fallback), "Bild nicht verfügbar") == 0,
               "neutral fallback text missing");
        expect(std::memcmp(&state, &before, sizeof(state)) == 0 &&
               state.alarm_active == before.alarm_active &&
               state.deadline.regular_monotonic_ms == before.deadline.regular_monotonic_ms &&
               state.deadline.effective_monotonic_ms == before.deadline.effective_monotonic_ms,
               "image error changed core alarm/deadline");
        backend.fail = false;
        backend.short_read = true;
        expect(katmed_images_select(0, ORIENTATION_0, &missing) == KATMED_IMAGE_IO_ERROR,
               "short partition read went unnoticed");
        backend.short_read = false;
        backend.corrupt = true;
        expect(katmed_images_select(0, ORIENTATION_0, &missing) == KATMED_IMAGE_BAD_HASH,
               "corrupted image bytes went unnoticed");
        backend.corrupt = false;
        expect(katmed_images_open_file_count() == 0, "LVGL file handles were not released");
        expect(katmed_images_present(image, fallback, 0, ORIENTATION_0) == KATMED_IMAGE_OK,
               "image path did not recover after failed reads");
        lv_obj_delete(fallback);
        backend.fail = true;
        lv_obj_invalidate(image);
        lv_refr_now(display);
        backend.fail = false;
        expect(katmed_images_open_file_count() == 0, "late error after label deletion leaked a handle");
        lv_display_delete(display);
        katmed_images_unmount();
        backend.corrupt = true;
        expect(katmed_images_mount_storage(storage) == KATMED_IMAGE_BAD_HASH,
               "corrupted pack passed boot hash validation");
        backend.corrupt = false;
        backend.short_read = true;
        expect(katmed_images_mount_storage(storage) == KATMED_IMAGE_IO_ERROR,
               "short boot read was accepted");
        backend.short_read = false;
        KatmedStorage undersized{&backend, backend.bytes - 1, read_pack};
        expect(katmed_images_mount_storage(undersized) == KATMED_IMAGE_BAD_CATALOG,
               "undersized image partition passed index validation");
        std::fclose(backend.file);
        std::printf("image reader: %u images, %u decoded variants, four angles, faults, fallback, alarm intact\n",
                    katmed_images_count(), decoded);
        return 0;
    }
    catch(const std::exception &e) {
        std::fprintf(stderr, "image reader failure: %s\n", e.what());
        return 1;
    }
}
