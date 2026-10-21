#include "katmed_acme.h"

#include <Acme.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <atomic>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if KATMED_ACME_RESOURCE_PROBE
#include "freertos/idf_additions.h"
#ifndef CONFIG_FREERTOS_USE_TICKLESS_IDLE
#define CONFIG_FREERTOS_USE_TICKLESS_IDLE 0
#endif
#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS || !CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64 || !CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#error "Resource probe requires opt-in 64-bit ESP timer runtime statistics"
#endif
#endif
#include "katmed_web.h"
#include "katmed_web_acme_route.h"
#include "katmed_source_export.h"

extern "C" bool katmed_acme_atomic_idle(void);

static const char *TAG = "katmed_acme";
static Acme *client;
static std::atomic<TaskHandle_t> client_owner;
static std::atomic<bool> acme_abort;
#if KATMED_ACME_RESOURCE_PROBE
static std::atomic<TaskHandle_t> probe_worker;
#endif
static bool certificate_changed;
static char challenge_host[254];

extern "C" bool katmed_acme_http_host_allowed(httpd_req_t *req)
{
    if (req == NULL || client == NULL ||
        req->handle != katmed_web_acme_http_server()) return false;
    size_t length = httpd_req_get_hdr_value_len(req, "Host");
    if (length == 0 || length > 256) return false;
    char supplied[257];
    if (httpd_req_get_hdr_value_str(req, "Host", supplied,
                                    sizeof(supplied)) != ESP_OK) return false;
    return katmed_web_acme_host_matches(challenge_host, supplied);
}

bool katmed_acme_start(const KatmedAcmeConfig *config)
{
    if (client_owner.load() != NULL) return client_owner.load() == xTaskGetCurrentTaskHandle();
    if (acme_abort.load()) return false;
    certificate_changed = false;
    if (config == NULL || config->identifier == NULL || config->email == NULL ||
        config->server == NULL || config->state_prefix == NULL ||
        config->identifier[0] == '\0' || config->email[0] == '\0' ||
        config->server[0] == '\0' || config->state_prefix[0] == '\0') {
        ESP_LOGI(TAG, "ACME disabled: secure configuration is not present");
        return false;
    }
    /* The reset journal owns exactly this LittleFS subtree. An alternate
     * prefix would leave private ACME state outside its cleanup contract. */
    if (strcmp(config->state_prefix, "/katmed/acme") != 0) {
        ESP_LOGE(TAG, "ACME refused: state directory is outside reset contract");
        return false;
    }
    if (!katmed_web_acme_host_matches(config->identifier, config->identifier)) {
        ESP_LOGE(TAG, "ACME refused: invalid challenge identifier");
        return false;
    }
    if (!katmed_source_writer_enter()) return false;
    httpd_handle_t server = katmed_web_acme_http_server();
    if (server == NULL) {
        ESP_LOGE(TAG, "ACME refused: Katmed HTTP-01 server is unavailable");
        katmed_source_writer_leave();
        return false;
    }
    client = new Acme();
    if (client == NULL) { katmed_source_writer_leave(); return false; }
    client_owner.store(xTaskGetCurrentTaskHandle());
    strlcpy(challenge_host, config->identifier, sizeof(challenge_host));
    client->setUrl(config->identifier);
    client->setEmail(config->email);
    client->setAcmeServer(config->server);
    client->setFilenamePrefix(config->state_prefix);
    client->setAccountFilename("account.json");
    client->setDirectoryFilename("directory.json");
    client->setAccountKeyFilename("account.pem");
    client->setOrderFilename("order.json");
    client->setCertKeyFilename("certkey.pem");
    client->setCertificateFilename("certificate.pem");
    client->setWebServer(server);
    ESP_LOGW(TAG, "ACME enabled with HTTP-01 only; no CA issuance is asserted");
    return true;
}

void katmed_acme_poll(uint64_t now_ms)
{
    (void)now_ms;
    if (client != NULL) certificate_changed = client->loop(time(NULL));
}

