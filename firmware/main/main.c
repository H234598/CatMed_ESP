/* Board, durable local runtime, authenticated group and real time source. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "esp_event.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "runtime_metrics.h"
#include "boot_restart.h"

#if KATMED_FULL_STACK && KATMED_WITH_CORE
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "esp_mesh_lite.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "c3_resource_policy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "katmed_board.h"
#include "katmed_core.h"
#include "katmed_config_bridge.h"
#include "katmed_web.h"
#include "katmed_images.h"
#include "katmed_image_store.h"
#include "katmed_mesh.h"
#include "katmed_mesh_names.h"
#include "katmed_dhcp_diag.h"
#include "katmed_store.h"
#include "katmed_store_idf.h"
#include "katmed_names_idf.h"
#include "katmed_nvs_init.h"
#include "katmed_source_export.h"
#include "katmed_import.h"
#include "katmed_i18n.h"
#include "katmed_reset_idf.h"
#include "katmed_acme.h"
#include <private_wifi_defaults.h>
#include "katmed_time.h"
#include "katmed_time_idf.h"
#include "katmed_ui.h"
#include "usb_console.h"
#include "display_ui_mailbox.h"
#if KATMED_APPSTORE_MAINTENANCE
#include "esp_partition.h"
#include "appmaint_target_match.h"
#if !CONFIG_NVS_ENCRYPTION || !CONFIG_IDF_TARGET_ESP32C6
#error "AppStore maintenance requires encrypted C6 security profile"
#endif
#endif

/* A transport integration can provide a bounded, private sink. A NULL sink
 * leaves the normal boot untouched; no snapshot is ever emitted implicitly. */
__attribute__((weak)) const KatmedSourceExportSink *katmed_source_snapshot_boot_sink(void)
{
    return NULL;
}

static bool source_snapshot_writers_idle(void)
{
    return katmed_image_store_quiescent() && katmed_acme_quiescent();
}

static ClockSample clock_sample(uint64_t domain);
static uint64_t monotonic_ms(void);

typedef struct {
    Store store;
    StoreIdfNvs nvs;
    NamesIdf names_nvs;
    AppState state;
    LocalSettings local;
    KatmedDisplayPolicy display_policy;
    bool backlight_applied;
    uint8_t brightness_applied;
    uint8_t brightness_preview; /* app-loop owned; zero means no transient override */
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
    int8_t shared_feedback; /* 0 none, 1 accepted, -1 rejected, -2 timeout. */
    bool store_ready;
    uint8_t utc_source; /* 0 unknown,1 SNTP,2 Mesh,3 manual unsynchronised. */
    bool setup_ap;
    char setup_ssid[33], setup_password[17];
    uint32_t emergency_button_sequence;
    uint64_t evidence_sequence;
    char witness_nonce[33]; /* One request, one boot, never persisted. */
    bool witness_armed;
    bool manual_restore_attempted;
    bool manual_restore_waiting;
    bool manual_restore_rejected;
    bool manual_restore_retry;
    EventId manual_restore_event_id;
    KatmedRestoreStatus manual_restore_status;
    KatmedResetConfirmation reset_confirmation;
    bool reset_prepared;
    bool reset_warning_ready;
    bool reset_web_offer;
    uint64_t reset_web_offer_epoch;
    bool reset_web_rejected;
    bool reset_web_close;
    uint64_t reset_generation;
    uint64_t reset_web_close_generation;
    uint64_t reset_prepared_at_ms;
    bool reset_requested;
    bool reset_active;
    uint64_t reset_retry_due_ms;
} Runtime;

static Runtime runtime;
static RTC_DATA_ATTR uint32_t manual_clock_marker;
#define MANUAL_CLOCK_MARKER UINT32_C(0x4b4d4331)

static StaticSemaphore_t reset_mutex_storage;
static SemaphoreHandle_t reset_mutex;
static KatmedResetJournalBackend reset_backend(void)
{
    return katmed_reset_idf_backend();
}

static bool reset_random_bytes(void *context,uint8_t *out,size_t length)
{
    (void)context;
    if (!out) return false;
    esp_fill_random(out,length);
    return true;
}

static bool reset_reason_allows_warning(esp_reset_reason_t reason)
{
    return reason==ESP_RST_EXT || reason==ESP_RST_POWERON;
}

static StoreResult setup_exit_after_physical_boot(Store *store,
    bool store_ready, bool reset_warning, esp_reset_reason_t reason)
{
    if (!store_ready || reset_warning || !store->record.setup_requested ||
        !reset_reason_allows_warning(reason))
        return STORE_OK;
    return store_exit_setup(store);
}

static bool katmed_runtime_reset_arm(void *context,bool from_web)
{
    Runtime *rt=context;
    if (!rt || !reset_mutex || xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(1000))!=pdTRUE)
        return false;
    const bool ok=!rt->reset_active && !rt->reset_requested &&
        !rt->reset_warning_ready && !rt->reset_prepared &&
#if KATMED_APPSTORE_MAINTENANCE
        !from_web &&
        katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_ABSENT &&
        katmed_reset_idf_maintenance_preflight() &&
#endif
        (from_web ? rt->reset_web_offer : !rt->reset_web_offer) &&
        katmed_reset_idf_prepare();
    if (ok) {
        ++rt->reset_generation;
        rt->reset_prepared=true;
        rt->reset_prepared_at_ms=monotonic_ms();
        rt->reset_web_offer=false;
        rt->reset_web_rejected=false;
    }
    xSemaphoreGive(reset_mutex);
    return ok;
}

static bool reset_cancel_locked(Runtime *rt)
{
    const bool ok=!rt->reset_requested && !rt->reset_active &&
        katmed_reset_idf_cancel_preparation();
    if (ok) {
        rt->reset_prepared=false;
        rt->reset_warning_ready=false;
        rt->reset_web_offer=false;
        rt->reset_web_rejected=false;
        rt->reset_confirmation.valid=false;
        memset(&rt->reset_confirmation,0,sizeof(rt->reset_confirmation));
    }
    return ok;
}

static bool boot_pin_high(void *context)
{
    (void)context;
    return board_boot_button_high();
}

static bool boot_try_reset_lock(void *context)
{
    (void)context;
    return xSemaphoreTake(reset_mutex,0)==pdTRUE;
}

static void boot_unlock_reset(void *context)
{
    (void)context;
    xSemaphoreGive(reset_mutex);
}

static bool boot_reset_busy(void *context)
{
    const Runtime *rt=context;
    return rt->reset_active || rt->reset_requested;
}

static bool boot_reset_prepared(void *context)
{
    return ((const Runtime *)context)->reset_prepared;
}

static bool boot_cancel_preparation(void *context)
{
    Runtime *rt=context;
    if (!reset_cancel_locked(rt)) return false;
    /* If GPIO9 goes Low again after the durable cancellation, BOOT will not
     * restart. Close the now-stale instruction tile on the normal UI path. */
    rt->reset_web_close_generation=rt->reset_generation;
    rt->reset_web_close=true;
    return true;
}

static uint64_t boot_now_ms(void *context)
{
    (void)context;
    return monotonic_ms();
}

static void boot_restart_now(void *context)
{
    (void)context;
    esp_restart();
}

static bool katmed_runtime_reset_cancel(void *context)
{
    Runtime *rt=context;
    if (!rt || !reset_mutex || xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(1000))!=pdTRUE)
        return false;
    const bool ok=reset_cancel_locked(rt);
    xSemaphoreGive(reset_mutex);
    return ok;
}

/* Only the display calls this after a consumed one-boot preparation. */
bool katmed_runtime_reset_prepare(void *context,char *out,size_t capacity,size_t *length)
{
    Runtime *rt=context;
    if (!rt || !reset_mutex || xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(1000))!=pdTRUE)
        return false;
    const bool ok=rt->reset_warning_ready && !rt->reset_active && !rt->reset_requested &&
#if KATMED_APPSTORE_MAINTENANCE
        katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_ABSENT &&
#endif
        katmed_reset_confirmation_prepare(&rt->reset_confirmation,reset_random_bytes,NULL,
            monotonic_ms(),90000u,out,capacity,length);
    xSemaphoreGive(reset_mutex);
    return ok;
}

/* Result values intentionally match KatmedWebResetResult: 0 accepted,
 * 1 stale/consumed, 2 failed. This callback queues only RAM state. */
int katmed_runtime_reset_confirm(void *context,const char *token,size_t length)
{
    Runtime *rt=context;
    if (!rt || !reset_mutex || xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(1000))!=pdTRUE)
        return 2;
    const bool valid=rt->reset_warning_ready &&
#if KATMED_APPSTORE_MAINTENANCE
        katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_ABSENT &&
#endif
        katmed_reset_confirmation_consume(&rt->reset_confirmation,
        token,length,monotonic_ms());
    if (valid) {
        rt->reset_warning_ready=false;
        rt->reset_requested=true;
    }
    xSemaphoreGive(reset_mutex);
    return valid ? 0 : 1;
}

static bool web_prepare_listener_stop(void *context)
{
    (void)context;
    return katmed_acme_prepare_listener_stop();
}

static bool reset_runtime_step(Runtime *rt,uint64_t now_ms)
{
    if (!rt || !reset_mutex) return false;
    if (xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(100))!=pdTRUE) return rt->reset_active;
    const bool requested=rt->reset_requested;
    const bool active=rt->reset_active;
    xSemaphoreGive(reset_mutex);
    if (requested && !active) {
        /* A persisted intent resumes before Web on the next boot. Stop the
         * HTTPS listener and its borrowed PSA key before writing that intent. */
        if (!web_prepare_listener_stop(rt) || !katmed_web_stop()) return true;
        KatmedResetJournalBackend backend=reset_backend();
        if (!katmed_reset_begin(backend)) {
            ESP_LOGE("katmed_reset","could not commit reset intent; user data is unchanged");
            return true;
        }
        if (xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(100))!=pdTRUE) return true;
        rt->reset_requested=false;
        rt->reset_active=true;
        xSemaphoreGive(reset_mutex);
        katmed_mesh_stop();
        names_idf_close(&rt->names_nvs);
        store_idf_close_backend(&rt->nvs);
        rt->store_ready=false;
        rt->reset_retry_due_ms=0;
    }
    if (!rt->reset_active) return false;
    if (now_ms<rt->reset_retry_due_ms) return true;
    bool pending=true;
    if (katmed_reset_resume(reset_backend(),&pending) && !pending) {
        ESP_LOGI("katmed_reset","journaled user-data reset complete; restarting");
        esp_restart();
    }
    rt->reset_retry_due_ms=now_ms+5000u;
    ESP_LOGE("katmed_reset","reset stage remains pending; recovery will retry");
    return true;
}

