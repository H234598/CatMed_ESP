#include "katmed_acme.h"

#include <Acme.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "katmed_web.h"
#include "katmed_web_acme_route.h"
#include "katmed_source_export.h"

extern "C" bool katmed_acme_atomic_idle(void);

static const char *TAG = "katmed_acme";
static Acme *client;
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
    if (client != NULL) return true;
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
}

bool katmed_acme_quiescent(void)
{
    /* A failed HTTP teardown retains the client and its writer lease. Atomic
     * slots additionally catch an orphaned file handle. */
    return client == NULL && katmed_acme_atomic_idle();
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
static bool probe_running;
static bool probe_monitor_valid;
static bool probe_launched;
static TaskHandle_t probe_worker;
static bool probe_success;
static bool probe_window;
static int64_t probe_due;
static BaseType_t probe_core;

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
        probe_monitor_valid ? 1u : 0u);
}

static void probe_next(unsigned phase)
{
    if (!probe_running || phase > 8) return;
    probe_sample("running");
    if (probe_monitor_valid && heap_caps_monitor_local_minimum_free_size_stop() != ESP_OK)
        probe_monitor_valid = false;
    probe_phase = phase;
    probe_monitor_valid = heap_caps_monitor_local_minimum_free_size_start() == ESP_OK;
}

extern "C" void katmed_acme_probe_mark(unsigned phase)
{
    if (!probe_running) return;
    if (phase == 8) {
        probe_next(8);
        probe_window = true;
        probe_sample("running");
    } else if (phase == 9 || phase == 10) {
        if (probe_window) probe_sample("running");
        if (phase == 10) probe_window = false;
    } else if (!probe_window) {
        probe_next(phase);
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
                    while (esp_timer_get_time() < deadline) {
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
        probe_next(7);
        katmed_acme_stop();
        probe_sample("running");
        if (probe_monitor_valid && heap_caps_monitor_local_minimum_free_size_stop() != ESP_OK)
            probe_monitor_valid = false;
        probe_running = false;
        probe_success = success && katmed_acme_quiescent() && probe_monitor_valid;
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
    return true;
}
void katmed_acme_resource_probe_observe(void)
{
    if (!probe_launched || xPortGetCoreID() != probe_core) return;
    if (probe_worker != NULL) {
        if (eTaskGetState(probe_worker) != eSuspended) return;
        vTaskDelete(probe_worker);
        probe_worker = NULL;
        probe_phase = 9;
        probe_sample(probe_success ? "ok" : "error");
        probe_due = esp_timer_get_time() + 12000000LL;
        return;
    }
    if (probe_cycle == 3 || esp_timer_get_time() < probe_due) return;
    ++probe_cycle;
    probe_sequence = 0;
    probe_phase = 0;
    probe_monitor_valid = true; /* direct snapshot, no local monitor allocated */
    probe_sample("running");
    BaseType_t result = xTaskCreatePinnedToCore(probe_task, "acme_resource", 12288,
        NULL, tskIDLE_PRIORITY + 1, &probe_worker, probe_core);
    if (result != pdPASS) {
        probe_phase = 9;
        probe_sample("error");
        probe_cycle = 3;
    }
}

#else
bool katmed_acme_resource_probe_launch(void) { return false; }
void katmed_acme_resource_probe_observe(void) {}
#endif
