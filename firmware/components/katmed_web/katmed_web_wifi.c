#include "katmed_web_wifi.h"

#include "esp_err.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIFI_BODY_MAX 8192u
#define WIFI_QUERY_MAX 48u

static KatmedWebWifiServices wifi_services;
#ifdef KATMED_WEB_WIFI_TEST_WIPE_OBSERVER
extern void katmed_web_wifi_test_wipe_observer(const void *memory, size_t length);
#endif

static void secure_wipe(void *memory, size_t length)
{
    const size_t original_length = length;
    volatile unsigned char *bytes = memory;
    while (length--) *bytes++ = 0;
#ifdef KATMED_WEB_WIFI_TEST_WIPE_OBSERVER
    katmed_web_wifi_test_wipe_observer(memory, original_length);
#else
    (void)original_length;
#endif
}

bool katmed_web_wifi_credentials_valid(const char *ssid, size_t ssid_length,
                                      const char *password, size_t password_length,
                                      KatmedWebWifiCredentialAction action)
{
    if (!ssid || ssid_length == 0 || ssid_length > 32 ||
        action < KATMED_WEB_WIFI_CREDENTIAL_KEEP ||
        action > KATMED_WEB_WIFI_CREDENTIAL_OPEN)
        return false;
    if (action == KATMED_WEB_WIFI_CREDENTIAL_OPEN)
        return password_length == 0;
    if (action == KATMED_WEB_WIFI_CREDENTIAL_KEEP)
        return password_length == 0;
    if (!password || password_length > 64) return false;
    if (password_length >= 8 && password_length <= 63) return true;
    if (password_length != 64) return false;
    for (size_t i = 0; i < password_length; ++i) {
        const char ch = password[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
              (ch >= 'A' && ch <= 'F')))
            return false;
    }
    return true;
}

