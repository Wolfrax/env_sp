#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "globals.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_err.h"
#include "freertos/event_groups.h"

char ip_str[IP_STR_LEN] = "UNKNOWN";
char hostname[HOSTNAME_LEN] = "UNKNOWN";
char current_ssid[SSID_LEN] = "UNKNOWN";
char ssid[SSID_LEN] = {0};
char password[PASSWORD_LEN] = {0};
char macstr[MAC_STR_LEN] = {0};

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t wifi_event_group;


esp_err_t wifi_credentials_save(const char *ssid, const char *password)
{
    nvs_handle_t handle;

    esp_err_t err = nvs_open("wifi_creds", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "password", password);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

esp_err_t wifi_credentials_load(char *ssid, size_t ssid_len,
                                char *password, size_t password_len)
{
    nvs_handle_t handle;

    esp_err_t err = nvs_open("wifi_creds", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_str(handle, "ssid", ssid, &ssid_len);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "password", password, &password_len);
    }

    nvs_close(handle);
    return err;
}

static void wifi_credentials_init(void)
{
    // Check if credentials exist, if not create default ones and save to NVS.
    // ssid and password are globals

    if (wifi_credentials_load(ssid, sizeof(ssid), password, sizeof(password)) != ESP_OK) {
        ESP_LOGI(TAG, "No WiFi credentials found in NVS");

        strcpy(ssid, SSID_STR);
        strcpy(password, WIFI_PW_STR);
        wifi_credentials_save(ssid, password);
    }
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;

        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", ip_str);

        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);

        ESP_LOGW(TAG, "WiFi disconnected, retrying...");
        esp_wifi_connect();
    }
}

esp_err_t wifi_wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(timeout_ms)
    );

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void wifi_init_base(void)
{
    wifi_credentials_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Set hostname to "esp32-XXXXXX" where XXXXXX are the last 3 bytes of the MAC address
    // which will be used in DHCP and mDNS responses 
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    snprintf(hostname, sizeof(hostname), "esp32-%02X%02X%02X", mac[3], mac[4], mac[5]);
    snprintf(macstr, sizeof(macstr), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Set the hostname for DHCP, visible in the router's client list
    esp_netif_set_hostname(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), hostname);

    // Initialize mDNS with the device's hostname to make it easier to find the device on the network. 
    mdns_init();

    mdns_hostname_set(hostname);
    mdns_instance_name_set("ESP PS Sensor"); // This is the name that will show up in mDNS browser apps

    // Advertise an HTTP service on port 80
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

void wifi_start_sta(void)
{
    strncpy(current_ssid, ssid, sizeof(current_ssid) - 1);
    current_ssid[sizeof(current_ssid) - 1] = '\0';

    if (strlen(ssid) > 32) {
        ESP_LOGE(TAG, "SSID too long");
        return;
    }

    if (strlen(password) > 64) {
        ESP_LOGE(TAG, "Password too long");
        return;
    }

    wifi_config_t wifi_config = {0};

    memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    memcpy(wifi_config.sta.password, password, strlen(password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to WiFi SSID: %s", ssid);
}
