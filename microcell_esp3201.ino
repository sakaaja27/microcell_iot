#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include "secrets.h"

//====================================================
// RELAY CONFIG
//====================================================
struct RelayConfig {
    const char *relayPath;
    const char *statusPath;
    int         pin;
    bool        hasPhysicalPin;
};

static const RelayConfig RELAYS[] = {
    { "/devices/main/relays/relay1", "/devices/main/status/relay1", 23, true  },
    { "/devices/main/relays/relay2", "/devices/main/status/relay2", 22, true  },
    { "/devices/main/relays/relay3", "/devices/main/status/relay3", 21, true  },
    { "/devices/main/relays/relay4", "/devices/main/status/relay4", 19, true  },
};
static const int RELAY_COUNT = sizeof(RELAYS) / sizeof(RELAYS[0]);

//====================================================
// TIMING
//====================================================
const unsigned long RELAY_CHECK_INTERVAL_MS  = 20;
const unsigned long STATUS_PRINT_INTERVAL_MS = 2000;
const unsigned long WIFI_TIMEOUT_MS          = 20000;
const unsigned long FIREBASE_TIMEOUT_MS      = 1500;

//====================================================
// STATE
//====================================================
FirebaseData   fbdo;
FirebaseAuth   auth;
FirebaseConfig config;

unsigned long lastRelayCheckMillis  = 0;
unsigned long lastStatusPrintMillis = 0;

bool relayStates[RELAY_COUNT] = {};

//====================================================
void printConnectionIndicator()
{
    if (millis() - lastStatusPrintMillis < STATUS_PRINT_INTERVAL_MS) return;
    lastStatusPrintMillis = millis();

    Serial.print("[STATUS] WiFi: ");
    Serial.print(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
    Serial.print(" | Firebase: ");
    Serial.println(Firebase.ready() ? "CONNECTED" : "DISCONNECTED");
}

//====================================================
void connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED) return;

    Serial.println("\nConnecting WiFi");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
        Serial.print(".");
        delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected | IP: " + WiFi.localIP().toString());
    } else {
        Serial.println("\nWiFi Connection Failed");
    }
}

//====================================================
void handleRelay(int index)
{
    const RelayConfig &r = RELAYS[index];

    if (!Firebase.RTDB.getBool(&fbdo, r.relayPath)) {
        Serial.printf("Relay Error (%s): %s\n", r.relayPath, fbdo.errorReason().c_str());
        return; // fail-safe: biarkan kondisi terakhir
    }

    bool newState = fbdo.boolData();

    if (newState != relayStates[index]) {
        Serial.printf("%s -> %s\n", r.relayPath, newState ? "ON" : "OFF");
        relayStates[index] = newState;
    }

    if (r.hasPhysicalPin)
        digitalWrite(r.pin, newState ? LOW : HIGH); // active LOW

    Firebase.RTDB.setBool(&fbdo, r.statusPath, newState);
}

//====================================================
void setup()
{
    Serial.begin(115200);

    for (int i = 0; i < RELAY_COUNT; i++) {
        if (RELAYS[i].hasPhysicalPin) {
            pinMode(RELAYS[i].pin, OUTPUT);
            digitalWrite(RELAYS[i].pin, HIGH); // relay OFF saat boot (active LOW)
        }
    }

    connectWiFi();

    config.api_key              = API_KEY;
    config.database_url         = DATABASE_URL;
    auth.user.email             = USER_EMAIL;
    auth.user.password          = USER_PASSWORD;
    config.token_status_callback = tokenStatusCallback;

    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    Serial.println("Waiting Firebase Login...");
    unsigned long start = millis();
    while (!Firebase.ready() && millis() - start < FIREBASE_TIMEOUT_MS) {
        Serial.print(".");
        delay(500);
    }

    Serial.println(Firebase.ready() ? "\nFirebase Connected" : "\nFirebase Login Failed");
}

//====================================================
void loop()
{
    printConnectionIndicator();

    if (WiFi.status() != WL_CONNECTED) { connectWiFi(); return; }
    if (!Firebase.ready()) return;
    if (millis() - lastRelayCheckMillis < RELAY_CHECK_INTERVAL_MS) return;

    lastRelayCheckMillis = millis();

    for (int i = 0; i < RELAY_COUNT; i++)
        handleRelay(i);
}