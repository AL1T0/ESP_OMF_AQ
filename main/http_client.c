// http_client.c
// OMF over HTTPS for ESP-IDF: OAuth2 client-credentials token, one-time OMF
// registration (persisted in NVS) and periodic data publishing.
//
// Design notes
// - The OMF connection is kept alive and reused between publishes (one TLS
//   handshake instead of one per minute). This saves time, power and, above
//   all, avoids allocating/freeing ~40 KB of TLS buffers every cycle, which
//   fragments the heap over long runs.
// - Response bodies go into fixed buffers (no realloc in the event handler).
// - The data payload is built with snprintf into a static buffer instead of
//   ~40 cJSON allocations per message.
// - The token task refreshes before expiry (from expires_in), retries with
//   backoff on failure, and is woken immediately if the server answers 401/403.
// - http_xfer_mutex keeps at most one TLS session active at a time to cap
//   peak heap usage.

#include "headers.h"
#include "keys.h"

static const char *TAG_H = "HTTP_HANDLER";

#define OMF_TS_LEN            25
#define TOKEN_RX_BUF_SIZE     4096   // token JSON (JWT is ~1-2 KB)
#define OMF_RX_BUF_SIZE       512    // OMF replies are short; only logged on error
#define OMF_PAYLOAD_BUF_SIZE  1536
#define HTTP_TIMEOUT_MS       10000

#define TOKEN_DEFAULT_LIFETIME_S  3600
#define TOKEN_RETRY_MIN_S         10
#define TOKEN_RETRY_MAX_S         300

// -----------------------------------------------------------------------------
// Shared state
// -----------------------------------------------------------------------------
static char             *s_oauth_token    = NULL;  // protected by s_token_mutex
static SemaphoreHandle_t s_token_mutex    = NULL;
static SemaphoreHandle_t s_http_xfer_mutex = NULL; // one TLS session at a time
static TaskHandle_t      s_token_task     = NULL;

static esp_http_client_handle_t s_omf_client = NULL; // persistent keep-alive client (publisher only)

static volatile uint32_t s_last_success_s = 0;     // uptime (s) of last accepted OMF message

static inline uint32_t uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000LL); }

uint32_t omf_seconds_since_last_success(void)
{
    return uptime_s() - s_last_success_s;
}

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------

// Formats system time as ISO8601 UTC "YYYY-MM-DDTHH:MM:SSZ".
static bool rtc_now_iso8601(char *dst, size_t cap)
{
    if (!dst || cap < 21) return false;
    time_t now = 0;
    time(&now);
    if (now < 1700000000) return false; // SNTP not synchronized yet
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    return (strftime(dst, cap, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) > 0);
}

// Returns a malloc'd "Bearer <token>" (caller frees) or NULL if no token.
static char *token_build_bearer_alloc(void)
{
    char *out = NULL;
    xSemaphoreTake(s_token_mutex, portMAX_DELAY);
    if (s_oauth_token) {
        size_t n = strlen(s_oauth_token) + 8; // "Bearer " + token + '\0'
        out = (char *)malloc(n);
        if (out) snprintf(out, n, "Bearer %s", s_oauth_token);
    }
    xSemaphoreGive(s_token_mutex);
    return out;
}

static void token_set(char *new_tok)
{
    xSemaphoreTake(s_token_mutex, portMAX_DELAY);
    char *old = s_oauth_token;
    s_oauth_token = new_tok;
    xSemaphoreGive(s_token_mutex);
    free(old);
}

// Masks a secret for logs, e.g. "abc*****xyz". Output is always NUL-terminated.
static void mask_id(const char *s, char *buf, size_t cap)
{
    size_t n = s ? strlen(s) : 0;
    if (!n) { snprintf(buf, cap, "(empty)"); return; }
    if (n >= cap) n = cap - 1;
    size_t show = (n > 6 ? 3 : 1);
    for (size_t i = 0; i < n; ++i) {
        buf[i] = (i < show || i >= n - show) ? s[i] : '*';
    }
    buf[n] = '\0';
}