static bool resume_reset_at_startup(void)
{
    KatmedResetJournalBackend backend=reset_backend();
    KatmedResetJournal journal={0};
    if (!backend.load(backend.context,&journal)) return false;
    if (!journal.pending) return true;
    ESP_LOGW("katmed_reset","resuming durable reset journal before normal startup");
    for (;;) {
        if (katmed_image_store_mount()!=KATMED_IMAGE_OK) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        bool pending=true;
        if (katmed_reset_resume(backend,&pending) && !pending) {
            esp_restart();
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
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
    if (!out->alarm_active) interaction_queue.quality_pending = false;
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

static void tick_runtime(Runtime *rt, const ClockSample *now)
{
    static AppState candidate;
    candidate = rt->state;
    AppEffects effects;
    const AppResult status = app_tick(&candidate, now, &effects);
    if ((status != APP_OK && status != APP_NO_CHANGE) || effects.persistence_required) return;
    if (status == APP_OK) rt->state = candidate;
    /* RANDOM is selected by the bound coordinator, or the explicit Offline
     * local authority. Both use the same durable, revisioned Store event. */
    const bool offline_local = rt->store.record.provision.configured &&
        rt->store.record.provision.network_mode == STORE_NETWORK_OFFLINE &&
        !rt->mesh_started && rt->state.revision.coordinator == rt->boot.device;
    if (!rt->state.image_due_pending || rt->state.alarm_active ||
        rt->state.settings.rotation != ROTATION_RANDOM ||
        rt->state.catalog_mismatch || rt->state.image_count == 0 ||
        rt->state.revision.number == UINT64_MAX || !rt->store_ready ||
        rt->reboot_required || rt->shared_pending ||
        !rt->store.record.provision.configured ||
        store_get_status(&rt->store, NULL, NULL, NULL) != STORE_OK ||
        (!offline_local && (!rt->mesh_started || !katmed_mesh_is_coordinator()))) return;
    uint32_t image = 0;
    if (rt->state.image_count > 1) {
        image = esp_random() % (rt->state.image_count - 1);
        if (image >= rt->state.image_id) ++image;
    }
    EventId id;
    if (store_next_event_id(&rt->store, &id) != STORE_OK) return;
    AppEvent event = {
        .kind = EVENT_SET_IMAGE,
        .id = id,
        .sender = id.device,
        .round = rt->state.round,
        .parent_revision = rt->state.revision,
        .occurred_at = *now,
        .coordinator_accepted = true,
        .value.image_id = image,
    };
    AppEffects committed_effects;
    AppResult core_result;
    if (store_apply_event(&rt->store, &rt->state, &event, now, false,
                         &committed_effects, &core_result) == STORE_OK && rt->mesh_started)
        katmed_mesh_notify_local_commit();
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

static bool time_shareable(void *context) {
    const Runtime *rt=context;return rt && (rt->utc_source==1 || rt->utc_source==2);
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
    else {rt->utc_source=2;manual_clock_marker=0;}
}

static void mesh_shared_result(void *context, EventId id, KatmedMeshSharedOutcome outcome)
{
    Runtime *rt = (Runtime *)context;
    if (!rt || !rt->shared_pending || !same_id(id, rt->shared_id)) return;
    rt->shared_pending = false;
    rt->shared_feedback = outcome==MESH_SHARED_ACCEPTED ? 1 :
        outcome==MESH_SHARED_REJECTED ? -1 : -2;
    if (rt->manual_restore_waiting) {
        rt->manual_restore_waiting = false;
        if (outcome==MESH_SHARED_REJECTED) {
            rt->manual_restore_rejected = true;
            rt->manual_restore_retry = false;
        } else if (outcome==MESH_SHARED_TIMEOUT) {
            rt->manual_restore_attempted = false;
            rt->manual_restore_retry = true;
        } else {
            rt->manual_restore_retry = false;
        }
    }
}

static bool accept_manual_clock(Runtime *rt, const char *text);
static bool same_alarm_round(AlarmRoundId a, AlarmRoundId b);

static bool orientation_auto_supported(void)
{
#if CONFIG_IDF_TARGET_ESP32C6
    return true;
#else
    return false;
#endif
}
static bool orientation_auto_for(const Runtime *rt)
{
    return orientation_auto_supported() && rt && rt->store_ready &&
           !rt->store.record.orientation_manual;
}

static void poll_auto_orientation(Runtime *rt)
{
#if CONFIG_IDF_TARGET_ESP32C6
    Orientation sensed;
    if (orientation_auto_for(rt) && board_auto_orientation(&sensed) &&
        sensed!=rt->local.orientation && board_set_orientation(sensed)==BOARD_OK)
        rt->local.orientation=sensed; /* no Store, shared event or clock mutation */
#else
    (void)rt;
#endif
}

static DisplayUiMailbox display_mailbox=DISPLAY_MB_INITIALIZER;
/* The LVGL callback must not read the app-loop-owned Store mode. Reuse the
 * mailbox's short POD lock for this published scalar, never for Store/I2C. */
typedef struct {
    bool valid, ready, divergent, has_confirmation, configured, setup_open;
    bool setup_dismissed, manual_clock_required, mesh_started, connected, mesh_active;
    uint8_t mesh_channel;
    KatmedMeshCounts mesh_counts;
    uint8_t pending, conflicts;
    AppState state;
    LocalSettings local;
    ClockSample clock, confirmation;
    UiConflict conflict[KATMED_STORE_CONFLICT_MAX];
    char setup_ssid[33], setup_password[17];
} UiOwnerSnapshot;
static UiOwnerSnapshot ui_owner_snapshot;
static void publish_ui_owner(Runtime *rt)
{
    UiOwnerSnapshot next={0};
    next.valid=rt!=NULL;
    if (rt) {
        next.state=rt->state;next.local=rt->local;next.clock=clock_sample(rt->boot_domain);
        next.ready=rt->store_ready && store_get_status(&rt->store,NULL,&next.pending,NULL)==STORE_OK;
        next.mesh_started=rt->mesh_started;next.setup_open=rt->setup_ap;
        if (rt->store_ready) {
            next.configured=rt->store.record.provision.configured;
            next.setup_dismissed=rt->store.record.setup_dismissed;
            next.manual_clock_required=(next.configured && rt->store.record.provision.network_mode!=STORE_NETWORK_ONLINE) ||
                (!next.configured && next.setup_dismissed && !next.setup_open);
            next.divergent=store_durable_divergent(&rt->store);
            StoreRecovery recovery;
            if(store_get_recovery(&rt->store,&recovery)==STORE_OK && recovery.has_last_confirmation){
                next.has_confirmation=true;next.confirmation=recovery.last_confirmation;
            }
            uint8_t count=0;
            if(store_conflict_count(&rt->store,&count)==STORE_OK) for(size_t i=0;i<count && i<KATMED_STORE_CONFLICT_MAX;++i){
                StorePendingConfirm entry;
                if(store_conflict_at(&rt->store,i,&entry)!=STORE_OK)break;
                next.conflict[next.conflicts++]=(UiConflict){entry.id,entry.round,entry.occurred_at};
            }
        }
        if(next.mesh_started){
            next.connected=katmed_mesh_connected();
            if(next.configured){next.mesh_active=katmed_mesh_radio_channel(&next.mesh_channel);katmed_mesh_counts(&next.mesh_counts);}
        }
        if(next.setup_open){memcpy(next.setup_ssid,rt->setup_ssid,sizeof(next.setup_ssid));memcpy(next.setup_password,rt->setup_password,sizeof(next.setup_password));}
    }
    DISPLAY_MB_LOCK(&display_mailbox);ui_owner_snapshot=next;DISPLAY_MB_UNLOCK(&display_mailbox);
}
static bool published_orientation_automatic;
static bool orientation_automatic(void *context)
{
    (void)context;
    DISPLAY_MB_LOCK(&display_mailbox);
    const bool automatic=published_orientation_automatic;
    DISPLAY_MB_UNLOCK(&display_mailbox);
    return automatic;
}
/* UI callbacks only read a published snapshot; NightJob itself is app-loop owned. */
static void display_settings(void *context,KatmedDisplaySettings *out)
{
    (void)context;display_mb_snapshot(&display_mailbox,out,NULL);
}
static bool brightness_preview(void *context,uint8_t percent)
{
    (void)context;return display_mb_preview(&display_mailbox,percent);
}
static void night_copy_status(void *context,KatmedNightCopyStatus *out)
{
    (void)context;display_mb_snapshot(&display_mailbox,NULL,out);
}
static void display_ui_publish(Runtime *rt)
{
    publish_ui_owner(rt);
    const KatmedDisplaySettings settings=rt->store_ready ? rt->store.record.display : katmed_display_defaults();
    KatmedNightCopyStatus night={0};katmed_mesh_night_status(&night);
    const bool automatic=orientation_auto_for(rt);
    display_mb_publish(&display_mailbox,settings,night);
    DISPLAY_MB_LOCK(&display_mailbox);
    published_orientation_automatic=automatic;
    DISPLAY_MB_UNLOCK(&display_mailbox);
}
static void display_policy_step(Runtime *rt,const ClockSample *now)
{
    const KatmedDisplaySettings settings=rt->store_ready ? rt->store.record.display : katmed_display_defaults();
    LocalDateTime local;
    bool valid=now->utc_valid && app_berlin_local_time(now->utc_ms,&local)==APP_OK;
    const uint16_t minute=valid ? (uint16_t)(local.hour*60+local.minute) : 0;
    const bool awake=katmed_display_evaluate(&rt->display_policy,settings,valid,minute,
                                            rt->state.alarm_active,now->monotonic_ms);
    if(!rt->store_ready || rt->reboot_required || store_get_status(&rt->store,NULL,NULL,NULL)!=STORE_OK)
        rt->brightness_preview=0;
    const uint8_t brightness=rt->brightness_preview ? rt->brightness_preview : settings.brightness;
    if(rt->brightness_applied!=brightness){
        if(board_set_brightness(brightness)==BOARD_OK)rt->brightness_applied=brightness;
        else rt->brightness_preview=0; /* retry only the accepted value on the next policy step */
    }
    if(rt->backlight_applied!=awake && lvgl_port_lock(10)){
        ui_set_display_awake(awake);lvgl_port_unlock();
        if(board_set_backlight(awake)==BOARD_OK)rt->backlight_applied=awake;
    }
}

/* app_main only: same non-wrapping uint32 HA sequence as the original single
 * press, applied in O(1) for a counted batch. This never touches Store. */
static UiSubmitResult submit_emergency_count(Runtime *rt,uint64_t count)
{
    if(!rt || rt->reboot_required || !count)return UI_SUBMIT_UNAVAILABLE;
    const uint32_t room=UINT32_MAX-rt->emergency_button_sequence;
    rt->emergency_button_sequence+=count<room ? (uint32_t)count : room;
    return UI_SUBMIT_OBSERVED;
}
static void emergency_ui_poll(Runtime *rt)
{
    /* Only scalar RAM work under the existing lock: no Store, UI, allocation,
     * transport or foreign callback. Completion counts cannot exceed the
     * capacity reserved when their presses were accepted. */
    DISPLAY_MB_LOCK(&display_mailbox);
    const uint64_t count=display_mailbox.emergency_pending;
    if(count){
        const UiSubmitResult result=submit_emergency_count(rt,count);
        if(result==UI_SUBMIT_OBSERVED)display_mailbox.emergency_observed+=count;
        else display_mailbox.emergency_failed+=count;
        display_mailbox.emergency_pending=0;
    }
    DISPLAY_MB_UNLOCK(&display_mailbox);
}

static UiSubmitResult submit_ui_with_id(void *context, const UiRequest *request,
                                       const EventId *retry_id)
{
    Runtime *rt = (Runtime *)context;
    if (rt == NULL || request == NULL || rt->reboot_required)
        return UI_SUBMIT_UNAVAILABLE;
    if (request->guard_model &&
        (!same_alarm_round(request->expected_round,rt->state.round) ||
         request->expected_revision.number!=rt->state.revision.number ||
         request->expected_revision.coordinator!=rt->state.revision.coordinator))
        return UI_SUBMIT_STALE;
    if(request->kind==UI_REQUEST_EMERGENCY)
        return submit_emergency_count(rt,1);
    if (request->kind == UI_REQUEST_DISMISS_BOOT) {
        const ClockSample now = clock_sample(rt->boot_domain);
        return app_dismiss_boot_alarm(&rt->state, &now, NULL) == APP_OK
            ? UI_SUBMIT_DISMISSED : UI_SUBMIT_STALE;
    }
    if (!rt->store_ready) return UI_SUBMIT_UNAVAILABLE;
    /* A failed NVS commit may have persisted the candidate while leaving the
     * active copy unchanged. Even a matching local request is not a success
     * until the store is reopened and the durable value is known. */
    if (store_get_status(&rt->store, NULL, NULL, NULL) != STORE_OK)
        return UI_SUBMIT_UNAVAILABLE;
    if (request->kind == UI_REQUEST_SET_LANGUAGE) {
        const uint8_t language=request->value.language;
        if (!katmed_i18n_available(language)) return UI_SUBMIT_REJECTED;
        const StoreResult result=store_set_language(&rt->store,language);
        if (result!=STORE_OK && result!=STORE_DUPLICATE) return UI_SUBMIT_UNAVAILABLE;
        katmed_i18n_select(language);
        return UI_SUBMIT_COMMITTED;
    }
    if (request->kind == UI_REQUEST_SET_CLOCK_STYLE) {
        if (request->value.clock_style > ANALOG) return UI_SUBMIT_REJECTED;
        if (request->value.clock_style == rt->local.clock_style)
            return UI_SUBMIT_COMMITTED;
        LocalSettings wanted = rt->local;
        wanted.clock_style = request->value.clock_style;
        return store_set_local(&rt->store, &rt->local, &wanted) == STORE_OK
            ? UI_SUBMIT_COMMITTED : UI_SUBMIT_UNAVAILABLE;
    }
    if(request->kind>=UI_REQUEST_SET_BRIGHTNESS && request->kind<=UI_REQUEST_SET_NIGHT_END){
        KatmedDisplaySettings settings=rt->store.record.display;
        const uint8_t previous_brightness=settings.brightness;
        if(request->kind==UI_REQUEST_SET_BRIGHTNESS){
            if(request->value.display_value<1 || request->value.display_value>100)return UI_SUBMIT_REJECTED;
            settings.brightness=(uint8_t)request->value.display_value;
            if(rt->brightness_applied!=settings.brightness){
                if(board_set_brightness(settings.brightness)!=BOARD_OK)return UI_SUBMIT_UNAVAILABLE;
                rt->brightness_applied=settings.brightness;
            }
        }else if(request->kind==UI_REQUEST_SET_NIGHT_ENABLED){
            if(request->value.display_value>1)return UI_SUBMIT_REJECTED;
            settings.night_enabled=request->value.display_value!=0;
        }else if(request->kind==UI_REQUEST_SET_NIGHT_START)settings.night_start=request->value.display_value;
        else settings.night_end=request->value.display_value;
        const StoreResult result=store_set_display(&rt->store,&settings);
        if(request->kind==UI_REQUEST_SET_BRIGHTNESS && result!=STORE_OK && result!=STORE_DUPLICATE &&
           board_set_brightness(previous_brightness)==BOARD_OK)
            rt->brightness_applied=previous_brightness;
        return result==STORE_INVALID ? UI_SUBMIT_REJECTED :
            result==STORE_OK || result==STORE_DUPLICATE ? UI_SUBMIT_COMMITTED : UI_SUBMIT_UNAVAILABLE;
    }
    if(request->kind==UI_REQUEST_COPY_NIGHT){
        if(!rt->mesh_started)return UI_SUBMIT_UNAVAILABLE;
        const KatmedMeshResult result=katmed_mesh_copy_night(monotonic_ms());
        return result==MESH_ACCEPTED || result==MESH_PENDING ? UI_SUBMIT_COMMITTED : UI_SUBMIT_UNAVAILABLE;
    }
    if (request->kind == UI_REQUEST_SET_ORIENTATION_AUTOMATIC) {
        if (!orientation_auto_supported()) return UI_SUBMIT_REJECTED;
        const StoreResult result=store_set_orientation(&rt->store,&rt->local,
                                                       rt->local.orientation,false);
        return result==STORE_OK || result==STORE_DUPLICATE ? UI_SUBMIT_COMMITTED : UI_SUBMIT_UNAVAILABLE;
    }
    if (request->kind == UI_REQUEST_SET_ORIENTATION) {
        if (request->value.orientation > ORIENTATION_270) return UI_SUBMIT_REJECTED;
        const Orientation previous=rt->local.orientation;
        if (request->value.orientation!=previous &&
            board_set_orientation(request->value.orientation)!=BOARD_OK)
            return UI_SUBMIT_UNAVAILABLE;
        const StoreResult result=store_set_orientation(&rt->store,&rt->local,
            request->value.orientation,orientation_auto_supported());
        if (result!=STORE_OK && result!=STORE_DUPLICATE) {
            if (request->value.orientation!=previous) (void)board_set_orientation(previous);
            return UI_SUBMIT_UNAVAILABLE;
        }
        return UI_SUBMIT_COMMITTED;
    }
    if (request->kind==UI_REQUEST_SET_SYSTEM_TIME)
        return accept_manual_clock(rt,request->value.local_datetime) ? UI_SUBMIT_COMMITTED : UI_SUBMIT_REJECTED;
    if (request->kind==UI_REQUEST_OPEN_SETUP) {
        if (store_request_setup(&rt->store)!=STORE_OK) return UI_SUBMIT_UNAVAILABLE;
        rt->reboot_required=true;rt->reboot_due_ms=monotonic_ms()+1000u;
        return UI_SUBMIT_COMMITTED;
    }
    if (request->kind==UI_REQUEST_EXIT_SETUP) {
        if (!rt->setup_ap || store_exit_setup(&rt->store)!=STORE_OK)
            return UI_SUBMIT_UNAVAILABLE;
        rt->reboot_required=true;rt->reboot_due_ms=monotonic_ms()+1000u;
        return UI_SUBMIT_COMMITTED;
    }
    if (request->kind == UI_REQUEST_CONFIRM) {
        if (rt->state.start_reason == START_BOOT) return UI_SUBMIT_STALE;
        const bool offline = rt->store.record.provision.configured &&
            rt->store.record.provision.network_mode == STORE_NETWORK_OFFLINE;
        if (offline && (rt->mesh_started || rt->shared_pending ||
                        rt->state.revision.coordinator != rt->boot.device))
            return UI_SUBMIT_UNAVAILABLE;
        /* Intentional Offline has durable local authority, not a Mesh outbox. */
        const bool coordinator = offline || (rt->mesh_started && katmed_mesh_connected() &&
                                            katmed_mesh_is_coordinator());
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
    /* A provably fresh, never-bound device owns its local settings before
     * provisioning. A reset/unknown former group must not gain this bypass. */
    const bool local_interval_or_rotation =
        request->kind == UI_REQUEST_SET_MEDICATION_SECONDS ||
        request->kind == UI_REQUEST_SET_IMAGE_SECONDS ||
        request->kind == UI_REQUEST_SET_ROTATION_MODE;
    const bool offline=rt->store.record.provision.configured && rt->store.record.provision.network_mode==STORE_NETWORK_OFFLINE;
    bool standalone=(local_interval_or_rotation ||
        (offline && request->kind==UI_REQUEST_SET_NEXT_LOCAL_TIME)) && !rt->mesh_started &&
        (offline || (!rt->store.record.provision.configured && rt->store.record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND)) &&
        rt->state.revision.coordinator==rt->boot.device;
    if (!offline) for(size_t i=0;i<sizeof(rt->store.record.bound_group);++i)
        if(rt->store.record.bound_group[i]) standalone=false;
    if (rt->shared_pending || (!standalone && (!rt->mesh_started ||
        (!katmed_mesh_is_coordinator() && !katmed_mesh_connected()))))
        return UI_SUBMIT_UNAVAILABLE;
    AppEventKind kind;
    switch (request->kind) {
    case UI_REQUEST_SET_MEDICATION_SECONDS: kind = EVENT_SET_MEDICATION_INTERVAL; break;
    case UI_REQUEST_SET_IMAGE_SECONDS: kind = EVENT_SET_IMAGE_INTERVAL; break;
    case UI_REQUEST_SET_ROTATION_MODE: kind = EVENT_SET_ROTATION_MODE; break;
    case UI_REQUEST_SET_NEXT_LOCAL_TIME: kind = EVENT_SET_NEXT_LOCAL_TIME; break;
    default: return UI_SUBMIT_REJECTED;
    }
    /* Preserve the active-alarm medical guard before reserving durable IDs. */
    if (standalone && rt->state.alarm_active && kind == EVENT_SET_MEDICATION_INTERVAL)
        return UI_SUBMIT_UNAVAILABLE;
    EventId id;
    if (retry_id) id=*retry_id;
    else if (store_next_event_id(&rt->store, &id) != STORE_OK)
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
    if (standalone) {
        /* Same reducer and durable commit as coordinator settings, but no
         * transport/confirmation queue and no relaxation of Core guards. */
        event.coordinator_accepted = true;
        AppResult core_result = APP_INVALID;
        return store_apply_event(&rt->store, &rt->state, &event, &now, false,
                                 &effects, &core_result) == STORE_OK
            ? UI_SUBMIT_COMMITTED : UI_SUBMIT_UNAVAILABLE;
    }
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

static UiSubmitResult submit_ui(void *context, const UiRequest *request)
{
    return submit_ui_with_id(context,request,NULL);
}

/* taskLVGL only copies an intent; every Runtime/Core/Store action executes on
 * app_main. Never wait for the owner while holding the LVGL lock. */
static UiSubmitResult submit_ui_dispatch(void *context,const UiRequest *request)
{
    if (!context || !request || request->kind>UI_REQUEST_COPY_NIGHT)
        return UI_SUBMIT_UNAVAILABLE;
    if(request->kind==UI_REQUEST_EMERGENCY)
        return display_mb_emergency_submit(&display_mailbox) ? UI_SUBMIT_QUEUED : UI_SUBMIT_UNAVAILABLE;
    return display_mb_submit(&display_mailbox,request) ? UI_SUBMIT_QUEUED : UI_SUBMIT_UNAVAILABLE;
}
/* Sole consumer, on app_main before rendering; no UI lock held. */
static void display_ui_poll(Runtime *rt)
{
    emergency_ui_poll(rt);
    uint8_t preview;
    if(display_mb_take_preview(&display_mailbox,&preview))
        rt->brightness_preview=rt->store_ready && !rt->reboot_required &&
            store_get_status(&rt->store,NULL,NULL,NULL)==STORE_OK ? preview : 0;
    UiRequest request;uint32_t sequence;
    if(display_mb_take(&display_mailbox,&request,&sequence)){
        if(request.kind==UI_REQUEST_SET_BRIGHTNESS)rt->brightness_preview=0;
        UiSubmitResult result=submit_ui(rt,&request);
        display_ui_publish(rt);
        (void)display_mb_complete(&display_mailbox,sequence,result);
    }else display_ui_publish(rt);
    emergency_ui_poll(rt); /* Includes presses that arrived during ordinary work. */
}

static uint64_t config_mix(uint64_t hash, uint64_t value)
{
    for (unsigned i = 0; i < 8; ++i) {
        hash ^= (uint8_t)(value >> (8u * i));
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* A compare-and-swap token, never an authorization credential. */
static void config_revision(const Runtime *rt, char out[KATMED_CONFIG_REVISION_MAX])
{
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = config_mix(hash, rt->boot.epoch);
    hash = config_mix(hash, rt->store.record.next_sequence);
    hash = config_mix(hash, rt->store.record.provision.configured);
    hash = config_mix(hash, rt->store.record.provision.network_mode);
    hash = config_mix(hash, rt->store.record.setup_requested);
    hash = config_mix(hash, rt->state.revision.number);
    hash = config_mix(hash, rt->state.revision.coordinator);
    hash = config_mix(hash, rt->state.countdown_origin.epoch);
    hash = config_mix(hash, rt->state.countdown_origin.sequence);
    hash = config_mix(hash, (uint64_t)rt->state.alarm_active);
    hash = config_mix(hash, (uint64_t)rt->state.start_reason);
    hash = config_mix(hash, (uint64_t)(rt->store_ready ? rt->store.record.local.orientation : rt->local.orientation));
    hash = config_mix(hash, orientation_auto_for(rt));
    if(rt->store_ready){const KatmedDisplaySettings d=rt->store.record.display;
        hash=config_mix(hash,d.brightness);hash=config_mix(hash,d.night_enabled);
        hash=config_mix(hash,d.night_start);hash=config_mix(hash,d.night_end);}
    hash = config_mix(hash, (uint64_t)rt->local.clock_style);
    hash = config_mix(hash, (uint64_t)rt->store.record.manual_intent.pending);
    (void)snprintf(out, KATMED_CONFIG_REVISION_MAX, "%016" PRIx64, hash);
}

static void config_hours(char out[24], uint32_t seconds)
{
    const unsigned long hours = seconds / 3600u;
    const unsigned long fraction =
        ((uint64_t)(seconds % 3600u) * 1000000u + 1800u) / 3600u;
    if (!fraction) (void)snprintf(out, 24, "%lu", hours);
    else {
        (void)snprintf(out, 24, "%lu.%06lu", hours, fraction);
        size_t length = strlen(out);
        while (length && out[length - 1] == '0') out[--length] = '\0';
    }
}

static bool config_hhmm(const char *text, uint8_t *hour, uint8_t *minute)
{
    if (!text || strlen(text) != 5 || text[2] != ':') return false;
    if (text[0]<'0' || text[0]>'9' || text[1]<'0' || text[1]>'9' ||
        text[3]<'0' || text[3]>'9' || text[4]<'0' || text[4]>'9') return false;
    const unsigned h = (unsigned)(text[0]-'0')*10u+(unsigned)(text[1]-'0');
    const unsigned m = (unsigned)(text[3]-'0')*10u+(unsigned)(text[4]-'0');
    if (h>23 || m>59) return false;
    *hour=(uint8_t)h; *minute=(uint8_t)m;
    return true;
}

static void config_write_hhmm(char out[6], uint8_t hour, uint8_t minute)
{
    out[0]=(char)('0'+hour/10u); out[1]=(char)('0'+hour%10u);
    out[2]=':';
    out[3]=(char)('0'+minute/10u); out[4]=(char)('0'+minute%10u);
    out[5]='\0';
}

static void config_snapshot(const Runtime *rt, KatmedConfigSnapshot *out)
{
    *out=(KatmedConfigSnapshot){0};
    config_hours(out->medication_hours,rt->state.settings.medication_seconds);
    config_hours(out->image_hours,rt->state.settings.image_seconds);
    out->clock_style=rt->local.clock_style;
    out->orientation=rt->local.orientation;
    out->orientation_auto_supported=orientation_auto_supported();
    out->orientation_automatic=orientation_auto_for(rt);
    out->display=rt->store_ready ? rt->store.record.display : katmed_display_defaults();
    katmed_mesh_night_status(&out->night_copy);
    out->rotation=rt->state.settings.rotation;
    out->restore_status=rt->state.start_reason==START_MANUAL ?
        KATMED_RESTORE_NONE : rt->manual_restore_status;
    if (rt->store.record.manual_intent.pending &&
        rt->state.start_reason!=START_MANUAL) {
        const StoreManualIntent *intent=&rt->store.record.manual_intent;
        config_write_hhmm(out->next_due_hhmm,intent->hour,intent->minute);
        out->next_due_state=KATMED_DUE_PAUSED_BOOT;
    } else if (!rt->state.alarm_active && rt->state.deadline.effective_utc.valid) {
        LocalDateTime local;
        if (app_berlin_local_time(rt->state.deadline.effective_utc.ms,&local)==APP_OK) {
            (void)snprintf(out->next_due_hhmm,sizeof(out->next_due_hhmm),
                           "%02u:%02u",local.hour,local.minute);
            out->next_due_state=KATMED_DUE_ACTIVE;
        }
    }
    config_revision(rt,out->revision);
}

static void config_preview_token(const char *revision,
                                 const ManualTimePreview *preview,
                                 char out[KATMED_CONFIG_PREVIEW_TOKEN_MAX])
{
    uint64_t hash=UINT64_C(14695981039346656037);
    for (const char *p=revision; *p; ++p) {
        hash^=(uint8_t)*p;
        hash*=UINT64_C(1099511628211);
    }
    hash=config_mix(hash,(uint64_t)preview->regular_utc.ms);
    hash=config_mix(hash,(uint64_t)preview->effective_utc.ms);
    (void)snprintf(out,KATMED_CONFIG_PREVIEW_TOKEN_MAX,"%016" PRIx64,hash);
}

static bool accept_manual_clock(Runtime *rt, const char *text) {
    int64_t utc;
    if (!rt || !text || app_parse_local_datetime(text,&utc)!=APP_OK) return false;
    KatmedTime previous=rt->time;
    if (!katmed_time_accept_utc(&rt->time,monotonic_ms(),utc,CLOCK_HOLDOVER)) return false;
    ClockSample sample=katmed_time_sample(&rt->time,monotonic_ms());
    struct timeval old_wall;
    if(gettimeofday(&old_wall,NULL)!=0) {rt->time=previous;return false;}
    struct timeval wall={.tv_sec=utc/1000,.tv_usec=(utc%1000)*1000};
    if (settimeofday(&wall,NULL)!=0) {rt->time=previous;return false;}
    if (!apply_time_sample(rt,&sample)) {
        (void)settimeofday(&old_wall,NULL);rt->time=previous;return false;
    }
    manual_clock_marker=MANUAL_CLOCK_MARKER;rt->utc_source=3;
    return true;
}

static bool start_setup_ap(Runtime *rt) {
    if (esp_netif_init()!=ESP_OK) return false;
    esp_err_t e=esp_event_loop_create_default();
    if (e!=ESP_OK && e!=ESP_ERR_INVALID_STATE) return false;
    if (!esp_netif_create_default_wifi_ap()) return false;
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init)!=ESP_OK || esp_wifi_set_storage(WIFI_STORAGE_RAM)!=ESP_OK) return false;
    uint8_t random[8];esp_fill_random(random,sizeof(random));
    for(size_t i=0;i<sizeof(random);++i) snprintf(rt->setup_password+2*i,3,"%02x",random[i]);
    memset(random,0,sizeof(random));
    memcpy(rt->setup_ssid,"Katmed_ESP_SETUP",sizeof("Katmed_ESP_SETUP"));
    wifi_config_t config={0};
    memcpy(config.ap.ssid,rt->setup_ssid,strlen(rt->setup_ssid));config.ap.ssid_len=strlen(rt->setup_ssid);
    memcpy(config.ap.password,rt->setup_password,16);config.ap.authmode=WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection=1;config.ap.channel=1;
    bool ok=esp_wifi_set_mode(WIFI_MODE_AP)==ESP_OK && esp_wifi_set_config(WIFI_IF_AP,&config)==ESP_OK && esp_wifi_start()==ESP_OK;
    memset(&config,0,sizeof(config));rt->setup_ap=ok;return ok;
}

static void wifi_execute(Runtime *rt,const KatmedConfigRequest *request,KatmedConfigResponse *response);
static void config_execute(void *context, const KatmedConfigRequest *request,
                           KatmedConfigResponse *response)
{
    Runtime *rt=context;
    if (!response) return;
    response->status=KATMED_CONFIG_UNAVAILABLE;
    if(rt && request && request->operation>=KATMED_CONFIG_WIFI_STATUS && request->operation<=KATMED_CONFIG_WIFI_RECONNECT){
        wifi_execute(rt,request,response);return;
    }
    if (!rt || !request || !response || !rt->store_ready || rt->reboot_required ||
        store_get_status(&rt->store,NULL,NULL,NULL)!=STORE_OK) return;
    if (request->operation==KATMED_CONFIG_LANGUAGE_GET ||
        request->operation==KATMED_CONFIG_LANGUAGE_SAVE) {
        if (request->operation==KATMED_CONFIG_LANGUAGE_SAVE) {
            const UiRequest language={.kind=UI_REQUEST_SET_LANGUAGE,
                                      .value.language=request->language};
            const UiSubmitResult result=submit_ui(rt,&language);
            if (result!=UI_SUBMIT_COMMITTED) {
                response->status=result==UI_SUBMIT_REJECTED ? KATMED_CONFIG_INVALID :
                    rt->store.uncertain ? KATMED_CONFIG_COMMIT_UNKNOWN : KATMED_CONFIG_UNAVAILABLE;
                return;
            }
        }
        response->language=katmed_i18n_available(rt->store.record.language) ?
            rt->store.record.language : KATMED_LANG_EN;
        response->status=KATMED_CONFIG_OK;
        return;
    }
    if (request->operation==KATMED_CONFIG_HA_GET) {
        const ClockSample clock=clock_sample(rt->boot_domain);
        snprintf(response->ha.device_id,sizeof(response->ha.device_id),"%012" PRIx64,rt->boot.device);
        snprintf(response->ha.boot_id,sizeof(response->ha.boot_id),"%" PRIu64,rt->boot_domain);
        response->ha.boot_notice=rt->state.alarm_active && rt->state.start_reason==START_BOOT;
        response->ha.alarm_active=rt->state.alarm_active && !response->ha.boot_notice;
        response->ha.alarm_severity=response->ha.alarm_active ? 1 : 0; /* Same untimed blue fallback as UI. */
        AlarmAnimation animation;
        if(response->ha.alarm_active && app_alarm_animation(&rt->state,&clock,&animation)==APP_OK) response->ha.alarm_severity=(uint8_t)animation.color+1u;
        response->ha.next_due_ms=clock.utc_valid && rt->state.deadline.effective_utc.valid ? rt->state.deadline.effective_utc.ms : 0;
        const StoreConfirmationHistory *last=&rt->store.record.last_confirmation;
        response->ha.last_dose_known=last->valid;
        response->ha.last_dose_ms=last->valid && last->occurred_at.utc_valid ? last->occurred_at.utc_ms : 0;
        response->ha.emergency_button_sequence=rt->emergency_button_sequence;
        response->status=KATMED_CONFIG_OK;return;
    }
    if (request->operation==KATMED_CONFIG_SETUP_GET) {
        config_snapshot(rt,&response->snapshot);
        response->setup.configured=rt->store.record.provision.configured;
        response->setup.mode=rt->store.record.provision.network_mode;
        response->setup.setup_open=rt->setup_ap;
        response->setup.clock_required=!clock_sample(rt->boot_domain).utc_valid;
        response->status=KATMED_CONFIG_OK;return;
    }
    if (request->operation==KATMED_CONFIG_SETUP_EXIT) {
        char revision[KATMED_CONFIG_REVISION_MAX];config_revision(rt,revision);
        if (!rt->setup_ap || !memchr(request->revision,0,sizeof(request->revision)) ||
            strcmp(revision,request->revision)) {response->status=KATMED_CONFIG_STALE;return;}
        const StoreResult result=store_exit_setup(&rt->store);
        if(result!=STORE_OK) {response->status=result==STORE_INVALID?KATMED_CONFIG_INVALID:KATMED_CONFIG_COMMIT_UNKNOWN;return;}
        rt->reboot_required=true;rt->reboot_due_ms=monotonic_ms()+1000u;
        response->status=KATMED_CONFIG_OK;return;
    }
    if (request->operation==KATMED_CONFIG_SETUP_SAVE) {
        char revision[KATMED_CONFIG_REVISION_MAX];config_revision(rt,revision);
        if (!memchr(request->revision,0,sizeof(request->revision)) || strcmp(revision,request->revision)) {response->status=KATMED_CONFIG_STALE;return;}
        const uint8_t mode=request->setup.mode;
        int64_t manual=0;
        if (mode>STORE_NETWORK_OFFLINE || request->setup.group_action>2 || !memchr(request->setup.local_datetime,0,sizeof(request->setup.local_datetime)) ||
            (mode!=STORE_NETWORK_ONLINE && app_parse_local_datetime(request->setup.local_datetime,&manual)!=APP_OK)) {response->status=KATMED_CONFIG_INVALID;return;}
        StoreProvision p={0};p.configured=true;p.network_mode=mode;
        if (mode!=STORE_NETWORK_OFFLINE) {
            if (request->setup.group_action==0) {memcpy(p.group,rt->store.record.provision.group,16);memcpy(p.secret,rt->store.record.provision.secret,32);}
            else if (request->setup.group_action==1) {esp_fill_random(p.group,16);esp_fill_random(p.secret,32);}
            else if (request->setup.group_action==2) {memcpy(p.group,request->setup.group,16);memcpy(p.secret,request->setup.secret,32);}
            else {response->status=KATMED_CONFIG_INVALID;return;}
        }
        if (mode==STORE_NETWORK_ONLINE) {
            size_t ssid=strnlen(request->setup.ssid,sizeof(request->setup.ssid)),password=strnlen(request->setup.password,sizeof(request->setup.password)),source=strnlen(request->setup.time_source,sizeof(request->setup.time_source));
            if(ssid>32 || password>63 || source>63) {memset(&p,0,sizeof(p));response->status=KATMED_CONFIG_INVALID;return;}
            memcpy(p.ssid,request->setup.ssid,ssid);p.ssid_length=ssid;
            memcpy(p.password,request->setup.password,password);p.password_length=password;
            memcpy(p.time_source,request->setup.time_source,source);p.time_source_length=source;
        }
        StoreResult saved=store_save_setup(&rt->store,&p);memset(&p,0,sizeof(p));
        if(saved!=STORE_OK) {response->status=saved==STORE_STALE?KATMED_CONFIG_STALE:saved==STORE_INVALID?KATMED_CONFIG_INVALID:KATMED_CONFIG_COMMIT_UNKNOWN;return;}
        if(mode!=STORE_NETWORK_ONLINE && !accept_manual_clock(rt,request->setup.local_datetime)) {response->status=KATMED_CONFIG_COMMIT_UNKNOWN;return;}
        rt->reboot_required=true;rt->reboot_due_ms=monotonic_ms()+1000u;
        response->status=KATMED_CONFIG_OK;return;
    }
    if (request->operation==KATMED_CONFIG_GET) {
        config_snapshot(rt,&response->snapshot);
        response->status=KATMED_CONFIG_OK;
        return;
    }
    if (request->operation!=KATMED_CONFIG_PREVIEW_TIME &&
        request->operation!=KATMED_CONFIG_SAVE) {
        response->status=KATMED_CONFIG_INVALID;
        return;
    }
    if (!memchr(request->revision,0,sizeof(request->revision)) ||
        !memchr(request->value,0,sizeof(request->value)) ||
        !memchr(request->preview_token,0,sizeof(request->preview_token))) {
        response->status=KATMED_CONFIG_INVALID;
        return;
    }
    char current[KATMED_CONFIG_REVISION_MAX];
    config_revision(rt,current);
    if (strcmp(request->revision,current)!=0) {
        response->status=KATMED_CONFIG_STALE;
        return;
    }
    UiRequest ui_request={0};
    if (request->operation==KATMED_CONFIG_PREVIEW_TIME ||
        request->field==KATMED_CONFIG_NEXT_DUE_HHMM) {
        uint8_t hour,minute;
        if (!config_hhmm(request->value,&hour,&minute)) {
            response->status=KATMED_CONFIG_INVALID;
            return;
        }
        const ClockSample now=clock_sample(rt->boot_domain);
        ManualTimePreview preview;
        const AppResult result=app_preview_local_time(&now,hour,minute,&preview);
        if (result!=APP_OK) {
            response->status=result==APP_TIME_UNAVAILABLE ?
                KATMED_CONFIG_TIME_UNAVAILABLE : KATMED_CONFIG_INVALID;
            return;
        }
        char token[KATMED_CONFIG_PREVIEW_TOKEN_MAX];
        config_preview_token(current,&preview,token);
        if (request->operation==KATMED_CONFIG_PREVIEW_TIME) {
            config_write_hhmm(response->preview.time,hour,minute);
            response->preview.regular_local=preview.regular_local;
            response->preview.effective_local=preview.effective_local;
            response->preview.night_advanced=preview.night_advanced;
            response->preview.skipped_nonexistent_time=preview.skipped_nonexistent_time;
            (void)snprintf(response->preview.preview_token,
                           sizeof(response->preview.preview_token),"%s",token);
            response->status=KATMED_CONFIG_OK;
            return;
        }
        if (strcmp(request->preview_token,token)!=0) {
            response->status=KATMED_CONFIG_STALE;
            return;
        }
        ui_request.kind=UI_REQUEST_SET_NEXT_LOCAL_TIME;
        ui_request.value.local_time.hour=hour;
        ui_request.value.local_time.minute=minute;
        ui_request.value.local_time.preview=preview;
    } else {
        if (request->operation!=KATMED_CONFIG_SAVE) {
            response->status=KATMED_CONFIG_INVALID;
            return;
        }
        switch (request->field) {
        case KATMED_CONFIG_MEDICATION_HOURS:
        case KATMED_CONFIG_IMAGE_HOURS: {
            uint32_t seconds=0;
            if (app_parse_hours(request->value,&seconds)!=APP_OK) {
                response->status=KATMED_CONFIG_INVALID; return;
            }
            ui_request.kind=request->field==KATMED_CONFIG_MEDICATION_HOURS ?
                UI_REQUEST_SET_MEDICATION_SECONDS:UI_REQUEST_SET_IMAGE_SECONDS;
            ui_request.value.seconds=seconds;
            break;
        }
        case KATMED_CONFIG_CLOCK_STYLE:
            ui_request.kind=UI_REQUEST_SET_CLOCK_STYLE;
            if (strcmp(request->value,"digital_large")==0) ui_request.value.clock_style=DIGITAL_LARGE;
            else if (strcmp(request->value,"digital_date")==0) ui_request.value.clock_style=DIGITAL_DATE;
            else if (strcmp(request->value,"analog")==0) ui_request.value.clock_style=ANALOG;
            else { response->status=KATMED_CONFIG_INVALID; return; }
            break;
        case KATMED_CONFIG_BRIGHTNESS: {
            unsigned value=0;size_t length=strlen(request->value);
            if(length<1 || length>3){response->status=KATMED_CONFIG_INVALID;return;}
            for(size_t i=0;i<length;++i){if(request->value[i]<'0' || request->value[i]>'9'){response->status=KATMED_CONFIG_INVALID;return;}value=value*10u+(unsigned)(request->value[i]-'0');}
            ui_request.kind=UI_REQUEST_SET_BRIGHTNESS;ui_request.value.display_value=(uint16_t)value;break;
        }
        case KATMED_CONFIG_NIGHT_ENABLED:
            ui_request.kind=UI_REQUEST_SET_NIGHT_ENABLED;
            if(strcmp(request->value,"on")==0)ui_request.value.display_value=1;
            else if(strcmp(request->value,"off")!=0){response->status=KATMED_CONFIG_INVALID;return;}
            break;
        case KATMED_CONFIG_NIGHT_START:
        case KATMED_CONFIG_NIGHT_END: {
            uint8_t hour,minute;
            if(!config_hhmm(request->value,&hour,&minute)){response->status=KATMED_CONFIG_INVALID;return;}
            ui_request.kind=request->field==KATMED_CONFIG_NIGHT_START ? UI_REQUEST_SET_NIGHT_START : UI_REQUEST_SET_NIGHT_END;
            ui_request.value.display_value=(uint16_t)(hour*60u+minute);break;
        }
        case KATMED_CONFIG_COPY_NIGHT:
            if(strcmp(request->value,"copy")!=0){response->status=KATMED_CONFIG_INVALID;return;}
            ui_request.kind=UI_REQUEST_COPY_NIGHT;break;
        case KATMED_CONFIG_ORIENTATION:
            ui_request.kind=UI_REQUEST_SET_ORIENTATION;
            if (strcmp(request->value,"auto")==0 && orientation_auto_supported())
                ui_request.kind=UI_REQUEST_SET_ORIENTATION_AUTOMATIC;
            else if (strcmp(request->value,"0")==0) ui_request.value.orientation=ORIENTATION_0;
            else if (strcmp(request->value,"90")==0) ui_request.value.orientation=ORIENTATION_90;
            else if (strcmp(request->value,"180")==0) ui_request.value.orientation=ORIENTATION_180;
            else if (strcmp(request->value,"270")==0) ui_request.value.orientation=ORIENTATION_270;
            else { response->status=KATMED_CONFIG_INVALID; return; }
            break;
        case KATMED_CONFIG_ROTATION:
            ui_request.kind=UI_REQUEST_SET_ROTATION_MODE;
            if (strcmp(request->value,"off")==0) ui_request.value.rotation=ROTATION_OFF;
            else if (strcmp(request->value,"name_order")==0) ui_request.value.rotation=ROTATION_NAME_ORDER;
            else if (strcmp(request->value,"random")==0) ui_request.value.rotation=ROTATION_RANDOM;
            else { response->status=KATMED_CONFIG_INVALID; return; }
            break;
        default: response->status=KATMED_CONFIG_INVALID; return;
        }
    }
    const UiSubmitResult result=submit_ui(rt,&ui_request);
    if (result==UI_SUBMIT_COMMITTED) {
        if (request->field==KATMED_CONFIG_NEXT_DUE_HHMM)
            rt->manual_restore_status=KATMED_RESTORE_NONE;
        config_snapshot(rt,&response->snapshot);
        response->status=KATMED_CONFIG_OK;
    } else if (result==UI_SUBMIT_PENDING_SHARED || result==UI_SUBMIT_PENDING)
        response->status=KATMED_CONFIG_PENDING;
    else if (result==UI_SUBMIT_STALE) response->status=KATMED_CONFIG_STALE;
    else if (result==UI_SUBMIT_REJECTED) response->status=KATMED_CONFIG_INVALID;
    else response->status=rt->store.uncertain ? KATMED_CONFIG_COMMIT_UNKNOWN :
        KATMED_CONFIG_UNAVAILABLE;
}

static void restore_manual_if_ready(Runtime *rt, const ClockSample *now)
{
    if (rt->manual_restore_rejected) {
        if (rt->store_ready && !rt->reboot_required &&
            store_clear_manual_intent(&rt->store)==STORE_OK) {
            rt->manual_restore_status=KATMED_RESTORE_CONFLICT;
            rt->manual_restore_rejected=false;
            rt->manual_restore_attempted=true;
        }
        return;
    }
    if (!rt->store_ready || rt->manual_restore_attempted || rt->reboot_required ||
        !rt->store.record.manual_intent.pending || !now->utc_valid) return;
    const StoreManualIntent intent=rt->store.record.manual_intent;
    /* A coordinator may have committed our timed-out request while its ACK
     * was lost. A matching durable snapshot proves adoption without another
     * event; otherwise retries use the same EventId for deduplication. */
    if (rt->state.start_reason==START_MANUAL &&
        rt->state.deadline.regular_utc.ms==intent.regular_utc.ms &&
        rt->state.deadline.effective_utc.ms==intent.effective_utc.ms) {
        rt->manual_restore_attempted=true;
        rt->manual_restore_retry=false;
        return;
    }
    if (memcmp(intent.bound_group,rt->store.record.bound_group,
               sizeof(intent.bound_group))!=0 ||
        intent.revision.number!=rt->state.settings.revision.number ||
        intent.revision.coordinator!=rt->state.settings.revision.coordinator) {
        if (store_clear_manual_intent(&rt->store)==STORE_OK) {
            rt->manual_restore_status=KATMED_RESTORE_CONFLICT;
            rt->manual_restore_attempted=true;
        }
        return;
    }
    if (intent.regular_utc.ms<=now->utc_ms ||
        intent.effective_utc.ms<=now->utc_ms) {
        if (store_clear_manual_intent(&rt->store)==STORE_OK) {
            rt->manual_restore_status=KATMED_RESTORE_EXPIRED;
            rt->manual_restore_attempted=true;
        }
        return;
    }
    /* Only a closed boot notice permits restoration. Closing that notice
     * cannot itself create a confirmation or restart the medication clock. */
    if (rt->state.alarm_active || rt->state.start_reason==START_MANUAL ||
        !rt->mesh_started) return;
    ManualTimePreview preview;
    if (app_preview_local_time(now,intent.hour,intent.minute,&preview)!=APP_OK ||
        preview.regular_utc.ms!=intent.regular_utc.ms ||
        preview.effective_utc.ms!=intent.effective_utc.ms) {
        if (store_clear_manual_intent(&rt->store)==STORE_OK) {
            rt->manual_restore_status=KATMED_RESTORE_CONFLICT;
            rt->manual_restore_attempted=true;
        }
        return;
    }
    const UiRequest request={
        .kind=UI_REQUEST_SET_NEXT_LOCAL_TIME,
        .value.local_time={.hour=intent.hour,.minute=intent.minute,.preview=preview},
    };
    const UiSubmitResult result=submit_ui_with_id(rt,&request,
        rt->manual_restore_retry ? &rt->manual_restore_event_id : NULL);
    if (result==UI_SUBMIT_REJECTED || result==UI_SUBMIT_STALE) {
        /* A definitive rejection cannot leave the old intent advertised as
         * paused across this boot or the next one. Retry a failed clear. */
        if (store_clear_manual_intent(&rt->store)==STORE_OK) {
            rt->manual_restore_status=KATMED_RESTORE_CONFLICT;
            rt->manual_restore_attempted=true;
            rt->manual_restore_retry=false;
        }
    } else if (result!=UI_SUBMIT_UNAVAILABLE) {
        if (result==UI_SUBMIT_PENDING_SHARED) {
            rt->manual_restore_waiting=true;
            rt->manual_restore_event_id=rt->shared_id;
        } else rt->manual_restore_retry=false;
        rt->manual_restore_attempted=true;
    }
}

/* Read current SDK interfaces, never stored provisioning addresses. A cached
 * netif address is not usable unless the STA is associated and up now. */
static void network_status(void *context, UiNetworkStatus *out)
{
    if (!out) return;
    *out = (UiNetworkStatus){0};
    snprintf(out->device_name, sizeof(out->device_name), "KatmedESP");
    katmed_mesh_device_name(out->device_name,sizeof(out->device_name),&out->device_name_assigned,
                            &out->device_name_uncertain);
    if(!context)return;
    DISPLAY_MB_LOCK(&display_mailbox);
    out->configured=ui_owner_snapshot.configured;
    out->setup_open=ui_owner_snapshot.setup_open;
    out->offline_setup_exited=ui_owner_snapshot.valid && !out->configured &&
        ui_owner_snapshot.setup_dismissed && !out->setup_open;
    out->manual_clock_required=ui_owner_snapshot.manual_clock_required && !ui_owner_snapshot.clock.utc_valid;
    const bool mesh_started=ui_owner_snapshot.mesh_started;
    out->mesh_active=ui_owner_snapshot.mesh_active;out->mesh_channel=ui_owner_snapshot.mesh_channel;
    out->mesh_current_count=ui_owner_snapshot.mesh_counts.current;out->mesh_history_count=ui_owner_snapshot.mesh_counts.history;
    out->mesh_quality=ui_owner_snapshot.mesh_counts.quality;out->mesh_current_valid=ui_owner_snapshot.mesh_counts.current_valid;
    out->mesh_history_complete=ui_owner_snapshot.mesh_counts.history_complete;
    if(out->setup_open){memcpy(out->setup_ssid,ui_owner_snapshot.setup_ssid,sizeof(out->setup_ssid));memcpy(out->setup_password,ui_owner_snapshot.setup_password,sizeof(out->setup_password));}
    DISPLAY_MB_UNLOCK(&display_mailbox);
    if (!out->configured || !mesh_started) return;
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

static void web_wipe(void *buffer, size_t length)
{
    volatile uint8_t *p = (volatile uint8_t *)buffer;
    while (length--) *p++ = 0;
}

static bool web_wifi_ready(Runtime *rt)
{
    return rt != NULL && rt->store_ready &&
        store_get_status(&rt->store, NULL, NULL, NULL) == STORE_OK;
}

/* These language callbacks never dereference Runtime/Store on HTTPD.
 * Validation, transaction and snapshot execute on the app-loop owner. */
static KatmedConfigStatus web_language_get(void *context, uint8_t *out)
{
    if (!context || !out) return KATMED_CONFIG_UNAVAILABLE;
    const KatmedConfigRequest request={.operation=KATMED_CONFIG_LANGUAGE_GET};
    KatmedConfigResponse response={0};
    const KatmedConfigStatus status=katmed_config_exchange(&request,&response,1500u);
    if (status==KATMED_CONFIG_OK) *out=response.language;
    return status;
}

static KatmedConfigStatus web_language_set(void *context, uint8_t language,
                                           uint8_t *committed)
{
    if (!context || !committed) return KATMED_CONFIG_UNAVAILABLE;
    const KatmedConfigRequest request={.operation=KATMED_CONFIG_LANGUAGE_SAVE,
                                       .language=language};
    KatmedConfigResponse response={0};
    const KatmedConfigStatus status=katmed_config_exchange(&request,&response,1500u);
    if (status==KATMED_CONFIG_OK) *committed=response.language;
    return status;
}

_Static_assert(KATMED_CONFIG_WIFI_MAX==KATMED_STORE_WIFI_PROFILE_MAX,"WiFi bridge capacity must match Store");
static bool owner_wifi_configured(void *context, bool *out)
{
    Runtime *rt = context;
    if (!out || !web_wifi_ready(rt)) return false;
    *out = rt->store.record.provision.configured;
    return true;
}

static bool owner_wifi_recovery_available(void *context, bool *out)
{
    Runtime *rt = context;
    if (!out) return false;
    *out = web_wifi_ready(rt) && !rt->store.uncertain;
    return true;
}

static bool owner_wifi_profile_limit(void *context, size_t *out)
{
    if (!out || !web_wifi_ready((Runtime *)context)) return false;
    *out = store_wifi_profile_limit();
    return true;
}

static bool owner_wifi_list_profiles(void *context, KatmedWebWifiProfileView *out,
                                   size_t capacity, size_t *count)
{
    Runtime *rt = context;
    StoreWifiProfile profiles[KATMED_STORE_WIFI_PROFILE_MAX] = {0};
    size_t loaded = 0;
    bool ok = false;
    if (!count || (!out && capacity) || !web_wifi_ready(rt)) goto done;
    if (store_wifi_profiles_load(&rt->store, profiles,
                                 KATMED_STORE_WIFI_PROFILE_MAX, &loaded) != STORE_OK ||
        capacity < loaded) goto done;
    for (size_t i = 0; i < loaded; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].id = profiles[i].id;
        out[i].priority = profiles[i].priority;
        out[i].secured = profiles[i].secured;
        memcpy(out[i].ssid, profiles[i].ssid, profiles[i].ssid_length);
    }
    *count = loaded;
    ok = true;
done:
    web_wipe(profiles, sizeof(profiles));
    return ok;
}

static bool owner_wifi_save_profiles(void *context,
                                   const KatmedWebWifiProfileUpdate *input,
                                   size_t count)
{
    Runtime *rt = context;
    StoreWifiProfileUpdate updates[KATMED_STORE_WIFI_PROFILE_MAX] = {0};
    bool ok = false;
    if (!web_wifi_ready(rt) || count > KATMED_STORE_WIFI_PROFILE_MAX ||
        (count && !input)) goto done;
    for (size_t i = 0; i < count; ++i) {
        size_t ssid_len = 0, password_len = 0;
        while (ssid_len < sizeof(input[i].ssid) && input[i].ssid[ssid_len]) ++ssid_len;
        while (password_len < sizeof(input[i].password) && input[i].password[password_len])
            ++password_len;
        if(ssid_len>sizeof(updates[i].ssid) || password_len>sizeof(updates[i].password))goto done;
        updates[i].id = input[i].id;
        updates[i].priority = input[i].priority;
        updates[i].ssid_length = (uint8_t)ssid_len;
        memcpy(updates[i].ssid, input[i].ssid, ssid_len);
        if (input[i].credential_action == KATMED_WEB_WIFI_CREDENTIAL_KEEP)
            updates[i].credential_action = STORE_WIFI_CREDENTIAL_KEEP;
        else if (input[i].credential_action == KATMED_WEB_WIFI_CREDENTIAL_REPLACE) {
            updates[i].credential_action = STORE_WIFI_CREDENTIAL_REPLACE;
            updates[i].password_length = (uint8_t)password_len;
            memcpy(updates[i].password, input[i].password, password_len);
        } else if (input[i].credential_action == KATMED_WEB_WIFI_CREDENTIAL_OPEN)
            updates[i].credential_action = STORE_WIFI_CREDENTIAL_OPEN;
        else goto done;
    }
    ok = store_wifi_profiles_save_atomic(&rt->store, updates, count) == STORE_OK;
done:
    web_wipe(updates, sizeof(updates));
    return ok;
}

static bool owner_wifi_delete_profile(void *context, uint32_t id)
{
    Runtime *rt = context;
    return web_wifi_ready(rt) &&
        store_wifi_profile_delete_atomic(&rt->store, id) == STORE_OK;
}

static bool owner_wifi_schedule_reconnect(void *context)
{
    Runtime *rt = context;
    return rt && rt->mesh_started && katmed_mesh_schedule_reconnect();
}

static void wifi_execute(Runtime *rt,const KatmedConfigRequest *request,KatmedConfigResponse *response)
{
    bool ok=false;
    if(request->operation==KATMED_CONFIG_WIFI_STATUS){
        size_t limit=store_wifi_profile_limit();
        bool configured=false,recovery=false;
        /* Recovery=false is a valid status even when Store is unavailable. */
        ok=owner_wifi_recovery_available(rt,&recovery);
        if(web_wifi_ready(rt))ok=owner_wifi_configured(rt,&configured) && owner_wifi_profile_limit(rt,&limit);
        response->wifi.ready=web_wifi_ready(rt);response->wifi.configured=configured;response->wifi.recovery=recovery;response->wifi.limit=(uint8_t)limit;
    }else if(request->operation==KATMED_CONFIG_WIFI_LIST){
        KatmedWebWifiProfileView views[KATMED_CONFIG_WIFI_MAX]={0};size_t count=0;
        ok=owner_wifi_list_profiles(rt,views,KATMED_CONFIG_WIFI_MAX,&count);
        if(ok){
            response->wifi.count=(uint8_t)count;
            for(size_t i=0;i<count;++i){
                response->wifi.profiles[i]=(KatmedConfigWifiView){.id=views[i].id,.priority=views[i].priority,.secured=views[i].secured};
                memcpy(response->wifi.profiles[i].ssid,views[i].ssid,sizeof(views[i].ssid));
            }
        }
        web_wipe(views,sizeof(views));
    }else if(request->operation==KATMED_CONFIG_WIFI_SAVE){
        KatmedWebWifiProfileUpdate updates[KATMED_CONFIG_WIFI_MAX]={0};
        if(request->wifi.count>KATMED_CONFIG_WIFI_MAX)return;
        for(size_t i=0;i<request->wifi.count;++i){
            updates[i]=(KatmedWebWifiProfileUpdate){.id=request->wifi.profiles[i].id,.priority=request->wifi.profiles[i].priority,.credential_action=(KatmedWebWifiCredentialAction)request->wifi.profiles[i].credential_action};
            memcpy(updates[i].ssid,request->wifi.profiles[i].ssid,sizeof(updates[i].ssid));
            memcpy(updates[i].password,request->wifi.profiles[i].password,sizeof(updates[i].password));
        }
        ok=owner_wifi_save_profiles(rt,updates,request->wifi.count);
        web_wipe(updates,sizeof(updates));
    }else if(request->operation==KATMED_CONFIG_WIFI_DELETE)ok=owner_wifi_delete_profile(rt,request->wifi.id);
    else if(request->operation==KATMED_CONFIG_WIFI_RECONNECT)ok=owner_wifi_schedule_reconnect(rt);
    response->status=ok ? KATMED_CONFIG_OK : rt->store.uncertain ? KATMED_CONFIG_COMMIT_UNKNOWN : KATMED_CONFIG_UNAVAILABLE;
}

/* HTTPD only copies POD through the existing single-owner bridge. */
static bool web_wifi_status(void *context,KatmedConfigResponse *response,unsigned stage,
                            KatmedWebWifiStatusDiagnostic *out_diagnostic)
{
    if(!context || !response)return false;
    const KatmedConfigRequest request={.operation=KATMED_CONFIG_WIFI_STATUS};
    KatmedConfigBridgeDiagnostic diagnostic=KATMED_CONFIG_BRIDGE_DIAG_RESPONSE;
    const KatmedConfigStatus status=katmed_config_exchange_diagnostic(
        &request,response,1500u,&diagnostic);
    const bool valid=status==KATMED_CONFIG_OK;
    if(out_diagnostic)*out_diagnostic=(KatmedWebWifiStatusDiagnostic){
        .bridge=(int)status,.bridge_diag=(unsigned)diagnostic,
        .ready_valid=valid,.ready=valid && response->wifi.ready,.available=true};
    if(!valid || (stage!=2u && !response->wifi.ready))
        ESP_LOGW("katmed_wifi","status_stage=%u bridge=%d bridge_diag=%u ready_valid=%u ready=%u",
                 stage,(int)status,(unsigned)diagnostic,(unsigned)valid,
                 (unsigned)(valid && response->wifi.ready));
    return valid;
}
static bool web_wifi_configured(void *context,bool *out,KatmedWebWifiStatusDiagnostic *diagnostic)
{
    KatmedConfigResponse response={0};
    if(!out || !web_wifi_status(context,&response,1u,diagnostic) || !response.wifi.ready)return false;
    *out=response.wifi.configured;return true;
}
static bool web_wifi_recovery_available(void *context,bool *out,KatmedWebWifiStatusDiagnostic *diagnostic)
{
    KatmedConfigResponse response={0};
    if(!out || !web_wifi_status(context,&response,2u,diagnostic))return false;
    *out=response.wifi.recovery;return true;
}
static bool web_wifi_profile_limit(void *context,size_t *out,KatmedWebWifiStatusDiagnostic *diagnostic)
{
    KatmedConfigResponse response={0};
    if(!out || !web_wifi_status(context,&response,3u,diagnostic) || !response.wifi.ready)return false;
    *out=response.wifi.limit;return true;
}
static bool web_wifi_list_profiles(void *context,KatmedWebWifiProfileView *out,size_t capacity,size_t *count)
{
    if(!context || !count || (!out && capacity))return false;
    const KatmedConfigRequest request={.operation=KATMED_CONFIG_WIFI_LIST};
    KatmedConfigResponse response={0};
    if(katmed_config_exchange(&request,&response,1500u)!=KATMED_CONFIG_OK || response.wifi.count>capacity || response.wifi.count>KATMED_CONFIG_WIFI_MAX)return false;
    for(size_t i=0;i<response.wifi.count;++i){
        out[i]=(KatmedWebWifiProfileView){.id=response.wifi.profiles[i].id,.priority=response.wifi.profiles[i].priority,.secured=response.wifi.profiles[i].secured};
        memcpy(out[i].ssid,response.wifi.profiles[i].ssid,sizeof(out[i].ssid));
    }
    *count=response.wifi.count;return true;
}
static bool web_wifi_save_profiles(void *context,const KatmedWebWifiProfileUpdate *input,size_t count)
{
    if(!context || count>KATMED_CONFIG_WIFI_MAX || (count && !input))return false;
    KatmedConfigRequest request={.operation=KATMED_CONFIG_WIFI_SAVE};
    request.wifi.count=(uint8_t)count;
    for(size_t i=0;i<count;++i){
        if(input[i].credential_action!=KATMED_WEB_WIFI_CREDENTIAL_KEEP &&
           input[i].credential_action!=KATMED_WEB_WIFI_CREDENTIAL_REPLACE &&
           input[i].credential_action!=KATMED_WEB_WIFI_CREDENTIAL_OPEN){web_wipe(&request,sizeof(request));return false;}
        request.wifi.profiles[i]=(KatmedConfigWifiUpdate){.id=input[i].id,.priority=input[i].priority,.credential_action=(uint8_t)input[i].credential_action};
        memcpy(request.wifi.profiles[i].ssid,input[i].ssid,sizeof(input[i].ssid));
        memcpy(request.wifi.profiles[i].password,input[i].password,sizeof(input[i].password));
    }
    KatmedConfigResponse response={0};
    const bool ok=katmed_config_exchange(&request,&response,1500u)==KATMED_CONFIG_OK;
    web_wipe(&request,sizeof(request));return ok;
}
static bool web_wifi_delete_profile(void *context,uint32_t id)
{
    if(!context)return false;
    const KatmedConfigRequest request={.operation=KATMED_CONFIG_WIFI_DELETE,.wifi.id=id};
    KatmedConfigResponse response={0};
    return katmed_config_exchange(&request,&response,1500u)==KATMED_CONFIG_OK;
}
static bool web_wifi_schedule_reconnect(void *context)
{
    if(!context)return false;
    const KatmedConfigRequest request={.operation=KATMED_CONFIG_WIFI_RECONNECT};
    KatmedConfigResponse response={0};
    return katmed_config_exchange(&request,&response,1500u)==KATMED_CONFIG_OK;
}

static bool web_reset_offer(void *context)
{
    Runtime *rt=context;
    if (!rt || !reset_mutex || xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(1000))!=pdTRUE)
        return false;
    const bool ok=!rt->reset_active && !rt->reset_requested &&
        !rt->reset_warning_ready && !rt->reset_prepared && !rt->reset_web_offer;
    if (ok) {
        rt->reset_web_offer=true;
        ++rt->reset_web_offer_epoch;
    }
    if (ok) rt->reset_web_rejected=false;
    xSemaphoreGive(reset_mutex);
    return ok;
}

static KatmedWebResetStatus web_reset_status(void *context)
{
    Runtime *rt=context;
    if (!rt || !reset_mutex || xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(1000))!=pdTRUE)
        return KATMED_WEB_RESET_UNAVAILABLE;
    const KatmedWebResetStatus status=rt->reset_active ?
        KATMED_WEB_RESET_COMMITTED : rt->reset_requested ? KATMED_WEB_RESET_STOP_PENDING :
        rt->reset_warning_ready ? KATMED_WEB_RESET_WARNING :
        rt->reset_prepared ? KATMED_WEB_RESET_PREPARED :
        rt->reset_web_offer ? KATMED_WEB_RESET_PREPARING :
        rt->reset_web_rejected ? KATMED_WEB_RESET_UNAVAILABLE : KATMED_WEB_RESET_IDLE;
    xSemaphoreGive(reset_mutex);
    return status;
}

static KatmedWebResetCancelResult web_reset_cancel(void *context)
{
    Runtime *rt=context;
    if (!rt || !reset_mutex || xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(1000))!=pdTRUE)
        return KATMED_WEB_RESET_CANCEL_FAILED;
    if (rt->reset_requested || rt->reset_active) {
        xSemaphoreGive(reset_mutex);
        return KATMED_WEB_RESET_CANCEL_TOO_LATE;
    }
    if (!rt->reset_prepared && !rt->reset_warning_ready && !rt->reset_web_offer) {
        xSemaphoreGive(reset_mutex);
        return KATMED_WEB_RESET_CANCEL_UNAVAILABLE;
    }
    if (!reset_cancel_locked(rt)) {
        xSemaphoreGive(reset_mutex);
        return KATMED_WEB_RESET_CANCEL_FAILED;
    }
    rt->reset_web_close_generation=rt->reset_generation;
    rt->reset_web_close=true;
    xSemaphoreGive(reset_mutex);
    return KATMED_WEB_RESET_CANCELLED;
}

static bool reset_close_matches_locked(const Runtime *rt,uint64_t generation)
{
    return rt->reset_generation==generation && !rt->reset_prepared &&
        !rt->reset_warning_ready && !rt->reset_requested && !rt->reset_active;
}

static void reset_web_offer_complete(Runtime *rt,uint64_t epoch,bool opened)
{
    if (!opened && xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(100))==pdTRUE) {
        if (rt->reset_web_offer && rt->reset_web_offer_epoch==epoch) {
            rt->reset_web_offer=false;
            rt->reset_web_rejected=true;
        }
        xSemaphoreGive(reset_mutex);
    }
}

static bool web_device_info(void *context, KatmedWebDeviceInfo *out)
{
    Runtime *rt = context;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    snprintf(out->device_name, sizeof(out->device_name), "KatmedESP");
    (void)rt;
    katmed_mesh_device_name(out->device_name,sizeof(out->device_name),&out->assigned,&out->uncertain);
    return true;
}

static bool group_connected(void *context)
{
    Runtime *rt = (Runtime *)context;
    if(!rt)return false;
    DISPLAY_MB_LOCK(&display_mailbox);const bool connected=ui_owner_snapshot.connected;DISPLAY_MB_UNLOCK(&display_mailbox);
    return connected;
}

static bool historical_confirmation(void *context, ClockSample *historical)
{
    if(!context || !historical)return false;
    DISPLAY_MB_LOCK(&display_mailbox);
    const bool ok=ui_owner_snapshot.valid && ui_owner_snapshot.has_confirmation;
    if(ok)*historical=ui_owner_snapshot.confirmation;
    DISPLAY_MB_UNLOCK(&display_mailbox);return ok;
}
static uint8_t pending_confirmations(void *context)
{
    if(!context)return 0;
    DISPLAY_MB_LOCK(&display_mailbox);const uint8_t n=ui_owner_snapshot.pending;DISPLAY_MB_UNLOCK(&display_mailbox);return n;
}
static bool storage_ready(void *context)
{
    if(!context)return false;
    DISPLAY_MB_LOCK(&display_mailbox);const bool ready=ui_owner_snapshot.ready;DISPLAY_MB_UNLOCK(&display_mailbox);return ready;
}
static bool read_ui_model(void *context, AppState *state, LocalSettings *local, ClockSample *clock)
{
    if(!context || !state || !local || !clock)return false;
    DISPLAY_MB_LOCK(&display_mailbox);
    const bool ok=ui_owner_snapshot.valid;
    if(ok){*state=ui_owner_snapshot.state;*local=ui_owner_snapshot.local;*clock=ui_owner_snapshot.clock;}
    DISPLAY_MB_UNLOCK(&display_mailbox);return ok;
}
static bool durable_divergence(void *context)
{
    if(!context)return false;
    DISPLAY_MB_LOCK(&display_mailbox);const bool value=ui_owner_snapshot.divergent;DISPLAY_MB_UNLOCK(&display_mailbox);return value;
}
static uint8_t conflict_count(void *context)
{
    if(!context)return 0;
    DISPLAY_MB_LOCK(&display_mailbox);const uint8_t n=ui_owner_snapshot.conflicts;DISPLAY_MB_UNLOCK(&display_mailbox);return n;
}
static bool conflict_at(void *context, size_t index, UiConflict *out)
{
    if(!context || !out)return false;
    DISPLAY_MB_LOCK(&display_mailbox);
    const bool ok=index<ui_owner_snapshot.conflicts;
    if(ok)*out=ui_owner_snapshot.conflict[index];
    DISPLAY_MB_UNLOCK(&display_mailbox);return ok;
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
    /* The default API writes EUI-64 on C6. Device identity is factory MAC-48. */
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) != ESP_OK) return false;
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

static void display_touch_sample(const AppState *state,const BoardTouch *touch,bool valid)
{
    const bool allow=katmed_display_touch(&runtime.display_policy,valid,touch->down,
                                          state && state->alarm_active,touch->monotonic_ms);
    if(allow || !valid || (valid && !touch->down && runtime.display_policy.awake &&
                           !runtime.display_policy.consume_contact))
        ui_touch_sample(touch->down,touch->x,touch->y,valid,touch->monotonic_ms);
    else ui_touch_sample(false,0,0,false,touch->monotonic_ms);
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
    static Orientation projected_orientation;
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
    uint64_t emergency_observed,emergency_failed;
    display_mb_emergency_results(&display_mailbox,&emergency_observed,&emergency_failed);
    const UiRequest emergency={.kind=UI_REQUEST_EMERGENCY};
    /* One feedback per same-result batch; every press remains counted in RAM.
     * These completions never consume or overwrite the ordinary result. */
    if(emergency_observed)ui_deferred_result(&emergency,UI_SUBMIT_OBSERVED);
    if(emergency_failed)ui_deferred_result(&emergency,UI_SUBMIT_UNAVAILABLE);
    UiRequest completed_request;UiSubmitResult completed_result;
    if(display_mb_result(&display_mailbox,&completed_request,&completed_result))
        ui_deferred_result(&completed_request,completed_result);
    /* Deliver the initial PENDING_SHARED completion before its final result,
     * even if a previous render could not acquire the LVGL lock. */
    if(runtime.shared_feedback){
        if(runtime.shared_feedback==-2)ui_shared_timeout();
        else ui_shared_result(runtime.shared_feedback>0);
        runtime.shared_feedback=0;
    }
    ui_set_display_awake(!runtime.display_policy.initialized || runtime.display_policy.awake);
    if (contact_interrupted) {
        ui_touch_sample(false, 0, 0, false, clock->monotonic_ms);
        contact_interrupted = false;
    }
    /* A new alarm or round must reach the UI before a release is evaluated.
     * This holds even on the 25 ms tick between scheduled redraws. */
    const bool alarm_changed = state != NULL && local != NULL &&
        (!projected_state_available || projected_orientation!=local->orientation ||
        projected_alarm_active != state->alarm_active ||
        (state->alarm_active && !same_alarm_round(projected_round, state->round)));
    if (alarm_changed) {
        ui_render(state, local, ui_current_view(), clock);
        projected_orientation=local->orientation;
        projected_alarm_active = state->alarm_active;
        projected_round = state->round;
        projected_state_available = true;
    }
    if (touch != NULL) {
        display_touch_sample(state,touch,touch_valid);
        ui_set_display_awake(!runtime.display_policy.initialized || runtime.display_policy.awake);
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
        while (board_touch_sampler_take(&queued, &result)) {
            const bool valid=result==BOARD_OK && queued.monotonic_ms>=round_floor;
            display_touch_sample(state,&queued,valid);
        }
        ui_set_display_awake(!runtime.display_policy.initialized || runtime.display_policy.awake);
    }
#endif
    if (render_due && state != NULL && local != NULL)
        ui_render(state, local, ui_current_view(), clock);
    lvgl_port_unlock();
    return true;
}
#endif

#if KATMED_APPSTORE_MAINTENANCE && KATMED_FULL_STACK && KATMED_WITH_CORE
static bool appmaint_target_and_existing_guard(void)
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_EFUSE_FACTORY) != ESP_OK) return false;
    if (!katmed_appmaint_target_matches(mac)) return false;
    static const uint8_t marker[16] = {
        'K','A','T','M','E','D','-','N','V','S','-','S','1',0,0,1
    };
    const esp_partition_t *guard = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "nvs_guard");
    uint8_t observed[sizeof(marker)];
    return guard && guard->size >= sizeof(marker) &&
        esp_partition_read_raw(guard, 0, observed, sizeof(observed)) == ESP_OK &&
        memcmp(observed, marker, sizeof(marker)) == 0;
}

