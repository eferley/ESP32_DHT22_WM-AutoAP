// ESP32 + DHT Temp.Humid. sensor + Async Web server + NTP client
// + WiFi Autoconfig mode (AP) with LED status feedback
// + Config reset push button
// The status LED will blink when booting, fast-blink when in WiFi Autoconfig (AP) mode
// (if Wifi can't connect),
// then stay on until the first measurement and will subsequently
// only give one short flash on every measurement

// ============================== INCLUDES ==============================
#include <Arduino.h>

// WiFiManager for WiFi Autoconfig mode (AP)
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager (development branch Jan.2020)
#include <Ticker.h>      //(from framework-arduinoespressif32) for WiFi Autoconfig mode (AP) LED status

// AsyncWebServer
#include <FS.h> // Required for AsyncWebServer
#include <ESPAsyncWebServer.h>

// NTP Client
#include <WiFiUdp.h>
#include <NTPClient.h>

// DHT Temperature & humidity sensor
#include "DHT.h"

// ============================== GLOBAL SYMBOLS ==============================
#define LED_ON HIGH
#define LED_OFF LOW

#define STATUS_LED_PIN LED_BUILTIN // for WiFi Autoconfig mode (AP) LED status
#define RESET_CONFIG_PIN 32        // to trigger WiFi Autoconfig mode (AP) / reset WiFi settings

#define DHT_PIN 27            // pin for DHT data
#define DHT_TYPE DHT22        // DHT11 / DHT21 / DHT22
#define DHT_MEASURETIME 30000 // measure every 15s

// ============================== GLOBAL VARS/CONSTS ==============================

// Include the main Web page definition
#include "index_html.h"

// Ticker object for WiFi Autoconfig mode (AP) LED status
Ticker ledTicker;

// Temp./Humidity sensor object
DHT dhtSensor(DHT_PIN, DHT_TYPE);

// Sensor measurements
float fTmp;          // Temperature (Celcius)
float fHum;          // Humidity (percent)
float fHtIdx;        // Heat Index (Celcius)
float fSndSpd;       // Sound Speed (m/s)
String sMeasureTime; // measurement time (HH:MM:SS)

// AsyncWebServer object on port 80
AsyncWebServer oWebServer(80);

// Create UDP client for NTP
WiFiUDP ntpUDP;

// NTP Client :
// You can specify the time server pool and the offset (in seconds, can be
// changed later with setTimeOffset() ). Additionaly you can specify the
// update interval (in milliseconds, can be changed using setUpdateInterval() ).
NTPClient clientNTP(ntpUDP, "europe.pool.ntp.org", 3600, 60000);

// Measurement timing
unsigned long ulTime;              // current time (milliseconds)
unsigned long ulMeasureTime = 0UL; // last measurement time (milliseconds)

// Server running normally ?
bool bRunServer;

// ============================== FUNCTION PROTOTYPES ==============================

void configModeCallback(WiFiManager *myWiFiManager);
void tickLED();
String processOutput(const String &var);
String outputTemperature();
String outputHumidity();
String outputMeasureTime();
String outputCurrentTime();

// ============================== ARDUINO SETUP+LOOP ==============================

