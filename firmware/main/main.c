/* Board, durable local runtime, authenticated group and real time source. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "runtime_metrics.h"

#if KATMED_FULL_STACK && KATMED_WITH_CORE
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_mesh_lite.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "katmed_board.h"
#include "katmed_core.h"
#include "katmed_images.h"
#include "katmed_mesh.h"
#include "katmed_dhcp_diag.h"
#include "katmed_store.h"
#include "katmed_store_idf.h"
#include <private_wifi_defaults.h>
#include "katmed_time.h"
#include "katmed_time_idf.h"
#include "katmed_ui.h"
#include "usb_console.h"

static ClockSample clock_sample(uint64_t domain);
static uint64_t monotonic_ms(void);

typedef struct {
    Store store;
    StoreIdfNvs nvs;
    AppState state;
    LocalSettings local;
    EventId boot;
    uint64_t boot_domain; /* Distinguishes independent monotonic clocks. */
    KatmedTime time;
    uint64_t time_sequence;
    uint64_t time_sequence_end;
    uint64_t reboot_due_ms;
    EventId shared_id;
    bool mesh_started;
    bool reboot_required;
    bool shared_pending;
    int8_t shared_feedback; /* 0 none, 1 accepted, -1 rejected. */
    bool store_ready;
    uint8_t utc_source; /* 0 unknown, 1 local SNTP, 2 authenticated Mesh. */
    uint64_t evidence_sequence;
    char witness_nonce[33]; /* One request, one boot, never persisted. */
    bool witness_armed;
} Runtime;

static Runtime runtime;
/* LVGL callbacks enqueue bounded transient inputs. Only the application loop
 * invokes Mesh, including the final ALARM input before any queued old Down. */
#define INTERACTION_QUEUE_CAPACITY 32u
static struct {
    KatmedInteractionInput input[INTERACTION_QUEUE_CAPACITY];
    uint64_t time[INTERACTION_QUEUE_CAPACITY];
    unsigned head, count;
    bool overflow, quality_pending;
    uint64_t latest_entry, invalid_through_entry;
} interaction_queue;
static KatmedInteractionStatus interaction_snapshot;
#ifdef ESP_PLATFORM
static portMUX_TYPE interaction_lock = portMUX_INITIALIZER_UNLOCKED;
#define INTERACTION_LOCK() portENTER_CRITICAL(&interaction_lock)
#define INTERACTION_UNLOCK() portEXIT_CRITICAL(&interaction_lock)
#else
#define INTERACTION_LOCK() ((void)0)
#define INTERACTION_UNLOCK() ((void)0)
#endif
static bool interaction_submit(void *context, const KatmedInteractionInput *input,
                                uint64_t now)
{
    (void)context;
    INTERACTION_LOCK();
    if (input->menu_entry > interaction_queue.latest_entry)
        interaction_queue.latest_entry = input->menu_entry;
    bool poisoned = input->menu_entry &&
        input->menu_entry <= interaction_queue.invalid_through_entry &&
        input->kind != KATMED_INTERACTION_LEAVE &&
        input->kind != KATMED_INTERACTION_CONSUME_IMPULSE &&
        input->kind != KATMED_INTERACTION_DISMISS_GREETING;
    bool accepted = !poisoned && interaction_queue.count < INTERACTION_QUEUE_CAPACITY;
    if (accepted) {
        unsigned slot = (interaction_queue.head+interaction_queue.count)%INTERACTION_QUEUE_CAPACITY;
        interaction_queue.input[slot] = *input;
        interaction_queue.time[slot] = now;
        ++interaction_queue.count;
    } else if (!poisoned) {
        /* Invalidate the whole affected entry before a lost Up can be
         * overtaken by any remaining old Down. Recovery requires a new entry. */
        interaction_queue.overflow = interaction_queue.quality_pending = true;
        interaction_queue.invalid_through_entry = interaction_queue.latest_entry;
        interaction_queue.head = interaction_queue.count = 0;
    }
    INTERACTION_UNLOCK();
    return accepted;
}
static void interaction_status(void *context, KatmedInteractionStatus *out, uint64_t now)
{
    (void)context; (void)now;
    *out = interaction_snapshot; /* serialized with render_ui by the LVGL lock */
    INTERACTION_LOCK();
    out->overflow = out->overflow || interaction_queue.quality_pending;
    if (!runtime.state.alarm_active) interaction_queue.quality_pending = false;
    INTERACTION_UNLOCK();
}
static void interaction_drain(uint64_t now)
{
    static bool alarm;
    if (runtime.state.alarm_active != alarm) {
        alarm = runtime.state.alarm_active;
        KatmedInteractionInput input = {.kind = KATMED_INTERACTION_ALARM, .active = alarm};
        katmed_mesh_interaction_input(&input, now);
    }
    for (unsigned n = 0; n < INTERACTION_QUEUE_CAPACITY; ++n) {
        INTERACTION_LOCK();
        bool overflow = interaction_queue.overflow;
        interaction_queue.overflow = false;
        bool have = interaction_queue.count != 0;
        KatmedInteractionInput input = {0};
        uint64_t at = now;
        if (have) {
            input = interaction_queue.input[interaction_queue.head];
            at = interaction_queue.time[interaction_queue.head];
            interaction_queue.head = (interaction_queue.head+1)%INTERACTION_QUEUE_CAPACITY;
            --interaction_queue.count;
        }
        INTERACTION_UNLOCK();
        if (overflow) {
            /* Read the actual active entry, not a possibly pre-ENTER UI
             * snapshot. All inputs from the poisoned entries were discarded. */
            KatmedInteractionStatus current;
            katmed_mesh_interaction_status(&current, now);
            if (current.menu_open) {
                KatmedInteractionInput leave = {.kind = KATMED_INTERACTION_LEAVE,
                    .menu_entry = current.menu_entry, .grant_id = current.grant_id};
                katmed_mesh_interaction_input(&leave, now);
            }
        }
        if (!have) break;
        if (input.kind == KATMED_INTERACTION_DOWN || input.kind == KATMED_INTERACTION_UP) {
            input.contact_time_ms = at;
            input.contact_time_valid = true;
        }
        if (!alarm || (input.kind != KATMED_INTERACTION_DOWN &&
                       input.kind != KATMED_INTERACTION_ENTER))
            katmed_mesh_interaction_input(&input, now);
    }
}


