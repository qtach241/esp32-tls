#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/event_groups.h"
#include "esp_http_client.h"

#include "main.h"
//#include "wifi_sta.h"   // WIFI module configuration, connecting to an access point.
//#include "iap_https.h"  // Coordinating firmware updates


#define TAG "main"
#define BUFFSIZE 1024

static app_config_struct_t app_config;

static esp_err_t app_event_handler(void *ctx, system_event_t *event);

static char response_buffer[BUFFSIZE + 1] = { 0 };

/* Root cert for howsmyssl.com, taken from howsmyssl_com_root_cert.pem

   The PEM file was extracted from the output of this command:
   openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null

   The CA root cert is the last cert given in the chain of certs.

   To embed it in the app binary, the PEM file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
extern const char howsmyssl_com_root_cert_pem_start[] asm("_binary_howsmyssl_com_root_cert_pem_start");
extern const char howsmyssl_com_root_cert_pem_end[]   asm("_binary_howsmyssl_com_root_cert_pem_end");


// Description:
// A keep-alive indicator that flashes an LED the same number of times as the
// current software version of the application.
void blink_task(void *pvParameter)
{
    /* Configure the IOMUX register for pad BLINK_GPIO (some pads are
       muxed to GPIO on reset already, but some default to other
       functions and need to be switched to GPIO. Consult the
       Technical Reference for a list of pads and their default
       functions.)
    */
    gpio_pad_select_gpio(GPIO_NUM_4);
    /* Set the GPIO as a push/pull output */
    gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    while(1)
	{
		for (int i = 0; i < SOFTWARE_VERSION; i++)
		{
            gpio_set_level(GPIO_NUM_4, 1);
            vTaskDelay(150 / portTICK_PERIOD_MS);
            gpio_set_level(GPIO_NUM_4, 0);
            vTaskDelay(150 / portTICK_PERIOD_MS);
        }
		vTaskDelay(800 / portTICK_PERIOD_MS);
    }
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGI(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGI(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Write out data
                // printf("%.*s", evt->data_len, (char*)evt->data);
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            break;
    }
    return ESP_OK;
}

static void test_https()
{
    esp_http_client_config_t config = {
        .url = "https://www.howsmyssl.com",
        .event_handler = _http_event_handler,
        .cert_pem = howsmyssl_com_root_cert_pem_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d",
                esp_http_client_get_status_code(client),
                esp_http_client_get_content_length(client));
    } else {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void test_https_perform_as_stream_reader()
{
    //char *buffer = malloc(512 + 1);
    //if (buffer == NULL) {
    //    ESP_LOGE(TAG, "Cannot malloc http receive buffer");
    //    return;
    //}
    esp_http_client_config_t config = {
        .url = "https://www.howsmyssl.com/a/check",
        //.url = "https://www.howsmyssl.com",
        //.url = "https://papapill.net",
        .event_handler = _http_event_handler,
        .cert_pem = howsmyssl_com_root_cert_pem_start,
        //.timeout_ms = 70000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err;
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        //free(buffer);
        return;
    }
    esp_http_client_fetch_headers(client);
	int binary_file_length = 0;
	while (1) {
        int data_read = esp_http_client_read(client, response_buffer, BUFFSIZE);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error: SSL data read error");
            esp_http_client_close(client);
    		esp_http_client_cleanup(client);
            //task_fatal_error();
        } else if (data_read > 0) {
        	response_buffer[data_read] = 0;
            ESP_LOGI(TAG, "Received Data: %s", response_buffer);
            if (err != ESP_OK) {
                esp_http_client_close(client);
    			esp_http_client_cleanup(client);
				ESP_LOGE(TAG, "Error: SSL data read error");
                //task_fatal_error();
            }
            binary_file_length += data_read;
            ESP_LOGD(TAG, "Written image length %d", binary_file_length);
        } else if (data_read == 0) {
            ESP_LOGI(TAG, "Connection closed,all data received");
            break;
        }
    }

    ESP_LOGI(TAG, "HTTP Stream reader Status = %d, content_length = %d",
                    esp_http_client_get_status_code(client),
                    esp_http_client_get_content_length(client));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    //free(buffer);
}


