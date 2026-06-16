// PotPi ESP8266 firmware (production, calibrated)
// -----------------------------------------------
// Two capacitive moisture sensors on MCP3008 channels 0 and 1. Pump 2 sits behind
// an active-LOW relay on D1. WiFi credentials and the GitHub fine-grained PAT live
// in secrets.h (gitignored).
//
// Architecture:
//   - WiFi is brought up ONCE at boot and kept up (so OTA is always reachable
//     and the GitHub POST has minimal latency). On a USB-powered device the
//     extra ~80 mA WiFi idle current is negligible.
//   - ArduinoOTA is started at boot — future firmware updates over WiFi.
//   - NTP synced at boot (so the very first decision has a valid clock).
//   - Every MEASUREMENT_INTERVAL_MS the firmware pulse-samples the MCP3008,
//     applies per-sensor calibration, decides whether to water, and POSTs a
//     repository_dispatch event to GitHub.
//
// Hardware safety:
//   - Active-LOW pump output is set HIGH BEFORE pinMode(OUTPUT) (no boot glitch).
//   - Pump pulse is hard-capped (MAX_PUMP_SECONDS).
//   - Sensor readings outside [V_VALID_LOW, V_VALID_HIGH] are flagged invalid
//     and the watering decision skips them.

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <SPI.h>
#include <time.h>
#include <ArduinoJson.h>

#include "secrets.h"

// --- Pin map ---
// (PIN_SPI_MISO / PIN_SPI_MOSI are already #defined as macros by the core; use PIN_MCP_*.)
constexpr uint8_t  PIN_MCP_CS     = 16;  // D0   software CS for MCP3008
constexpr uint8_t  PIN_MCP_CLK    = 14;  // D5   HSPI CLK
constexpr uint8_t  PIN_MCP_MISO   = 12;  // D6   HSPI MISO
constexpr uint8_t  PIN_MCP_MOSI   = 13;  // D7   HSPI MOSI
constexpr uint8_t  PIN_SENSOR_PWR = 4;   // D2   (vestigial — no transistor; toggles a disconnected pin)
constexpr uint8_t  PIN_PUMP       = 5;   // D1   active-LOW relay (IN2), idles HIGH = pump off

// --- Sampling parameters ---
constexpr uint8_t  SAMPLES_PER_CYCLE     = 12;
constexpr uint8_t  CYCLES_PER_READING    = 3;
constexpr uint8_t  DISCARD_INITIAL       = 2;
constexpr uint16_t SAMPLE_INTERVAL_MS    = 50;
constexpr uint16_t SETTLE_MS             = 1500;
constexpr uint16_t INTER_CYCLE_MS        = 1000;

// --- Calibration (per channel) — measured 2026-06-16 ---
constexpr float V_DRY_CH0 = 2.939f;   // sensor 1 voltage in dry air
constexpr float V_WET_CH0 = 1.397f;   // sensor 1 voltage fully submerged
constexpr float V_DRY_CH1 = 1.832f;   // sensor 2 voltage in dry air
constexpr float V_WET_CH1 = 0.645f;   // sensor 2 voltage fully submerged

// --- Decision parameters ---
constexpr float    THRESHOLD_PERCENT          = 25.0f;       // water if mean < this
constexpr float    WATERING_SECONDS           = 2.0f;        // pump pulse target
constexpr float    MAX_PUMP_SECONDS           = 10.0f;       // hard cap
constexpr uint32_t MIN_HOURS_BETWEEN_WATERING = 6;
constexpr uint32_t MEASUREMENT_INTERVAL_MS    = 60UL * 60UL * 1000UL;  // hourly
constexpr uint8_t  WINDOW_START_HOUR          = 8;           // UTC
constexpr uint8_t  WINDOW_END_HOUR            = 22;

// --- Plausibility checks ---
constexpr float V_VALID_LOW  = 0.1f;
constexpr float V_VALID_HIGH = 3.2f;

// --- Global state ---
WiFiClientSecure secureClient;
time_t lastWateredEpoch = 0;
bool ntpSynced = false;

// --- Utilities -----------------------------------------------------------

static int compareFloat(const void* a, const void* b) {
    float fa = *(const float*)a;
    float fb = *(const float*)b;
    return (fa > fb) - (fa < fb);
}

float median(float* arr, int n) {
    if (n <= 0) return 0.0f;
    qsort(arr, n, sizeof(float), compareFloat);
    return (n & 1) ? arr[n / 2] : 0.5f * (arr[n / 2 - 1] + arr[n / 2]);
}

float adcToVoltage(uint16_t raw) {
    return (raw / 1023.0f) * 3.3f;
}

float voltageToPercent(float v, float vDry, float vWet) {
    float denom = (vWet - vDry);
    if (fabs(denom) < 1e-6f) return 0.0f;
    float p = ((v - vDry) / denom) * 100.0f;
    return constrain(p, 0.0f, 100.0f);
}