void katmed_acme_stop(void)
{
#if KATMED_ACME_RESOURCE_PROBE
    if (probe_worker != NULL && probe_worker != xTaskGetCurrentTaskHandle()) {
        acme_abort.store(true);
        return;
    }
#endif
    if (client_owner.load() != NULL && client_owner.load() != xTaskGetCurrentTaskHandle()) {
        acme_abort.store(true);
        return;
    }
    bool was_active = client != NULL;
    if (client != NULL && !client->Cleanup()) {
        ESP_LOGE(TAG, "ACME cleanup failed; writer lease retained");
        return;
    }
    delete client;
    client = NULL;
    certificate_changed = false;
    memset(challenge_host, 0, sizeof(challenge_host));
    if (was_active) katmed_source_writer_leave();
    client_owner.store(NULL);
}

bool katmed_acme_quiescent(void)
{
    /* A failed HTTP teardown retains the client and its writer lease. Atomic
     * slots additionally catch an orphaned file handle. */
#if KATMED_ACME_RESOURCE_PROBE
    if (probe_worker != NULL) return false;
#endif
    return client_owner.load() == NULL && katmed_acme_atomic_idle();
}

bool katmed_acme_certificate_changed(void)
{
    bool changed = certificate_changed;
    certificate_changed = false;
    return changed;
}

#if KATMED_ACME_RESOURCE_PROBE
/* The two builds share this observer and all normal services. Only the client
 * variant enters the official 1.1.1 API. Numeric hooks in the resolved copy
 * mark real upstream call boundaries; no phase is synthesized on failure. */
static unsigned probe_cycle;
static unsigned probe_sequence;
static unsigned probe_phase;
static std::atomic<bool> probe_running;
static std::atomic<bool> probe_monitor_valid;
static std::atomic<bool> probe_measurement_valid{true};
static bool probe_launched;
static std::atomic<bool> probe_success;
static bool probe_window;
static int64_t probe_due;
static BaseType_t probe_core;

/* All operation aggregates use the existing global local-minimum monitor.
 * Connection boundaries reset that monitor, but never reset these aggregates.
 * Main samples blocks at 10 Hz; operation hooks add points during blocking work. */
struct ProbeWindow {
    bool active;
    bool valid;
    unsigned attempt;
    unsigned first_seq;
    uint64_t start;
    uint64_t last;
    uint64_t gap;
    unsigned observations;
    size_t internal_min;
    size_t dma_min;
    size_t internal_block;
    size_t dma_block;
};
static ProbeWindow probe_windows[6];
static portMUX_TYPE probe_window_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t probe_block_due;
static const char *const probe_operations[] = {"challenge", "issuance", "renewal", "download", "key", "connection"};
static const char *probe_variant()
{
    return KATMED_ACME_RESOURCE_CLIENT ? "client" : "baseline";
}
static void probe_windows_sample(bool local)
{
    portENTER_CRITICAL(&probe_window_mux);
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint32_t in = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    const size_t im = local && probe_monitor_valid ? heap_caps_get_minimum_free_size(in) : heap_caps_get_free_size(in);
    const size_t dm = local && probe_monitor_valid ? heap_caps_get_minimum_free_size(dma) : heap_caps_get_free_size(dma);
    const size_t ib = heap_caps_get_largest_free_block(in);
    const size_t db = heap_caps_get_largest_free_block(dma);
    for (ProbeWindow &w : probe_windows) {
        if (!w.active) continue;
        if (local && (!probe_monitor_valid || !probe_measurement_valid)) w.valid = false;
        const uint64_t gap = now - w.last;
        if (gap > w.gap) w.gap = gap;
        w.last = now;
        ++w.observations;
        if (im < w.internal_min) w.internal_min = im;
        if (dm < w.dma_min) w.dma_min = dm;
        if (ib < w.internal_block) w.internal_block = ib;
        if (db < w.dma_block) w.dma_block = db;
    }
    portEXIT_CRITICAL(&probe_window_mux);
}
static void probe_window_log(unsigned op, const char *event, const char *status, const ProbeWindow &w)
{
    ESP_LOGI("katmed_acme_window",
        "schema=1 variant=%s cycle=%u op=%s attempt=%u event=%s status=%s "
        "start_seq=%u end_seq=%u start_us=%llu end_us=%llu observations=%u "
        "max_gap_us=%llu period_us=100000 internal_min=%u dma_min=%u "
        "internal_block_observed=%u dma_block_observed=%u valid=%u",
        probe_variant(), probe_cycle, probe_operations[op], w.attempt, event, status,
        w.first_seq, probe_sequence, (unsigned long long)w.start,
        (unsigned long long)w.last, w.observations, (unsigned long long)w.gap,
        (unsigned)w.internal_min, (unsigned)w.dma_min, (unsigned)w.internal_block,
        (unsigned)w.dma_block, w.valid ? 1u : 0u);
}
static void probe_window_begin(unsigned op)
{
    if (probe_windows[op].active) return; /* pending calls retain their parent */
    portENTER_CRITICAL(&probe_window_mux);
    ProbeWindow &w = probe_windows[op];
    const unsigned attempt = w.attempt + 1;
    w = {};
    w.active = true;
    w.valid = probe_monitor_valid && probe_measurement_valid;
    w.attempt = attempt;
    w.first_seq = probe_sequence;
    w.start = w.last = (uint64_t)esp_timer_get_time();
    w.internal_min = w.dma_min = w.internal_block = w.dma_block = SIZE_MAX;
    portEXIT_CRITICAL(&probe_window_mux);
    probe_windows_sample(false);
    portENTER_CRITICAL(&probe_window_mux);
    ProbeWindow first = w;
    portEXIT_CRITICAL(&probe_window_mux);
    first.last = first.start;
    first.gap = 0;
    probe_window_log(op, "begin", "running", first);
}
static void probe_window_end(unsigned op, const char *status)
{
    if (!probe_windows[op].active) return;
    probe_windows_sample(true);
    portENTER_CRITICAL(&probe_window_mux);
    ProbeWindow copy = probe_windows[op];
    probe_windows[op].active = false;
    portEXIT_CRITICAL(&probe_window_mux);
    probe_window_log(op, "end", status, copy);
}