static esp_err_t wifi_reply(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_error(httpd_req_t *req, const char *status,
                            const char *code, const char *message)
{
    char body[192];
    (void)snprintf(body, sizeof(body), "{\"error\":\"%s\",\"message\":\"%s\"}",
                   code, message);
    return wifi_reply(req, status, body);
}

static bool append_diagnostic_u32(char *body, size_t capacity, size_t *used,
                                  const char *name, uint32_t value)
{
    const int length=snprintf(body+*used,capacity-*used,
                              ",\"%s\":%" PRIu32,name,value);
    if (length<0 || (size_t)length>=capacity-*used) return false;
    *used+=(size_t)length;
    return true;
}

/* Relative to this exchange's admission start; absolute uptime stays internal. */
static uint32_t trace_offset_ms(const KatmedConfigBridgeTrace *trace, uint32_t tick)
{
    const uint64_t elapsed=(uint64_t)(uint32_t)(tick-trace->gate_start_tick)*
                           trace->tick_period_ms;
    return elapsed>UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
}

static esp_err_t wifi_status_error(httpd_req_t *req, unsigned stage,
                                   const KatmedWebWifiStatusDiagnostic *diagnostic)
{
    char body[1024];
    int length;
    if (stage <= 3u && diagnostic && diagnostic->available) {
        length = snprintf(body, sizeof(body),
            "{\"error\":\"wifi_unavailable\",\"message\":\"Der WLAN-Status ist derzeit nicht verfügbar.\","
            "\"status_stage\":%u,\"bridge\":%d,\"bridge_diag\":%u,\"ready_valid\":%u,\"ready\":%u",
            stage, diagnostic->bridge, diagnostic->bridge_diag,
            (unsigned)diagnostic->ready_valid, (unsigned)diagnostic->ready);
        if (length<0 || (size_t)length>=sizeof(body)) return ESP_ERR_NO_MEM;
        size_t used=(size_t)length;
        const KatmedConfigBridgeTrace *trace=&diagnostic->trace;
        const struct { uint32_t valid; const char *name; uint32_t value; } fields[]={
            {KATMED_CONFIG_TRACE_GATE_START,"trace_tick_ms",trace->tick_period_ms},
            {KATMED_CONFIG_TRACE_GATE_START,"admission_start_ms",0u},
            {KATMED_CONFIG_TRACE_GATE_START | KATMED_CONFIG_TRACE_GATE_RETURN,
                "gate_return_elapsed_ms",trace_offset_ms(trace,trace->gate_return_tick)},
            {KATMED_CONFIG_TRACE_GATE_RETURN,"gate_acquired",trace->gate_acquired},
            {KATMED_CONFIG_TRACE_GATE_START | KATMED_CONFIG_TRACE_ENQUEUE,
                "enqueue_elapsed_ms",trace_offset_ms(trace,trace->enqueue_tick)},
            {KATMED_CONFIG_TRACE_GATE_START | KATMED_CONFIG_TRACE_POLL_ENTER,
                "owner_poll_elapsed_ms",trace_offset_ms(trace,trace->poll_enter_tick)},
            {KATMED_CONFIG_TRACE_GATE_START | KATMED_CONFIG_TRACE_POLL_COMPLETE,
                "owner_complete_elapsed_ms",trace_offset_ms(trace,trace->poll_complete_tick)},
            {KATMED_CONFIG_TRACE_GATE_START | KATMED_CONFIG_TRACE_WAIT_RETURN,
                "wait_return_elapsed_ms",trace_offset_ms(trace,trace->wait_return_tick)},
            {KATMED_CONFIG_TRACE_WAIT_RETURN,"wait_elapsed_ms",trace->wait_elapsed_ms},
            {KATMED_CONFIG_TRACE_WAIT_RETURN,"wait_signalled",trace->wait_signalled},
            {KATMED_CONFIG_TRACE_GATE_START | KATMED_CONFIG_TRACE_DECISION,
                "decision_elapsed_ms",trace_offset_ms(trace,trace->decision_tick)},
            {KATMED_CONFIG_TRACE_DECISION,"slot_at_decision",trace->slot_at_decision},
            {KATMED_CONFIG_TRACE_PHASE_ENQUEUE,"owner_phase_enqueue",trace->owner_phase_at_enqueue},
            {KATMED_CONFIG_TRACE_PHASE_DECISION,"owner_phase_decision",trace->owner_phase_at_decision},
            {KATMED_CONFIG_TRACE_PHASE_SEEN,"owner_phase_seen",trace->owner_phase_seen_mask},
            {KATMED_CONFIG_TRACE_PHASE_SEEN,"render_call_overlap_ms",trace->render_overlap_ms},
        };
        for (size_t i=0;i<sizeof(fields)/sizeof(fields[0]);++i) {
            if ((trace->valid & fields[i].valid)==fields[i].valid &&
                !append_diagnostic_u32(body,sizeof(body),&used,fields[i].name,fields[i].value))
                return ESP_ERR_NO_MEM;
        }
        if (used+2u>sizeof(body)) return ESP_ERR_NO_MEM;
        body[used++]='}';
        body[used]='\0';
        return wifi_reply(req,"503 Service Unavailable",body);
    } else {
        length = snprintf(body, sizeof(body),
            "{\"error\":\"wifi_unavailable\",\"message\":\"Der WLAN-Status ist derzeit nicht verfügbar.\","
            "\"status_stage\":%u}", stage);
    }
    if (length < 0 || (size_t)length >= sizeof(body)) return ESP_ERR_NO_MEM;
    return wifi_reply(req, "503 Service Unavailable", body);
}

static bool authorize(httpd_req_t *req, bool mutate)
{
    return wifi_services.authorize &&
           wifi_services.authorize(req, mutate, wifi_services.context);
}

static bool get_limit(size_t *limit, KatmedWebWifiStatusDiagnostic *diagnostic)
{
    return wifi_services.profile_limit && limit &&
           wifi_services.profile_limit(wifi_services.context, limit, diagnostic);
}

static size_t escaped_length(const char *value)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\' || *p < 0x20) n += 6;
        else ++n;
    }
    return n;
}

