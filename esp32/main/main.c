#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "globals.h"
#include "i2c_reader.h"
#include "wifi.h"

#if SIMULATE_SENSOR
#include "esp_random.h"
#endif

/* ---------------- CONFIG ---------------- */

#define SIMULATE_SENSOR 0
#define SAMPLING_INTERVAL (60*1000)
#define LOG_WEB_BUF_SIZE 8192
#define LOG_WEB_TMP_LINE 256

/* ---------------- GLOBALS ---------------- */

static char starttime_str[64] = "UNKNOWN";
static char log_web_buf[LOG_WEB_BUF_SIZE];
static size_t log_web_head = 0;
static bool log_web_wrapped = false;
static SemaphoreHandle_t log_web_mutex = NULL;
static vprintf_like_t log_old_vprintf = NULL;


/* ---------------- SAMPLE STRUCT ---------------- */

typedef struct {
    char ts[32];
    float t, h, p;
} sample_t;

static sample_t latest_sample = {0};

/* ---------------- WEB LOG BUFFER ---------------- */

static void log_web_append(const char *s, size_t len)
{
    if (len == 0) return;

    // If one message is larger than the whole buffer, keep only the tail
    if (len >= LOG_WEB_BUF_SIZE) {
        s += (len - (LOG_WEB_BUF_SIZE - 1));
        len = LOG_WEB_BUF_SIZE - 1;
    }

    for (size_t i = 0; i < len; i++) {
        log_web_buf[log_web_head] = s[i];
        log_web_head = (log_web_head + 1) % LOG_WEB_BUF_SIZE;

        if (log_web_head == 0) {
            log_web_wrapped = true;
        }
    }
}

static int log_web_vprintf(const char *fmt, va_list ap)
{
    char tmp[LOG_WEB_TMP_LINE];

    // Format once for the web buffer
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap_copy);
    va_end(ap_copy);

    if (n > 0 && log_web_mutex != NULL) {
        size_t len = (n < (int)sizeof(tmp)) ? (size_t)n : (sizeof(tmp) - 1);

        if (xSemaphoreTake(log_web_mutex, 0) == pdTRUE) {
            log_web_append(tmp, len);
            xSemaphoreGive(log_web_mutex);
        }
    }

    // Still send logs to the normal output (monitor/UART)
    if (log_old_vprintf) {
        return log_old_vprintf(fmt, ap);
    }

    return n;
}

static void init_web_log_buffer(void)
{
    log_web_mutex = xSemaphoreCreateMutex();
    assert(log_web_mutex != NULL);

    log_old_vprintf = esp_log_set_vprintf(log_web_vprintf);
}

char *make_post_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *sample = cJSON_CreateObject();
    cJSON *node = cJSON_CreateObject();

    cJSON_AddStringToObject(sample, "ts", latest_sample.ts);
    cJSON_AddNumberToObject(sample, "t", latest_sample.t);
    cJSON_AddNumberToObject(sample, "h", latest_sample.h);
    cJSON_AddNumberToObject(sample, "p", latest_sample.p);

    cJSON_AddItemToObject(root, "sample", sample);

    cJSON_AddStringToObject(root, "log", log_web_buf);

    cJSON_AddStringToObject(node, "name", hostname);
    cJSON_AddStringToObject(node, "ip", ip_str);
    cJSON_AddStringToObject(node, "ssid", current_ssid);
    cJSON_AddStringToObject(node, "starttime", starttime_str);
    cJSON_AddStringToObject(node, "mac", macstr);
    cJSON_AddItemToObject(root, "node", node);

    char *json = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);

    return json;
}

static void send_to_server(void)
{   
    char *post_data = make_post_json();

    if (post_data != NULL) {
        esp_http_client_config_t config = {
            .url = SERVER_URL,
            .method = HTTP_METHOD_POST,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            return;
        }   

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
        esp_err_t err = esp_http_client_perform(client); 
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %"PRId64,
                    esp_http_client_get_status_code(client),
                    esp_http_client_get_content_length(client));
        } else {
            ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        }
        free(post_data);
    }
    else {
        ESP_LOGE(TAG, "Failed to create JSON payload");
    }
}

#if SIMULATE_SENSOR
static float get_temperature() { return 20 + (esp_random() % 100) / 10.0; }
static float get_humidity()    { return 40 + (esp_random() % 200) / 10.0; }
static float get_pressure()    { return 1000 + (esp_random() % 100); }
#endif

static void sampling_task(void *arg)
{
    esp_err_t err;

    while (1) {
        sample_t s;

        time_t now;
        time(&now);

        struct tm tinfo;
        gmtime_r(&now, &tinfo);

        strftime(s.ts, sizeof(s.ts), "%Y-%m-%dT%H:%M:%SZ", &tinfo);

#if SIMULATE_SENSOR
        s.t = get_temperature();
        s.h = get_humidity();
        s.p = get_pressure();
        err = ESP_OK;
#else
        err = i2c_reader_read(&s.t, &s.h, &s.p);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Read failed: %s", esp_err_to_name(err));
        }
#endif

        if (err == ESP_OK) {
            latest_sample = s;
            send_to_server();
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLING_INTERVAL)); 
    }
}

/* ---------------- TIME ---------------- */

static void obtain_time(void)
{
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // Set timezone (Sweden)
    setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
    tzset();

    time_t now = 0;
    struct tm timeinfo = { 0 };

    while (timeinfo.tm_year < (2020 - 1900)) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    // Set global time_str to the current time in human-readable format for logging purposes
    strftime(starttime_str, sizeof(starttime_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
}

/* ---------------- MAIN ---------------- */

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());  // Needed for WiFi credentials storage
    init_web_log_buffer();  

    wifi_init_base();
    wifi_start_sta();

    obtain_time();
    ESP_LOGI(TAG, "System time obtained: %s", starttime_str);

#if SIMULATE_SENSOR
#else
    esp_err_t err = i2c_reader_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sensor init failed: %s", esp_err_to_name(err));
        return;
    }
#endif
    xTaskCreate(sampling_task, "sampling", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "%s ready", hostname);
}