#include "app.h"
#include "app_config.h"
#include "logger.h"
#include "curl_device.h"

static const char *TAG = "app";

void app_start(void)
{
    ESP_LOGI(TAG, "IMU step counter firmware started");
    if (curl_device_start() != 0) {
        ESP_LOGE(TAG, "step device initialization failed");
    }
}
