#ifndef PYLONTECH_H
#define PYLONTECH_H

// +++ START CONFIGURATION +++

// Firmware Version
// 1 old firmware
// 2 new firmware
const int FW_VERSION=1;

// IMPORTANT: Specify your WiFi network credentials
#define WIFI_SSID "*** your ssid ***"
#define WIFI_PASS "*** your wifi pass ***"
#define WIFI_HOSTNAME "PylontechBattery"

// Uncomment to enable static IP configuration
//#define STATIC_IP
// Static IP address
IPAddress ip(192, 168, 11, 10);
IPAddress subnet(255, 255, 255, 0);
IPAddress gateway(192, 168, 11, 1);
IPAddress dns(192, 168, 11, 1);

// Uncomment to enable web-based authentication
//#define AUTHENTICATION
// HTTP Authentication credentials
const char* www_username = "admin";
const char* www_password = "password";

// IMPORTANT: Uncomment this line if you want to enable MQTT
#define ENABLE_MQTT

// Set your time offset in seconds (e.g., GMT +1 = 3600, GMT +2 = 7200, etc.)
#define GMT 7200

// MQTT broker settings
#define MQTT_SERVER        "*** your MQTT server ***"
#define MQTT_PORT          1883
#define MQTT_USER          "*** your MQTT username ***"
#define MQTT_PASSWORD      "*** your MQTT password ***"

// MQTT topic root
// For example, if "soc" is published, it becomes: pylontech/sensor/grid_battery/soc
#define MQTT_TOPIC_ROOT    "pylontech/sensor/grid_battery/"

// Frequency (in seconds) for pushing data to the MQTT broker
#define MQTT_PUSH_FREQ_SEC 2

// Max Batteries
#define MAX_PYLON_BATTERIES 2

static const char HA_DISCOVERY_SENSOR_PREFIX[] = "homeassistant/sensor/";

// +++ END CONFIGURATION +++

#endif // PYLONTECH_H