static void appmaint_status_ui(unsigned status)
{
    if (lvgl_port_lock(1000)) {
        ui_appmaint_show_status(status);
        lvgl_port_unlock();
    }
}

static void appmaint_main(void)
{
    static const char *tag = "katmed_appmaint";
    /* A failed identity or existing guard check reaches no NVS, FS or UI. */
    if (!appmaint_target_and_existing_guard()) {
        ESP_LOGE(tag, "target or existing guard unavailable; maintenance held");
        return;
    }
    if (katmed_nvs_init() != ESP_OK) {
        ESP_LOGE(tag, "existing encrypted NVS unavailable; maintenance held");
        return;
    }
    uint8_t stage = 0;
    KatmedAppmaintStatus status = katmed_reset_idf_maintenance_status(&stage);
    if (status == KATMED_APPMAINT_PENDING) {
        bool pending = true;
        if (stage != KATMED_RESET_FILES || katmed_reset_idf_maintenance_preflight()) {
            if (katmed_reset_resume(reset_backend(), &pending) && !pending)
                status = katmed_reset_idf_maintenance_status(NULL);
            else status = KATMED_APPMAINT_HOLD;
        } else status = KATMED_APPMAINT_HOLD;
    }
    if (status == KATMED_APPMAINT_ABSENT) {
        if (!katmed_reset_idf_take_preparation(&runtime.reset_warning_ready))
            status = KATMED_APPMAINT_HOLD;
        else runtime.reset_warning_ready = katmed_reset_warning_after_boot(
            runtime.reset_warning_ready, reset_reason_allows_warning(esp_reset_reason()));
    }
    if (!boot_identity(&runtime.boot)) return;
    reset_mutex = xSemaphoreCreateMutexStatic(&reset_mutex_storage);
    if (!reset_mutex) return;
    lv_display_t *display = NULL;
    if (board_init(&display) != BOARD_OK || !display) { board_deinit(); return; }
    runtime.local = (LocalSettings){.orientation=ORIENTATION_0,
        .clock_style=DIGITAL_LARGE,.device=runtime.boot.device};
    runtime.boot_domain = new_boot_domain(runtime.boot);
    katmed_time_init(&runtime.time, runtime.boot_domain);
    ClockSample now = clock_sample(runtime.boot_domain);
    SharedSettings defaults = app_default_settings();
    if (app_init(&runtime.state, &defaults, &now, runtime.boot, 0,
                 katmed_images_catalog_sha256()) != APP_OK ||
        board_set_orientation(runtime.local.orientation) != BOARD_OK ||
        !lvgl_port_lock(1000)) { board_deinit(); return; }
    const UiServices services = {
        .context = &runtime,
        .reset_arm = katmed_runtime_reset_arm,
        .reset_cancel = katmed_runtime_reset_cancel,
        .reset_prepare = katmed_runtime_reset_prepare,
        .reset_confirm = katmed_runtime_reset_confirm,
    };
    ui_set_services(&services);
    katmed_i18n_select(KATMED_LANG_EN);
    board_set_touch_filter(ui_lvgl_touch_allowed);
    ui_render(&runtime.state, &runtime.local, ui_current_view(), &now);
    ui_appmaint_show_status(status == KATMED_APPMAINT_ABSENT ? 0 :
                            status == KATMED_APPMAINT_COMPLETED ? 2 : 1);
    if (status == KATMED_APPMAINT_ABSENT && runtime.reset_warning_ready)
        ui_reset_show_warning();
    lvgl_port_unlock();
    if (board_touch_sampler_start() != BOARD_OK) return;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        if (status == KATMED_APPMAINT_ABSENT && reset_mutex &&
            xSemaphoreTake(reset_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            bool requested = runtime.reset_requested;
            runtime.reset_requested = false;
            if (requested) runtime.reset_active = true;
            xSemaphoreGive(reset_mutex);
            if (requested) {
                bool pending = true;
                if (katmed_reset_idf_maintenance_preflight() &&
                    katmed_reset_begin(reset_backend()) &&
                    katmed_reset_resume(reset_backend(), &pending) && !pending &&
                    katmed_reset_idf_maintenance_status(NULL) == KATMED_APPMAINT_COMPLETED)
                    status = KATMED_APPMAINT_COMPLETED;
                else status = KATMED_APPMAINT_HOLD;
                appmaint_status_ui(status == KATMED_APPMAINT_COMPLETED ? 2 : 1);
            }
        }
        now = clock_sample(runtime.boot_domain);
        (void)render_ui(&runtime.state, &runtime.local, &now, NULL, false, false);
    }
}
#endif

