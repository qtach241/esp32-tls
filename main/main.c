#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"

#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"

#include "main.h"
//#include "iap_https.h"  // Coordinating firmware updates

#include "cJSON.h"

#define TAG "main"
#define BUFFSIZE 1024
#define NETWORK_BUFFSIZE 4095
#define HASH_LEN 32 /* SHA-256 digest length */

static app_config_struct_t app_config;

static esp_err_t app_event_handler(void *ctx, system_event_t *event);

static char response_buffer[BUFFSIZE + 1] = { 0 };
static char network_buffer[NETWORK_BUFFSIZE + 1] = { 0 };
static char ota_write_data[BUFFSIZE + 1] = { 0 };

/* Root cert for howsmyssl.com, taken from howsmyssl_com_root_cert.pem

   The PEM file was extracted from the output of this command:
   openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null

   The CA root cert is the last cert given in the chain of certs.

   To embed it in the app binary, the PEM file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
extern const char howsmyssl_com_root_cert_pem_start[] asm("_binary_howsmyssl_com_root_cert_pem_start");
extern const char howsmyssl_com_root_cert_pem_end[]   asm("_binary_howsmyssl_com_root_cert_pem_end");

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");



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
                //ESP_LOGI(TAG, "%.*s", evt->data_len, (char*)evt->data);
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

static void parse_json(const char * const monitor)
{
	hows_my_ssl_check_t hows_my_ssl_check;

	const cJSON *tls_version = NULL;
	const cJSON *rating = NULL;
	int status = 0;
	cJSON *monitor_json = cJSON_Parse(monitor);
    if (monitor_json == NULL)
    {
        const char *error_ptr = cJSON_GetErrorPtr();
        if (error_ptr != NULL)
        {
            fprintf(stderr, "Error before: %s\n", error_ptr);
        }
        status = 0;
        goto end;
    }

	tls_version = cJSON_GetObjectItemCaseSensitive(monitor_json, "tls_version");
    if (cJSON_IsString(tls_version) && (tls_version->valuestring != NULL))
    {
        printf("Parsed tls_version: \"%s\"\n", tls_version->valuestring);
    }

	rating = cJSON_GetObjectItemCaseSensitive(monitor_json, "rating");
    if (cJSON_IsString(rating) && (rating->valuestring != NULL))
    {
        printf("Parsed rating: \"%s\"\n", rating->valuestring);
    }

end:
    cJSON_Delete(monitor_json);
    return status;
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
            //ESP_LOGI(TAG, "Received Data: %s", response_buffer);
            if (err != ESP_OK) {
                esp_http_client_close(client);
    			esp_http_client_cleanup(client);
				ESP_LOGE(TAG, "Error: SSL data read error");
                //task_fatal_error();
            }
			strcpy(&network_buffer[binary_file_length], response_buffer);
            binary_file_length += data_read;
            ESP_LOGI(TAG, "Written image length %d", binary_file_length);
        } else if (data_read == 0) {
            ESP_LOGI(TAG, "Connection closed,all data received");
            break;
        }
    }

    ESP_LOGI(TAG, "HTTP Stream reader Status = %d, content_length = %d",
                    esp_http_client_get_status_code(client),
                    esp_http_client_get_content_length(client));

	// Null terminate the final byte of the buffer before we print it.
	network_buffer[binary_file_length] = 0;

	// Print out the network buffer.
	ESP_LOGI(TAG, "Received Data: %s", network_buffer);

	parse_json(network_buffer);
	
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

static void http_cleanup(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void __attribute__((noreturn)) task_fatal_error()
{
    ESP_LOGE(TAG, "Exiting task due to fatal error...");
    (void)vTaskDelete(NULL);

    while (1) {
        ;
    }
}

void print_sha256 (const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i) {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s: %s", label, hash_print);
}


void ota_example_task(void * pvParameter)
{
	uint8_t sha_256[HASH_LEN] = { 0 };
	esp_partition_t partition;

	esp_err_t err;
    /* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */
    esp_ota_handle_t update_handle = 0 ;
    const esp_partition_t *update_partition = NULL;

    wifi_sta_wait_connected();

	ESP_LOGI(TAG, "Connected to AP, begin ota example...");

	vTaskDelay(5000 / portTICK_PERIOD_MS);
	
	for (int countdown = 10; countdown > 0; countdown--)
	{
		ESP_LOGI(TAG, "Commencing ota example in... %d", countdown);
		vTaskDelay(1000 / portTICK_PERIOD_MS);	
	}

	// get sha256 digest for the partition table
    partition.address   = ESP_PARTITION_TABLE_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_MAX_LEN;
    partition.type      = ESP_PARTITION_TYPE_DATA;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for the partition table: ");

    // get sha256 digest for bootloader
    partition.address   = ESP_BOOTLOADER_OFFSET;
    partition.size      = ESP_PARTITION_TABLE_OFFSET;
    partition.type      = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader: ");

    // get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware: ");

	const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running = esp_ota_get_running_partition();

	if (configured != running) {
        ESP_LOGW(TAG, "Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
                 configured->address, running->address);
        ESP_LOGW(TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)");
    }
    ESP_LOGI(TAG, "Running partition type %d subtype %d (offset 0x%08x)",
             running->type, running->subtype, running->address);
    
    esp_http_client_config_t config = {
        //.url = CONFIG_FIRMWARE_UPGRADE_URL,
        .url = "https://papapilldevstorage.blob.core.windows.net/ota/app-template.bin?st=2019-07-13T04%3A29%3A56Z&se=2022-07-14T04%3A29%3A00Z&sp=r&sv=2018-03-28&sr=b&sig=K2BVHPNs%2BjU2JkM%2BVZFlAMCJguXYugqjxdGULw8sRMo%3D",
        //.cert_pem = (char *)server_cert_pem_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialise HTTP connection");
        task_fatal_error();
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        task_fatal_error();
    }
    esp_http_client_fetch_headers(client);

    update_partition = esp_ota_get_next_update_partition(NULL);
    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
             update_partition->subtype, update_partition->address);
    assert(update_partition != NULL);

    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        http_cleanup(client);
        task_fatal_error();
    }
    ESP_LOGI(TAG, "esp_ota_begin succeeded");

    int binary_file_length = 0;
    /*deal with all receive packet*/
    while (1) {
        int data_read = esp_http_client_read(client, ota_write_data, BUFFSIZE);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error: SSL data read error");
            http_cleanup(client);
            task_fatal_error();
        } else if (data_read > 0) {
            err = esp_ota_write( update_handle, (const void *)ota_write_data, data_read);
            if (err != ESP_OK) {
                http_cleanup(client);
                task_fatal_error();
            }
            binary_file_length += data_read;
            ESP_LOGD(TAG, "Written image length %d", binary_file_length);
        } else if (data_read == 0) {
            ESP_LOGI(TAG, "Connection closed,all data received");
            break;
        }
    }
    ESP_LOGI(TAG, "Total Write binary data length : %d", binary_file_length);

    if (esp_ota_end(update_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed!");
        http_cleanup(client);
        task_fatal_error();
    }

    if (esp_partition_check_identity(esp_ota_get_running_partition(), update_partition) == true) {
        ESP_LOGI(TAG, "The current running firmware is same as the firmware just downloaded");
        int i = 0;
        ESP_LOGI(TAG, "When a new firmware is available on the server, press the reset button to download it");
        while(1) {
            ESP_LOGI(TAG, "Waiting for a new firmware ... %d", ++i);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        http_cleanup(client);
        task_fatal_error();
    }
    ESP_LOGI(TAG, "Prepare to restart system!");
    esp_restart();
    return ;
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
	// Initialize ESP32 non-volatile flash.
    nvs_flash_init();

	// Create an event loop task and register the app event handler callback.
	// The app event handler delegates events posted by the event daemon to
	// the responsible modules.
    esp_event_loop_init(&app_event_handler, NULL);

	// Initialize the Wi-Fi station module. This module maintains the ESP32's
	// connection to the AP.
	app_config.wifi_info.network_ssid = CONFIG_ESP_WIFI_SSID;
	app_config.wifi_info.network_password = CONFIG_ESP_WIFI_PASSWORD;
	
    wifi_sta_init(&app_config.wifi_info);

	// Initialize the OTA update module. This module manages the OTA firmware
	// update component of the application.
    //init_ota();

	// Test https task
	xTaskCreate(&http_test_task, "http_test_task", 8192, NULL, 5, NULL);

    // Blink an LED from a task
	xTaskCreate(&blink_task, "blink_task", configMINIMAL_STACK_SIZE, NULL, 5, NULL);

	// Test ota task
	xTaskCreate(&ota_example_task, "ota_example_task", 8192, NULL, 5, NULL);

    gpio_set_direction(GPIO_NUM_5, GPIO_MODE_OUTPUT);
    while (1) {
		
		// Main app is simply to pulse an LED once if Wi-Fi is not connected
		// and twice if the Wi-Fi is connected.
        int nofFlashes = 1;
        if (wifi_sta_is_connected())
		{
            nofFlashes += 1;
        }
        for (int i = 0; i < nofFlashes; i++)
		{
            gpio_set_level(GPIO_NUM_5, 1);
            vTaskDelay(150 / portTICK_PERIOD_MS);
            gpio_set_level(GPIO_NUM_5, 0);
            vTaskDelay(150 / portTICK_PERIOD_MS);
        }
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
    
    // Should never arrive here.
}

// Description:
// This function handles events posted by the event daemon by delegating them
// to the various component event handlers.
static esp_err_t app_event_handler(void *ctx, system_event_t *event)
{
    esp_err_t result = ESP_OK;
    int handled = 0;
    
    ESP_LOGI(TAG, "app_event_handler: event: %d", event->event_id);

    result = wifi_sta_handle_event(ctx, event, &handled);
    if (ESP_OK != result || handled)
	{
        return result;
    }
    
    ESP_LOGW(TAG, "app_event_handler: unhandled event: %d", event->event_id);
    return ESP_OK;
}

