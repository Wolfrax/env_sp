#ifndef GLOBALS_H
#define GLOBALS_H

#define TAG "ENV"

#define IP_STR_LEN      16
#define HOSTNAME_LEN    32
#define SSID_LEN        33
#define PASSWORD_LEN    65
#define MAC_STR_LEN     18
#define SERVER_HOST     "g10.tplinkdns.com"
#define SERVER_PATH     "/esp_ps"
#define SERVER_PORT     "8095"
#define SERVER_URL      "http://" SERVER_HOST ":" SERVER_PORT SERVER_PATH
#define SSID_STR        "G10"
#define WIFI_PW_STR     "Rafmagn1"
#define LOCATION_STR    "Test location"

extern char ip_str[IP_STR_LEN];
extern char hostname[HOSTNAME_LEN];
extern char current_ssid[SSID_LEN];
extern char ssid[SSID_LEN];
extern char password[PASSWORD_LEN];
extern char macstr[MAC_STR_LEN];

#endif