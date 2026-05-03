#ifndef GLOBALS_H
#define GLOBALS_H

#define TAG "ENV"

#define IP_STR_LEN      16
#define HOSTNAME_LEN    32
#define SSID_LEN        33
#define PASSWORD_LEN    65
#define SERVER_HOST     "wolfrax.local"
#define SERVER_PATH     "/esp_ps"
#define SERVER_PORT     5000

extern char ip_str[IP_STR_LEN];
extern char hostname[HOSTNAME_LEN];
extern char current_ssid[SSID_LEN];
extern char ssid[SSID_LEN];
extern char password[PASSWORD_LEN];

#endif