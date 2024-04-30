#define STA_HOSTNAME "air-quality-logging"
#define STA_SSID ""
#define STA_PASS ""

#define MQTT_HOST "mqtt.netpie.io"
#define MQTT_PORT 1883
#define MQTT_ID ""
#define MQTT_USER ""
#define MQTT_PASS ""
#define MQTT_PUBLISH "@shadow/data/update"
#define MQTT_PUBLISH_BATCH "@shadow/batch/update"
#define MQTT_BUFFER_SIZE 16383

#define NTP_HOST "pool.ntp.org"
#define NTP_UPDATE_INTERVAL 3600000UL
#define NTP_OFFSET 25200L

#define LOOP_INTERVAL 1000UL

#define LED_HIGH LOW
#define LED_LOW HIGH

// --------------------

#ifdef DEBUG
    #define PRINT(...) Serial.printf(__VA_ARGS__)
#else
    #define PRINT(...)
#endif

#include <vector>
#include <tuple>

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

#include <ArduinoJson.h>
#include <DHT.h>
#include <NTPClient.h>
#include <PubSubClient.h>

void mqtt_connect();
void batch_re_publish();

std::vector<std::tuple<unsigned long, float, float, float>> batch_data;

DHT dht(D3, DHT11);
WiFiUDP NTP_UDP;
NTPClient NTP(NTP_UDP, NTP_HOST, NTP_OFFSET, NTP_UPDATE_INTERVAL);
WiFiClient MQTT_TCP;
PubSubClient mqtt(MQTT_HOST, MQTT_PORT, MQTT_TCP);

float temperature_old = 0.0F;
float humidity_old = 0.0F;

void setup() {
    #ifdef DEBUG
        Serial.begin(DEBUG);
    #endif

    PRINT("Air Quality Logging\n");
    PRINT("Board: %s (%s)\n", ARDUINO_BOARD, ARDUINO_BOARD_ID);
    PRINT("WiFi Hostname: %s\n", STA_HOSTNAME);
    PRINT("WiFi SSID: %s\n", STA_SSID);
    PRINT("NTP Host: %s\n", NTP_HOST);
    PRINT("NTP Offset: %lds\n", NTP_OFFSET);
    PRINT("NTP Update interval: %ldms\n", NTP_UPDATE_INTERVAL);
    PRINT("MQTT Host: %s:%d\n", MQTT_HOST, MQTT_PORT);

    PRINT("Set pin mode GPIO %d as Output\n", LED_BUILTIN);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LED_HIGH);

    PRINT("WiFi Begin Station Mode\n");
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(STA_HOSTNAME);
    WiFi.begin(STA_SSID, STA_PASS);

    PRINT("Waiting for WiFi Connect\n");
    while (WiFi.status() != WL_CONNECTED) {
        PRINT(".");
        delay(100);
    }
    PRINT("\nWiFi Connected\n");

    PRINT("WiFi Local IP: %s\n", WiFi.localIP().toString().c_str());

    PRINT("Enable WiFi Auto reconnect\n");
    WiFi.setAutoReconnect(true);
    WiFi.persistent(true);

    PRINT("NTP Begin\n");
    NTP.begin();
    NTP.update();
    PRINT("NTP Synced : %lu [%s]\n", NTP.getEpochTime(), NTP.getFormattedTime().c_str());
    PRINT("DHT Begin\n");
    dht.begin();
    PRINT("MQTT Connect\n");
    mqtt_connect();

    mqtt.setBufferSize(MQTT_BUFFER_SIZE);

    digitalWrite(LED_BUILTIN, LED_LOW);

    PRINT("System free heap size: %lu\n", system_get_free_heap_size());
}

void loop() {
    NTP.update();

    const unsigned long epoch_time = NTP.getEpochTime();

    float temperature = dht.readTemperature(false, false);
    float humidity = dht.readHumidity(false);

    if (isnan(temperature) || isnan(humidity)) {
        PRINT("DHT Sensor Failed\n");

        delay(1000);
        digitalWrite(LED_BUILTIN, LED_LOW);
        delay(1000);

        return;
    }

    if ((temperature != temperature_old) || (humidity != humidity_old)) {
        digitalWrite(LED_BUILTIN, LED_HIGH);

        temperature_old = temperature;
        humidity_old = humidity;

        const float heat_index = dht.computeHeatIndex(false);

        PRINT("[%lu] DHT Temperature: %f°C Humidity: %f%c Heat Index: %f°C\n", epoch_time, temperature, humidity, '%', heat_index);

        JsonDocument doc;
        JsonObject data = doc.createNestedObject("data");

        data["time"] = epoch_time;
        data["temp"] = temperature;
        data["humid"] = humidity;
        data["index"] = heat_index;

        if (!mqtt.connected()) mqtt_connect();

        if (mqtt.publish(MQTT_PUBLISH, doc.as<String>().c_str())) {
            PRINT("MQTT Data is published\n");
        } else {
            batch_data.push_back({ epoch_time, temperature, humidity, heat_index });
            PRINT("MQTT Failed to publish data, add to batch %u\n", batch_data.size());
        }

        PRINT("System free heap size: %lu\n", system_get_free_heap_size());
        delay(100);
        digitalWrite(LED_BUILTIN, LED_LOW);
    }

    batch_re_publish();

    delay(LOOP_INTERVAL);
}

void mqtt_connect() {
    if (mqtt.connect(MQTT_ID, MQTT_USER, MQTT_PASS)) {
        PRINT("MQTT Connected\n");
    } else {
        PRINT("MQTT Failed to connect, Retry...\n");
        digitalWrite(LED_BUILTIN, LED_HIGH);
        delay(LOOP_INTERVAL);
        digitalWrite(LED_BUILTIN, LED_LOW);
    }
}

void batch_re_publish() {
    if (!batch_data.empty()) {
        PRINT("MQTT Batch data is found\n");

        JsonDocument doc;
        JsonArray batch = doc.createNestedArray("batch");

        const unsigned long epoch_time = NTP.getEpochTime();

        for (auto it : batch_data) {
            JsonObject obj = batch.createNestedObject();
            JsonObject data = obj.createNestedObject("data");

            data["time"] = std::get<0>(it);
            data["temp"] = std::get<1>(it);
            data["humid"] = std::get<2>(it);
            data["index"] = std::get<3>(it);

            obj["ts"] = epoch_time - std::get<0>(it);
        }

        if (!mqtt.connected()) mqtt_connect();

        if (mqtt.publish(MQTT_PUBLISH_BATCH, doc.as<String>().c_str())) {
            PRINT("MQTT Batch data is published\n");
            batch_data.clear();
        } else {
            PRINT("MQTT Failed to publish batch data\n");
        }

        PRINT("System free heap size: %lu\n", system_get_free_heap_size());
    }
}
