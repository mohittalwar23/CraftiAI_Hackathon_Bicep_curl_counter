#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ── Setup ────────────────────────────────────────────────────────────────
 * This is a template. Copy it to app_config.h (already gitignored) and fill
 * in your own values there:
 *
 *   cp main/app_config.example.h main/app_config.h
 */

/* ── Wi-Fi ────────────────────────────────────────────────────────────────
 * Point these at your phone's mobile hotspot. Your laptop must join the
 * SAME hotspot network to see the device on the LAN (e.g. for flashing/
 * monitoring, or a local dashboard) — the two SSIDs/passwords below only
 * affect what the ESP32 itself connects to.
 *
 * Notes for phone hotspots:
 *  - ESP32 is 2.4 GHz only. If your phone defaults its hotspot to 5 GHz
 *    (common on Android when few devices are around), force it to 2.4 GHz
 *    (or "Auto"/"Dual") in hotspot settings, or it will never see the SSID.
 *  - iPhone Personal Hotspot broadcasts 2.4 GHz automatically — no change
 *    needed there.
 */
#define APP_WIFI_SSID "YOUR_WIFI_SSID"
#define APP_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

/* ── MQTT: ThingSpeak ─────────────────────────────────────────────────────
 * ThingSpeak's MQTT broker requires per-account MQTT credentials (not the
 * channel Write API Key) plus a channel-scoped topic. To get these values:
 *   1. thingspeak.com -> sign in -> Channels -> New Channel
 *      (enable at least "Field 1") -> Save. Note the numeric Channel ID.
 *   2. Devices -> MQTT -> "Add a new device" -> generate credentials.
 *      This gives you a Client ID, Username, and Password (MQTT API Key) —
 *      paste those below.
 *   3. Leave the broker URI as-is (mqtt3.thingspeak.com); it's the same
 *      for every account.
 */
#define APP_MQTT_URI "mqtt://mqtt3.thingspeak.com:1883"
#define APP_MQTT_CLIENT_ID "YOUR_THINGSPEAK_MQTT_CLIENT_ID"
#define APP_MQTT_USERNAME "YOUR_THINGSPEAK_MQTT_USERNAME"
#define APP_MQTT_PASSWORD "YOUR_THINGSPEAK_MQTT_PASSWORD"
#define APP_THINGSPEAK_CHANNEL_ID "YOUR_CHANNEL_ID"

#define APP_WIFI_RETRY_LIMIT 8
#define APP_NETWORK_WAIT_MS 15000

/* ThingSpeak's free tier rejects updates to the same channel faster than
 * once every 15 seconds — keep this at 15000 or higher. */
#define APP_PUBLISH_INTERVAL_MS 15000

#define APP_LED_GPIO 8
#define APP_BUTTON_GPIO 9
#define APP_I2C_SDA_GPIO 4
#define APP_I2C_SCL_GPIO 5
#define APP_I2C_FREQUENCY_HZ 100000
#define APP_CALIBRATION_MS 5000
#define APP_SENSOR_PERIOD_MS 20
#define APP_STEP_THRESHOLD_G 0.20f
#define APP_STEP_RELEASE_G 0.10f
#define APP_STEP_REFRACTORY_MS 300
#define APP_BUTTON_DEBOUNCE_MS 50
#define APP_BUTTON_LONG_PRESS_MS 2000
#define APP_RECALIBRATION_MS 5000
#define APP_CURL_SAMPLE_PERIOD_MS 20
#define APP_CURL_CAPTURE_TIMEOUT_MS 10000
#define APP_CURL_GYRO_THRESHOLD_DPS 35.0f
#define APP_CURL_GYRO_RELEASE_DPS 10.0f
#define APP_CURL_MIN_REP_MS 500
#define APP_CURL_MAX_REP_MS 3000
#define APP_LED_LEVEL 48
#define APP_LED_STEP_FLASH_MS 60

#endif
