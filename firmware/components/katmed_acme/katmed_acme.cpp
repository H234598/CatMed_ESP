#include "katmed_acme.h"

#include <Acme.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "katmed_web.h"
#include "katmed_source_export.h"

extern "C" bool katmed_acme_atomic_idle(void);

static const char *TAG = "katmed_acme";
static Acme *client;
static bool certificate_changed;

bool katmed_acme_start(const KatmedAcmeConfig *config)
{
    certificate_changed = false;
    if (client != NULL) return true;
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
    if (!katmed_source_writer_enter()) return false;
    httpd_handle_t server = katmed_web_acme_http_server();
    if (server == NULL) {
        ESP_LOGE(TAG, "ACME refused: Katmed HTTP-01 server is unavailable");
        katmed_source_writer_leave();
        return false;
    }
    client = new Acme();
    if (client == NULL) { katmed_source_writer_leave(); return false; }
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
    delete client;
    client = NULL;
    certificate_changed = false;
    if (was_active) katmed_source_writer_leave();
}

bool katmed_acme_quiescent(void)
{
    /* The client holds a writer lease for its entire lifetime. Inspect only
     * atomic slots here, whose lock also catches an orphaned file handle. */
    return katmed_acme_atomic_idle();
}

bool katmed_acme_certificate_changed(void)
{
    bool changed = certificate_changed;
    certificate_changed = false;
    return changed;
}
