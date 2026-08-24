#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include <LiquidCrystal_I2C.h>
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
    { "/devices/main/relays/relay1", "/devices/main/status/relay1", 23, true },
    { "/devices/main/relays/relay2", "/devices/main/status/relay2", 22, true },
    { "/devices/main/relays/relay3", "/devices/main/status/relay3", 21, true },
    { "/devices/main/relays/relay4", "/devices/main/status/relay4", 19, true },
};
static const int RELAY_COUNT = sizeof(RELAYS) / sizeof(RELAYS[0]);

//====================================================
// I2C & SENSOR CONFIG
//====================================================
#define I2C_SDA_PIN  16
#define I2C_SCL_PIN  17

// INA219 — baterai 12V lead acid
// Range: kosong = 10.5V, penuh = 12.7V
#define BATT_MIN_V   10.5f
#define BATT_MAX_V   12.7f

// Sensor tegangan DC 0-25V di pin 34
#define VOLT_SENSOR_PIN  18
#define ADC_RES          4095.0f
#define VREF             3.3f
#define VOLTAGE_DIVIDER  7.576f  // rasio modul 0-25V (R1=30k, R2=7.5k → (30+7.5)/7.5)
                                 // kalibrasi manual jika hasil tidak akurat

// Firebase paths
#define PATH_BATTERY  "/devices/main/status/batteryLevel"
#define PATH_CURRENT  "/devices/main/status/current"
#define PATH_VOLTAGE  "/devices/main/status/voltage"
#define PATH_ACVOLT   "/devices/main/status/acVoltage"

Adafruit_INA219   ina219;
LiquidCrystal_I2C lcd(0x27, 20, 4);
bool ina219Ready = false;

//====================================================
// TIMING
//====================================================
const unsigned long RELAY_CHECK_INTERVAL_MS   = 20;
const unsigned long STATUS_PRINT_INTERVAL_MS  = 2000;
const unsigned long SENSOR_UPLOAD_INTERVAL_MS = 5000;
const unsigned long WIFI_TIMEOUT_MS           = 20000;
const unsigned long FIREBASE_TIMEOUT_MS       = 1500;

//====================================================
// STATE
//====================================================
FirebaseData   fbdo;
FirebaseData   fbdoSensor;
FirebaseAuth   auth;
FirebaseConfig config;

unsigned long lastRelayCheckMillis   = 0;
unsigned long lastStatusPrintMillis  = 0;
unsigned long lastSensorUploadMillis = 0;

bool  relayStates[RELAY_COUNT] = {};
float g_voltage    = 0;
float g_current    = 0;
float g_power      = 0;
int   g_battery    = 0;
float g_acVoltage  = 0;

//====================================================
// HELPER
//====================================================
int calcBatteryLevel(float v)
{
    if (v >= BATT_MAX_V) return 100;
    if (v <= BATT_MIN_V) return 0;
    return (int)((v - BATT_MIN_V) / (BATT_MAX_V - BATT_MIN_V) * 100.0f);
}

float readDCVoltage()
{
    long sum = 0;
    for (int i = 0; i < 50; i++) {
        sum += analogRead(VOLT_SENSOR_PIN);
        delay(1);
    }
    float avgRaw   = sum / 50.0f;
    float adcVolt  = (avgRaw / ADC_RES) * VREF;
    return adcVolt * VOLTAGE_DIVIDER;
}

//====================================================
void lcdSplash()
{
    lcd.clear();
    lcd.setCursor(0, 0); lcd.print("   MicroCell v1.0   ");
    lcd.setCursor(0, 1); lcd.print("  Initializing...   ");
}

void lcdUpdate()
{
    lcd.setCursor(0, 0); lcd.printf("Batt : %3d%%  %5.2fV  ", g_battery, g_voltage);
    lcd.setCursor(0, 1); lcd.printf("Curr : %7.2f mA      ", g_current);
    lcd.setCursor(0, 2); lcd.printf("Pwr  : %7.2f mW      ", g_power);
    lcd.setCursor(0, 3); lcd.printf("AC   : %6.2f V       ", g_acVoltage);
}

//====================================================
void printConnectionIndicator()
{
    if (millis() - lastStatusPrintMillis < STATUS_PRINT_INTERVAL_MS) return;
    lastStatusPrintMillis = millis();

    Serial.printf("[STATUS] WiFi: %s | Firebase: %s | INA219: %s\n",
        WiFi.status() == WL_CONNECTED ? "OK" : "DISCONNECTED",
        Firebase.ready()              ? "OK" : "DISCONNECTED",
        ina219Ready                   ? "OK" : "NOT FOUND");
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

    Serial.println(WiFi.status() == WL_CONNECTED
        ? "\nWiFi Connected | IP: " + WiFi.localIP().toString()
        : "\nWiFi Connection Failed");
}