static KatmedRuntimeEvidenceState evidence_state(const Runtime *rt,
                                                 const ClockSample *now,
                                                 uint64_t sequence)
{
    const AppState *s=&rt->state;
    return (KatmedRuntimeEvidenceState){
        .seq=sequence,.uptime_ms=now->monotonic_ms,.boot_domain=rt->boot_domain,
        .round_device=s->round.origin.device,.round_epoch=s->round.origin.epoch,
        .round_sequence=s->round.origin.sequence,.round_ordinal=s->round.ordinal,
        .alarm=s->alarm_active,.reason=s->start_reason,
        .origin_device=s->countdown_origin.device,
        .origin_epoch=s->countdown_origin.epoch,
        .origin_sequence=s->countdown_origin.sequence,
        .start_domain=s->countdown_started.domain,
        .start_ms=s->countdown_started.monotonic_ms,
        .start_utc_valid=s->countdown_started.utc_valid,
        .start_utc_ms=s->countdown_started.utc_ms,
        .clock_domain=s->clock.domain,
        .regular_ms=s->deadline.regular_monotonic_ms,
        .effective_ms=s->deadline.effective_monotonic_ms,
        .deadline_local=s->clock.domain==rt->boot_domain,
        .regular_utc_valid=s->deadline.regular_utc.valid,
        .regular_utc_ms=s->deadline.regular_utc.ms,
        .effective_utc_valid=s->deadline.effective_utc.valid,
        .effective_utc_ms=s->deadline.effective_utc.ms,
        .now_utc_valid=now->utc_valid,.now_utc_ms=now->utc_ms,
        .now_quality=now->quality,.utc_source=rt->utc_source,
        .image_id=s->image_id,.image_remaining_ms=s->image_remaining_ms,
        .image_pending=s->image_due_pending,
    };
}

static uint64_t new_boot_domain(EventId boot)
{
    /* A Store epoch is only local to one device. Combine its device identity
     * with a fresh SDK nonce so equal epoch numbers on peers cannot make
     * their independent monotonic counters look related. This labels time;
     * it is not an authentication or cryptographic primitive. */
    const uint64_t nonce = ((uint64_t)esp_random() << 32) | esp_random();
    const uint64_t identity = (boot.device << 16) ^ boot.epoch;
    const uint64_t domain = nonce ^ identity;
    return domain != 0 ? domain : (identity != 0 ? identity : 1);
}

static bool same_id(EventId a, EventId b)
{
    return a.device == b.device && a.epoch == b.epoch && a.sequence == b.sequence;
}

static bool time_event_id(Runtime *rt, EventId *id)
{
    if (rt->time_sequence == rt->time_sequence_end) {
        EventId first;
        if (store_reserve_event_ids(&rt->store, 1024, &first) != STORE_OK)
            return false;
        rt->time_sequence = first.sequence;
        rt->time_sequence_end = first.sequence + 1024;
    }
    *id = (EventId){rt->boot.device, rt->boot.epoch, rt->time_sequence++};
    return true;
}

static bool apply_time_sample(Runtime *rt, const ClockSample *sample)
{
    if (!rt->store_ready || !sample || !sample->utc_valid ||
        store_get_status(&rt->store, NULL, NULL, NULL) != STORE_OK)
        return false;
    EventId id;
    if (!time_event_id(rt, &id)) return false;
    const ClockSample now = clock_sample(rt->boot_domain);
    AppEvent event = {
        .kind = EVENT_TIME_SYNC,
        .id = id,
        .sender = id.device,
        .round = rt->state.round,
        .parent_revision = rt->state.revision,
        .occurred_at = *sample,
        .value.time_sync = *sample,
    };
    AppEffects effects;
    AppResult core_result;
    return store_apply_event(&rt->store, &rt->state, &event, &now, false,
                             &effects, &core_result) == STORE_OK;
}

static void mesh_utc_received(void *context, uint64_t observed_mono_ms,
                              int64_t utc_ms, ClockQuality quality)
{
    Runtime *rt = (Runtime *)context;
    if (!rt || rt->reboot_required) return;
    const KatmedTime previous = rt->time;
    if (!katmed_time_accept_utc(&rt->time, observed_mono_ms, utc_ms, quality))
        return;
    const ClockSample sample = katmed_time_sample(&rt->time, observed_mono_ms);
    if (!apply_time_sample(rt, &sample)) rt->time = previous;
    else rt->utc_source=2;
}

static void mesh_shared_result(void *context, EventId id, bool accepted)
{
    Runtime *rt = (Runtime *)context;
    if (!rt || !rt->shared_pending || !same_id(id, rt->shared_id)) return;
    rt->shared_pending = false;
    rt->shared_feedback = accepted ? 1 : -1;
}