// -----------------------------------------------------------------------------
// HTTP plumbing
// -----------------------------------------------------------------------------

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;        // capacity including the terminating NUL
    bool   truncated;
} http_rx_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_rx_ctx_t *rx = (http_rx_ctx_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rx && rx->buf && evt->data && evt->data_len > 0) {
        size_t room = rx->cap - 1 - rx->len;
        size_t n = (size_t)evt->data_len;
        if (n > room) { n = room; rx->truncated = true; }
        memcpy(rx->buf + rx->len, evt->data, n);
        rx->len += n;
        rx->buf[rx->len] = '\0';
    }
    return ESP_OK;
}

static inline void rx_reset(http_rx_ctx_t *rx)
{
    rx->len = 0;
    rx->truncated = false;
    if (rx->buf) rx->buf[0] = '\0';
}

// -----------------------------------------------------------------------------
// OAuth2
// -----------------------------------------------------------------------------

// Percent-encodes a string for application/x-www-form-urlencoded bodies
// (needed because Base64 client_secret values contain '+', '/', and '=').
static bool url_encode(const char *src, char *dst, size_t dst_cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src && src[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)src[i];
        bool unreserved = (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved) {
            if (j + 1 >= dst_cap) return false;
            dst[j++] = (char)c;
        } else {
            if (j + 3 >= dst_cap) return false;
            dst[j++] = '%';
            dst[j++] = hex[(c >> 4) & 0x0F];
            dst[j++] = hex[c & 0x0F];
        }
    }
    if (j >= dst_cap) return false;
    dst[j] = '\0';
    return true;
}

// Requests a token. On success returns a malloc'd token and sets *lifetime_s.
static char *retrieve_token(int *lifetime_s)
{
    static char rx_buf[TOKEN_RX_BUF_SIZE];
    http_rx_ctx_t rx = { .buf = rx_buf, .cap = sizeof(rx_buf) };
    rx_reset(&rx);

    char enc_id[128], enc_secret[192], enc_scope[128];
    if (!url_encode(CLIENT_ID, enc_id, sizeof(enc_id)) ||
        !url_encode(CLIENT_SECRET, enc_secret, sizeof(enc_secret)) ||
        !url_encode(OAUTH_SCOPE, enc_scope, sizeof(enc_scope))) {
        ESP_LOGE(TAG_H, "Failed to URL-encode OAuth credentials");
        return NULL;
    }

    char body[512];
    int blen = snprintf(body, sizeof(body),
                        "grant_type=client_credentials&client_id=%s&client_secret=%s&scope=%s",
                        enc_id, enc_secret, enc_scope);
    memset(enc_secret, 0, sizeof(enc_secret));
    if (blen <= 0 || blen >= (int)sizeof(body)) {
        ESP_LOGE(TAG_H, "Token request body too long");
        return NULL;
    }

    char masked[64];
    mask_id(CLIENT_ID, masked, sizeof(masked));
    ESP_LOGI(TAG_H, "Token request (client: %s)", masked);

    esp_http_client_config_t cfg = {
        .url               = CDS_AUTH,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .event_handler     = http_event_handler,
        .user_data         = &rx,
        .buffer_size       = 1024,
        .buffer_size_tx    = 1024,
        .keep_alive_enable = false,
        .auth_type         = HTTP_AUTH_TYPE_NONE,
    };

    xSemaphoreTake(s_http_xfer_mutex, portMAX_DELAY);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = ESP_FAIL;
    int status = 0;
    if (client) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        esp_http_client_set_post_field(client, body, blen);
        err = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
    }
    xSemaphoreGive(s_http_xfer_mutex);
    memset(body, 0, sizeof(body)); // do not leave the secret on the stack

    if (!client) {
        ESP_LOGE(TAG_H, "Failed to init HTTP client for token");
        return NULL;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG_H, "Token transport error: %s", esp_err_to_name(err));
        return NULL;
    }
    if (status != 200) {
        ESP_LOGE(TAG_H, "Token HTTP status=%d, body: %.200s", status, rx.len ? rx.buf : "<empty>");
        return NULL;
    }
    if (rx.truncated) {
        ESP_LOGE(TAG_H, "Token response larger than %d bytes", TOKEN_RX_BUF_SIZE);
        return NULL;
    }

    cJSON *json = cJSON_Parse(rx.buf);
    if (!json) {
        ESP_LOGE(TAG_H, "Token JSON parse failed. First bytes: '%.160s'", rx.buf);
        return NULL;
    }

    char *token = NULL;
    const cJSON *tok = cJSON_GetObjectItem(json, "access_token");
    const cJSON *exp = cJSON_GetObjectItem(json, "expires_in");
    if (cJSON_IsString(tok) && tok->valuestring) {
        token = strdup(tok->valuestring);
        *lifetime_s = cJSON_IsNumber(exp) && exp->valueint > 60 ? exp->valueint : TOKEN_DEFAULT_LIFETIME_S;
        ESP_LOGI(TAG_H, "Token OK (expires_in=%d s)", *lifetime_s);
    } else {
        ESP_LOGE(TAG_H, "No access_token in response");
    }
    cJSON_Delete(json);
    return token;
}