// --- MCP3008 read --------------------------------------------------------
uint16_t readMCP3008(uint8_t channel) {
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_MCP_CS, LOW);
    SPI.transfer(0x01);
    uint8_t hi = SPI.transfer((0x08 | (channel & 0x07)) << 4);
    uint8_t lo = SPI.transfer(0x00);
    digitalWrite(PIN_MCP_CS, HIGH);
    SPI.endTransaction();
    return ((hi & 0x03) << 8) | lo;
}

void sensorPowerOn()  { digitalWrite(PIN_SENSOR_PWR, LOW);  }
void sensorPowerOff() { digitalWrite(PIN_SENSOR_PWR, HIGH); }

// --- Pump ---------------------------------------------------------------
float runPump(float seconds) {
    seconds = constrain(seconds, 0.0f, MAX_PUMP_SECONDS);
    if (seconds <= 0.0f) return 0.0f;
    digitalWrite(PIN_PUMP, LOW);
    uint32_t end = millis() + (uint32_t)(seconds * 1000);
    while ((int32_t)(end - millis()) > 0) {
        delay(20);
        yield();
        ArduinoOTA.handle();   // keep OTA responsive even during pump
    }
    digitalWrite(PIN_PUMP, HIGH);
    return seconds;
}

// --- Reading ------------------------------------------------------------
struct Reading {
    float ch0_voltage = 0;
    float ch1_voltage = 0;
    float ch0_percent = 0;
    float ch1_percent = 0;
    float agg_percent = 0;
    bool  ch0_valid = false;
    bool  ch1_valid = false;
    bool  valid = false;
};

Reading takeReading() {
    Reading r;
    float ch0_cycles[CYCLES_PER_READING];
    float ch1_cycles[CYCLES_PER_READING];

    for (int c = 0; c < CYCLES_PER_READING; c++) {
        sensorPowerOn();
        for (uint16_t t = 0; t < SETTLE_MS; t += 100) {
            delay(100); yield(); ArduinoOTA.handle();
        }

        float ch0_samples[SAMPLES_PER_CYCLE];
        float ch1_samples[SAMPLES_PER_CYCLE];
        for (int i = 0; i < SAMPLES_PER_CYCLE; i++) {
            ch0_samples[i] = adcToVoltage(readMCP3008(0));
            ch1_samples[i] = adcToVoltage(readMCP3008(1));
            delay(SAMPLE_INTERVAL_MS);
            yield();
        }
        sensorPowerOff();

        int kept = SAMPLES_PER_CYCLE - DISCARD_INITIAL;
        ch0_cycles[c] = median(ch0_samples + DISCARD_INITIAL, kept);
        ch1_cycles[c] = median(ch1_samples + DISCARD_INITIAL, kept);

        if (c < CYCLES_PER_READING - 1) {
            for (uint16_t t = 0; t < INTER_CYCLE_MS; t += 100) {
                delay(100); yield(); ArduinoOTA.handle();
            }
        }
    }

    r.ch0_voltage = median(ch0_cycles, CYCLES_PER_READING);
    r.ch1_voltage = median(ch1_cycles, CYCLES_PER_READING);
    r.ch0_percent = voltageToPercent(r.ch0_voltage, V_DRY_CH0, V_WET_CH0);
    r.ch1_percent = voltageToPercent(r.ch1_voltage, V_DRY_CH1, V_WET_CH1);
    r.ch0_valid   = (r.ch0_voltage >= V_VALID_LOW && r.ch0_voltage <= V_VALID_HIGH);
    r.ch1_valid   = (r.ch1_voltage >= V_VALID_LOW && r.ch1_voltage <= V_VALID_HIGH);
    r.valid       = r.ch0_valid || r.ch1_valid;

    int n = 0; float sum = 0;
    if (r.ch0_valid) { sum += r.ch0_percent; n++; }
    if (r.ch1_valid) { sum += r.ch1_percent; n++; }
    r.agg_percent = (n > 0) ? (sum / n) : 0.0f;

    return r;
}

// --- WiFi / NTP ---------------------------------------------------------
bool connectWiFi(uint32_t timeoutMs = 30000) {
    Serial.printf("WiFi connecting to %s ", WIFI_SSID);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
        delay(250); Serial.print("."); yield();
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi up: %s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
    }
    Serial.println("WiFi connect failed");
    return false;
}

bool syncTime(uint32_t timeoutMs = 20000) {
    configTime("UTC0", "pool.ntp.org", "time.google.com");
    time_t now;
    uint32_t start = millis();
    while ((now = time(nullptr)) < 1700000000 && (millis() - start) < timeoutMs) {
        delay(250); yield(); ArduinoOTA.handle();
    }
    if (now >= 1700000000) {
        Serial.printf("NTP synced: %ld\n", (long)now);
        return true;
    }
    Serial.println("NTP sync failed");
    return false;
}