/* Fixed boot checkpoints only: no retained samples, heap tracing or admission
 * changes. Concurrent tasks can allocate between these sequential API reads;
 * differences locate a startup interval, not an exclusive allocation owner. */
static void startup_heap_sample(const char *stage)
{
    (void)stage; /* Also valid when informational logging is compiled out. */
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = internal | MALLOC_CAP_DMA;
    const size_t internal_free = heap_caps_get_free_size(internal);
    const size_t internal_largest = heap_caps_get_largest_free_block(internal);
    const size_t dma_free = heap_caps_get_free_size(dma);
    const size_t dma_largest = heap_caps_get_largest_free_block(dma);
    ESP_LOGI("katmed_heap",
             "startup stage=%s internal_free=%zu internal_largest=%zu "
             "dma_free=%zu dma_largest=%zu",
             stage, internal_free, internal_largest, dma_free, dma_largest);
}

/* The same startup path is exercised by the Host AppLoop harness. An explicit
 * offline Exit suppresses only the setup AP; it does not create Mesh trust. */
static bool start_runtime_network(Runtime *rt)
{
    const bool configured=rt->store_ready && rt->store.record.provision.configured;
    const bool setup_needed=rt->store_ready &&
        (rt->store.record.setup_requested ||
         (!configured && !rt->store.record.setup_dismissed));
    if (setup_needed && !start_setup_ap(rt))
        ESP_LOGE("katmed","setup access point unavailable");
    if (configured && !setup_needed &&
        rt->store.record.provision.network_mode!=STORE_NETWORK_OFFLINE) {
        katmed_mesh_history_backend(store_idf_history_backend(&rt->nvs));
        const KatmedMeshServices mesh_services = {
            .store = &rt->store,
            .state = &rt->state,
            .context = rt,
            .shared_result = mesh_shared_result,
            .utc_received = mesh_utc_received,
            .time_shareable = time_shareable,
            .startup_heap_sample = startup_heap_sample,
        };
        const KatmedMeshResult result=katmed_mesh_start(
            &rt->store.record.provision,&mesh_services);
        rt->mesh_started=result==MESH_ACCEPTED || result==MESH_PENDING;
        if (!rt->mesh_started) ESP_LOGE("katmed","group service unavailable: %d",result);
    }
    const bool management=setup_needed ||
        (configured && rt->store.record.provision.network_mode==STORE_NETWORK_ONLINE);
    katmed_web_set_enabled(management);
    return management;
}