static char *append_escaped(char *out, const char *value)
{
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { *out++ = '\\'; *out++ = (char)*p; }
        else if (*p < 0x20) {
            *out++ = '\\'; *out++ = 'u'; *out++ = '0'; *out++ = '0';
            *out++ = hex[*p >> 4]; *out++ = hex[*p & 0xf];
        } else *out++ = (char)*p;
    }
    return out;
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    bool configured = false, recovery = false;
    size_t limit = 0, count = 0, capacity = 0;
    KatmedWebWifiProfileView *profiles = NULL;
    char *body = NULL;
    char *cursor;
    size_t body_capacity = 128;
    if (!authorize(req, false)) return ESP_OK;
    KatmedWebWifiStatusDiagnostic diagnostic = {0};
    unsigned failed_stage = 0u;
    if (!wifi_services.configured(wifi_services.context, &configured, &diagnostic))
        failed_stage = 1u;
    if (!failed_stage) {
        diagnostic = (KatmedWebWifiStatusDiagnostic){0};
        if (!wifi_services.recovery_available(wifi_services.context, &recovery, &diagnostic))
            failed_stage = 2u;
    }
    if (!failed_stage) {
        diagnostic = (KatmedWebWifiStatusDiagnostic){0};
        if (!get_limit(&limit, &diagnostic)) failed_stage = 3u;
    }
    if (failed_stage) {
        ESP_LOGW("katmed_wifi", "get_failed stage=%u", failed_stage);
        return wifi_status_error(req, failed_stage, &diagnostic);
    }
    if (limit > 0) {
        profiles = calloc(limit, sizeof(*profiles));
        if (!profiles) {
            ESP_LOGW("katmed_wifi", "get_failed stage=%u", 4u);
            return wifi_status_error(req, 4u, NULL);
        }
    }
    capacity = limit;
    if (!wifi_services.list_profiles(wifi_services.context, profiles, capacity, &count) ||
        count > limit) {
        free(profiles);
        return wifi_error(req, "503 Service Unavailable", "wifi_unavailable",
                          "Die gespeicherten WLAN-Profile sind derzeit nicht verfügbar.");
    }
    for (size_t i = 0; i < count; ++i) {
        const size_t n = escaped_length(profiles[i].ssid);
        if (n > SIZE_MAX - body_capacity - 128) { free(profiles); return ESP_ERR_NO_MEM; }
        body_capacity += n + 128;
    }
    body = malloc(body_capacity);
    if (!body) { free(profiles); return ESP_ERR_NO_MEM; }
    cursor = body;
    cursor += (size_t)snprintf(cursor, body_capacity,
        "{\"configured\":%s,\"recovery_available\":%s,\"profile_limit\":%zu,\"profiles\":[",
        configured ? "true" : "false", recovery ? "true" : "false", limit);
    for (size_t i = 0; i < count; ++i) {
        if (i) *cursor++ = ',';
        cursor += (size_t)snprintf(cursor, body_capacity - (size_t)(cursor - body),
                                   "{\"id\":%" PRIu32 ",\"ssid\":\"", profiles[i].id);
        cursor = append_escaped(cursor, profiles[i].ssid);
        cursor += (size_t)snprintf(cursor, body_capacity - (size_t)(cursor - body),
            "\",\"priority\":%" PRIu32 ",\"secured\":%s}", profiles[i].priority,
            profiles[i].secured ? "true" : "false");
    }
    (void)snprintf(cursor, body_capacity - (size_t)(cursor - body), "]}");
    free(profiles);
    const esp_err_t result = wifi_reply(req, "200 OK", body);
    free(body);
    return result;
}

static bool read_body(httpd_req_t *req, char **out, size_t *length)
{
    if (!out || !length || req->content_len <= 0 ||
        (size_t)req->content_len > WIFI_BODY_MAX) return false;
    char *body = malloc((size_t)req->content_len + 1u);
    if (!body) return false;
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        const int count = httpd_req_recv(req, body + received,
                                         (size_t)req->content_len - received);
        if (count <= 0) { secure_wipe(body, (size_t)req->content_len + 1u); free(body); return false; }
        received += (size_t)count;
    }
    body[received] = '\0';
    *out = body;
    *length = received;
    return true;
}

typedef struct {
    const char *input;
    size_t length;
    size_t offset;
} JsonCursor;

static void json_skip_space(JsonCursor *cursor)
{
    while (cursor->offset < cursor->length) {
        const char c = cursor->input[cursor->offset];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++cursor->offset;
    }
}

static bool json_expect(JsonCursor *cursor, char expected)
{
    json_skip_space(cursor);
    if (cursor->offset >= cursor->length || cursor->input[cursor->offset] != expected)
        return false;
    ++cursor->offset;
    return true;
}

