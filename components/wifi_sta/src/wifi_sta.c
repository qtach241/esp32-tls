#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_log.h"

#include "wifi_sta.h"

#define TAG "wifi_sta"

/* FreeRTOS event group to signal when we are connected & ready to make a request. */
static EventGroupHandle_t wifi_sta_event_group;

/* Internal helper to set sta connected state. */
static void wifi_sta_set_connected(bool c);

// Description:
// wifi_sta_init is called from the main task. It's purpose is to set up the
// esp32 wifi driver in station mode.
esp_err_t wifi_sta_init(wifi_sta_init_struct_t *param)
{
    // First, validate the wifi credentials passed in by the user/app does not
    // exceed the buffer size in wifi_config_t.
    static wifi_config_t wifi_sta_config;
    
    if (strlen(param->network_ssid) >= sizeof(wifi_sta_config.sta.ssid) / sizeof(char)) {
        ESP_LOGE(TAG, "wifi_sta_init: invalid parameter: network_ssid too long");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (strlen(param->network_password) >= sizeof(wifi_sta_config.sta.password) / sizeof(char)) {
        ESP_LOGE(TAG, "wifi_sta_init: invalid parameter: network_password too long");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "wifi_sta_init: network = '%s'", param->network_ssid);
	
	// First we call tcpip_adapter_init(). This initializes the LwIP submodules
	// and starts the core LwIP task.
    tcpip_adapter_init();
    
    // Init WIFI (driver memory, buffers and so on).
    ESP_LOGD(TAG, "wifi_sta_init: esp_wifi_init");

	// As instructed by the API reference, we should always initialize the Wi-Fi
	// config to default values via the WIFI_INIT_CONFIG_DEFAULT() macro. This
	// guarantees correct format for future releases.
    wifi_init_config_t init_config_struct = WIFI_INIT_CONFIG_DEFAULT();

	// Next, we call esp_wifi_init() with the default Wi-Fi configuration. This 
	// allocates resources for the Wi-Fi driver such as the control structure,
	// RX/TX buffer, Wi-Fi NVS structure, etc. This also starts the Wi-Fi task.
    esp_err_t init_result = esp_wifi_init(&init_config_struct);
    if (ESP_OK != init_result) {
        // typically ESP_ERR_WIFI_NO_MEM, ...
        ESP_LOGE(TAG, "wifi_sta_init: esp_wifi_init failed: %d", init_result);
        return init_result;
    }
    
    // Set the Wi-Fi configuration storage type. If the Wi-Fi NVS flash is 
    // enabled, all Wi-Fi configurations set via the Wi-Fi APIs will be stored
    // into flash and the wifi driver will start up with these configurations
    // next time it powers on/reboots. However, the application can choose to
    // disable the Wi-Fi NVS flash if it does not need to store configurations
    // into persistent memory.
    ESP_LOGD(TAG, "wifi_sta_init: esp_wifi_set_storage");
    esp_err_t storage_result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ESP_OK != storage_result) {
        // typically ESP_ERR_WIFI_NOT_INIT, ESP_ERR_WIFI_ARG, ...
        ESP_LOGE(TAG, "wifi_sta_init: esp_wifi_set_storage failed: %d", storage_result);
        return storage_result;
    }

    // Next, we call esp_wifi_set_mode() to set the driver to STA (station) mode.
    // In this mode, esp_wifi_start() will init the internal station data while 
    // the station’s interface is ready for the RX and TX Wi-Fi data. After 
    // esp_wifi_connect() is called, the driver will connect to the target AP.
    ESP_LOGD(TAG, "wifi_sta_init: esp_wifi_set_mode");
    esp_err_t set_mode_result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ESP_OK != set_mode_result) {
        // typically ESP_ERR_WIFI_NOT_INIT, ESP_ERR_WIFI_ARG, ...
        ESP_LOGE(TAG, "wifi_sta_init: esp_wifi_set_mode failed: %d", set_mode_result);
        return set_mode_result;
    }

    // Now that the Wi-Fi driver has been set to STA mode, we can configure it.
    ESP_LOGD(TAG, "wifi_sta_init: esp_wifi_set_config");
	
	// Remember those parameters we passed in and validated at the start? Now
	// we'll finally use those to configure the Wi-Fi driver.
    strcpy((char*)wifi_sta_config.sta.ssid, param->network_ssid);
    strcpy((char*)wifi_sta_config.sta.password, param->network_password);
	
    // For STA configuration, bssid_set needs to be 0.
    wifi_sta_config.sta.bssid_set = false;

	// Now call esp_wifi_set_config() with our sta structure to set the Wi-Fi
	// driver configuration.
    esp_err_t set_config_result = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_sta_config);
    if (ESP_OK != set_config_result) {
        ESP_LOGE(TAG, "wifi_sta_init: esp_wifi_set_config failed: %d", set_config_result);
        return set_config_result;
    }
    
    vTaskDelay(200 / portTICK_PERIOD_MS); // WORKAROUND
    wifi_sta_event_group = xEventGroupCreate();
 
    // Start WIFI according to the current configuration.
    ESP_LOGD(TAG, "wifi_sta_init: esp_wifi_start");

	// Finally, start the Wi-Fi driver, then our initialization function is
	// complete. The Wi-Fi driver will post events to the event daemon, which
	// will then forward them to the application event handler. The application
	// event handler should delegate wifi events to wifi_sta_handle_event.
    esp_err_t start_result = esp_wifi_start();
    if (ESP_OK != start_result) {
        ESP_LOGE(TAG, "wifi_sta_init: esp_wifi_set_config failed: %d", start_result);
        return start_result;
    }
    
    vTaskDelay(200 / portTICK_PERIOD_MS); // WORKAROUND

    return ESP_OK;
}