void app_main(void)
{
#if KATMED_APPSTORE_MAINTENANCE
    appmaint_main();
    return;
#endif
#if KATMED_FULL_STACK && KATMED_WITH_CORE
    static const char *tag = "katmed";
    startup_heap_sample("entry");
#if CONFIG_NVS_ENCRYPTION
    /* Hold normal startup until encrypted NVS and any import journal are
     * verified. The IDF replay adapter reads the local Factory-MAC itself. */
    if (katmed_nvs_init() != ESP_OK) {
        ESP_LOGE(tag, "encrypted NVS unavailable; startup held for recovery");
        return;
    }
    if (!katmed_import_resume_idf()) {
        ESP_LOGE(tag, "migration journal unavailable; startup held for recovery");
        return;
    }
#endif
    startup_heap_sample("after_nvs_import");
    ESP_LOGI(tag, "Board/UI runtime with durable store and bounded group services");
    ESP_LOGI(tag, "free heap at entry: %" PRIu32, esp_get_free_heap_size());

    reset_mutex=xSemaphoreCreateMutexStatic(&reset_mutex_storage);
    if (!reset_mutex) {
        ESP_LOGE(tag,"reset confirmation lock unavailable");
        return;
    }

    /* Consume the marker before any later board or Store failure can cause a
     * second boot to reuse an old preparation. No user data is touched here. */
    if (!katmed_reset_idf_take_preparation(&runtime.reset_warning_ready)) {
        ESP_LOGE(tag,"reset preparation could not be consumed; normal startup is held");
        return;
    }
    /* CHIP_PU/ESP_RST may be reported as external or power-on reset. Power
     * restoration cannot be distinguished reliably here; it can only show
     * the warning and never starts the destructive journal. */
    const esp_reset_reason_t reset_reason=esp_reset_reason();
    runtime.reset_warning_ready=katmed_reset_warning_after_boot(
        runtime.reset_warning_ready,reset_reason_allows_warning(reset_reason));

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
    startup_heap_sample("after_board");

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(tag, "LVGL lock unavailable during startup");
        board_deinit();
        return;
    }
    katmed_images_register_lvgl_fs();
    lvgl_port_unlock();
    const KatmedImageStatus image_status = katmed_images_mount_partition();
    ESP_LOGI(tag, "read-only image partition status=%d", image_status);
    const KatmedImageStatus user_image_status = katmed_image_store_mount();
    ESP_LOGI(tag, "writable user image filesystem status=%d", user_image_status);