static UiSubmitResult submit_ui(void *context, const UiRequest *request)
{
    Runtime *rt = (Runtime *)context;
    if (rt == NULL || request == NULL || !rt->store_ready || rt->reboot_required)
        return UI_SUBMIT_UNAVAILABLE;
    /* A failed NVS commit may have persisted the candidate while leaving the
     * active copy unchanged. Even a matching local request is not a success
     * until the store is reopened and the durable value is known. */
    if (store_get_status(&rt->store, NULL, NULL, NULL) != STORE_OK)
        return UI_SUBMIT_UNAVAILABLE;
    if (request->kind == UI_REQUEST_SET_CLOCK_STYLE) {
        if (request->value.clock_style > ANALOG) return UI_SUBMIT_REJECTED;
        if (request->value.clock_style == rt->local.clock_style)
            return UI_SUBMIT_COMMITTED;
        LocalSettings wanted = rt->local;
        wanted.clock_style = request->value.clock_style;
        return store_set_local(&rt->store, &rt->local, &wanted) == STORE_OK
            ? UI_SUBMIT_COMMITTED : UI_SUBMIT_UNAVAILABLE;
    }
    if (request->kind == UI_REQUEST_SET_ORIENTATION) {
        if (request->value.orientation > ORIENTATION_270) return UI_SUBMIT_REJECTED;
        if (request->value.orientation == rt->local.orientation)
            return UI_SUBMIT_COMMITTED;
        const Orientation previous = rt->local.orientation;
        if (board_set_orientation(request->value.orientation) != BOARD_OK)
            return UI_SUBMIT_UNAVAILABLE;
        LocalSettings wanted = rt->local;
        wanted.orientation = request->value.orientation;
        if (store_set_local(&rt->store, &rt->local, &wanted) != STORE_OK) {
            (void)board_set_orientation(previous);
            return UI_SUBMIT_UNAVAILABLE;
        }
        return UI_SUBMIT_COMMITTED;
    }
    if (request->kind == UI_REQUEST_CONFIRM) {
        const bool coordinator = rt->mesh_started && katmed_mesh_connected() &&
                                 katmed_mesh_is_coordinator();
        uint8_t pending = 0;
        if (store_get_status(&rt->store, NULL, &pending, NULL) != STORE_OK ||
            (!coordinator && pending >= KATMED_STORE_PENDING_MAX))
            return UI_SUBMIT_UNAVAILABLE;
        EventId id;
        if (store_next_event_id(&rt->store, &id) != STORE_OK)
            return UI_SUBMIT_UNAVAILABLE;
        const ClockSample now = clock_sample(rt->boot_domain);
        AppEvent event = {
            .kind = EVENT_CONFIRM,
            .id = id,
            .sender = id.device,
            .round = rt->state.round,
            .parent_revision = rt->state.revision,
            .occurred_at = now,
            .coordinator_accepted = coordinator,
        };
        AppEffects effects;
        AppResult core_result = APP_INVALID;
        const StoreResult result = store_apply_event(&rt->store, &rt->state,
            &event, &now, !coordinator, &effects, &core_result);
        if (result == STORE_OK) {
            if (rt->mesh_started) katmed_mesh_notify_local_commit();
            return coordinator ? UI_SUBMIT_COMMITTED : UI_SUBMIT_PENDING;
        }
        if (result == STORE_STALE || core_result == APP_STALE)
            return UI_SUBMIT_STALE;
        if (result == STORE_DUPLICATE || core_result == APP_DUPLICATE)
            return UI_SUBMIT_REJECTED;
        return UI_SUBMIT_UNAVAILABLE;
    }
    /* A provisioned coordinator accepts its own durable settings even when
     * there is no authenticated peer. Followers still require that session. */
    if (rt->shared_pending || !rt->mesh_started ||
        (!katmed_mesh_is_coordinator() && !katmed_mesh_connected()))
        return UI_SUBMIT_UNAVAILABLE;
    AppEventKind kind;
    switch (request->kind) {
    case UI_REQUEST_SET_MEDICATION_SECONDS: kind = EVENT_SET_MEDICATION_INTERVAL; break;
    case UI_REQUEST_SET_IMAGE_SECONDS: kind = EVENT_SET_IMAGE_INTERVAL; break;
    case UI_REQUEST_SET_ROTATION_MODE: kind = EVENT_SET_ROTATION_MODE; break;
    case UI_REQUEST_SET_NEXT_LOCAL_TIME: kind = EVENT_SET_NEXT_LOCAL_TIME; break;
    default: return UI_SUBMIT_REJECTED;
    }
    EventId id;
    if (store_next_event_id(&rt->store, &id) != STORE_OK)
        return UI_SUBMIT_UNAVAILABLE;
    const ClockSample now = clock_sample(rt->boot_domain);
    AppEvent event = {
        .kind = kind,
        .id = id,
        .sender = id.device,
        .round = rt->state.round,
        .parent_revision = rt->state.revision,
        .occurred_at = now,
    };
    if (kind == EVENT_SET_MEDICATION_INTERVAL || kind == EVENT_SET_IMAGE_INTERVAL)
        event.value.interval_seconds = request->value.seconds;
    else if (kind == EVENT_SET_ROTATION_MODE)
        event.value.rotation = request->value.rotation;
    else {
        event.value.local_time.hour = request->value.local_time.hour;
        event.value.local_time.minute = request->value.local_time.minute;
        event.value.local_time.selected_utc = request->value.local_time.preview.regular_utc;
        event.value.local_time.selected_effective_utc =
            request->value.local_time.preview.effective_utc;
    }
    AppEffects effects;
    const KatmedMeshResult result = katmed_mesh_submit_shared(&event, &now, &effects);
    if (result == MESH_ACCEPTED) return UI_SUBMIT_COMMITTED;
    if (result == MESH_PENDING) {
        rt->shared_id = id;
        rt->shared_pending = true;
        return UI_SUBMIT_PENDING_SHARED;
    }
    if (result == MESH_REJECTED) return UI_SUBMIT_REJECTED;
    return UI_SUBMIT_UNAVAILABLE;
}

/* Read current SDK interfaces, never stored provisioning addresses. A cached
 * netif address is not usable unless the STA is associated and up now. */