static void http_test_task(void *pvParameters)
{
    wifi_sta_wait_connected();
    ESP_LOGI(TAG, "Connected to AP, begin http example");
    //test_https();
    test_https_perform_as_stream_reader();
    ESP_LOGI(TAG, "Finish http example");
    vTaskDelete(NULL);
}


// Description: 
// app_main is called by main_task which is spawned by FreeRTOS in cpu_start.c.
// main_task's stack size and priority can be configured via menuconfig. The 
// application can use this task for initial application-specific setup, for 
// example to launch other tasks. The application can also use main task for 
// event loops and other general purpose activities. If app_main function 
// returns, main task is deleted.
void app_main(void)
{
	ESP_LOGI(TAG, "---------- Intialization started ----------");
    ESP_LOGI(TAG, "---------- Software version: %2d -----------", SOFTWARE_VERSION);

    
    nvs_flash_init();
    
    
    // Configure the application event handler.
    // The handler is centrally implemented in this module.
    // From here, we delegate the event handling to the responsible modules.
    
    esp_event_loop_init(&app_event_handler, NULL);


    // Configure the WIFI module. This module maintains the connection to the
    // defined access point.

    //init_wifi();
    ESP_LOGI(TAG, "Set up WIFI network connection.");

	app_config.wifi_info.network_ssid = CONFIG_ESP_WIFI_SSID;
	app_config.wifi_info.network_password = CONFIG_ESP_WIFI_PASSWORD;
    
    wifi_sta_init(&app_config.wifi_info);
    
    // Configure the over-the-air update module. This module periodically checks
    // for firmware updates by polling a web server. If an update is available,
    // the module downloads and installs it.
    
    //init_ota();

	// Test https task
	xTaskCreate(&http_test_task, "http_test_task", 8192, NULL, 5, NULL);

    // Blink an LED from a task
	xTaskCreate(&blink_task, "blink_task", configMINIMAL_STACK_SIZE, NULL, 5, NULL);

    gpio_set_direction(GPIO_NUM_5, GPIO_MODE_OUTPUT);
    while (1) {
        
        int nofFlashes = 1;
        if (wifi_sta_is_connected())
		{
            nofFlashes += 1;
        }
        /*if (iap_https_update_in_progress()) {
            nofFlashes += 2; // results in 3 (not connected) or 4 (connected) flashes
        }*/
        
        for (int i = 0; i < nofFlashes; i++)
		{
            gpio_set_level(GPIO_NUM_5, 1);
            vTaskDelay(150 / portTICK_PERIOD_MS);
            gpio_set_level(GPIO_NUM_5, 0);
            vTaskDelay(150 / portTICK_PERIOD_MS);
        }
        
        // If the application could only re-boot at certain points, you could
        // manually query iap_https_new_firmware_installed and manually trigger
        // the re-boot. What we do in this example is to let the firmware updater
        // re-boot automatically after installing the update (see init_ota below).
        //
        // if (iap_https_new_firmware_installed()) {
        //     ESP_LOGI(TAG, "New firmware has been installed - rebooting...");
        //     esp_restart();
        // }
        
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    
    // Should never arrive here.
}

// Description:
// This function handles events posted by the event daemon.
static esp_err_t app_event_handler(void *ctx, system_event_t *event)
{
    esp_err_t result = ESP_OK;
    int handled = 0;
    
    ESP_LOGI(TAG, "app_event_handler: event: %d", event->event_id);

    // Let the wifi_sta module handle all WIFI STA events.
    
    result = wifi_sta_handle_event(ctx, event, &handled);
    if (ESP_OK != result || handled)
	{
        return result;
    }
    
    // TODO - handle other events
    
    ESP_LOGW(TAG, "app_event_handler: unhandled event: %d", event->event_id);
    return ESP_OK;
}

