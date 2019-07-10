#ifndef __MAIN_H__
#define __MAIN_H__ 1

#include "wifi_sta.h"

// Adjust these values for your environment.
// -------------------------------------------------------------------------------------

// Used by the OTA module to check if the current version is different from the version
// on the server, i.e. if an upgrade or downgrade should be performed.
#define SOFTWARE_VERSION          4

// Provide server name, path to metadata file and polling interval for OTA updates.
#define OTA_SERVER_HOST_NAME      "www.classycode.io"
#define OTA_SERVER_METADATA_PATH  "/esp32/ota.txt"
#define OTA_POLLING_INTERVAL_S    5
#define OTA_AUTO_REBOOT           1

// Structure holding all information used to configure the main application.
typedef struct app_config_struct_ {

	uint32_t serial_number;
	uint32_t software_version;
	
	const char *ota_url;
    
    // Network SSID to connect to.
    const char *network_ssid;
    
    // Network password.
    const char *network_password;

	wifi_sta_init_struct_t wifi_info;
    
} app_config_struct_t;


#endif // __MAIN_H__