// Description:
// This function handles events posted by the Wi-Fi driver task.
esp_err_t wifi_sta_handle_event(void *ctx, system_event_t *event, int *handled)
{
    esp_err_t result = ESP_OK;
    *handled = 1;
    
    switch(event->event_id) {
            
        case SYSTEM_EVENT_STA_START:
            ESP_LOGI(TAG, "wifi_sta_handle_event: SYSTEM_EVENT_STA_START");
            result = esp_wifi_connect();
            break;
            
        case SYSTEM_EVENT_STA_GOT_IP:
            ESP_LOGI(TAG, "wifi_sta_handle_event: SYSTEM_EVENT_STA_GOT_IP");
            wifi_sta_set_connected(true);
            break;
            
        case SYSTEM_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "wifi_sta_handle_event: SYSTEM_EVENT_STA_CONNECTED");
            break;
            
        case SYSTEM_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "wifi_sta_handle_event: SYSTEM_EVENT_STA_DISCONNECTED");
            wifi_sta_set_connected(false);
            // try to re-connect
            result = esp_wifi_connect();
            break;
            
        default:
            ESP_LOGI(TAG, "wifi_sta_handle_event: event is not for us: %d", event->event_id);
            *handled = 0;
            break;
    }

    return result;
}

EventGroupHandle_t wifi_sta_get_event_group()
{
    return wifi_sta_event_group;
}

int wifi_sta_is_connected()
{
    return (xEventGroupGetBits(wifi_sta_event_group) & WIFI_STA_EVENT_GROUP_CONNECTED_FLAG) ? 1 : 0;
}

void wifi_sta_wait_connected()
{
    xEventGroupWaitBits(wifi_sta_event_group, WIFI_STA_EVENT_GROUP_CONNECTED_FLAG, false, true, portMAX_DELAY);
}


static void wifi_sta_set_connected(bool c)
{
    if (wifi_sta_is_connected() == c) {
        return;
    }
    
    if (c) {
        xEventGroupSetBits(wifi_sta_event_group, WIFI_STA_EVENT_GROUP_CONNECTED_FLAG);
    } else {
        xEventGroupClearBits(wifi_sta_event_group, WIFI_STA_EVENT_GROUP_CONNECTED_FLAG);
    }
    
    ESP_LOGI(TAG, "Device is now %s WIFI network", c ? "connected to" : "disconnected from");
}

