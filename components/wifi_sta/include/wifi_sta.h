#ifndef __WIFI_STA__
#define __WIFI_STA__ 1


#define WIFI_STA_EVENT_GROUP_CONNECTED_FLAG (1 << 0)


typedef struct wifi_sta_init_struct_ {
    
    // Network SSID to connect to.
    const char *network_ssid;
    
    // Network password.
    const char *network_password;
    
} wifi_sta_init_struct_t;


// Configure this device in 'station' mode and connect to the specified network.
esp_err_t wifi_sta_init(wifi_sta_init_struct_t *param);

// Sets "handled" to 1 if the event was handled, 0 if it was not for us.
esp_err_t wifi_sta_handle_event(void *ctx, system_event_t *event, int *handled);

// Returns 1 if the device is currently connected to the specified network, 0 otherwise.
int wifi_sta_is_connected();

// Let other modules wait on connectivity changes.
EventGroupHandle_t wifi_sta_get_event_group();


#endif // __WIFI_STA__

