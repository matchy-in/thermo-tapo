#include <Arduino.h>
#include <DHT.h>
#include <WiFi.h>
#include <time.h>
#include <cmath>

#if __has_include(<MPython.h>)
#include <MPython.h>
#define HAVE_MPYTHON 1
#endif

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif

#include "tapo_client.h"

// ---------------------------------------------------------------------------
// Pin map. The board labels (P0, P1 ...) are NOT ESP32 GPIO numbers; Arduino
// code needs the GPIO number.
//   Big edge pads:   P0=33  P1=32  P2=35 (input-only, DHT cannot work)
//   Front touch pads: P23 "P"=27  P24 "Y"=14  P25 "T"=12*  P26 "H"=13
//                     P27 "O"=15* P28 "N"=4
//   On-board:  P4=39 light  P5=0 button A  P6=16 buzzer  P7=17 pixels
//              P10=36 sound  P11=2 button B  P19=22 SCL  P20=23 SDA (screen)
//   * boot strapping pins: a sensor pull-up on P25 (GPIO12) can stop the
//     board booting, so don't wire the sensor there.
// ---------------------------------------------------------------------------
constexpr int GPIO_P0 = 33;
constexpr int GPIO_P1 = 32;
constexpr int GPIO_P23 = 27;
constexpr int BUTTON_A_GPIO = 0;
constexpr int BUTTON_B_GPIO = 2;

// Set to a GPIO number to force a pin, or -1 to auto-detect at boot.
constexpr int SENSOR_GPIO = -1;
constexpr int SENSOR_SCAN_PINS[] = {GPIO_P1, GPIO_P23, GPIO_P0};
constexpr uint8_t SENSOR_TYPE = DHT11;  // DHT22 for the white AM2302 sensor

// Thermostat
constexpr float HEAT_ON_AT_C = 16.0f;   // heater on at or below this
constexpr float HEAT_OFF_AT_C = 18.0f;  // heater off at or above this
constexpr uint32_t MIN_SWITCH_INTERVAL_MS = 60UL * 1000;
constexpr uint32_t SENSOR_FAILSAFE_MS = 2UL * 60 * 1000;  // no readings -> heater off
constexpr uint32_t MANUAL_OVERRIDE_MS = 30UL * 60 * 1000;

constexpr uint32_t SENSOR_INTERVAL_MS = 2500;
constexpr uint32_t STATUS_INTERVAL_MS = 5000;
constexpr uint32_t PLUG_SYNC_INTERVAL_MS = 60UL * 1000;
constexpr uint32_t TAPO_RETRY_MS = 30UL * 1000;
constexpr uint32_t WIFI_RETRY_MS = 15UL * 1000;

const char* NTP_SERVER = "uk.pool.ntp.org";
const char* UK_TZ = "GMT0BST,M3.5.0/1,M10.5.0";

TapoClient tapo;
DHT* dht = nullptr;
int sensorPin = -1;

enum class Mode { Auto, Manual };
Mode mode = Mode::Auto;
int plugState = -1;  // -1 unknown, 0 off, 1 on

float lastTempC = NAN;
float lastHumidity = NAN;
uint32_t lastValidReadingAt = 0;
uint32_t lastSwitchAt = 0;
bool everSwitched = false;
uint32_t manualSince = 0;

uint32_t lastSensorAt = 0, lastStatusAt = 0, lastSyncAt = 0, lastTapoAttemptAt = 0, lastWifiAttemptAt = 0;

struct Button {
    int pin;
    bool lastLevel = HIGH;
    uint32_t changedAt = 0;
    bool pressed() {
        bool level = digitalRead(pin);
        bool fired = false;
        if (level != lastLevel && millis() - changedAt > 40) {
            fired = (level == LOW);
            lastLevel = level;
            changedAt = millis();
        }
        return fired;
    }
};
Button buttonA{BUTTON_A_GPIO};
Button buttonB{BUTTON_B_GPIO};

bool tryReadSensor(DHT& d, float& t, float& h) {
    t = d.readTemperature(false, true);
    h = d.readHumidity();
    return !std::isnan(t) && !std::isnan(h);
}

void setupSensor() {
    if (SENSOR_GPIO >= 0) {
        sensorPin = SENSOR_GPIO;
        dht = new DHT(sensorPin, SENSOR_TYPE);
        dht->begin();
        Serial.printf("[Sensor] Using GPIO %d (fixed)\n", sensorPin);
        return;
    }

    Serial.println("[Sensor] Scanning for the DHT sensor...");
    for (int pin : SENSOR_SCAN_PINS) {
        DHT* candidate = new DHT(pin, SENSOR_TYPE);
        candidate->begin();
        delay(100);
        for (int attempt = 0; attempt < 2; attempt++) {
            float t, h;
            if (tryReadSensor(*candidate, t, h)) {
                Serial.printf("[Sensor] Found on GPIO %d: %.1f C, %.0f %%\n", pin, t, h);
                sensorPin = pin;
                dht = candidate;
                lastTempC = t;
                lastHumidity = h;
                lastValidReadingAt = millis();
                return;
            }
            delay(1100);
        }
        Serial.printf("[Sensor] Nothing on GPIO %d\n", pin);
        delete candidate;
    }
    Serial.println("[Sensor] NOT FOUND. Check wiring: VCC->3V3, GND->GND, DATA->P1, "
                   "and a 10k pull-up from DATA to 3V3 if your module has none.");
}

void connectWifi() {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
        Serial.print('.');
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] Connected, IP %s\n", WiFi.localIP().toString().c_str());
        configTzTime(UK_TZ, NTP_SERVER);
    } else {
        Serial.println("\n[WiFi] Timeout - will keep retrying in the background.");
    }
    lastWifiAttemptAt = millis();
}