static bool json_append_codepoint(char *output, size_t capacity, size_t *length,
                                  uint32_t codepoint)
{
    unsigned char bytes[4];
    size_t count;
    if (codepoint == 0 || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) return false;
    if (codepoint <= 0x7f) { bytes[0] = (unsigned char)codepoint; count = 1; }
    else if (codepoint <= 0x7ff) {
        bytes[0] = (unsigned char)(0xc0 | (codepoint >> 6));
        bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3f)); count = 2;
    } else if (codepoint <= 0xffff) {
        bytes[0] = (unsigned char)(0xe0 | (codepoint >> 12));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3f)); count = 3;
    } else {
        bytes[0] = (unsigned char)(0xf0 | (codepoint >> 18));
        bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f));
        bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f));
        bytes[3] = (unsigned char)(0x80 | (codepoint & 0x3f)); count = 4;
    }
    if (count >= capacity - *length) return false;
    memcpy(output + *length, bytes, count);
    *length += count;
    return true;
}

static bool json_hex4(JsonCursor *cursor, uint32_t *value)
{
    uint32_t result = 0;
    if (cursor->length - cursor->offset < 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        const char c = cursor->input[cursor->offset++];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
        else return false;
        result = (result << 4) | digit;
    }
    *value = result;
    return true;
}

static bool json_string(JsonCursor *cursor, char *output, size_t capacity,
                        size_t *length_out)
{
    size_t length = 0;
    if (!output || capacity == 0 || !json_expect(cursor, '"')) return false;
    while (cursor->offset < cursor->length) {
        const unsigned char c = (unsigned char)cursor->input[cursor->offset++];
        if (c == '"') {
            output[length] = '\0';
            if (length_out) *length_out = length;
            return true;
        }
        if (c < 0x20) return false;
        if (c != '\\') {
            if (length + 1 >= capacity) return false;
            output[length++] = (char)c;
            continue;
        }
        if (cursor->offset >= cursor->length) return false;
        const char escape = cursor->input[cursor->offset++];
        switch (escape) {
        case '"': case '\\': case '/':
            if (length + 1 >= capacity) return false;
            output[length++] = escape;
            break;
        case 'b': case 'f': case 'n': case 'r': case 't':
            /* Control characters are not valid SSID/password bytes here. */
            return false;
        case 'u': {
            uint32_t codepoint;
            if (!json_hex4(cursor, &codepoint)) return false;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if (cursor->length - cursor->offset < 6 ||
                    cursor->input[cursor->offset] != '\\' ||
                    cursor->input[cursor->offset + 1] != 'u') return false;
                cursor->offset += 2;
                uint32_t low;
                if (!json_hex4(cursor, &low) || low < 0xdc00 || low > 0xdfff) return false;
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
            }
            if (!json_append_codepoint(output, capacity, &length, codepoint)) return false;
            break;
        }
        default: return false;
        }
    }
    return false;
}