#if CONFIG_NVS_ENCRYPTION
    /* A relocated security partition must never turn a missing old image
     * filesystem into a normal boot with apparently absent user pictures. */
    if (user_image_status != KATMED_IMAGE_OK) {
        ESP_LOGE(tag, "secure user image filesystem unavailable; startup held");
        return;
    }
#endif
    if (!resume_reset_at_startup()) {
        ESP_LOGE(tag,"reset journal unavailable; normal startup is held");
        return;
    }
    /* Import replay, reset replay and mount cleanup have completed. No Store,
     * Mesh, Web, image upload or ACME service has started. Close this one-shot
     * window before opening the live Store backend. */
    katmed_source_snapshot_boot_window_open();
    const KatmedSourceExportSink *snapshot_sink = katmed_source_snapshot_boot_sink();
    if (snapshot_sink && !katmed_source_snapshot_boot(snapshot_sink,
                                                       source_snapshot_writers_idle))
        ESP_LOGE(tag, "source snapshot refused or incomplete");
    katmed_source_snapshot_boot_window_close();
    startup_heap_sample("after_images_reset");
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
    /* A software reboot after Open setup retains the durable manual intent.
     * CHIP_PU is reported as EXT or POWERON depending on the board; POWERON
     * also includes power restoration, which cannot be distinguished here.
     * Only a manual setup request is eligible. Untouched first boot still has
     * no request and retains its automatic onboarding AP.
     * A pending physical-reset warning keeps its own confirmation flow. */
    if (setup_exit_after_physical_boot(&runtime.store,runtime.store_ready,
                                       runtime.reset_warning_ready,reset_reason)!=STORE_OK)
        ESP_LOGE(tag,"manual setup exit could not be stored; setup remains active");
    const NamesResult names_result=katmed_mesh_names_backend(
        names_idf_backend(&runtime.names_nvs,&runtime.nvs),runtime.boot.device,
        runtime.store.record.provision.configured?runtime.store.record.provision.group:
                                                  runtime.store.record.bound_group);
    if(names_result!=NAMES_OK&&names_result!=NAMES_BUSY)ESP_LOGE(tag,"durable Mesh names unavailable: %d",names_result);
    runtime.boot_domain = new_boot_domain(runtime.boot);
    katmed_time_init(&runtime.time, runtime.boot_domain);
    ClockSample now = clock_sample(runtime.boot_domain);
    if (app_init(&runtime.state, &settings, &now, runtime.boot, image_count,
                 katmed_images_catalog_sha256()) != APP_OK) {
        ESP_LOGE(tag, "initial state unavailable");
        names_idf_close(&runtime.names_nvs);
        store_idf_close_backend(&runtime.nvs);
        katmed_images_unmount();
        board_deinit();
        return;
    }
    runtime.state.divergent = store_durable_divergent(&runtime.store);
    startup_heap_sample("after_store_app");
    if(esp_reset_reason()==ESP_RST_POWERON) manual_clock_marker=0;
    else if(manual_clock_marker==MANUAL_CLOCK_MARKER && runtime.store_ready && runtime.store.record.provision.network_mode!=STORE_NETWORK_ONLINE) {
        struct timeval wall;
        if(gettimeofday(&wall,NULL)==0 && katmed_time_accept_utc(&runtime.time,monotonic_ms(),(int64_t)wall.tv_sec*1000+wall.tv_usec/1000,CLOCK_HOLDOVER)) {
            ClockSample manual=clock_sample(runtime.boot_domain);
            if(apply_time_sample(&runtime,&manual)) runtime.utc_source=3;
        }
    }
    if (board_set_orientation(runtime.local.orientation) != BOARD_OK) {
        ESP_LOGE(tag, "initial orientation failed");
        names_idf_close(&runtime.names_nvs);
        store_idf_close_backend(&runtime.nvs);
        katmed_images_unmount();
        board_deinit();
        return;
    }
    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(tag, "LVGL lock unavailable for UI creation");
        names_idf_close(&runtime.names_nvs);
        store_idf_close_backend(&runtime.nvs);
        katmed_images_unmount();
        board_deinit();
        return;
    }
    runtime.display_policy.awake=true;runtime.backlight_applied=true;
    display_ui_publish(&runtime);
    const UiServices services = {
        .context = &runtime,
        .display_settings=display_settings,
        .brightness_preview=brightness_preview,
        .night_copy_status=night_copy_status,
        .orientation_auto_available = orientation_auto_supported(),
        .orientation_automatic = orientation_automatic,
        .submit = submit_ui_dispatch,
        .read_model = read_ui_model,
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
        .reset_arm = katmed_runtime_reset_arm,
        .reset_cancel = katmed_runtime_reset_cancel,
        .reset_prepare = katmed_runtime_reset_prepare,
        .reset_confirm = katmed_runtime_reset_confirm,
    };
    ui_set_services(&services);
    katmed_i18n_select(runtime.store_ready ? runtime.store.record.language : KATMED_LANG_EN);
    board_set_touch_filter(ui_lvgl_touch_allowed);
    ui_render(&runtime.state, &runtime.local, ui_current_view(), &now);
    if (runtime.reset_warning_ready) ui_reset_show_warning();
    lvgl_port_unlock();
    startup_heap_sample("after_ui");

    if (board_touch_sampler_start() != BOARD_OK) {
        ESP_LOGE(tag, "touch sampler unavailable");
        names_idf_close(&runtime.names_nvs);
        store_idf_close_backend(&runtime.nvs);
        katmed_images_unmount();
        board_deinit();
        return;
    }
    if (board_boot_button_init() != BOARD_OK)
        ESP_LOGE(tag, "BOOT input unavailable; app continues without button restart");