const char* plugText() { return plugState == 1 ? "ON" : plugState == 0 ? "OFF" : "UNKNOWN"; }

bool setHeater(bool on, const char* reason) {
    Serial.printf("[Heater] Turning %s (%s)\n", on ? "ON" : "OFF", reason);
    lastTapoAttemptAt = millis();
    if (!tapo.setPower(on)) {
        Serial.println("[Heater] Command failed: " + tapo.lastError());
        return false;
    }
    plugState = on ? 1 : 0;
    lastSwitchAt = millis();
    everSwitched = true;
    return true;
}

void readSensor() {
    if (!dht) return;
    float t, h;
    if (tryReadSensor(*dht, t, h)) {
        lastTempC = t;
        lastHumidity = h;
        lastValidReadingAt = millis();
    }
}

void runThermostat() {
    uint32_t now = millis();

    if (mode == Mode::Manual && now - manualSince >= MANUAL_OVERRIDE_MS) {
        mode = Mode::Auto;
        Serial.println("[Mode] Manual override expired, back to AUTO");
    }
    if (mode != Mode::Auto) return;

    bool sensorFresh = lastValidReadingAt != 0 && now - lastValidReadingAt < SENSOR_FAILSAFE_MS;
    if (!sensorFresh) {
        if (plugState == 1 && now - lastTapoAttemptAt > TAPO_RETRY_MS) setHeater(false, "sensor failsafe");
        return;
    }

    bool canSwitch = !everSwitched || now - lastSwitchAt >= MIN_SWITCH_INTERVAL_MS;
    bool canRetry = now - lastTapoAttemptAt >= 10000;
    if (!canSwitch || !canRetry) return;

    if (lastTempC <= HEAT_ON_AT_C && plugState != 1) {
        setHeater(true, "temperature low");
    } else if (lastTempC >= HEAT_OFF_AT_C && plugState != 0) {
        setHeater(false, "temperature reached target");
    }
}

void printStatus() {
    struct tm tmNow;
    char timeStr[12] = "--:--:--";
    if (getLocalTime(&tmNow, 0)) strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &tmNow);

    Serial.println("----------------------------------------");
    Serial.printf("Time: %s | WiFi: %s | Mode: %s\n", timeStr,
                  WiFi.status() == WL_CONNECTED ? "OK" : "DOWN", mode == Mode::Auto ? "AUTO" : "MANUAL");
    if (lastValidReadingAt && millis() - lastValidReadingAt < 10000) {
        Serial.printf("Sensor (GPIO %d): %.1f C, %.0f %% | on <= %.1f C, off >= %.1f C\n", sensorPin, lastTempC,
                      lastHumidity, HEAT_ON_AT_C, HEAT_OFF_AT_C);
    } else {
        Serial.println("Sensor: no valid reading");
    }
    Serial.printf("Plug: %s | Tapo link: %s\n", plugText(), tapo.connected() ? tapo.protocolName() : "disconnected");
}

void setup() {
#ifdef HAVE_MPYTHON
    mPython.begin();
#endif
    Serial.begin(115200);
    delay(1500);
    Serial.println("\n\n=== MATRIXBIT THERMOSTAT BOOTING ===");

    pinMode(BUTTON_A_GPIO, INPUT_PULLUP);
    pinMode(BUTTON_B_GPIO, INPUT_PULLUP);

    setupSensor();
    connectWifi();

    tapo.begin(TAPO_IP, TAPO_EMAIL, TAPO_PASSWORD);
    Serial.printf("[Tapo] Connecting to plug at %s\n", TAPO_IP);
    lastTapoAttemptAt = millis();
    if (tapo.connect()) {
        plugState = tapo.getPower();
        Serial.printf("[Tapo] Connected via %s, plug is %s\n", tapo.protocolName(), plugText());
    }
    lastSyncAt = millis();
    Serial.println("=== SETUP COMPLETE ===\n");
    Serial.println("Button B: toggle heater (manual for 30 min). Button A: back to AUTO.");
}

void loop() {
    uint32_t now = millis();

    if (buttonB.pressed()) {
        mode = Mode::Manual;
        manualSince = now;
        setHeater(plugState != 1, "button B");
    }
    if (buttonA.pressed() && mode != Mode::Auto) {
        mode = Mode::Auto;
        Serial.println("[Mode] Back to AUTO");
    }

    if (WiFi.status() != WL_CONNECTED && now - lastWifiAttemptAt >= WIFI_RETRY_MS) {
        Serial.println("[WiFi] Reconnecting...");
        WiFi.reconnect();
        lastWifiAttemptAt = now;
    }

    if (now - lastSensorAt >= SENSOR_INTERVAL_MS) {
        lastSensorAt = now;
        readSensor();
        runThermostat();
    }

    if (!tapo.connected() && WiFi.status() == WL_CONNECTED && now - lastTapoAttemptAt >= TAPO_RETRY_MS) {
        lastTapoAttemptAt = now;
        if (tapo.connect()) plugState = tapo.getPower();
    }

    // Picks up changes made from the Tapo app or the plug's own button.
    if (tapo.connected() && now - lastSyncAt >= PLUG_SYNC_INTERVAL_MS) {
        lastSyncAt = now;
        int state = tapo.getPower();
        if (state >= 0) plugState = state;
    }

    if (now - lastStatusAt >= STATUS_INTERVAL_MS) {
        lastStatusAt = now;
        printStatus();
    }

    delay(10);
}