static bool json_uint32(JsonCursor *cursor, uint32_t *out)
{
    uint32_t value = 0;
    json_skip_space(cursor);
    if (cursor->offset >= cursor->length || cursor->input[cursor->offset] < '0' ||
        cursor->input[cursor->offset] > '9') return false;
    if (cursor->input[cursor->offset] == '0' && cursor->offset + 1 < cursor->length &&
        cursor->input[cursor->offset + 1] >= '0' && cursor->input[cursor->offset + 1] <= '9')
        return false;
    do {
        const uint32_t digit = (uint32_t)(cursor->input[cursor->offset++] - '0');
        if (value > (UINT32_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
    } while (cursor->offset < cursor->length && cursor->input[cursor->offset] >= '0' &&
             cursor->input[cursor->offset] <= '9');
    *out = value;
    return true;
}

static bool parse_profile(JsonCursor *cursor, KatmedWebWifiProfileUpdate *profile)
{
    enum { SEEN_ID=1u, SEEN_SSID=2u, SEEN_PRIORITY=4u, SEEN_CREDENTIAL=8u, SEEN_PASSWORD=16u };
    unsigned seen = 0;
    size_t ssid_length = 0, password_length = 0;
    char key[16], credential[9];
    if (!json_expect(cursor, '{')) return false;
    json_skip_space(cursor);
    if (cursor->offset < cursor->length && cursor->input[cursor->offset] == '}') return false;
    for (;;) {
        size_t key_length = 0;
        if (!json_string(cursor, key, sizeof(key), &key_length) || !json_expect(cursor, ':')) return false;
        if (strcmp(key, "id") == 0) {
            if ((seen & SEEN_ID) || !json_uint32(cursor, &profile->id)) return false;
            seen |= SEEN_ID;
        } else if (strcmp(key, "ssid") == 0) {
            if ((seen & SEEN_SSID) || !json_string(cursor, profile->ssid,
                    sizeof(profile->ssid), &ssid_length)) return false;
            seen |= SEEN_SSID;
        } else if (strcmp(key, "priority") == 0) {
            if ((seen & SEEN_PRIORITY) || !json_uint32(cursor, &profile->priority)) return false;
            seen |= SEEN_PRIORITY;
        } else if (strcmp(key, "credential") == 0) {
            if ((seen & SEEN_CREDENTIAL) || !json_string(cursor, credential,
                    sizeof(credential), NULL)) return false;
            if (strcmp(credential, "keep") == 0)
                profile->credential_action = KATMED_WEB_WIFI_CREDENTIAL_KEEP;
            else if (strcmp(credential, "replace") == 0)
                profile->credential_action = KATMED_WEB_WIFI_CREDENTIAL_REPLACE;
            else if (strcmp(credential, "open") == 0)
                profile->credential_action = KATMED_WEB_WIFI_CREDENTIAL_OPEN;
            else return false;
            seen |= SEEN_CREDENTIAL;
        } else if (strcmp(key, "password") == 0) {
            if ((seen & SEEN_PASSWORD) || !json_string(cursor, profile->password,
                    sizeof(profile->password), &password_length)) return false;
            seen |= SEEN_PASSWORD;
        } else {
            return false;
        }
        json_skip_space(cursor);
        if (cursor->offset < cursor->length && cursor->input[cursor->offset] == '}') {
            ++cursor->offset;
            break;
        }
        if (!json_expect(cursor, ',')) return false;
    }
    if ((seen & (SEEN_SSID|SEEN_PRIORITY|SEEN_CREDENTIAL|SEEN_PASSWORD)) !=
        (SEEN_SSID|SEEN_PRIORITY|SEEN_CREDENTIAL|SEEN_PASSWORD) || profile->priority == 0 ||
        !katmed_web_wifi_credentials_valid(profile->ssid, ssid_length, profile->password,
                                            password_length, profile->credential_action))
        return false;
    return true;
}

static bool parse_profiles(const char *body, size_t body_length, size_t limit,
                           KatmedWebWifiProfileUpdate *profiles, size_t *count_out)
{
    JsonCursor cursor = {.input = body, .length = body_length, .offset = 0};
    char key[16];
    size_t count = 0;
    if (!json_expect(&cursor, '{') || !json_string(&cursor, key, sizeof(key), NULL) ||
        strcmp(key, "profiles") != 0 || !json_expect(&cursor, ':') ||
        !json_expect(&cursor, '[')) return false;
    json_skip_space(&cursor);
    if (cursor.offset < cursor.length && cursor.input[cursor.offset] != ']') {
        for (;;) {
            if (count >= limit || !parse_profile(&cursor, &profiles[count])) return false;
            for (size_t j = 0; j < count; ++j) {
                if ((profiles[count].id && profiles[j].id == profiles[count].id) ||
                    strcmp(profiles[j].ssid, profiles[count].ssid) == 0) return false;
            }
            ++count;
            json_skip_space(&cursor);
            if (cursor.offset < cursor.length && cursor.input[cursor.offset] == ']') break;
            if (!json_expect(&cursor, ',')) return false;
        }
    }
    if (!json_expect(&cursor, ']') || !json_expect(&cursor, '}')) return false;
    json_skip_space(&cursor);
    if (cursor.offset != cursor.length) return false;
    *count_out = count;
    return true;
}

static esp_err_t wifi_profiles_save(httpd_req_t *req)
{
    char content_type[48];
    char *body = NULL;
    size_t body_length = 0, limit = 0, count = 0;
    KatmedWebWifiProfileUpdate *profiles = NULL;
    if (!authorize(req, true)) return ESP_OK;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK ||
        strcmp(content_type, "application/json") != 0) {
        (void)httpd_resp_set_hdr(req, "Connection", "close");
        return wifi_error(req, "415 Unsupported Media Type", "wifi_input_type",
                          "Die WLAN-Profile müssen als JSON übertragen werden.");
    }
    if (!get_limit(&limit, NULL) || !read_body(req, &body, &body_length)) {
        (void)httpd_resp_set_hdr(req, "Connection", "close");
        return wifi_error(req, "413 Content Too Large", "wifi_input_size",
                          "Die WLAN-Profileingabe ist zu groß oder unvollständig.");
    }
    if (limit > SIZE_MAX / sizeof(*profiles)) {
        secure_wipe(body, body_length); free(body);
        return wifi_error(req, "503 Service Unavailable", "wifi_unavailable",
                          "Der WLAN-Speicher ist derzeit nicht verfügbar.");
    }
    if (limit) profiles = calloc(limit, sizeof(*profiles));
    if ((limit && !profiles) || !parse_profiles(body, body_length, limit, profiles, &count)) {
        if (profiles) { secure_wipe(profiles, limit * sizeof(*profiles)); free(profiles); }
        secure_wipe(body, body_length); free(body);
        return wifi_error(req, "422 Unprocessable Content", "wifi_input",
                          "WLAN-Profile, Priorität oder Schlüssel haben ein ungültiges Format.");
    }
    if (!wifi_services.save_profiles_atomic(wifi_services.context, profiles, count)) {
        if (profiles) { secure_wipe(profiles, limit * sizeof(*profiles)); free(profiles); }
        secure_wipe(body, body_length); free(body);
        return wifi_error(req, "503 Service Unavailable", "wifi_save_failed",
                          "Die WLAN-Profile konnten nicht dauerhaft gespeichert werden.");
    }
    if (profiles) { secure_wipe(profiles, limit * sizeof(*profiles)); free(profiles); }
    secure_wipe(body, body_length); free(body);
    return wifi_reply(req, "200 OK", "{\"saved\":true,\"reconnect_required\":true}");
}

static esp_err_t wifi_profile_delete(httpd_req_t *req)
{
    char query[WIFI_QUERY_MAX], id_text[16];
    uint32_t id = 0;
    if (!authorize(req, true)) return ESP_OK;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id_text, sizeof(id_text)) != ESP_OK ||
        !id_text[0])
        return wifi_error(req, "400 Bad Request", "wifi_id", "WLAN-Profilkennung fehlt.");
    for (const char *p = id_text; *p; ++p) {
        if (*p < '0' || *p > '9' || id > (UINT32_MAX - (uint32_t)(*p - '0')) / 10u)
            return wifi_error(req, "400 Bad Request", "wifi_id", "WLAN-Profilkennung ist ungültig.");
        id = id * 10u + (uint32_t)(*p - '0');
    }
    if (!id || !wifi_services.delete_profile_atomic(wifi_services.context, id))
        return wifi_error(req, "503 Service Unavailable", "wifi_delete_failed",
                          "Das WLAN-Profil konnte nicht entfernt werden.");
    return wifi_reply(req, "200 OK", "{\"removed\":true}");
}