struct ProbeCpuWindow {
    uint64_t start;
    uint64_t idle[configNUMBER_OF_CORES];
    const char *name;
    unsigned cycle;
};
static ProbeCpuWindow probe_cpu;
static void probe_cpu_begin(const char *name, unsigned cycle)
{
    probe_cpu.name = name;
    probe_cpu.cycle = cycle;
    probe_cpu.start = (uint64_t)esp_timer_get_time();
    for (BaseType_t core = 0; core < configNUMBER_OF_CORES; ++core)
        probe_cpu.idle[core] = ulTaskGetIdleRunTimeCounterForCore(core);
}
static void probe_cpu_end()
{
    const uint64_t end = (uint64_t)esp_timer_get_time();
    for (BaseType_t core = 0; core < configNUMBER_OF_CORES; ++core) {
        const uint64_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
        const uint64_t elapsed = end >= probe_cpu.start ? end - probe_cpu.start : 0;
        const uint64_t delta = idle >= probe_cpu.idle[core] ? idle - probe_cpu.idle[core] : 0;
        /* A wrap/regression, delayed accounting beyond the window, zero time,
         * sleep accounting or an oversized window is unavailable, never 0%. */
        const bool valid = end > probe_cpu.start && elapsed <= 3600000000ULL &&
            idle >= probe_cpu.idle[core] && delta <= elapsed && !CONFIG_FREERTOS_USE_TICKLESS_IDLE;
        ESP_LOGI("katmed_acme_cpu",
            "schema=1 variant=%s cycle=%u core=%u cores=%u window=%s "
            "start_us=%llu end_us=%llu idle_start=%llu idle_end=%llu idle_delta=%llu "
            "elapsed_us=%llu counter_bits=64 timer_hz=1000000 tickless=%u idle_priority_shared=1 valid=%u",
            probe_variant(), probe_cpu.cycle, (unsigned)core, (unsigned)configNUMBER_OF_CORES,
            probe_cpu.name, (unsigned long long)probe_cpu.start, (unsigned long long)end,
            (unsigned long long)probe_cpu.idle[core], (unsigned long long)idle,
            (unsigned long long)delta, (unsigned long long)elapsed,
            (unsigned)CONFIG_FREERTOS_USE_TICKLESS_IDLE, valid ? 1u : 0u);
    }
}

static const char *probe_phase_name(unsigned phase)
{
    static const char *const names[] = {
        "idle", "key", "connection", "challenge", "issuance",
        "renewal", "certificate_switch", "cleanup", "certificate_download", "released"
    };
    return phase < sizeof(names) / sizeof(names[0]) ? names[phase] : "invalid";
}

