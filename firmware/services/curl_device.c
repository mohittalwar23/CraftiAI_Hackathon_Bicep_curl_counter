#include "curl_device.h"
#include "app_config.h"
#include "logger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "mqtt_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "curl";
static led_strip_handle_t s_led;
static i2c_master_dev_handle_t s_mpu;
static esp_mqtt_client_handle_t s_mqtt;
static bool s_mqtt_connected;
static uint32_t s_reps;
static uint32_t s_pending_reps;
static int64_t s_last_publish_ms;
static char s_topic[64];
static TickType_t s_flash_until;
static bool s_capture;
static bool s_training_complete;
static int s_curl_axis = 1;

#define MPU_ADDR 0x68
#define MPU_WHO_AM_I 0x75
#define MPU_PWR_MGMT_1 0x6B
#define MPU_ACCEL_XOUT_H 0x3B
#define CURL_CAL_NAMESPACE "curl_cal"
#define CURL_CAL_AXIS_KEY "axis"
#define CURL_CAL_VALID_KEY "valid"

static bool load_curl_calibration(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CURL_CAL_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return false;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not open curl calibration: %s", esp_err_to_name(err));
        return false;
    }
    uint8_t axis = 0, valid = 0;
    esp_err_t axis_err = nvs_get_u8(handle, CURL_CAL_AXIS_KEY, &axis);
    esp_err_t valid_err = nvs_get_u8(handle, CURL_CAL_VALID_KEY, &valid);
    nvs_close(handle);
    if (axis_err != ESP_OK || valid_err != ESP_OK || valid != 1 || axis > 2) {
        ESP_LOGW(TAG, "saved curl calibration is missing or invalid");
        return false;
    }
    s_curl_axis = axis;
    s_training_complete = true;
    ESP_LOGI(TAG, "loaded saved curl calibration: axis=%d", s_curl_axis);
    return true;
}

static bool save_curl_calibration(int axis)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CURL_CAL_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not open curl calibration for write: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u8(handle, CURL_CAL_AXIS_KEY, (uint8_t)axis);
    if (err == ESP_OK) err = nvs_set_u8(handle, CURL_CAL_VALID_KEY, 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not save curl calibration: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void led_color(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led) return;
    led_strip_set_pixel(s_led, 0, r, g, b);
    led_strip_refresh(s_led);
}

static void led_update(void)
{
    if (xTaskGetTickCount() < s_flash_until) {
        led_color(0, APP_LED_LEVEL, APP_LED_LEVEL);
    } else if (s_capture) {
        led_color(APP_LED_LEVEL, 0, APP_LED_LEVEL);
    } else if (s_mqtt_connected) {
        led_color(0, APP_LED_LEVEL, 0);
    } else {
        led_color(APP_LED_LEVEL, 0, 0);
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    if (id == MQTT_EVENT_CONNECTED) {
        s_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
    }
}

static void start_mqtt(void)
{
    if (s_mqtt) return;
    esp_mqtt_client_config_t mc = {
        .broker.address.hostname = "mqtt3.thingspeak.com",
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .broker.address.port = 1883,
        .credentials.client_id = APP_MQTT_CLIENT_ID,
        .credentials.username = APP_MQTT_USERNAME,
        .credentials.authentication.password = APP_MQTT_PASSWORD,
    };
    s_mqtt = esp_mqtt_client_init(&mc);
    if (!s_mqtt) {
        ESP_LOGE(TAG, "MQTT initialization failed");
        return;
    }
    esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (esp_mqtt_client_start(s_mqtt) != ESP_OK) {
        ESP_LOGE(TAG, "MQTT start failed");
        s_mqtt = NULL;
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_mqtt_connected = false;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "Wi-Fi connected; starting MQTT");
        start_mqtt();
    }
}

static int network_init(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    if (nvs != ESP_OK) return -1;
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return -1;
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return -1;
    if (!esp_netif_create_default_wifi_sta()) return -1;
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&wc) != ESP_OK) return -1;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, APP_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, APP_WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK || esp_wifi_start() != ESP_OK) return -1;
    snprintf(s_topic, sizeof(s_topic), "channels/%s/publish", APP_THINGSPEAK_CHANNEL_ID);
    return 0;
}

