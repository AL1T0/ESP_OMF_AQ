// http_client.c
// HTTP client for OMF over ESP-IDF: OAuth2, one-time OMF register (NVS persisted), data publish.
// Concurrency: http_xfer_mutex serializes any HTTP transfer (refresh/register/publish).
// token_mutex protects the oauth_token pointer. http_semaphore signals "token available".
// Comments: English & professional.

#include "headers.h"
#include "keys.h"

static const char *TAG_H = "HTTP_HANDLER";
#define OMF_TS_LEN 25

// Refresh interval (can be adjusted dynamically using expires_in if desired)
static const TickType_t token_refresh_interval = pdMS_TO_TICKS(3000000);

// -----------------------------------------------------------------------------
// Shared state & synchronization (definitions)
// -----------------------------------------------------------------------------
char *oauth_token                 = NULL;  // protected by token_mutex
SemaphoreHandle_t http_semaphore  = NULL;  // binary: "token available"
SemaphoreHandle_t http_xfer_mutex = NULL;  // mutex: any HTTP transfer (refresh / register / publish)
SemaphoreHandle_t token_mutex     = NULL;  // mutex: protect oauth_token pointer

// Provided elsewhere
extern QueueHandle_t measurement_queue;

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------

// Formats system time (RTC) as ISO8601 UTC "YYYY-MM-DDTHH:MM:SSZ".
static bool rtc_now_iso8601(char *dst, size_t cap)
{
    if (!dst || cap < 21) return false; // minimal capacity for "YYYY-MM-DDTHH:MM:SSZ"
    time_t now = 0;
    time(&now);
    if (now < 1700000000) return false; // sanity: SNTP not synchronized yet
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    return (strftime(dst, cap, "%Y-%m-%dT%H:%M:%SZ", &tm_utc) > 0);
}

// Builds "Bearer <token>" into out[] safely; returns false if token not available.
static char *token_build_bearer_alloc(void) {
    char *out = NULL;
    xSemaphoreTake(token_mutex, portMAX_DELAY);
    if (oauth_token) {
        size_t n = strlen(oauth_token) + 8; // "Bearer " + token + '\0'
        out = (char *)malloc(n);
        if (out) snprintf(out, n, "Bearer %s", oauth_token);
    }
    xSemaphoreGive(token_mutex);
    return out;
}
// Mask helper for logs (do not print secrets in clear).
static const char* mask_id(const char *s) {
    static char buf[64];
    size_t n = s ? strlen(s) : 0;
    if (!n) { strcpy(buf,"(empty)"); return buf; }
    size_t keep = (n > 6 ? 3 : 1), show = (n > 6 ? 3 : 1), i = 0;
    for (; i < keep && i < n; ++i) buf[i] = s[i];
    for (; i < n - show; ++i) buf[i] = '*';
    for (; i < n; ++i) buf[i] = s[i];
    buf[n] = 0;
    return buf;
}