static void probe_sample(const char *status)
{
    if (probe_running) probe_windows_sample(true);
    const uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    ESP_LOGI("katmed_acme_resource",
        "schema=2 variant=%s cycle=%u seq=%u phase=%s status=%s uptime_ms=%llu "
        "internal_free=%u internal_min=%u internal_largest=%u "
        "dma_free=%u dma_min=%u dma_largest=%u monitor_valid=%u",
#if KATMED_ACME_RESOURCE_CLIENT
        "client",
#else
        "baseline",
#endif
        probe_cycle, ++probe_sequence, probe_phase_name(probe_phase), status,
        (unsigned long long)(esp_timer_get_time() / 1000),
        (unsigned)heap_caps_get_free_size(internal),
        (unsigned)(probe_running ? heap_caps_get_minimum_free_size(internal) : heap_caps_get_free_size(internal)),
        (unsigned)heap_caps_get_largest_free_block(internal),
        (unsigned)heap_caps_get_free_size(dma),
        (unsigned)(probe_running ? heap_caps_get_minimum_free_size(dma) : heap_caps_get_free_size(dma)),
        (unsigned)heap_caps_get_largest_free_block(dma),
        probe_monitor_valid && probe_measurement_valid ? 1u : 0u);
}

static void probe_next(unsigned phase)
{
    if (!probe_running || phase > 8) return;
    probe_sample("running");
    if (probe_monitor_valid && heap_caps_monitor_local_minimum_free_size_stop() != ESP_OK)
        probe_measurement_valid = false;
    probe_phase = phase;
    probe_monitor_valid = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
    if (!probe_monitor_valid) probe_measurement_valid = false;
}

extern "C" void katmed_acme_probe_mark(unsigned phase)
{
    if (!probe_running) return;
    if (phase == 9) { probe_sample("running"); return; }
    if (phase == 10 || phase == 13) {
        probe_sample("running");
        probe_window_end(3, phase == 13 ? "ok" : "error");
        probe_window = false;
        return;
    }
    if (phase == 11 || phase == 12) {
        probe_window_end(0, "error");
        if (phase == 12) probe_window_end(1, "error");
        return;
    }
    if (probe_window) return;
    probe_next(phase);
    probe_sample("running"); /* explicit entry bound for interval consumers */
    if (phase != 1) probe_window_end(4, "observed");
    if (phase != 2) probe_window_end(5, "observed");
    if (phase == 1) probe_window_begin(4);
    if (phase == 2) probe_window_begin(5);
    if (phase == 3) probe_window_begin(0);
    if (phase == 4) {
        probe_window_end(0, "ok");
        probe_window_begin(1);
    }
    if (phase == 5) probe_window_begin(2);
    if (phase == 8) {
        probe_window_begin(3);
        probe_window = true;
    }
    if (phase == 6) {
        probe_window_end(1, "ok");
        probe_window_end(2, "ok");
    }
}

static void probe_task(void *)
{
#if KATMED_ACME_RESOURCE_CLIENT
    /* A deliberately local, non-credential endpoint. A later authorized
     * fixture may serve it; without one, the real client aborts visibly. */
    static const KatmedAcmeConfig config = {
        "katmed-probe.invalid", "probe@invalid", KATMED_ACME_RESOURCE_SERVER,
        "/katmed/acme"
    };
#endif
#if KATMED_ACME_RESOURCE_CLIENT
    const unsigned cycle = probe_cycle;
#endif
    {
        probe_running = true;
        probe_monitor_valid = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
        if (!probe_monitor_valid) probe_measurement_valid = false;
        bool success = false;
#if KATMED_ACME_RESOURCE_CLIENT
        if (time(NULL) >= 1700000000 && katmed_acme_start(&config)) {
            if (client->getAccountKey() == 0) client->GenerateAccountKey();
            if (client->getCertificateKey() == 0) client->GenerateCertificateKey();
            if (client->getAccountKey() != 0 && client->getCertificateKey() != 0 &&
                client->CreateNewAccount()) {
                if (cycle == 1) client->CreateNewOrder();
                else if (client->HaveValidCertificate(time(NULL))) client->RenewCertificate();
                if (cycle == 1 || client->HaveValidCertificate(time(NULL))) {
                    const int64_t deadline = esp_timer_get_time() + 120000000LL;
                    while (!acme_abort.load() && esp_timer_get_time() < deadline) {
                        katmed_acme_poll((uint64_t)(esp_timer_get_time() / 1000));
                        if (katmed_acme_certificate_changed()) {
                            success = client->HaveValidCertificate(time(NULL));
                            break;
                        }
                        vTaskDelay(pdMS_TO_TICKS(1000));
                    }
                }
            }
        }
#else
        vTaskDelay(pdMS_TO_TICKS(12000));
        success = true;
#endif
        for (unsigned op=0; op<6; ++op) probe_window_end(op, "error");
        probe_next(7);
        do {
            katmed_acme_stop();
            if (client_owner.load() != NULL || !katmed_acme_atomic_idle())
                vTaskDelay(pdMS_TO_TICKS(100));
        } while (client_owner.load() != NULL || !katmed_acme_atomic_idle());
        probe_sample("running");
        if (probe_monitor_valid && heap_caps_monitor_local_minimum_free_size_stop() != ESP_OK)
            probe_monitor_valid = false;
        probe_running = false;
        probe_success = success && !acme_abort.load() && client_owner.load() == NULL &&
            katmed_acme_atomic_idle() && probe_monitor_valid && probe_measurement_valid;
    }
    /* Main observes the suspended state on this same pinned core, then deletes
     * the non-running task. IDF 6.1 vTaskDelete frees its TCB/stack synchronously
     * in this case, unlike self-deletion through the idle termination list. */
    vTaskSuspend(NULL);
}