// Refreshes the token at ~80% of its lifetime, retries with exponential backoff
// on failure, and refreshes immediately when notified (publisher got 401/403).
void token_refresh_task(void *pvParameters)
{
    TickType_t wait = 0; // first token right away
    int backoff_s = TOKEN_RETRY_MIN_S;

    while (1) {
        // Sleep until it's time to refresh, or until someone asks for a new token
        ulTaskNotifyTake(pdTRUE, wait);

        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

        int lifetime_s = TOKEN_DEFAULT_LIFETIME_S;
        char *new_tok = retrieve_token(&lifetime_s);
        if (new_tok) {
            token_set(new_tok);
            backoff_s = TOKEN_RETRY_MIN_S;
            wait = pdMS_TO_TICKS((uint32_t)lifetime_s * 800U); // 80% of lifetime, in ms
            ESP_LOGI(TAG_H, "Token refreshed; next refresh in %d s", (lifetime_s * 8) / 10);
        } else {
            wait = pdMS_TO_TICKS((uint32_t)backoff_s * 1000U);
            ESP_LOGW(TAG_H, "Token refresh failed; retrying in %d s", backoff_s);
            backoff_s = MIN(backoff_s * 2, TOKEN_RETRY_MAX_S);
        }
    }
}

// -----------------------------------------------------------------------------
// OMF transport (persistent keep-alive connection)
// -----------------------------------------------------------------------------

static char s_omf_rx_buf[OMF_RX_BUF_SIZE];
static http_rx_ctx_t s_omf_rx = { .buf = s_omf_rx_buf, .cap = sizeof(s_omf_rx_buf) };

static esp_http_client_handle_t omf_client_get(void)
{
    if (s_omf_client) return s_omf_client;

    esp_http_client_config_t cfg = {
        .url               = CDS_OMF,
        .method            = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = HTTP_TIMEOUT_MS,
        .event_handler     = http_event_handler,
        .user_data         = &s_omf_rx,
        .buffer_size       = 1024,
        .buffer_size_tx    = 2048, // must fit request line + headers (bearer token ~1-2 KB)
        .keep_alive_enable = true,
        .auth_type         = HTTP_AUTH_TYPE_NONE,
    };
    s_omf_client = esp_http_client_init(&cfg);
    if (!s_omf_client) {
        ESP_LOGE(TAG_H, "Failed to init OMF HTTP client");
        return NULL;
    }
    esp_http_client_set_header(s_omf_client, "Content-Type",  "application/json; charset=utf-8");
    esp_http_client_set_header(s_omf_client, "Accept",        "application/json");
    esp_http_client_set_header(s_omf_client, "Accept-Encoding", "identity");
    esp_http_client_set_header(s_omf_client, "messageformat", "json");
    esp_http_client_set_header(s_omf_client, "omfversion",    "1.2");
    esp_http_client_set_header(s_omf_client, "action",        "create");
    return s_omf_client;
}