// -----------------------------------------------------------------------------
// Robust HTTP client (event-based reader; supports chunked and Content-Length)
// -----------------------------------------------------------------------------

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} http_rx_ctx_t;

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    http_rx_ctx_t *rx = (http_rx_ctx_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (evt->data && evt->data_len > 0) {
            // Grow buffer if required
            if (rx->len + evt->data_len + 1 > rx->cap) {
                size_t new_cap = (rx->cap ? rx->cap : 2048);
                while (rx->len + evt->data_len + 1 > new_cap) new_cap <<= 1;
                char *nb = (char *)realloc(rx->buf, new_cap);
                if (!nb) return ESP_FAIL;
                rx->buf = nb;
                rx->cap = new_cap;
            }
            memcpy(rx->buf + rx->len, evt->data, evt->data_len);
            rx->len += evt->data_len;
            rx->buf[rx->len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t http_post(const char *url,
                           const char *payload,
                           const char *headers[][2],
                           int num_headers,
                           int *status_code,
                           char **response)
{
    http_rx_ctx_t rx = {0};

    esp_http_client_config_t cfg = {
        .url                   = url,
        .method                = HTTP_METHOD_POST,
        .crt_bundle_attach     = esp_crt_bundle_attach,
        .timeout_ms            = 10000,
        .event_handler         = _http_event_handler,
        .user_data             = &rx,
        .buffer_size           = 1024,
        .buffer_size_tx        = 8192,
        .disable_auto_redirect = false,
        .keep_alive_enable     = true,
        .auth_type             = HTTP_AUTH_TYPE_NONE, // evita que el cliente intente Basic/Digest
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&cfg);

    if (!client) {
        ESP_LOGE(TAG_H, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    // Reset on each call
    // esp_http_client_set_user_data(client, &rx);
    // esp_http_client_set_url(client, url);
    // esp_http_client_set_method(client, HTTP_METHOD_POST);
    
    // Standard headers for JSON APIs; avoid compressed responses
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    for (int i = 0; i < num_headers; i++) {
        if (headers[i][0] && headers[i][1]) {
            esp_http_client_set_header(client, headers[i][0], headers[i][1]);
        }
    }

    if (payload) {
        esp_http_client_set_post_field(client, payload, strlen(payload));
    }

    esp_err_t err = esp_http_client_perform(client);
    if (status_code) *status_code = esp_http_client_get_status_code(client);

    // Snapshot response headers for debugging (DEBUG level)
    char *ct = NULL, *ce = NULL, *te = NULL;
    esp_http_client_get_header(client, "Content-Type", &ct);
    esp_http_client_get_header(client, "Content-Encoding", &ce);
    esp_http_client_get_header(client, "Transfer-Encoding", &te);
    ESP_LOGD(TAG_H, "Resp headers: CT=%s CE=%s TE=%s",
            ct ? ct : "-", ce ? ce : "-", te ? te : "-");

    // Return body buffer (caller owns free())
    if (response) {
        if (rx.len > 0 && rx.buf) {
            *response = rx.buf;
        } else {
            *response = NULL;
            free(rx.buf);
        }
    } else {
        free(rx.buf);
    }
    
    esp_http_client_cleanup(client);
    return err;
}

// -----------------------------------------------------------------------------
// NVS helpers to persist OMF registration state
// -----------------------------------------------------------------------------

static bool omf_is_registered_from_nvs(void)
{
    nvs_handle_t nvh;
    esp_err_t err = nvs_open("omf", NVS_READONLY, &nvh);
    if (err != ESP_OK) return false;
    uint8_t flag = 0;
    err = nvs_get_u8(nvh, "reg_ok", &flag);
    nvs_close(nvh);
    return (err == ESP_OK && flag == 1);
}

static void omf_mark_registered_in_nvs(void)
{
    nvs_handle_t nvh;
    if (nvs_open("omf", NVS_READWRITE, &nvh) == ESP_OK) {
        uint8_t flag = 1;
        nvs_set_u8(nvh, "reg_ok", flag);
        nvs_commit(nvh);
        nvs_close(nvh);
    }
}

// Call this if you intentionally bump the schema and need to re-register
static void omf_clear_registered_in_nvs(void)
{
    nvs_handle_t nvh;
    if (nvs_open("omf", NVS_READWRITE, &nvh) == ESP_OK) {
        nvs_erase_key(nvh, "reg_ok");
        nvs_commit(nvh);
        nvs_close(nvh);
    }
}

// -----------------------------------------------------------------------------
// OAuth2
// -----------------------------------------------------------------------------

// Retrieves an OAuth2 token with client_credentials grant.
// Returns newly allocated string (caller frees) or NULL on failure.
static char *retrieve_token(void)
{
    char *resp = NULL;
    int status = 0;

    // Build form body: IMPORTANT → single '&' before scope
    char body[512];
    snprintf(body, sizeof(body),
             "grant_type=client_credentials&client_id=%s&client_secret=%s&scope=%s",
             CLIENT_ID, CLIENT_SECRET, OAUTH_SCOPE);

    const char *hdrs[][2] = {
        {"Content-Type", "application/x-www-form-urlencoded"}
    };

    ESP_LOGI(TAG_H, "Token request → %s, scope: %s, client: %s",
             CDS_AUTH, OAUTH_SCOPE, mask_id(CLIENT_ID));

    xSemaphoreTake(http_xfer_mutex, portMAX_DELAY);
    esp_err_t err = http_post(CDS_AUTH, body, hdrs, 1, &status, &resp);
    xSemaphoreGive(http_xfer_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG_H, "Token transport error: %s", esp_err_to_name(err));
        free(resp);
        return NULL;
    }
    if (status != 200) {
        ESP_LOGE(TAG_H, "Token HTTP status=%d, body: %s", status, resp ? resp : "<empty>");
        free(resp);
        return NULL;
    }
    if (!resp || !*resp) {
        ESP_LOGE(TAG_H, "Token empty body (HTTP 200).");
        return NULL;
    }

    cJSON *json = cJSON_Parse(resp);
    if (!json) {
        int show = strlen(resp); if (show > 160) show = 160;
        ESP_LOGE(TAG_H, "Token JSON parse failed. First bytes: '%.*s'", show, resp);
        free(resp);
        return NULL;
    }

    cJSON *tok = cJSON_GetObjectItem(json, "access_token");
    cJSON *exp = cJSON_GetObjectItem(json, "expires_in");
    if (!cJSON_IsString(tok)) {
        char *raw = cJSON_PrintUnformatted(json);
        ESP_LOGE(TAG_H, "No access_token. Full JSON: %s", raw ? raw : "<null>");
        free(raw);
        cJSON_Delete(json);
        free(resp);
        return NULL;
    }

    char *token = strdup(tok->valuestring);
    if (cJSON_IsNumber(exp)) {
        ESP_LOGI(TAG_H, "Token OK (expires_in=%d s)", exp->valueint);
        // Optionally adjust k_token_refresh_interval here using exp->valueint * 0.9
    } else {
        ESP_LOGI(TAG_H, "Token OK");
    }

    free(resp);
    cJSON_Delete(json);
    return token;
}

// -----------------------------------------------------------------------------
// OMF registration (type + container), called once after the first token
// An alternative is omitting this step an create Types and Containters manally 
// using an external web client (for exameple CURL in Linux):
/*curl --request POST 'https://uswe.datahub.connect.aveva.com/api/v1/Tenants/<tenant>/Namespaces/<namespace>/omf' \
--header 'Authorization: Bearer <token>' \
--header 'Content-Type: application/json' \
--header 'messagetype: container' \
--header 'messageformat: json' \
--header 'omfversion: 1.2' \
--header 'action: create' \
--data-raw '[{"id":"ESP32.CO2", "name":"ESP32_CO2", "typeid":"TimeIndexed.Int32","datasource":"ESP32", "extrapolation":"none"}]'*/
// -----------------------------------------------------------------------------

static void register_omf_once_with_bearer(char *bearer)
{
    int status = 0;
    char *resp = NULL;

    if (!bearer) { ESP_LOGW(TAG_H, "No token for OMF register"); return; }

    // --- Type message ---
    const char *hdrs_type[][2] = {
        {"Content-Type", "application/json; charset=utf-8"},
        {"Authorization", bearer},
        {"messagetype",  "type"},
        {"messageformat","json"},
        {"omfversion",   "1.2"},
        {"action",       "create"}
    };

    const char *type_json =
        "[{\"id\":\"AQType\",\"type\":\"object\",\"classification\":\"dynamic\","
        "\"properties\":{"
            "\"Timestamp\":{\"type\":\"string\",\"format\":\"date-time\",\"isindex\":true},"
            "\"Temperature\":{\"type\":\"number\",\"format\":\"float64\"},"
            "\"Pressure\":{\"type\":\"number\",\"format\":\"float64\"},"
            "\"Humidity\":{\"type\":\"number\",\"format\":\"float64\"},"
            "\"CO2\":{\"type\":\"integer\",\"format\":\"int16\"},"
            "\"Light\":{\"type\":\"boolean\"}"
        "}}]";

    xSemaphoreTake(http_xfer_mutex, portMAX_DELAY);
    http_post(CDS_OMF, type_json, hdrs_type, 6, &status, &resp);
    xSemaphoreGive(http_xfer_mutex);
    ESP_LOGI(TAG_H, "OMF type status: %d", status);
    free(resp);

    bool type_ok = (status == 200 || status == 201 || status == 202 || status == 409);

    // --- Container message ---
    const char *hdrs_cont[][2] = {
        {"Content-Type", "application/json; charset=utf-8"},
        {"Authorization", bearer},
        {"messagetype",  "container"},
        {"messageformat","json"},
        {"omfversion",   "1.2"},
        {"action",       "create"}
    };

    // Composed stream with all values and individual streams with measurements

    const char *cont_json =
    "["
    "    {\"id\":\"ESP32_Enviro\",\"typeid\":\"AQType\",\"datasource\":\"ESP32\",\"extrapolation\":\"none\"},"
    "    {\"id\":\"ESP32.Temperature\",   \"typeid\":\"DoubleTimeIndexed.Double\", \"datasource\":\"ESP32\", \"extrapolation\":\"none\"},"
    "    {\"id\":\"ESP32.Pressure\",  \"typeid\":\"DoubleTimeIndexed.Double\", \"datasource\":\"ESP32\", \"extrapolation\":\"none\"},"
    "    {\"id\":\"ESP32.Humidity\",    \"typeid\":\"DoubleTimeIndexed.Double\", \"datasource\":\"ESP32\", \"extrapolation\":\"none\"},"
    "    {\"id\":\"ESP32.CO2\",    \"typeid\":\"DoubleTimeIndexed.Double\", \"datasource\":\"ESP32\", \"extrapolation\":\"none\"},"
    "    {\"id\":\"ESP32.Light\",  \"typeid\":\"BooleanTimeIndexed.Boolean\", \"datasource\":\"ESP32\", \"extrapolation\":\"none\"}"
    "]";

    xSemaphoreTake(http_xfer_mutex, portMAX_DELAY);
    http_post(CDS_OMF, cont_json, hdrs_cont, 6, &status, &resp);
    xSemaphoreGive(http_xfer_mutex);
    ESP_LOGI(TAG_H, "OMF container status: %d", status);
    free(resp);

    bool cont_ok = (status == 200 || status == 201 || status == 202 || status == 409);
    free(bearer);

    if (type_ok && cont_ok) {
        omf_mark_registered_in_nvs();
        ESP_LOGI(TAG_H, "OMF registration complete (persisted in NVS)");
    } else {
        ESP_LOGW(TAG_H, "OMF registration incomplete (type_ok=%d, cont_ok=%d)", type_ok, cont_ok);
    }
}

// -----------------------------------------------------------------------------
// Tasks
// -----------------------------------------------------------------------------

// Periodically refresh OAuth2 token (single HTTP at a time; swap pointer safely).
void token_refresh_task(void *pvParameters){
    EventBits_t bits;
    while(1) {
        // Block until Wi-Fi is connected
        xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
        
        vTaskDelay(token_refresh_interval);

        char *new_tok = retrieve_token();
        if (new_tok) {
            xSemaphoreTake(token_mutex, portMAX_DELAY);
            char *old = oauth_token;
            oauth_token = new_tok;
            xSemaphoreGive(token_mutex);
            free(old);

            ESP_LOGI(TAG_H, "Token refreshed");
            xSemaphoreGive(http_semaphore); // in case publisher was waiting
        }
    }
}

// Publish OMF data messages taken from measurement_queue.
// Waits for first token, optionally registers OMF artifacts once (NVS guarded),
// and uses RTC time at publish time.
void omf_publish_task(void *pvParameters){
    measurement_q_t data;
    EventBits_t bits;
    // Wait for first token; then register Type/Container only if not already registered.
    if (xSemaphoreTake(http_semaphore, portMAX_DELAY) == pdTRUE) {
        if (!omf_is_registered_from_nvs()) {
            char *bearer = token_build_bearer_alloc();
            if (bearer) {
                register_omf_once_with_bearer(bearer);
            }
        } else {
            ESP_LOGI(TAG_H, "OMF already registered (NVS), skipping");
        }
    }

    while (1) {

        if (xQueueReceive(measurement_queue, &data, portMAX_DELAY) == pdTRUE) {

            // Generate timestamp from RTC (publisher-owned)
            char ts[OMF_TS_LEN] = {0};
            if (!rtc_now_iso8601(ts, sizeof(ts))) {
                ESP_LOGW(TAG_H, "Skipping publish: RTC not synchronized");
                continue;
            }

            // Snapshot current bearer
            char *bearer = token_build_bearer_alloc();
            if (!bearer) {
                ESP_LOGW(TAG_H, "No token available; waiting...");
                xSemaphoreTake(http_semaphore, portMAX_DELAY);
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                continue;
            }

            // Build OMF data message for all containers
            cJSON *root = cJSON_CreateArray();

            // Object for the compound container (ESP32_Enviro)
            cJSON *compound_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(compound_obj, "containerid", "ESP32_Enviro");
            cJSON *compound_vals = cJSON_AddArrayToObject(compound_obj, "values");
            cJSON *compound_rec  = cJSON_CreateObject();
            cJSON_AddStringToObject(compound_rec, "Timestamp",   ts);
            cJSON_AddNumberToObject(compound_rec, "Temperature", data.temperature);
            cJSON_AddNumberToObject(compound_rec, "Pressure",    data.pressure);
            cJSON_AddNumberToObject(compound_rec, "Humidity",    data.humidity);
            cJSON_AddNumberToObject(compound_rec, "CO2",         data.co2);
            cJSON_AddBoolToObject  (compound_rec, "Light",       data.light_state ? true : false);
            cJSON_AddItemToArray(compound_vals, compound_rec);
            cJSON_AddItemToArray(root, compound_obj);

            // Object for the Temperature stream
            cJSON *temp_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(temp_obj, "containerid", "ESP32.Temperature");
            cJSON *temp_vals = cJSON_AddArrayToObject(temp_obj, "values");
            cJSON *temp_rec = cJSON_CreateObject();
            cJSON_AddStringToObject(temp_rec, "Timestamp", ts);
            cJSON_AddNumberToObject(temp_rec, "Value", data.temperature);
            cJSON_AddItemToArray(temp_vals, temp_rec);
            cJSON_AddItemToArray(root, temp_obj);

            // Object for the Pressure stream
            cJSON *press_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(press_obj, "containerid", "ESP32.Pressure");
            cJSON *press_vals = cJSON_AddArrayToObject(press_obj, "values");
            cJSON *press_rec = cJSON_CreateObject();
            cJSON_AddStringToObject(press_rec, "Timestamp", ts);
            cJSON_AddNumberToObject(press_rec, "Value", data.pressure);
            cJSON_AddItemToArray(press_vals, press_rec);
            cJSON_AddItemToArray(root, press_obj);

            // Object for the Humidity stream
            cJSON *hum_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(hum_obj, "containerid", "ESP32.Humidity");
            cJSON *hum_vals = cJSON_AddArrayToObject(hum_obj, "values");
            cJSON *hum_rec = cJSON_CreateObject();
            cJSON_AddStringToObject(hum_rec, "Timestamp", ts);
            cJSON_AddNumberToObject(hum_rec, "Value", data.humidity);
            cJSON_AddItemToArray(hum_vals, hum_rec);
            cJSON_AddItemToArray(root, hum_obj);

            // Object for the CO2 stream
            cJSON *co2_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(co2_obj, "containerid", "ESP32.CO2");
            cJSON *co2_vals = cJSON_AddArrayToObject(co2_obj, "values");
            cJSON *co2_rec = cJSON_CreateObject();
            cJSON_AddStringToObject(co2_rec, "Timestamp", ts);
            cJSON_AddNumberToObject(co2_rec, "Value", data.co2);
            cJSON_AddItemToArray(co2_vals, co2_rec);
            cJSON_AddItemToArray(root, co2_obj);

            // Object for the Light stream
            cJSON *light_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(light_obj, "containerid", "ESP32.Light");
            cJSON *light_vals = cJSON_AddArrayToObject(light_obj, "values");
            cJSON *light_rec = cJSON_CreateObject();
            cJSON_AddStringToObject(light_rec, "Timestamp", ts);
            cJSON_AddBoolToObject(light_rec, "Value", data.light_state);
            cJSON_AddItemToArray(light_vals, light_rec);
            cJSON_AddItemToArray(root, light_obj);

            char *payload = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);

            const char *hdrs[][2] = {
                {"Content-Type", "application/json; charset=utf-8"},
                {"Authorization", bearer},
                {"messagetype",  "data"},
                {"messageformat","json"},
                {"omfversion",   "1.2"},
                {"action",       "create"}
            };

            // Block until Wi-Fi is connected
            xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

            // Exactly one HTTP at a time across all tasks
            xSemaphoreTake(http_xfer_mutex, portMAX_DELAY);
            int status = 0; char *resp = NULL;
            http_post(CDS_OMF, payload, hdrs, 6, &status, &resp);
            xSemaphoreGive(http_xfer_mutex);

            ESP_LOGI(TAG_H, "OMF data status: %d", status);
            free(resp);
            free(payload);
        }
    }
}

// Public entry point: creates sync primitives, fetches token, and launches tasks.
void http_client_app_start(void)
{
    // Create sync primitives (idempotent if re-entered defensively)
    if (!http_semaphore)   http_semaphore   = xSemaphoreCreateBinary();
    if (!http_xfer_mutex)  http_xfer_mutex  = xSemaphoreCreateMutex();
    if (!token_mutex)      token_mutex      = xSemaphoreCreateMutex();

    if (!http_semaphore || !http_xfer_mutex || !token_mutex) {
        ESP_LOGE(TAG_H, "Failed to create sync primitives");
        return;
    }
    xSemaphoreTake(http_semaphore, 0); // start as "no token"

    // Optional: increase verbosity for TLS/HTTP (once)
    esp_log_level_set("esp-tls", ESP_LOG_INFO);
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_INFO);
    esp_log_level_set("HTTP_CLIENT", ESP_LOG_DEBUG);

    // Initial token (no races yet)
    oauth_token = retrieve_token();
    if (oauth_token != NULL) {
        xSemaphoreGive(http_semaphore);
    } else {
        ESP_LOGW(TAG_H, "Starting without token; publisher will wait");
    }

    // Launch background tasks
    xTaskCreate(token_refresh_task, "TokenRefresh", 4096, NULL, 3, NULL);
    xTaskCreate(omf_publish_task,   "OmfPublish",   8192, NULL, 2, NULL);
}