#if defined(ESP_PLATFORM) || defined(KATMED_PHASE_HOST_TEST)
    katmed_usb_console_phase_init(runtime.boot_domain, monotonic_ms,
                                  katmed_runtime_usb_phase_log);
#endif
    if (!katmed_usb_console_start())
        ESP_LOGE(tag, "USB setup input unavailable");

    startup_heap_sample("after_inputs");
    const bool management=start_runtime_network(&runtime);
    startup_heap_sample("after_network");
    if (!katmed_config_bridge_start())
        ESP_LOGE(tag,"web configuration bridge unavailable");
    else {
        startup_heap_sample("after_config_bridge");
        const KatmedWebServices web_services = {
            .context = &runtime,
            .prepare_listener_stop = web_prepare_listener_stop,
            .device_info = web_device_info,
            .language_get = web_language_get,
            .language_set = web_language_set,
            .wifi = {
                .configured = web_wifi_configured,
                .recovery_available = web_wifi_recovery_available,
                .profile_limit = web_wifi_profile_limit,
                .list_profiles = web_wifi_list_profiles,
                .save_profiles_atomic = web_wifi_save_profiles,
                .delete_profile_atomic = web_wifi_delete_profile,
                .schedule_reconnect = web_wifi_schedule_reconnect,
            },
            .reset = {
                .offer_on_display = web_reset_offer,
                .status = web_reset_status,
                .cancel_on_display = web_reset_cancel,
            },
        };
        if (management && !katmed_web_start(&web_services))
            ESP_LOGE(tag, "HTTPS configuration listener unavailable");
    }

    uint64_t last_render_ms = now.monotonic_ms;