static int sensor_init(void)
{
    const i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = APP_I2C_SDA_GPIO, .scl_io_num = APP_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return -1;
    if (i2c_master_probe(bus, MPU_ADDR, 50) != ESP_OK) return -1;
    const i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = MPU_ADDR, .scl_speed_hz = APP_I2C_FREQUENCY_HZ};
    if (i2c_master_bus_add_device(bus, &dc, &s_mpu) != ESP_OK) return -1;
    uint8_t reg = MPU_WHO_AM_I, who = 0;
    if (i2c_master_transmit_receive(s_mpu, &reg, 1, &who, 1, 100) != ESP_OK || who != 0x68) return -1;
    uint8_t wake[2] = {MPU_PWR_MGMT_1, 0};
    return i2c_master_transmit(s_mpu, wake, sizeof(wake), 100) == ESP_OK ? 0 : -1;
}

static void imu_error_task(void *arg)
{
    (void)arg;
    for (;;) {
        led_color(APP_LED_LEVEL, APP_LED_LEVEL, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        led_color(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        led_color(APP_LED_LEVEL, APP_LED_LEVEL, 0);
        vTaskDelay(pdMS_TO_TICKS(150));
        led_color(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(900));
    }
}

static bool read_imu(float *ax, float *ay, float *az, float *gx, float *gy, float *gz)
{
    uint8_t reg = MPU_ACCEL_XOUT_H, d[14];
    if (i2c_master_transmit_receive(s_mpu, &reg, 1, d, sizeof(d), 100) != ESP_OK) return false;
    int16_t a0=(int16_t)((d[0]<<8)|d[1]), a1=(int16_t)((d[2]<<8)|d[3]), a2=(int16_t)((d[4]<<8)|d[5]);
    int16_t g0=(int16_t)((d[8]<<8)|d[9]), g1=(int16_t)((d[10]<<8)|d[11]), g2=(int16_t)((d[12]<<8)|d[13]);
    *ax=a0/16384.0f; *ay=a1/16384.0f; *az=a2/16384.0f;
    *gx=g0/131.0f; *gy=g1/131.0f; *gz=g2/131.0f;
    return true;
}

static void publish_reps(void)
{
    int64_t now = esp_timer_get_time()/1000;
    if (!s_mqtt || !s_mqtt_connected || now-s_last_publish_ms < APP_PUBLISH_INTERVAL_MS) return;
    char payload[32]; snprintf(payload, sizeof(payload), "field1=%lu", (unsigned long)s_pending_reps);
    if (esp_mqtt_client_publish(s_mqtt, s_topic, payload, 0, 0, 0) >= 0) {
        s_last_publish_ms=now; ESP_LOGI(TAG, "published reps=%lu", (unsigned long)s_pending_reps);
    }
}

static void curl_task(void *arg)
{
    (void)arg;
    float ax,ay,az,gx,gy,gz;
    int64_t capture_started=0, last_rep=-APP_CURL_MIN_REP_MS;
    int axis=0, direction=0;
    int64_t direction_started_ms=0;
    bool waiting_neutral=false;
    float peak[3]={0,0,0};
    bool button_down=false;
    if (s_training_complete) {
        ESP_LOGI(TAG, "saved curl calibration active on axis=%d", s_curl_axis);
    } else {
        ESP_LOGI(TAG, "curl capture ready; press BOOT to start/stop capture");
    }
    for (;;) {
        int64_t now=esp_timer_get_time()/1000;
        int button=gpio_get_level(APP_BUTTON_GPIO);
        if (button==0 && !button_down) {
            button_down=true; s_capture=!s_capture; capture_started=now;
            if (s_capture) { peak[0]=peak[1]=peak[2]=0; direction=0; ESP_LOGI(TAG,"capture started; perform one complete curl"); }
            else {
                s_curl_axis = (peak[1] >= peak[0] && peak[1] >= peak[2]) ? 1 : ((peak[0] >= peak[2]) ? 0 : 2);
                direction = 0;
                if (save_curl_calibration(s_curl_axis)) {
                    s_training_complete = true;
                    ESP_LOGI(TAG,"capture stopped: peaks x=%.1f y=%.1f z=%.1f dps; dominant axis=%d; calibration saved; counting enabled",peak[0],peak[1],peak[2],s_curl_axis);
                } else {
                    s_training_complete = false;
                    ESP_LOGW(TAG,"capture stopped but calibration was not saved; fresh capture required");
                }
            }
        } else if (button!=0) button_down=false;
        if (read_imu(&ax,&ay,&az,&gx,&gy,&gz)) {
            float v[3]={gx,gy,gz};
            for (int i=0;i<3;i++) if (fabsf(v[i])>peak[i]) peak[i]=fabsf(v[i]);
            axis = s_curl_axis;
            float rate = v[axis];
            if (s_capture) {
                ESP_LOGI(TAG,"gyro=(%.1f,%.1f,%.1f) dps axis=%d",gx,gy,gz,axis);
                if (now-capture_started>APP_CURL_CAPTURE_TIMEOUT_MS) {
                    s_capture=false;
                    ESP_LOGW(TAG,"capture timeout; counting remains disabled until capture is stopped");
                }
            } else if (s_training_complete) {
                if (waiting_neutral) {
                    if (fabsf(rate) <= APP_CURL_GYRO_RELEASE_DPS) {
                        waiting_neutral = false;
                        direction = 0;
                    }
                } else if (direction == 0 && fabsf(rate) > APP_CURL_GYRO_THRESHOLD_DPS) {
                    direction = rate > 0 ? 1 : -1;
                    direction_started_ms = now;
                } else if (direction != 0 && now - direction_started_ms > APP_CURL_MAX_REP_MS) {
                    ESP_LOGW(TAG, "curl direction timed out after %lld ms; re-arming", (long long)(now - direction_started_ms));
                    direction = 0;
                    direction_started_ms = 0;
                } else if (direction != 0 && direction * rate < -APP_CURL_GYRO_THRESHOLD_DPS && now - last_rep >= APP_CURL_MIN_REP_MS) {
                    s_reps++;
                    s_pending_reps = s_reps;
                    last_rep = now;
                    waiting_neutral = true;
                    direction = 0;
                    direction_started_ms = 0;
                    s_flash_until = xTaskGetTickCount() + pdMS_TO_TICKS(APP_LED_STEP_FLASH_MS);
                    ESP_LOGI(TAG, "curl rep=%lu", (unsigned long)s_reps);
                }
            }
        }
        led_update(); publish_reps();
        vTaskDelay(pdMS_TO_TICKS(APP_CURL_SAMPLE_PERIOD_MS));
    }
}

int curl_device_start(void)
{
    gpio_config_t gc={.pin_bit_mask=1ULL<<APP_BUTTON_GPIO,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE,.intr_type=GPIO_INTR_DISABLE};
    if (gpio_config(&gc)!=ESP_OK) return -1;
    const led_strip_config_t lc={.strip_gpio_num=APP_LED_GPIO,.max_leds=1};
    const led_strip_rmt_config_t rc={.resolution_hz=10000000,.flags.with_dma=false};
    if (led_strip_new_rmt_device(&lc,&rc,&s_led)!=ESP_OK) return -1;
    led_strip_clear(s_led);
    if (network_init()!=0) ESP_LOGW(TAG,"network initialization incomplete; offline red status");
    load_curl_calibration();
    if (sensor_init()!=0) {
        ESP_LOGE(TAG,"MPU6050 initialization failed");
        if (xTaskCreate(imu_error_task, "imu_error", 2048, NULL, 2, NULL) != pdPASS) {
            led_color(APP_LED_LEVEL, APP_LED_LEVEL, 0);
        }
        return -1;
    }
    s_last_publish_ms=-(int64_t)APP_PUBLISH_INTERVAL_MS;
    return xTaskCreate(curl_task,"curl_task",6144,NULL,5,NULL)==pdPASS?0:-1;
}