static void network_status(void *context, UiNetworkStatus *out)
{
    Runtime *rt = context;
    if (!out) return;
    *out = (UiNetworkStatus){0};
    out->configured = rt && rt->store_ready &&
                      rt->store.record.provision.configured;
    if (!out->configured || !rt->mesh_started) return;
    out->mesh_active = katmed_mesh_radio_channel(&out->mesh_channel);
    KatmedMeshCounts mesh_counts = {0};
    katmed_mesh_counts(&mesh_counts);
    out->mesh_current_count = mesh_counts.current;
    out->mesh_history_count = mesh_counts.history;
    out->mesh_quality = mesh_counts.quality;
    out->mesh_current_valid = mesh_counts.current_valid;
    out->mesh_history_complete = mesh_counts.history_complete;
    esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {0};
    wifi_ap_record_t access_point;
    if (station && esp_netif_is_netif_up(station) &&
        esp_wifi_sta_get_ap_info(&access_point) == ESP_OK &&
        esp_netif_get_ip_info(station, &ip) == ESP_OK && ip.ip.addr != 0 &&
        esp_netif_is_netif_up(station) &&
        esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
        out->connected = true;
        snprintf(out->station_ip, sizeof(out->station_ip), IPSTR, IP2STR(&ip.ip));
        if (ip.gw.addr != 0)
            snprintf(out->gateway, sizeof(out->gateway), IPSTR, IP2STR(&ip.gw));
    }
    if (out->connected) {
        esp_netif_dns_info_t dns = {0};
        if (esp_netif_get_dns_info(station, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4 && dns.ip.u_addr.ip4.addr != 0)
            snprintf(out->dns_ip, sizeof(out->dns_ip), IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        esp_ip4_addr_t server = {.addr = katmed_time_idf_sync_server_ipv4()};
        if (server.addr != 0)
            snprintf(out->sync_server_ip, sizeof(out->sync_server_ip), IPSTR, IP2STR(&server));
        /* Suppress cached values if the station vanished during the snapshot. */
        if (!esp_netif_is_netif_up(station) || esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
            out->connected = false;
            out->station_ip[0] = out->gateway[0] = 0;
            out->dns_ip[0] = out->sync_server_ip[0] = 0;
        }
    }
    wifi_mode_t mode;
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (esp_wifi_get_mode(&mode) == ESP_OK &&
        (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA) && ap &&
        esp_netif_is_netif_up(ap) && esp_netif_get_ip_info(ap, &ip) == ESP_OK &&
        ip.ip.addr != 0)
        snprintf(out->ap_ip, sizeof(out->ap_ip), IPSTR, IP2STR(&ip.ip));
}

static bool group_connected(void *context)
{
    Runtime *rt = (Runtime *)context;
    return rt != NULL && rt->mesh_started && katmed_mesh_connected();
}

static bool historical_confirmation(void *context, ClockSample *historical)
{
    Runtime *rt = (Runtime *)context;
    StoreRecovery recovery;
    if (rt == NULL || historical == NULL ||
        store_get_recovery(&rt->store, &recovery) != STORE_OK ||
        !recovery.has_last_confirmation) return false;
    *historical = recovery.last_confirmation;
    return true;
}

static uint8_t pending_confirmations(void *context)
{
    Runtime *rt = (Runtime *)context;
    uint8_t pending = 0;
    if (rt != NULL) (void)store_get_status(&rt->store, NULL, &pending, NULL);
    return pending;
}

static bool storage_ready(void *context)
{
    Runtime *rt = (Runtime *)context;
    return rt != NULL && store_get_status(&rt->store, NULL, NULL, NULL) == STORE_OK;
}

static bool durable_divergence(void *context)
{
    Runtime *rt = (Runtime *)context;
    return rt != NULL && store_durable_divergent(&rt->store);
}

static uint8_t conflict_count(void *context)
{
    Runtime *rt = (Runtime *)context;
    uint8_t count = 0;
    if (rt != NULL) (void)store_conflict_count(&rt->store, &count);
    return count;
}

static bool conflict_at(void *context, size_t index, UiConflict *out)
{
    Runtime *rt = (Runtime *)context;
    StorePendingConfirm entry;
    if (rt == NULL || out == NULL ||
        store_conflict_at(&rt->store, index, &entry) != STORE_OK) return false;
    out->id = entry.id;
    out->round = entry.round;
    out->occurred_at = entry.occurred_at;
    return true;
}

static void stop_group_for_reboot(Runtime *rt, const ClockSample *now,
                                  bool new_group)
{
    katmed_mesh_stop();
    rt->mesh_started = false;
    (void)katmed_time_idf_set_root(&rt->time, false, false, NULL, 0);
    katmed_time_set_link(&rt->time, false);
    rt->utc_source = 0;
    rt->shared_pending = false;
    rt->shared_feedback = 0;
    rt->reboot_required = true;
    rt->reboot_due_ms = now->monotonic_ms + 1000u;
    if (new_group) {
        /* The old group's live alarm, revision and time must not be displayed
         * as a member of the newly provisioned trust domain. */
        katmed_time_init(&rt->time, rt->boot_domain);
        const ClockSample reset_clock = katmed_time_sample(&rt->time, now->monotonic_ms);
        (void)app_init(&rt->state, &rt->store.record.shared, &reset_clock,
                       rt->boot, rt->store.current_image_count,
                       rt->store.current_catalog);
        rt->state.divergent = store_durable_divergent(&rt->store);
    }
}

static void reboot_if_due(Runtime *rt, uint64_t monotonic_now_ms)
{
    if (rt->reboot_required && monotonic_now_ms >= rt->reboot_due_ms)
        esp_restart();
}

static bool witness_request(const KatmedUsbLine *line, const char *verb,
                            char nonce[33])
{
    const size_t prefix = strlen(verb);
    if (line->length != prefix + 32u + 1u ||
        memcmp(line->data, verb, prefix) != 0 || line->data[prefix + 32u] != '\n')
        return false;
    for (size_t i = 0; i < 32u; ++i) {
        const char c = line->data[prefix + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        nonce[i] = c;
    }
    nonce[32] = '\0';
    return true;
}

static void witness_catalog(const uint8_t bytes[KATMED_CATALOG_BYTES], char text[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < KATMED_CATALOG_BYTES; ++i) {
        text[2u * i] = hex[bytes[i] >> 4];
        text[2u * i + 1u] = hex[bytes[i] & 15u];
    }
    text[64] = '\0';
}

static void process_usb(Runtime *rt, const ClockSample *now)
{
    static const uint8_t build_wifi_ssid[] = KATMED_WIFI_DEFAULT_SSID;
    static const uint8_t build_wifi_password[] = KATMED_WIFI_DEFAULT_PASSWORD;
    KatmedUsbLine line;
    unsigned processed = 0;
    while (processed++ < 3 && katmed_usb_console_receive(&line)) {
#if defined(ESP_PLATFORM) || defined(KATMED_PHASE_HOST_TEST)
        const bool is_status = katmed_usb_console_is_status(&line);
        if (is_status) katmed_usb_console_note_main(KATMED_USB_MAIN_TAKE);
#endif
        if (line.overlong) {
            rt->witness_armed = false;
            katmed_usb_console_reply("KM1 ERR LENGTH");
            memset(&line, 0, sizeof(line));
            continue;
        }
        /* The full catalogue ID is reported; the ELF-hash prefix is a compact
         * build token within the 256-byte status line. */
        char build_id[25];
        (void)esp_app_get_elf_sha256(build_id, sizeof(build_id));
        StoreUsbStatusInfo status = {
            .firmware_version = esp_app_get_description()->version,
            .build_id = build_id,
            .device = rt->boot.device,
            .clock_quality = now->quality,
        };
        memcpy(status.catalog_id, katmed_images_catalog_sha256(),
               sizeof(status.catalog_id));
        char reply[KATMED_STORE_USB_REPLY_MAX] = {0};
        char nonce[33];
        if (witness_request(&line, "KM1 ARM n=", nonce)) {
            StoreResult error = STORE_UNAVAILABLE;
            if (!rt->reboot_required &&
                store_get_status(&rt->store, NULL, NULL, &error) == STORE_OK &&
                error == STORE_OK) {
                memcpy(rt->witness_nonce, nonce, sizeof(nonce));
                rt->witness_armed = true;
                (void)snprintf(reply, sizeof(reply), "KM1 OK ARM n=%s\n", nonce);
            } else {
                rt->witness_armed = false;
                memcpy(reply, "KM1 ERR UNAVAILABLE\n", sizeof("KM1 ERR UNAVAILABLE\n"));
            }
            katmed_usb_console_reply(reply);
            memset(&line, 0, sizeof(line));
            continue;
        }
        const bool proved = line.length >= sizeof("KM1 PROVEBOOT ") - 1u &&
            memcmp(line.data, "KM1 PROVEBOOT ", sizeof("KM1 PROVEBOOT ") - 1u) == 0;
        if (proved && !rt->witness_armed) {
            katmed_usb_console_reply("KM1 ERR COMMAND");
            memset(&line, 0, sizeof(line));
            continue;
        }
        if (!proved) rt->witness_armed = false;
        if (witness_request(&line, "KM1 BOOT n=", nonce)) {
            bool configured = false;
            StoreResult error = STORE_UNAVAILABLE;
            const StoreResult durable = store_get_status(&rt->store, &configured, NULL, &error);
            char catalog[65];
            witness_catalog(status.catalog_id, catalog);
            const int size = snprintf(reply, sizeof(reply),
                "KM1 OK BOOT n=%s c=%u d=%u dev=%012" PRIx64
                " cat=%s fw=%s b=%s e=%" PRIu64 " x=%" PRIu64 " u=%" PRIu64 "\n",
                nonce, configured ? 1u : 0u,
                durable == STORE_OK && error == STORE_OK ? 1u : 0u,
                status.device, catalog, status.firmware_version, status.build_id,
                rt->boot.epoch, rt->boot_domain, monotonic_ms());
            katmed_usb_console_reply(size < 0 || (size_t)size >= sizeof(reply) ?
                "KM1 ERR OUTPUT" : reply);
            memset(&line, 0, sizeof(line));
            continue;
        }
        const StoreProvision previous = rt->store.record.provision;
        const bool reset_requested = line.length == sizeof("KM1 RESET CONFIG\n") - 1 &&
            memcmp(line.data, "KM1 RESET CONFIG\n", sizeof("KM1 RESET CONFIG\n") - 1) == 0;
        if (proved) {
            /* PROVEBOOT and PROVISION are the same length: reuse the Store
             * validator and commit, never a second secret parser. */
            memcpy(nonce, rt->witness_nonce, sizeof(nonce));
            rt->witness_armed = false;
            memcpy(line.data + 4, "PROVISION", sizeof("PROVISION") - 1u);
        }
        const StoreResult result = store_usb_handle_line_with_wifi_defaults(
            &rt->store, line.data, line.length, &status,
            build_wifi_ssid, sizeof(build_wifi_ssid) - 1u,
            build_wifi_password, sizeof(build_wifi_password) - 1u,
            reply, sizeof(reply));
        if (proved && result == STORE_OK) {
            char catalog[65];
            witness_catalog(status.catalog_id, catalog);
            const int size = snprintf(reply, sizeof(reply),
                "KM1 OK PROVEBOOT n=%s dev=%012" PRIx64
                " cat=%s fw=%s b=%s e=%" PRIu64 " x=%" PRIu64 " u=%" PRIu64 "\n",
                nonce, status.device, catalog, status.firmware_version, status.build_id,
                rt->boot.epoch, rt->boot_domain, monotonic_ms());
            if (size < 0 || (size_t)size >= sizeof(reply))
                memcpy(reply, "KM1 ERR OUTPUT\n", sizeof("KM1 ERR OUTPUT\n"));
        }
        if (result == STORE_OK && (reset_requested ||
            memcmp(&previous, &rt->store.record.provision, sizeof(previous)) != 0)) {
            const bool new_group = reset_requested ||
                previous.configured != rt->store.record.provision.configured ||
                memcmp(previous.group, rt->store.record.provision.group,
                       sizeof(previous.group)) != 0 ||
                memcmp(previous.secret, rt->store.record.provision.secret,
                       sizeof(previous.secret)) != 0;
            stop_group_for_reboot(rt, now, new_group);
        }
        const char *wire_reply = reply[0] != 0 ? reply :
            (result == STORE_OK ? "KM1 OK" : "KM1 ERR STORE");
#if defined(ESP_PLATFORM) || defined(KATMED_PHASE_HOST_TEST)
        if (is_status) katmed_usb_console_note_main(KATMED_USB_REPLY_READY);
#endif
        katmed_usb_console_reply(wire_reply);
#if defined(ESP_PLATFORM) || defined(KATMED_PHASE_HOST_TEST)
        if (is_status) katmed_usb_console_note_main(KATMED_USB_REPLY_CALL);
#endif
        memset(&line, 0, sizeof(line));
    }
}

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000u;
}

static ClockSample clock_sample(uint64_t domain)
{
    if (runtime.time.domain == domain)
        return katmed_time_sample(&runtime.time, monotonic_ms());
    ClockSample clock = {
        .monotonic_ms = monotonic_ms(),
        .domain = domain,
        .utc_ms = 0,
        .utc_valid = false,
        .quality = CLOCK_UNAVAILABLE,
    };
    return clock;
}

static bool boot_identity(EventId *identity)
{
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) return false;
    uint64_t device = 0;
    for (unsigned i = 0; i < sizeof(mac); ++i) device = (device << 8) | mac[i];
    if (device == 0) return false;
    uint64_t epoch = ((uint64_t)esp_random() << 32) | esp_random();
    if (epoch == 0) epoch = 1;
    *identity = (EventId){.device = device, .epoch = epoch, .sequence = 1};
    return true;
}

static bool same_alarm_round(AlarmRoundId a, AlarmRoundId b)
{
    return a.origin.device == b.origin.device &&
           a.origin.epoch == b.origin.epoch &&
           a.origin.sequence == b.origin.sequence &&
           a.ordinal == b.ordinal;
}

static bool render_ui(const AppState *state, const LocalSettings *local,
                      const ClockSample *clock, const BoardTouch *touch,
                      bool touch_valid, bool render_due)
{
    /* A missed sample can hide either an I/O error or a swipe excursion.
     * The next successful lock must cancel the whole contact before it may
     * process another sample, including a valid-looking release. */
    static bool contact_interrupted;
    static bool projected_state_available;
    static bool projected_alarm_active;
    static AlarmRoundId projected_round;
    if (!lvgl_port_lock(100)) {
#if defined(ESP_PLATFORM) || defined(KATMED_HOST_TOUCH_SAMPLER)
        if (!board_touch_sampler_running()) contact_interrupted = true;
#else
        contact_interrupted = true;
#endif
        return false;
    }
#ifdef ESP_PLATFORM
    if (runtime.mesh_started)
        katmed_mesh_interaction_status(&interaction_snapshot, clock->monotonic_ms);
#endif
    if (contact_interrupted) {
        ui_touch_sample(false, 0, 0, false, clock->monotonic_ms);
        contact_interrupted = false;
    }
    /* A new alarm or round must reach the UI before a release is evaluated.
     * This holds even on the 25 ms tick between scheduled redraws. */
    const bool alarm_changed = state != NULL && local != NULL &&
        (!projected_state_available ||
        projected_alarm_active != state->alarm_active ||
        (state->alarm_active && !same_alarm_round(projected_round, state->round)));
    if (alarm_changed) {
        ui_render(state, local, ui_current_view(), clock);
        projected_alarm_active = state->alarm_active;
        projected_round = state->round;
        projected_state_available = true;
    }
    if (touch != NULL) {
        ui_touch_sample(touch->down, touch->x, touch->y,
                        touch_valid, touch->monotonic_ms);
    }
#if defined(ESP_PLATFORM) || defined(KATMED_HOST_TOUCH_SAMPLER)
    if (board_touch_sampler_running()) {
        BoardTouch queued;
        BoardResult result;
        uint64_t round_floor = 0;
        if (state && state->alarm_active) {
            if (state->start_reason == START_BOOT &&
                state->countdown_started.domain == clock->domain)
                round_floor = state->countdown_started.monotonic_ms;
            else if (state->start_reason != START_BOOT)
                round_floor = state->deadline.effective_monotonic_ms;
        }
        while (board_touch_sampler_take(&queued, &result))
            ui_touch_sample(queued.down, queued.x, queued.y,
                            result == BOARD_OK && queued.monotonic_ms >= round_floor,
                            queued.monotonic_ms);
    }
#endif
    if (render_due && state != NULL && local != NULL)
        ui_render(state, local, ui_current_view(), clock);
    lvgl_port_unlock();
    return true;
}
#endif

void app_main(void)
{
#if KATMED_FULL_STACK && KATMED_WITH_CORE
    static const char *tag = "katmed";
    ESP_LOGI(tag, "Board/UI runtime with durable store and bounded group services");
    ESP_LOGI(tag, "free heap at entry: %" PRIu32, esp_get_free_heap_size());

    if (!boot_identity(&runtime.boot)) {
        ESP_LOGE(tag, "read-only device identity unavailable");
        return;
    }

    lv_display_t *display = NULL;
    const BoardResult board_status = board_init(&display);
    if (board_status != BOARD_OK || display == NULL) {
        ESP_LOGE(tag, "board initialization failed: %d", board_status);
        board_deinit();
        return;
    }

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(tag, "LVGL lock unavailable during startup");
        board_deinit();
        return;
    }
    katmed_images_register_lvgl_fs();
    lvgl_port_unlock();
    const KatmedImageStatus image_status = katmed_images_mount_partition();
    ESP_LOGI(tag, "read-only image partition status=%d", image_status);

    SharedSettings settings = app_default_settings();
    const uint32_t image_count = image_status == KATMED_IMAGE_OK ? katmed_images_count() : 0;
    runtime.local = (LocalSettings){
        .orientation = ORIENTATION_0,
        .clock_style = DIGITAL_LARGE,
        .device = runtime.boot.device,
    };
    StoreBackend backend = {0};
    StoreRecovery recovery = {0};
    const StoreResult backend_result = store_idf_open_backend(&runtime.nvs, &backend);
    StoreResult open_result = backend_result;
    if (backend_result == STORE_OK) {
        open_result = store_open(&runtime.store, backend, runtime.boot.device,
            katmed_images_catalog_sha256(), image_count, &recovery);
        if (open_result == STORE_OK) {
            runtime.store_ready = true;
            runtime.boot = recovery.boot_identity;
            settings = recovery.shared;
            runtime.local = recovery.local;
        }
    } else {
        runtime.store.last_error = backend_result;
    }
    if (!runtime.store_ready) {
        ESP_LOGE(tag, "durable store unavailable: %d; boot alarm remains active", open_result);
    }
    runtime.boot_domain = new_boot_domain(runtime.boot);
    katmed_time_init(&runtime.time, runtime.boot_domain);
    ClockSample now = clock_sample(runtime.boot_domain);
    if (app_init(&runtime.state, &settings, &now, runtime.boot, image_count,
                 katmed_images_catalog_sha256()) != APP_OK) {
        ESP_LOGE(tag, "initial state unavailable");
        store_idf_close_backend(&runtime.nvs);
        katmed_images_unmount();
        board_deinit();
        return;
    }
    runtime.state.divergent = store_durable_divergent(&runtime.store);
    if (board_set_orientation(runtime.local.orientation) != BOARD_OK) {
        ESP_LOGE(tag, "initial orientation failed");
        store_idf_close_backend(&runtime.nvs);
        katmed_images_unmount();
        board_deinit();
        return;
    }
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(tag, "LVGL lock unavailable for UI creation");
        store_idf_close_backend(&runtime.nvs);
        katmed_images_unmount();
        board_deinit();
        return;
    }
    const UiServices services = {
        .context = &runtime,
        .submit = submit_ui,
        .interaction_input = interaction_submit,
        .interaction_status = interaction_status,
        .connected = group_connected,
        .network_status = network_status,
        .last_confirmation = historical_confirmation,
        .pending_count = pending_confirmations,
        .storage_ready = storage_ready,
        .durable_divergence = durable_divergence,
        .conflict_count = conflict_count,
        .conflict_at = conflict_at,
    };
    ui_set_services(&services);
    board_set_touch_filter(ui_lvgl_touch_allowed);
    ui_render(&runtime.state, &runtime.local, ui_current_view(), &now);
    lvgl_port_unlock();

    if (board_touch_sampler_start() != BOARD_OK) {
        ESP_LOGE(tag, "touch sampler unavailable");
        store_idf_close_backend(&runtime.nvs);
        katmed_images_unmount();
        board_deinit();
        return;
    }