// Arduino setup
void setup()
{
  // WiFi.mode(WIFI_MODE_APSTA);
  // WiFi.softAP(pWifiSsid_AP, pWifiPassword_AP);
  WiFi.mode(WIFI_STA); // explicitly set mode, esp defaults to STA+AP
  Serial.begin(115200);
  dhtSensor.begin();
  pinMode(RESET_CONFIG_PIN, INPUT_PULLUP); //set push-button pin as input
  pinMode(STATUS_LED_PIN, OUTPUT);         //set led pin as output
  ledTicker.attach(0.6, tickLED);          // start ledTicker with 0.6 because we start in AP mode and try to connect

  // WiFiManager, Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wm;

  // !! FOR TESTING !! Reset settings = wipe previous WiFi credentials from the ESP32
  // wm.resetSettings();

  // set dark theme for AP web server
  wm.setClass("invert");

  // Set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
  wm.setAPCallback(configModeCallback);

  // Set static ip
  // wm.setSTAStaticIPConfig(IPAddress(10,0,1,99), IPAddress(10,0,1,1), IPAddress(255,255,255,0)); // set static ip,gw,sn
  // wm.setShowStaticFields(true); // force show static ip fields
  // wm.setShowDnsFields(true);    // force show dns field always

  // wm.setConnectTimeout(20); // how long to try to connect for before continuing
  wm.setConfigPortalTimeout(120); // auto close configportal after n seconds
  // wm.setCaptivePortalEnable(false); // disable captive portal redirection
  wm.setAPClientCheck(true); // avoid timeout if client connected to softap

  // Wifi scan settings
  // wm.setRemoveDuplicateAPs(false); // do not remove duplicate ap names (true)
  // wm.setMinimumSignalQuality(20);  // set min RSSI (percentage) to show in scans, null = 8%
  // wm.setShowInfoErase(false);      // do not show erase button on info page
  // wm.setScanDispPerc(true);       // show RSSI as percentage not graph icons

  // wm.setBreakAfterConfig(true);   // always exit configportal even if wifi save fails

  // Automatically connect using saved credentials,
  // if connection fails, it starts an access point with the specified name ( "AutoConnectAP"),
  // if empty will auto generate SSID, if password is blank it will be anonymous AP (wm.autoConnect())
  // then goes into a blocking loop awaiting configuration and will return success result

  // Using anonymous mode because my W7 PC could not connect with a pwd
  bRunServer = wm.autoConnect(); // auto generated AP name from chipid
  // bRunServer = wm.autoConnect("AutoConnectAP");                 // anonymous ap
  // bRunServer = wm.autoConnect("AutoConnectAP","Passw0rd!");     // password protected ap

  if (!bRunServer)
  {
    Serial.println("Failed to connect");
    // ESP.restart();
  }
  else
  {
    //if you get here you have connected to the WiFi
    ledTicker.detach();
    Serial.print("Connected to WiFi : IP=");
    Serial.println(WiFi.localIP());
    digitalWrite(STATUS_LED_PIN, HIGH); //keep LED on until end of setup()

    // Initialize NTP client
    clientNTP.begin();
    clientNTP.update();

    // Routes for root / web page and measurement output
    oWebServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/html", index_html, processOutput);
    });
    oWebServer.on("/temperature", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/plain", outputTemperature().c_str());
    });
    oWebServer.on("/humidity", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/plain", outputHumidity().c_str());
    });
    oWebServer.on("/measuretime", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/plain", outputMeasureTime().c_str());
    });
    oWebServer.on("/refreshtime", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send_P(200, "text/plain", outputCurrentTime().c_str());
    });

    // Start server
    oWebServer.begin();

    Serial.println();
    Serial.print("Ready ! Time : ");
    Serial.println(clientNTP.getFormattedTime());
    // Serial.print("Soft-AP MAC  : ");  Serial.println(WiFi.softAPmacAddress());
    // Serial.print("Soft-AP IP   : ");  Serial.println(WiFi.softAPIP());
    Serial.print("Station IP   : ");
    Serial.println(WiFi.localIP());
    Serial.println();
    digitalWrite(STATUS_LED_PIN, LOW); //turn LED off
  }//bRunServer is true

} // void setup()
// ----------------------------------------------------------------------

