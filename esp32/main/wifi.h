#ifndef WIFI_H
#define WIFI_H

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init_base(void);
void wifi_start_sta(void);
esp_err_t wifi_wait_connected(uint32_t timeout_ms);


#ifdef __cplusplus
}
#endif

#endif // WIFI_H