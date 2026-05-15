#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/gpio.h"

#include "globals.h"
#include "i2c_reader.h"
#include "wifi.h"

#if SIMULATE_SENSOR
#include "esp_random.h"
#endif

#define SIMULATE_SENSOR 0
#define SLEEP_INTERVAL (1*60*1000*1000) // 10 minutes = 10 * 60 * 1000 * 1000 microseconds

RTC_DATA_ATTR static char starttime_str[64] = "UNKNOWN";
RTC_DATA_ATTR static int boot_count = 0;

typedef struct {
    char ts[32];
    float t, h, p;
} sample_t;

static sample_t latest_sample = {0};

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

    cJSON_AddStringToObject(node, "name", hostname);
    cJSON_AddStringToObject(node, "ip", ip_str);
    cJSON_AddStringToObject(node, "ssid", current_ssid);
    cJSON_AddStringToObject(node, "starttime", starttime_str);
    cJSON_AddStringToObject(node, "mac", macstr);
    cJSON_AddNumberToObject(node, "boot_count", boot_count);
    cJSON_AddStringToObject(node, "location", LOCATION_STR);
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

static void sample_and_post(void)
{
    esp_err_t err;
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

    if (strcmp(starttime_str, "UNKNOWN") == 0) {
        // Only set starttime_str on the first boot after flashing, not on every deep sleep wakeup
        strftime(starttime_str, sizeof(starttime_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    }
    
}

/* ---------------- MAIN ---------------- */

void app_main(void)
{
    boot_count++;

    ESP_ERROR_CHECK(nvs_flash_init());  // Needed for WiFi credentials storage

    wifi_init_base();
    wifi_start_sta();

    obtain_time();
    ESP_LOGI(TAG, "System time obtained: %s", starttime_str);
    ESP_LOGI(TAG, "%s ready (boot: %d)", hostname, boot_count);

#if SIMULATE_SENSOR
    sample_and_post();
#else
    sensor_power_on();
    esp_err_t err = i2c_reader_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Sensor init failed: %s", esp_err_to_name(err));
        return;
    }
    sample_and_post();
    sensor_power_off();
#endif

    esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL);
    esp_deep_sleep_start();
}