// --- GitHub repository_dispatch POST ------------------------------------
bool postReading(const Reading& r, bool watered, float pumpSeconds) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WiFi not connected, attempting reconnect");
        if (!connectWiFi(15000)) return false;
    }
    if (!ntpSynced) {
        ntpSynced = syncTime();
        if (!ntpSynced) return false;
    }

    secureClient.setBufferSizes(1024, 1024);
    secureClient.setInsecure();   // TODO: replace with proper CA pinning

    HTTPClient http;
    const String url =
        String("https://api.github.com/repos/") + GITHUB_OWNER + "/" + GITHUB_REPO + "/dispatches";

    if (!http.begin(secureClient, url)) {
        Serial.println("http.begin failed");
        return false;
    }
    http.addHeader("Accept",                "application/vnd.github+json");
    http.addHeader("Authorization",         String("Bearer ") + GITHUB_PAT);
    http.addHeader("X-GitHub-Api-Version",  "2022-11-28");
    http.addHeader("Content-Type",          "application/json");
    http.addHeader("User-Agent",            String("PotPi-ESP/") + DEVICE_ID);

    time_t now = time(nullptr);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

    // GitHub repository_dispatch limits client_payload to <= 10 top-level properties.
    JsonDocument doc;
    doc["event_type"] = "potpi-reading";
    JsonObject p = doc["client_payload"].to<JsonObject>();
    p["ts"]      = ts;
    p["device"]  = DEVICE_ID;
    p["v0"]      = serialized(String(r.ch0_voltage, 4));
    p["v1"]      = serialized(String(r.ch1_voltage, 4));
    p["p0"]      = serialized(String(r.ch0_percent, 1));
    p["p1"]      = serialized(String(r.ch1_percent, 1));
    p["pAgg"]    = serialized(String(r.agg_percent, 1));
    p["watered"] = watered;
    p["pumpSeconds"] = serialized(String(pumpSeconds, 2));

    String body;
    serializeJson(doc, body);
    Serial.printf("POST body: %s\n", body.c_str());

    int code = http.POST(body);
    Serial.printf("HTTP %d\n", code);
    if (code > 0) {
        String resp = http.getString();
        if (resp.length() > 0) Serial.println(resp);
    }
    http.end();

    return code >= 200 && code < 300;
}

// --- OTA setup ----------------------------------------------------------
void setupOTA() {
    ArduinoOTA.setHostname(DEVICE_ID);
    ArduinoOTA.onStart([]() { Serial.println("OTA: start"); });
    ArduinoOTA.onEnd([]()   { Serial.println("OTA: end");   });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); });
    ArduinoOTA.begin();
    Serial.println("OTA listening");
}

// --- Setup --------------------------------------------------------------
void setup() {
    // Active-LOW outputs idle HIGH before pinMode drives them.
    digitalWrite(PIN_SENSOR_PWR, HIGH);
    digitalWrite(PIN_PUMP,       HIGH);
    digitalWrite(PIN_MCP_CS,     HIGH);
    pinMode(PIN_SENSOR_PWR, OUTPUT);
    pinMode(PIN_PUMP,       OUTPUT);
    pinMode(PIN_MCP_CS,     OUTPUT);

    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.printf("PotPi ESP boot — device=%s, mac=%s\n",
                  DEVICE_ID, WiFi.macAddress().c_str());

    SPI.begin();
    SPI.setHwCs(false);

    connectWiFi();
    setupOTA();
    ntpSynced = syncTime();

    Serial.printf("setup done, free heap %u\n", ESP.getFreeHeap());
}

// --- Loop ---------------------------------------------------------------
uint32_t lastMeasurementMs = 0;
bool firstRun = true;

void loop() {
    ArduinoOTA.handle();

    uint32_t nowMs = millis();
    if (firstRun || (nowMs - lastMeasurementMs) >= MEASUREMENT_INTERVAL_MS) {
        firstRun = false;
        lastMeasurementMs = nowMs;

        Reading r = takeReading();
        Serial.printf("Reading: ch0=%.3fV (%.1f%%) ch1=%.3fV (%.1f%%) agg=%.1f%% valid=%d\n",
                      r.ch0_voltage, r.ch0_percent,
                      r.ch1_voltage, r.ch1_percent,
                      r.agg_percent, r.valid);

        bool watered = false;
        float pumpSeconds = 0.0f;

        if (r.valid && r.agg_percent < THRESHOLD_PERCENT) {
            time_t nowEpoch = time(nullptr);
            uint32_t hoursSince = (nowEpoch > lastWateredEpoch && lastWateredEpoch > 0)
                ? (uint32_t)((nowEpoch - lastWateredEpoch) / 3600)
                : UINT32_MAX;

            struct tm* utc = gmtime(&nowEpoch);
            uint8_t hour = (utc != nullptr) ? utc->tm_hour : 12;
            bool inWindow = (hour >= WINDOW_START_HOUR && hour < WINDOW_END_HOUR);

            if (hoursSince >= MIN_HOURS_BETWEEN_WATERING && inWindow) {
                Serial.println("Below threshold, in window, past cooldown — watering.");
                pumpSeconds = runPump(WATERING_SECONDS);
                watered = true;
                lastWateredEpoch = nowEpoch;
            } else {
                Serial.printf("Below threshold but skipping (hoursSince=%u, inWindow=%d)\n",
                              hoursSince, inWindow);
            }
        }

        bool ok = postReading(r, watered, pumpSeconds);
        Serial.printf("Publish: %s, free heap=%u\n", ok ? "OK" : "FAILED", ESP.getFreeHeap());
    }

    delay(10);
}