#if KATMED_ACME_RESOURCE_PROBE
    bool acme_probe_launched = false;
#endif
    KatmedRuntimeMetricsGate metrics_gate = {0};
    BootRestartState boot_restart = {0};
    const BootRestartOps boot_ops = {
        .pin_high = boot_pin_high,
        .try_reset_lock = boot_try_reset_lock,
        .unlock_reset = boot_unlock_reset,
        .reset_busy = boot_reset_busy,
        .reset_prepared = boot_reset_prepared,
        .cancel_preparation = boot_cancel_preparation,
        .now_ms = boot_now_ms,
        .restart = boot_restart_now,
    };
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        if (reset_runtime_step(&runtime,monotonic_ms())) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const BootRestartResult boot_result = boot_restart_step(&boot_restart,
            &boot_ops, &runtime, board_boot_button_released(monotonic_ms()));
        if (boot_result == BOOT_RESTART_CANCEL_FAILED)
            ESP_LOGE(tag,"BOOT restart refused: reset preparation remains active");
        reboot_if_due(&runtime, monotonic_ms());
        now = clock_sample(runtime.boot_domain);
        if (runtime.mesh_started && !runtime.reboot_required)
            katmed_mesh_poll(&now);
        display_ui_poll(&runtime);
        const bool root_ip = runtime.mesh_started && katmed_mesh_root_has_ip();
#if KATMED_ACME_RESOURCE_PROBE
        if (root_ip && !acme_probe_launched)
            acme_probe_launched = katmed_acme_resource_probe_launch();
        katmed_acme_resource_probe_observe();
#endif
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
                    else {runtime.utc_source=1;manual_clock_marker=0;}
                }
            }
        }
        now = clock_sample(runtime.boot_domain);
        tick_runtime(&runtime, &now);
        display_policy_step(&runtime,&now);
        poll_auto_orientation(&runtime);
        process_usb(&runtime, &now);
        now = clock_sample(runtime.boot_domain);
        restore_manual_if_ready(&runtime,&now);
        katmed_config_poll(config_execute,&runtime);
        katmed_web_poll(now.monotonic_ms);
        bool reset_offer=false, reset_close=false, reset_expired=false;
        uint64_t reset_close_generation=0, reset_expired_generation=0;
        uint64_t reset_offer_epoch=0;
        if (xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(100))==pdTRUE) {
            reset_offer=runtime.reset_web_offer;
            reset_offer_epoch=runtime.reset_web_offer_epoch;
            reset_close=runtime.reset_web_close;
            reset_close_generation=runtime.reset_web_close_generation;
            reset_expired=runtime.reset_prepared &&
                now.monotonic_ms-runtime.reset_prepared_at_ms>90000u;
            reset_expired_generation=runtime.reset_generation;
            runtime.reset_web_close=false;
            xSemaphoreGive(reset_mutex);
        }
        if (reset_offer || reset_close || reset_expired) {
            if (lvgl_port_lock(100)) {
                if (reset_close) {
                    if (xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(100))==pdTRUE) {
                        const bool current=reset_close_matches_locked(&runtime,
                            reset_close_generation);
                        if (current) ui_reset_close_remote();
                        xSemaphoreGive(reset_mutex);
                    }
                }
                if (reset_expired && xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(100))==pdTRUE) {
                    const bool current=runtime.reset_generation==reset_expired_generation &&
                        runtime.reset_prepared;
                    const bool cancelled=current && reset_cancel_locked(&runtime);
                    xSemaphoreGive(reset_mutex);
                    if (cancelled)
                        ui_reset_close_remote();
                }
                if (reset_offer) {
                    reset_web_offer_complete(&runtime,reset_offer_epoch,
                        ui_reset_offer_from_web());
                }
                lvgl_port_unlock();
            } else if (xSemaphoreTake(reset_mutex,pdMS_TO_TICKS(100))==pdTRUE) {
                runtime.reset_web_close |= reset_close;
                xSemaphoreGive(reset_mutex);
            }
        }
        char pairing_code[7] = {0};
        bool pairing_visible = katmed_web_pairing_code(pairing_code);
        if (lvgl_port_lock(10)) {
            if (pairing_visible) ui_set_pairing_code(pairing_code);
            else ui_clear_pairing_code();
            lvgl_port_unlock();
        }
        memset(pairing_code, 0, sizeof(pairing_code));
        if (runtime.mesh_started && !runtime.reboot_required)
            interaction_drain(now.monotonic_ms);
        const bool render_due = now.monotonic_ms - last_render_ms >= 50;
        publish_ui_owner(&runtime);
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