bool katmed_acme_resource_probe_launch(void)
{
    if (probe_launched) return true;
    if (katmed_web_acme_http_server() == NULL) return false;
    probe_launched = true;
    probe_core = xPortGetCoreID();
    probe_due = esp_timer_get_time() + 12000000LL;
    probe_cpu_begin("before", 1);
    return true;
}
void katmed_acme_resource_probe_observe(void)
{
    if (!probe_launched || xPortGetCoreID() != probe_core) return;
    if (probe_running && esp_timer_get_time() >= probe_block_due) {
        probe_windows_sample(false);
        probe_block_due = esp_timer_get_time() + 100000;
    }
    if (probe_worker != NULL) {
        if (eTaskGetState(probe_worker) != eSuspended || probe_running.load() ||
            client_owner.load() != NULL || !katmed_acme_atomic_idle()) return;
        vTaskDelete(probe_worker);
        probe_worker = NULL;
        probe_cpu_end();
        probe_cpu_begin("after", probe_cycle);
        probe_phase = 9;
        probe_sample(probe_success ? "ok" : "error");
        probe_due = esp_timer_get_time() + 12000000LL;
        return;
    }
    if (esp_timer_get_time() < probe_due || probe_cpu.name == NULL) return;
    if (strcmp(probe_cpu.name, "after") == 0) {
        probe_cpu_end();
        probe_cpu.name = NULL;
        if (acme_abort.load() || probe_cycle == 3) return;
        probe_cpu_begin("before", probe_cycle + 1);
        probe_due = esp_timer_get_time() + 12000000LL;
        return;
    }
    if (acme_abort.load()) { probe_cpu_end(); probe_cpu.name = NULL; return; }
    probe_cpu_end();
    probe_cpu_begin("active", probe_cycle + 1);
    ++probe_cycle;
    probe_sequence = 0;
    memset(probe_windows, 0, sizeof(probe_windows));
    probe_phase = 0;
    probe_measurement_valid = true;
    probe_monitor_valid = true; /* direct snapshot, no local monitor allocated */
    probe_sample("running");
    TaskHandle_t created = NULL;
    BaseType_t result = xTaskCreatePinnedToCore(probe_task, "acme_resource", 12288,
        NULL, tskIDLE_PRIORITY + 1, &created, probe_core);
    if (result == pdPASS) probe_worker.store(created);
    if (result != pdPASS) {
        probe_phase = 9;
        probe_sample("error");
        acme_abort.store(true);
        probe_cpu_end();
        probe_cpu.name = NULL;
    }
}

#else
bool katmed_acme_resource_probe_launch(void) { return false; }
void katmed_acme_resource_probe_observe(void) {}
#endif

/* Main-loop listener owners request cancellation; only the originating task
 * may release the client and its task-local source lease. */
bool katmed_acme_prepare_listener_stop(void)
{
#if KATMED_ACME_RESOURCE_PROBE
    if (probe_launched) acme_abort.store(true);
#endif
    if (katmed_acme_quiescent()) return true;
    acme_abort.store(true);
    katmed_acme_stop();
    katmed_acme_resource_probe_observe();
    return katmed_acme_quiescent();
}