static void omf_client_drop(void)
{
    if (s_omf_client) {
        esp_http_client_cleanup(s_omf_client);
        s_omf_client = NULL;
    }
}

// Sends one OMF message. Returns the HTTP status (0 on transport error).
// A stale keep-alive socket (closed by the server while idle) shows up as a
// transport error; in that case the connection is rebuilt and the request
// retried once.
static int omf_post(const char *messagetype, const char *bearer, const char *payload)
{
    int status = 0;

    xSemaphoreTake(s_http_xfer_mutex, portMAX_DELAY);
    for (int attempt = 0; attempt < 2; ++attempt) {
        esp_http_client_handle_t c = omf_client_get();
        if (!c) break;

        esp_http_client_set_header(c, "Authorization", bearer);
        esp_http_client_set_header(c, "messagetype", messagetype);
        esp_http_client_set_post_field(c, payload, strlen(payload));
        rx_reset(&s_omf_rx);

        esp_err_t err = esp_http_client_perform(c);
        if (err == ESP_OK) {
            status = esp_http_client_get_status_code(c);
            break;
        }
        ESP_LOGW(TAG_H, "OMF %s transport error (%s), reconnecting%s",
                 messagetype, esp_err_to_name(err), attempt ? "" : " and retrying");
        omf_client_drop();
    }
    xSemaphoreGive(s_http_xfer_mutex);

    bool ok = (status >= 200 && status < 300) || status == 409;
    if (status && !ok) {
        ESP_LOGW(TAG_H, "OMF %s status=%d body: %.200s", messagetype, status,
                 s_omf_rx.len ? s_omf_rx.buf : "<empty>");
    }
    if (status == 401 || status == 403) {
        // Token expired or revoked: ask for a fresh one right away
        if (s_token_task) xTaskNotifyGive(s_token_task);
    }
    return status;
}

static inline bool omf_status_ok(int status)
{
    return (status >= 200 && status < 300) || status == 409; // 409: already exists
}

// -----------------------------------------------------------------------------
// NVS helpers to persist OMF registration state
// -----------------------------------------------------------------------------

static bool omf_is_registered_from_nvs(void)
{
    nvs_handle_t nvh;
    if (nvs_open("omf", NVS_READONLY, &nvh) != ESP_OK) return false;
    uint8_t flag = 0;
    esp_err_t err = nvs_get_u8(nvh, "reg_ok", &flag);
    nvs_close(nvh);
    return (err == ESP_OK && flag == 1);
}

static void omf_mark_registered_in_nvs(void)
{
    nvs_handle_t nvh;
    if (nvs_open("omf", NVS_READWRITE, &nvh) == ESP_OK) {
        nvs_set_u8(nvh, "reg_ok", 1);
        nvs_commit(nvh);
        nvs_close(nvh);
    }
}

// Call this if you intentionally bump the schema and need to re-register
__attribute__((unused)) static void omf_clear_registered_in_nvs(void)
{
    nvs_handle_t nvh;
    if (nvs_open("omf", NVS_READWRITE, &nvh) == ESP_OK) {
        nvs_erase_key(nvh, "reg_ok");
        nvs_commit(nvh);
        nvs_close(nvh);
    }
}

// -----------------------------------------------------------------------------
// OMF registration (type + containers)
// An alternative is creating Types and Containers manually, e.g. with curl:
/*curl --request POST 'https://uswe.datahub.connect.aveva.com/api/v1/Tenants/<tenant>/Namespaces/<namespace>/omf' \
--header 'Authorization: Bearer <token>' \
--header 'Content-Type: application/json' \
--header 'messagetype: container' \
--header 'messageformat: json' \
--header 'omfversion: 1.2' \
--header 'action: create' \
--data-raw '[{"id":"ESP32.CO2", "name":"ESP32_CO2", "typeid":"TimeIndexed.Int32","datasource":"ESP32", "extrapolation":"none"}]'*/
// -----------------------------------------------------------------------------