static esp_err_t wifi_reconnect(httpd_req_t *req)
{
    if (!authorize(req, true)) return ESP_OK;
    if (!wifi_services.schedule_reconnect(wifi_services.context))
        return wifi_error(req, "503 Service Unavailable", "wifi_reconnect_failed",
                          "Die WLAN-Neuverbindung konnte nicht geplant werden.");
    return wifi_reply(req, "202 Accepted",
                      "{\"scheduled\":true,\"reevaluate_priority\":true,\"address_may_change\":true}");
}

bool katmed_web_wifi_register(httpd_handle_t server,
                              const KatmedWebWifiServices *services)
{
    if (!server || !services || !services->context || !services->authorize ||
        !services->configured || !services->recovery_available || !services->profile_limit ||
        !services->list_profiles || !services->save_profiles_atomic ||
        !services->delete_profile_atomic || !services->schedule_reconnect)
        return false;
    wifi_services = *services;
    const httpd_uri_t routes[] = {
        {.uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_get, .user_ctx = NULL},
        {.uri = "/api/wifi/profiles", .method = HTTP_PUT, .handler = wifi_profiles_save, .user_ctx = NULL},
        {.uri = "/api/wifi/profiles", .method = HTTP_DELETE, .handler = wifi_profile_delete, .user_ctx = NULL},
        {.uri = "/api/wifi/reconnect", .method = HTTP_POST, .handler = wifi_reconnect, .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        if (httpd_register_uri_handler(server, &routes[i]) != ESP_OK) return false;
    return true;
}