//====================================================
void initRelayPaths()
{
    for (int i = 0; i < RELAY_COUNT; i++) {
        if (!Firebase.RTDB.getBool(&fbdo, RELAYS[i].relayPath)) {
            Firebase.RTDB.setBool(&fbdo, RELAYS[i].relayPath, false);
            Firebase.RTDB.setBool(&fbdo, RELAYS[i].statusPath, false);
            Serial.printf("Init path: %s\n", RELAYS[i].relayPath);
        }
    }
}

//====================================================
void handleRelay(int index)
{
    const RelayConfig &r = RELAYS[index];

    if (!Firebase.RTDB.getBool(&fbdo, r.relayPath)) {
        Serial.printf("Relay Error (%s): %s\n", r.relayPath, fbdo.errorReason().c_str());
        return;
    }

    bool newState = fbdo.boolData();
    if (newState != relayStates[index]) {
        Serial.printf("%s -> %s\n", r.relayPath, newState ? "ON" : "OFF");
        relayStates[index] = newState;
    }

    if (r.hasPhysicalPin)
        digitalWrite(r.pin, newState ? LOW : HIGH);

    Firebase.RTDB.setBool(&fbdo, r.statusPath, newState);
}

//====================================================
void handleSensor()
{
    // Baca INA219
    if (ina219Ready) {
        float busVoltage   = ina219.getBusVoltage_V();
        float shuntVoltage = ina219.getShuntVoltage_mV();
        g_current  = ina219.getCurrent_mA();
        g_power    = ina219.getPower_mW();
        g_voltage  = busVoltage + (shuntVoltage / 1000.0f);
        g_battery  = calcBatteryLevel(g_voltage);
    }

    // Baca sensor tegangan DC
    g_acVoltage = readDCVoltage();

    // Update LCD tiap loop
    lcdUpdate();

    // Upload Firebase tiap 5 detik
    if (millis() - lastSensorUploadMillis < SENSOR_UPLOAD_INTERVAL_MS) return;
    lastSensorUploadMillis = millis();

    Serial.println("==========================");
    Serial.printf("Voltage     : %.2f V\n",  g_voltage);
    Serial.printf("Current     : %.2f mA\n", g_current);
    Serial.printf("Power       : %.2f mW\n", g_power);
    Serial.printf("Battery     : %d %%\n",   g_battery);
    Serial.printf("DC Voltage  : %.2f V\n",  g_acVoltage);

    Firebase.RTDB.setFloat(&fbdoSensor, PATH_VOLTAGE,  g_voltage);
    Firebase.RTDB.setFloat(&fbdoSensor, PATH_CURRENT,  g_current);
    Firebase.RTDB.setInt  (&fbdoSensor, PATH_BATTERY,  g_battery);
    Firebase.RTDB.setFloat(&fbdoSensor, PATH_ACVOLT,   g_acVoltage);
}

//====================================================
void setup()
{
    Serial.begin(115200);
    analogReadResolution(12);

    for (int i = 0; i < RELAY_COUNT; i++) {
        if (RELAYS[i].hasPhysicalPin) {
            pinMode(RELAYS[i].pin, OUTPUT);
            digitalWrite(RELAYS[i].pin, HIGH);
        }
    }

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

    lcd.init();
    lcd.backlight();
    lcdSplash();

    if (ina219.begin(&Wire)) {
        ina219Ready = true;
        ina219.setCalibration_32V_2A();
        Serial.println("INA219 Ready");
        lcd.setCursor(0, 2); lcd.print("  INA219 OK         ");
    } else {
        Serial.println("INA219 Not Found");
        lcd.setCursor(0, 2); lcd.print("  INA219 ERROR!     ");
    }

    delay(1500);
    connectWiFi();

    config.api_key               = API_KEY;
    config.database_url          = DATABASE_URL;
    auth.user.email              = USER_EMAIL;
    auth.user.password           = USER_PASSWORD;
    config.token_status_callback = tokenStatusCallback;

    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    Serial.println("Waiting Firebase Login...");
    unsigned long start = millis();
    while (!Firebase.ready() && millis() - start < FIREBASE_TIMEOUT_MS) {
        Serial.print(".");
        delay(500);
    }

    if (Firebase.ready()) {
        Serial.println("\nFirebase Connected");
        lcd.setCursor(0, 3); lcd.print("  Firebase OK       ");
        delay(1000);
        lcd.clear();
        initRelayPaths();
    } else {
        Serial.println("\nFirebase Login Failed");
        lcd.setCursor(0, 3); lcd.print("  Firebase FAIL!    ");
        delay(1000);
        lcd.clear();
    }
}

//====================================================
void loop()
{
    printConnectionIndicator();

    if (WiFi.status() != WL_CONNECTED) { connectWiFi(); return; }
    if (!Firebase.ready()) return;

    if (millis() - lastRelayCheckMillis >= RELAY_CHECK_INTERVAL_MS) {
        lastRelayCheckMillis = millis();
        for (int i = 0; i < RELAY_COUNT; i++)
            handleRelay(i);
    }

    handleSensor();
}