static const char OMF_TYPE_JSON[] =
    "[{\"id\":\"AQType\",\"type\":\"object\",\"classification\":\"dynamic\","
    "\"properties\":{"
        "\"Timestamp\":{\"type\":\"string\",\"format\":\"date-time\",\"isindex\":true},"
        "\"Temperature\":{\"type\":\"number\",\"format\":\"float64\"},"
        "\"Pressure\":{\"type\":\"number\",\"format\":\"float64\"},"
        "\"Humidity\":{\"type\":\"number\",\"format\":\"float64\"},"
        "\"CO2\":{\"type\":\"integer\",\"format\":\"int16\"},"
        "\"Light\":{\"type\":\"boolean\"}"
    "}}]";

// Composed stream with all values plus individual streams per measurement
static const char OMF_CONTAINER_JSON[] =
    "["
    "{\"id\":\"ESP32_Enviro\",\"typeid\":\"AQType\",\"datasource\":\"ESP32\",\"extrapolation\":\"none\"},"
    "{\"id\":\"ESP32.Temperature\",\"typeid\":\"DoubleTimeIndexed.Double\",\"datasource\":\"ESP32\",\"extrapolation\":\"none\"},"
    "{\"id\":\"ESP32.Pressure\",\"typeid\":\"DoubleTimeIndexed.Double\",\"datasource\":\"ESP32\",\"extrapolation\":\"none\"},"
    "{\"id\":\"ESP32.Humidity\",\"typeid\":\"DoubleTimeIndexed.Double\",\"datasource\":\"ESP32\",\"extrapolation\":\"none\"},"
    "{\"id\":\"ESP32.CO2\",\"typeid\":\"DoubleTimeIndexed.Double\",\"datasource\":\"ESP32\",\"extrapolation\":\"none\"},"
    "{\"id\":\"ESP32.Light\",\"typeid\":\"BooleanTimeIndexed.Boolean\",\"datasource\":\"ESP32\",\"extrapolation\":\"none\"}"
    "]";

static bool omf_register(const char *bearer)
{
    int st_type = omf_post("type", bearer, OMF_TYPE_JSON);
    ESP_LOGI(TAG_H, "OMF type status: %d", st_type);
    if (!omf_status_ok(st_type)) return false;

    int st_cont = omf_post("container", bearer, OMF_CONTAINER_JSON);
    ESP_LOGI(TAG_H, "OMF container status: %d", st_cont);
    if (!omf_status_ok(st_cont)) return false;

    omf_mark_registered_in_nvs();
    ESP_LOGI(TAG_H, "OMF registration complete (persisted in NVS)");
    return true;
}

// -----------------------------------------------------------------------------
// Data message
// -----------------------------------------------------------------------------

// Appends formatted text to buf; returns false if it does not fit.
static bool append(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *len) return false;
    *len += (size_t)n;
    return true;
}