// Arduino main loop
void loop()
{
  ulTime = millis();
  // Take a measurement every DHT_MEASURETIME milliseconds
  if (bRunServer && (ulTime - ulMeasureTime > DHT_MEASURETIME))
  {
    // Serial.println();
    digitalWrite(LED_BUILTIN, LED_ON);  //LED on during measurement update
    clientNTP.update();
    sMeasureTime = clientNTP.getFormattedTime();

    // Get readings from sensor
    fTmp = dhtSensor.readTemperature(false);
    fHum = dhtSensor.readHumidity();
    // Get Heat Index
    fHtIdx = dhtSensor.computeHeatIndex(fTmp, fHum, false);
    // Calculate the Speed of Sound in m/s
    fSndSpd = 331.4 + (0.606 * fTmp) + (0.0124 * fHum);

    ulMeasureTime = ulTime;

    Serial.print(sMeasureTime);
    Serial.print(" - ");
    Serial.print("Temp.  : ");
    Serial.print(fTmp, 1);
    Serial.print(" C");
    Serial.print(" - Humid. : ");
    Serial.print(fHum, 1);
    Serial.print(" %");
    Serial.print(" - Heat Idx. : ");
    Serial.print(fHtIdx, 1);
    Serial.print(" C");
    Serial.print(" - Snd.Sp.: ");
    Serial.print(fSndSpd, 1);
    Serial.print(" m/s ");
    Serial.println();

    digitalWrite(LED_BUILTIN, LED_OFF);  //LED off after measurement update
  } // if (bRunServer && (ulTime - ulMeasureTime > DHT_MEASURETIME))

  delayMicroseconds(100);
  // Serial.print(".");
} // void loop()
// ----------------------------------------------------------------------

// ============================== CALLBACK FUNCTIONS ==============================

// gets called when WiFiManager enters configuration mode
void configModeCallback(WiFiManager *myWiFiManager)
{
  Serial.println("Entered config mode");
  Serial.print("Soft-AP IP   : ");
  Serial.println(WiFi.softAPIP());
  Serial.print("Soft-AP SSID : ");
  Serial.println(myWiFiManager->getConfigPortalSSID()); //if you used auto generated SSID, print it
  ledTicker.attach(0.2, tickLED);                       //entered config mode, make led toggle faster
} // void configModeCallback (WiFiManager *myWiFiManager)
// ----------------------------------------------------------------------

// ============================== UTILITY FUNCTIONS ==============================

// blink STATUS_LED_PIN
void tickLED()
{
  //toggle STATUS_LED_PIN state
  digitalWrite(STATUS_LED_PIN, !digitalRead(STATUS_LED_PIN)); // set pin to the opposite state
} // void tickLED()
// ----------------------------------------------------------------------

// Replaces placeholder with DHT values
String processOutput(const String &var)
{
  //Serial.println(var);
  if (var == "TEMPERATURE")
  {
    return outputTemperature();
  }
  else if (var == "HUMIDITY")
  {
    return outputHumidity();
  }
  else if (var == "MEASURETIME")
  {
    return outputMeasureTime();
  }
  else if (var == "REFRESHTIME")
  {
    return outputCurrentTime();
  }
  return String();
} // String processOutput(const String& var)
//-------------------------------------

String outputTemperature()
{
  // Check if any reads failed and exit early (to try again).
  if (isnan(fTmp))
  {
    Serial.println("Failed to get Temperature from DHT sensor!");
    return "N/A";
  }
  else
  {
    // Serial.println(fTmp, 1);
    return String(fTmp);
  }
} // String outputTemperature()
//-------------------------------------

String outputHumidity()
{
  // Sensor readings may also be up to 2 seconds 'old' (its a very slow sensor)
  if (isnan(fHum))
  {
    Serial.println("Failed to get Humidity from DHT sensor!");
    return "N/A";
  }
  else
  {
    // Serial.println(fHum, 1);
    return String(fHum);
  }
} // String outputHumidity()
//-------------------------------------

String outputMeasureTime()
{
  return sMeasureTime;
} // String outputMeasureTime()
//-------------------------------------

String outputCurrentTime()
{
  return clientNTP.getFormattedTime();
} // String outputCurrentTime()
//-------------------------------------