#if defined(ESP_PLATFORM) || defined(KATMED_PHASE_HOST_TEST)
    katmed_usb_console_phase_init(runtime.boot_domain, monotonic_ms,
                                  katmed_runtime_usb_phase_log);
#endif
    if (!katmed_usb_console_start())
        ESP_LOGE(tag, "USB setup input unavailable");

    if (runtime.store_ready && runtime.store.record.provision.configured) {
        katmed_mesh_history_backend(store_idf_history_backend(&runtime.nvs));
        const KatmedMeshServices mesh_services = {
            .store = &runtime.store,
            .state = &runtime.state,
            .context = &runtime,
            .shared_result = mesh_shared_result,
            .utc_received = mesh_utc_received,
        };
        const KatmedMeshResult mesh_result = katmed_mesh_start(
            &runtime.store.record.provision, &mesh_services);
        runtime.mesh_started = mesh_result == MESH_ACCEPTED || mesh_result == MESH_PENDING;
        if (!runtime.mesh_started) ESP_LOGE(tag, "group service unavailable: %d", mesh_result);
    }

    uint64_t last_render_ms = now.monotonic_ms;
    KatmedRuntimeMetricsGate metrics_gate = {0};
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        reboot_if_due(&runtime, monotonic_ms());
        now = clock_sample(runtime.boot_domain);
        if (runtime.mesh_started && !runtime.reboot_required)
            katmed_mesh_poll(&now);
        const bool root_ip = runtime.mesh_started && katmed_mesh_root_has_ip();
        katmed_time_set_link(&runtime.time, root_ip ||
                             (runtime.mesh_started && katmed_mesh_connected() &&
                              !katmed_mesh_is_coordinator()));
        if (runtime.store_ready && runtime.store.record.provision.configured &&
            !runtime.reboot_required) {
            const StoreProvision *provision = &runtime.store.record.provision;
            const esp_err_t time_result = katmed_time_idf_set_root(&runtime.time,
                runtime.mesh_started && katmed_mesh_is_coordinator(), root_ip,
                provision->time_source, provision->time_source_length);
            if (time_result == ESP_OK) {
                const KatmedTime previous = runtime.time;
                ClockSample new_sync;
                if (katmed_time_idf_poll(&runtime.time, &new_sync)) {
                    if (!apply_time_sample(&runtime, &new_sync))
                        runtime.time = previous;
                    else runtime.utc_source=1;
                }
            }
        }
        now = clock_sample(runtime.boot_domain);
        static AppState candidate;
        candidate = runtime.state;
        AppEffects effects;
        const AppResult tick_status = app_tick(&candidate, &now, &effects);
        if (tick_status == APP_OK && !effects.persistence_required) {
            /* The reducer owns countdown and image changes. Group broadcast
             * remains unavailable until the authenticated Mesh adapter. */
            runtime.state = candidate;
        }
        process_usb(&runtime, &now);
        now = clock_sample(runtime.boot_domain);
        if (runtime.mesh_started && !runtime.reboot_required)
            interaction_drain(now.monotonic_ms);
        const bool render_due = now.monotonic_ms - last_render_ms >= 50;
        if (runtime.shared_feedback != 0 && lvgl_port_lock(100)) {
            ui_shared_result(runtime.shared_feedback > 0);
            runtime.shared_feedback = 0;
            lvgl_port_unlock();
        }
        if (render_ui(&runtime.state, &runtime.local, &now, NULL,
                      false, render_due) && render_due) {
            last_render_ms = now.monotonic_ms;
        }
        if (katmed_runtime_metrics_due(&metrics_gate, now.monotonic_ms)) {
            KatmedRuntimeMetricInput metrics;
            katmed_runtime_metrics_collect(now.monotonic_ms, &metrics);
            katmed_runtime_metrics_log(&metrics);
            if (runtime.evidence_sequence != UINT64_MAX) {
                const uint64_t sequence=++runtime.evidence_sequence;
                KatmedRuntimeEvidenceState state=evidence_state(&runtime,&now,sequence);
                KatmedRuntimeEvidenceHealth health={
                    .seq=sequence,.uptime_ms=now.monotonic_ms,
                    .boot_domain=runtime.boot_domain,
                    .image_valid=metrics.image_failure_valid,
                    .image_failures=metrics.image_failed_presentations,
                };
#ifdef ESP_PLATFORM
                StoreDiagnostics store_diag;
                if(store_diagnostics(&runtime.store,&store_diag)) {
                    health.store_valid=1;
                    health.store_read_failures=store_diag.backend_read_failures;
                    health.store_load_failures=store_diag.decoded_load_failures;
                    health.store_commit_failures=store_diag.backend_commit_failures;
                }
                KatmedMeshDiagnostics mesh_diag={0};
                if(katmed_mesh_diagnostics(&mesh_diag)) {
                    health.mesh_valid=1;
                    health.mesh_send_failures=mesh_diag.sdk_send_failures;
                    health.mesh_queue_drops=mesh_diag.rx_queue_drops;
                    health.mesh_level_changes=mesh_diag.observed_level_changes;
                    health.mesh_ip_got=mesh_diag.ip_got_transitions;
                    health.mesh_ip_lost=mesh_diag.ip_lost_transitions;
                    health.mesh_session_up=mesh_diag.authenticated_session_up;
                    health.mesh_session_down=mesh_diag.authenticated_session_down;
                    health.mesh_connected=katmed_mesh_connected();
                    health.mesh_root=katmed_mesh_is_coordinator();
                    health.mesh_root_ip=katmed_mesh_root_has_ip();
                }
                ESP_LOGI("katmed_net",
                    "schema=1 sample_seq=%" PRIu64 " uptime_ms=%" PRIu64
                    " boot_domain=%" PRIu64 " sta_v=%" PRIu32 " sta_up=%" PRIu32
                    " sta_ip=%" PRIu32 " sta_dhcp=%" PRIu32,
                    sequence,now.monotonic_ms,runtime.boot_domain,
                    mesh_diag.station_valid,mesh_diag.station_up,
                    mesh_diag.station_ipv4_nonzero,mesh_diag.station_dhcp_status);
                KatmedDhcpDiagnostics dhcp_diag;
                katmed_dhcp_diag_snapshot(&dhcp_diag);
                ESP_LOGI("katmed_dhcp",
                    "schema=1 sample_seq=%" PRIu64 " uptime_ms=%" PRIu64
                    " boot_domain=%" PRIu64 " core_v=%" PRIu32 " dhcp_v=%" PRIu32
                    " link_up=%" PRIu32 " state=%" PRIu32 " tries=%" PRIu32
                    " timeout=%" PRIu32 " fine=%" PRIu32
                    " udp_discover=%" PRIu32 " udp_request=%" PRIu32
                    " udp_other=%" PRIu32 " udp_invalid=%" PRIu32
                    " udp_ok=%" PRIu32 " udp_fail=%" PRIu32 " udp_error=%" PRId32
                    " tx_discover=%" PRIu32 " tx_request=%" PRIu32
                    " tx_other=%" PRIu32 " tx_invalid=%" PRIu32
                    " tx_ok=%" PRIu32 " tx_fail=%" PRIu32 " tx_error=%" PRId32,
                    sequence,now.monotonic_ms,runtime.boot_domain,
                    dhcp_diag.core_v,dhcp_diag.dhcp_v,dhcp_diag.link_up,
                    dhcp_diag.state,dhcp_diag.tries,dhcp_diag.timeout,dhcp_diag.fine,
                    dhcp_diag.udp_discover,dhcp_diag.udp_request,dhcp_diag.udp_other,
                    dhcp_diag.udp_invalid,dhcp_diag.udp_ok,dhcp_diag.udp_fail,dhcp_diag.udp_error,
                    dhcp_diag.tx_discover,dhcp_diag.tx_request,dhcp_diag.tx_other,
                    dhcp_diag.tx_invalid,dhcp_diag.tx_ok,dhcp_diag.tx_fail,dhcp_diag.tx_error);
                ESP_LOGI("katmed_txres",
                    "schema=1 sample_seq=%" PRIu64 " uptime_ms=%" PRIu64
                    " boot_domain=%" PRIu64 " task_v=%" PRIu32 " v=%" PRIu32
                    " count=%" PRIu32 " dhcp_type=%" PRIu32 " calls=%" PRIu32
                    " ok=%" PRIu32 " nulls=%" PRIu32 " resource_v=%" PRIu32
                    " size=%" PRIu32 " caps=%" PRIu32 " dma_free=%" PRIu32
                    " dma_largest=%" PRIu32 " result=%" PRId32,
                    sequence,now.monotonic_ms,runtime.boot_domain,
                    dhcp_diag.task_v,dhcp_diag.resource_v,dhcp_diag.resource_count,
                    dhcp_diag.resource_dhcp_type,dhcp_diag.resource_calls,
                    dhcp_diag.resource_ok,dhcp_diag.resource_nulls,dhcp_diag.resource_resource_v,
                    dhcp_diag.resource_size,dhcp_diag.resource_caps,
                    dhcp_diag.resource_dma_free,dhcp_diag.resource_dma_largest,
                    dhcp_diag.resource_result);
#endif
                katmed_runtime_evidence_log(&state,&health);
            }
        }
    }
#else
    ESP_LOGI("katmed", "Minimal build: Board/UI runtime disabled");
#endif
}