// Builds the OMF data message. Invalid readings are left out (the compound
// record simply omits the property, and the individual stream gets no value).
static bool build_data_payload(char *buf, size_t cap, const char *ts, const measurement_q_t *m)
{
    size_t len = 0;
    bool ok = true;

    // Compound container
    ok &= append(buf, cap, &len, "[{\"containerid\":\"ESP32_Enviro\",\"values\":[{\"Timestamp\":\"%s\"", ts);
    if (m->bme_valid) {
        ok &= append(buf, cap, &len, ",\"Temperature\":%.2f,\"Pressure\":%.2f,\"Humidity\":%.2f",
                     m->temperature, m->pressure, m->humidity);
    }
    if (m->co2_valid) {
        ok &= append(buf, cap, &len, ",\"CO2\":%d", m->co2);
    }
    ok &= append(buf, cap, &len, ",\"Light\":%s}]}", m->light_state ? "true" : "false");

    // Individual streams
    if (m->bme_valid) {
        ok &= append(buf, cap, &len,
                     ",{\"containerid\":\"ESP32.Temperature\",\"values\":[{\"Timestamp\":\"%s\",\"Value\":%.2f}]}"
                     ",{\"containerid\":\"ESP32.Pressure\",\"values\":[{\"Timestamp\":\"%s\",\"Value\":%.2f}]}"
                     ",{\"containerid\":\"ESP32.Humidity\",\"values\":[{\"Timestamp\":\"%s\",\"Value\":%.2f}]}",
                     ts, m->temperature, ts, m->pressure, ts, m->humidity);
    }
    if (m->co2_valid) {
        ok &= append(buf, cap, &len,
                     ",{\"containerid\":\"ESP32.CO2\",\"values\":[{\"Timestamp\":\"%s\",\"Value\":%d}]}",
                     ts, m->co2);
    }
    ok &= append(buf, cap, &len,
                 ",{\"containerid\":\"ESP32.Light\",\"values\":[{\"Timestamp\":\"%s\",\"Value\":%s}]}]",
                 ts, m->light_state ? "true" : "false");
    return ok;
}

// Publishes the latest measurement from measurement_queue. Registers the OMF
// type/containers first if needed (retried every cycle until it succeeds).
void omf_publish_task(void *pvParameters)
{
    static char payload[OMF_PAYLOAD_BUF_SIZE];
    measurement_q_t data;
    bool registered = omf_is_registered_from_nvs();
    if (registered) ESP_LOGI(TAG_H, "OMF already registered (NVS), skipping");

    while (1) {
        if (xQueueReceive(measurement_queue, &data, portMAX_DELAY) != pdTRUE) continue;

        if (!(xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT)) {
            ESP_LOGW(TAG_H, "Skipping publish: Wi-Fi not connected");
            continue;
        }

        char ts[OMF_TS_LEN];
        if (!rtc_now_iso8601(ts, sizeof(ts))) {
            ESP_LOGW(TAG_H, "Skipping publish: RTC not synchronized");
            continue;
        }

        char *bearer = token_build_bearer_alloc();
        if (!bearer) {
            ESP_LOGW(TAG_H, "Skipping publish: no token yet");
            continue;
        }

        if (!registered) {
            registered = omf_register(bearer);
        }

        if (registered) {
            if (build_data_payload(payload, sizeof(payload), ts, &data)) {
                int status = omf_post("data", bearer, payload);
                ESP_LOGI(TAG_H, "OMF data status: %d", status);
                if (omf_status_ok(status)) {
                    s_last_success_s = uptime_s();
                }
            } else {
                ESP_LOGE(TAG_H, "OMF payload buffer too small");
            }
        }

        free(bearer); // was leaked on every publish in the previous version
    }
}

// Public entry point: creates sync primitives and launches the HTTP tasks.
void http_client_app_start(void)
{
    if (!s_http_xfer_mutex) s_http_xfer_mutex = xSemaphoreCreateMutex();
    if (!s_token_mutex)     s_token_mutex     = xSemaphoreCreateMutex();
    if (!s_http_xfer_mutex || !s_token_mutex) {
        ESP_LOGE(TAG_H, "Failed to create sync primitives");
        return;
    }

    s_last_success_s = uptime_s(); // stall timer starts at boot

    esp_log_level_set("esp-tls", ESP_LOG_WARN);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);

    xTaskCreate(token_refresh_task, "TokenRefresh", 6144, NULL, PRIO_TOKEN_REFRESH, &s_token_task);
    xTaskCreate(omf_publish_task,   "OmfPublish",   8192, NULL, PRIO_OMF_PUBLISH,   NULL);
}
