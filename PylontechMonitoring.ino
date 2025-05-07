/********************************************************************************
 * Pylontech Battery Monitoring - Main Code
 * 
 * This code uses an ESP8266 to communicate with a Pylontech battery system over 
 * serial, parse battery data (voltage, current, SOC, etc.), and optionally publish 
 * it via MQTT for integration with Home Assistant or other platforms. 
 *
 ********************************************************************************/

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <circular_log.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <ESP8266TimerInterrupt.h>
#include "PylontechMonitoring.h"  // Configuration parameters (WiFi, MQTT, etc.)
#include "batteryStack.h"         // Contains pylonBattery and batteryStack structs

#ifdef ENABLE_MQTT
#include <PubSubClient.h>
WiFiClient espClient;
PubSubClient mqttClient(espClient);
#endif //ENABLE_MQTT

// A buffer to store text responses from the battery
char g_szRecvBuff[7000];

// Time-related globals
const long utcOffsetInSeconds = GMT;
char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

// Web server and logging
ESP8266WebServer server(80);
circular_log<7000> g_log;
bool ntpTimeReceived = false;
int g_baudRate = 0;

/**
 * Helper function to log messages to our circular_log buffer.
 */
void Log(const char* msg)
{
  g_log.Log(msg);
}

// Interrupt timer setup (not heavily used in this example)
#define USING_TIM_DIV1 true
ESP8266Timer ITimer;
bool setInterval(unsigned long interval, timer_callback callback);
#define TIMER_INTERVAL_MS 1000

// Global variables for additional power metering (if used)
unsigned long powerIN = 0;       
unsigned long powerOUT = 0;     
unsigned long powerINWh = 0;    
unsigned long powerOUTWh = 0;   

/**
 * Standard Arduino setup function: connect to Wi-Fi, start OTA, etc.
 */
void setup() {
  
  
  // Clear the receive buffer
  memset(g_szRecvBuff, 0, sizeof(g_szRecvBuff)); 

  pinMode(LED_BUILTIN, OUTPUT); 
  digitalWrite(LED_BUILTIN, HIGH); // HIGH = LED off

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.hostname(WIFI_HOSTNAME);

#ifdef STATIC_IP
  WiFi.config(ip, dns, gateway, subnet);
#endif

  // Start connecting to Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Try connecting up to 10 seconds
  for(int ix=0; ix<10; ix++)
  {
    Log("Wait for WIFI Connection");
    if(WiFi.status() == WL_CONNECTED)
    {
      break;
    }
    delay(1000);
  }

  // Over-the-Air updates
  ArduinoOTA.setHostname(WIFI_HOSTNAME);
  ArduinoOTA.begin();

  // Set up web server routes
  server.on("/", handleRoot);
  server.on("/log", handleLog);
  server.on("/req", handleReq);
  server.on("/jsonOut", handleJsonOut);
  server.on("/reboot", [](){
#ifdef AUTHENTICATION
    if (!server.authenticate(www_username, www_password)) {
      return server.requestAuthentication();
    }
#endif
    ESP.restart();
  });

  server.begin(); 
  timeClient.begin();

#ifdef ENABLE_MQTT
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setBufferSize(1024); //increased buffer for autodiscovery
#endif

  Log("Boot event");
}

/**
 * Handle the /log URL to print out the log buffer in plain text.
 */
void handleLog()
{
#ifdef AUTHENTICATION
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
#endif
  server.send(200, "text/html", g_log.c_str());
}

/**
 * Switch the UART baud rate for communicating with the battery console.
 */
void switchBaud(int newRate)
{
  if(g_baudRate == newRate)
  {
    return;
  }
  
  if(g_baudRate != 0)
  {
    Serial.flush();
    delay(20);
    Serial.end();
    delay(20);
  }

  char szMsg[50];
  snprintf(szMsg, sizeof(szMsg)-1, "New baud: %d", newRate);
  Log(szMsg);

  Serial.begin(newRate);
  g_baudRate = newRate;
  delay(20);
}

/**
 * Wait for data on Serial for a short time.
 */
void waitForSerial()
{
  for(int ix=0; ix<150;ix++)
  {
    if(Serial.available()) break;
    delay(10);
  }
}

/**
 * Read from the battery console until '>' is found or buffer is full.
 */
int readFromSerial()
{
  memset(g_szRecvBuff, 0, sizeof(g_szRecvBuff));
  int recvBuffLen = 0;
  bool foundTerminator = true;

  waitForSerial();

  while(Serial.available())
  {
    char szResponse[256] = "";
    const int readNow = Serial.readBytesUntil('>', szResponse, sizeof(szResponse)-1);
    if(readNow > 0 && 
       szResponse[0] != '\0')
    {
      if(readNow + recvBuffLen + 1 >= (int)(sizeof(g_szRecvBuff)))
      {
        Log("WARNING: Read too much data on the console!");
        break;
      }

      strcat(g_szRecvBuff, szResponse);
      recvBuffLen += readNow;

      if(strstr(g_szRecvBuff, "$$\r\n\rpylon"))
      {
        strcat(g_szRecvBuff, ">");
        foundTerminator = true;
        break;
      }

      if(strstr(g_szRecvBuff, "Press [Enter] to be continued,other key to exit"))
      {
        // Send newline so battery continues the output
        Serial.write("\r");
      }

      waitForSerial();
    }
  }

  if(recvBuffLen > 0 )
  {
    if(foundTerminator == false)
    {
      Log("Failed to find pylon> terminator");
    }
  }

  return recvBuffLen;
}

/**
 * Convenience function: read from Serial and send response to web client.
 */
bool readFromSerialAndSendResponse()
{
  const int recvBuffLen = readFromSerial();
  if(recvBuffLen > 0)
  {
    server.sendContent(g_szRecvBuff);
    return true;
  }
  return false;
}

/**
 * Send a command to the battery console and read the serial response.
 */
bool sendCommandAndReadSerialResponse(const char* pszCommand)
{
  switchBaud(115200);

  if(pszCommand[0] != '\0')
  {
    Serial.write(pszCommand);
  }
  Serial.write("\n");

  const int recvBuffLen = readFromSerial();
  if(recvBuffLen > 0)
  {
    return true;
  }

  // If nothing, try waking up the console
  wakeUpConsole();

  if(pszCommand[0] != '\0')
  {
    Serial.write(pszCommand);
  }
  Serial.write("\n");

  return readFromSerial() > 0;
}

/**
 * Handle requests from the web UI (/req?code=xxx).
 */
void handleReq()
{
#ifdef AUTHENTICATION
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
#endif

  bool respOK;
  if(server.hasArg("code") == false)
  {
    respOK = sendCommandAndReadSerialResponse("");
  }
  else
  {
    respOK = sendCommandAndReadSerialResponse(server.arg("code").c_str());
  }

  handleRoot();
}

/**
 * Return the battery data as JSON (/jsonOut).
 */
void handleJsonOut()
{
#ifdef AUTHENTICATION
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
#endif

  if(sendCommandAndReadSerialResponse("pwr") == false)
  {
    server.send(500, "text/plain", "Failed to get response to 'pwr' command");
    return;
  }

  parsePwrResponse(g_szRecvBuff);
  prepareJsonOutput(g_szRecvBuff, sizeof(g_szRecvBuff));
  server.send(200, "application/json", g_szRecvBuff);
}

/**
 * Main web page showing logs, battery info, etc.
 */
void handleRoot() {
#ifdef AUTHENTICATION
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
#endif

  // Update time from NTP
  timeClient.update(); 
  unsigned long days = 0, hours = 0, minutes = 0;
  unsigned long val = os_getCurrentTimeSec();
  days = val / (3600*24);
  val -= days * (3600*24);
  hours = val / 3600;
  val -= hours * 3600;
  minutes = val / 60;
  val -= minutes*60;

  time_t epochTime = timeClient.getEpochTime();
  String formattedTime = timeClient.getFormattedTime();
  struct tm *ptm = gmtime ((time_t *)&epochTime); 
  int currentMonth = ptm->tm_mon+1;

  static char szTmp[9500] = "";  
  long timezone= GMT / 3600;

  // Build a small HTML page
  snprintf(szTmp, sizeof(szTmp)-1, 
    "<html><b>Pylontech Battery</b><br>Time GMT: %s (%s %d)<br>Uptime: %02d:%02d:%02d.%02d<br><br>free heap: %u<br>Wifi RSSI: %d<BR>Wifi SSID: %s", 
    formattedTime.c_str(), "GMT ", timezone,
    (int)days, (int)hours, (int)minutes, (int)val, 
    ESP.getFreeHeap(), WiFi.RSSI(), WiFi.SSID().c_str());

  strncat(szTmp, "<BR><a href='/log'>Runtime log</a><HR>", sizeof(szTmp)-1);
  strncat(szTmp, "<form action='/req' method='get'>Command:<input type='text' name='code'/><input type='submit'> <a href='/req?code=pwr'>PWR</a> | <a href='/req?code=pwr%201'>Power 1</a> |  <a href='/req?code=pwr%202'>Power 2</a> | <a href='/req?code=pwr%203'>Power 3</a> | <a href='/req?code=pwr%204'>Power 4</a> | <a href='/req?code=help'>Help</a> | <a href='/req?code=log'>Event Log</a> | <a href='/req?code=time'>Time</a><br>", sizeof(szTmp)-1);
  strncat(szTmp, "<textarea rows='80' cols='180'>", sizeof(szTmp)-1);
  strncat(szTmp, g_szRecvBuff, sizeof(szTmp)-1);
  strncat(szTmp, "</textarea></form>", sizeof(szTmp)-1);
  strncat(szTmp, "</html>", sizeof(szTmp)-1);

  server.send(200, "text/html", szTmp);
}

/**
 * Get the current time in seconds since boot (with wrap handling).
 */
unsigned long os_getCurrentTimeSec()
{
  static unsigned int wrapCnt = 0;
  static unsigned long lastVal = 0;
  unsigned long currentVal = millis();

  if(currentVal < lastVal)
  {
    wrapCnt++;
  }
  lastVal = currentVal;
  unsigned long seconds = currentVal/1000;
  return (wrapCnt*4294967) + seconds;
}

/**
 * Send a special sequence to "wake up" the battery console if unresponsive.
 */
void wakeUpConsole()
{
  switchBaud(1200);
  Serial.write("~20014682C0048520FCC3\r");
  delay(1000);

  byte newLineBuff[] = {0x0E, 0x0A};
  switchBaud(115200);
  
  for(int ix=0; ix<10; ix++)
  {
    Serial.write(newLineBuff, sizeof(newLineBuff));
    delay(1000);

    if(Serial.available())
    {
      while(Serial.available())
      {
        Serial.read();
      }
      break;
    }
  }
}


// Global instance of the battery stack data
batteryStack g_stack;

/**
 * Helper functions to extract integers/strings from battery console output.
 */
long extractInt(const char* pStr, int pos)
{
  return atol(pStr+pos);
}

void extractStr(const char* pStr, int pos, char* strOut, int strOutSize)
{
  strOut[strOutSize-1] = '\0';
  strncpy(strOut, pStr+pos, strOutSize-1);
  strOutSize--;

  while(strOutSize > 0)
  {
    if(isspace(strOut[strOutSize-1]))
    {
      strOut[strOutSize-1] = '\0';
    }
    else
    {
      break;
    }
    strOutSize--;
  }
}


bool parsePwrResponse(const char* pStrOutput)
{
 if  (FW_VERSION == 1 )
  {
    return parse1(pStrOutput);
  }
  else
  {
    return parse1(pStrOutput);
  } 
}

/**
 * parsePwrResponse
 * 
 * Reads the battery console output from "pwr" command and populates g_stack.
 * - currentDC is in mA (milliamps).
 * - avgVoltage is in mV (millivolts).
 * 
 * Further down, these will be used to calculate approximate power in W or kW.
 */
bool parse1(const char* pStr)
{
  if(strstr(pStr, "Command completed successfully") == NULL)
  {
    return false;
  }

  int chargeCnt    = 0;
  int dischargeCnt = 0;
  int idleCnt      = 0;
  int alarmCnt     = 0;
  int socAvg       = 0;
  int socLow       = 0;
  int tempHigh     = 0;
  int tempLow      = 0;

  // Reset the entire g_stack each time we parse
  memset(&g_stack, 0, sizeof(g_stack));

  for(int ix=0; ix<MAX_PYLON_BATTERIES; ix++)
  {
    char szToFind[32];
    snprintf(szToFind, sizeof(szToFind), "\r\r\n%d     ", ix+1);

    const char* pLineStart = strstr(pStr, szToFind);
    if(pLineStart == NULL)
    {
      return false;
    }

    pLineStart += 3; // skip \r\r\n

    extractStr(pLineStart, 55, g_stack.batts[ix].baseState,    sizeof(g_stack.batts[ix].baseState));
    if(strcmp(g_stack.batts[ix].baseState, "Absent") == 0)
    {
      g_stack.batts[ix].isPresent = false;
    }
    else
    {
      g_stack.batts[ix].isPresent = true;
      extractStr(pLineStart, 64, g_stack.batts[ix].voltageState, sizeof(g_stack.batts[ix].voltageState));
      extractStr(pLineStart, 73, g_stack.batts[ix].currentState, sizeof(g_stack.batts[ix].currentState));
      extractStr(pLineStart, 82, g_stack.batts[ix].tempState,    sizeof(g_stack.batts[ix].tempState));
      extractStr(pLineStart, 100, g_stack.batts[ix].time,        sizeof(g_stack.batts[ix].time));
      extractStr(pLineStart, 121, g_stack.batts[ix].b_v_st,      sizeof(g_stack.batts[ix].b_v_st));
      extractStr(pLineStart, 130, g_stack.batts[ix].b_t_st,      sizeof(g_stack.batts[ix].b_t_st));

      // Each battery's voltage is in mW, current in mA, temperature in milli-deg C.
      g_stack.batts[ix].voltage = extractInt(pLineStart, 6);
      g_stack.batts[ix].current = extractInt(pLineStart, 13);
      g_stack.batts[ix].tempr   = extractInt(pLineStart, 20);
      g_stack.batts[ix].cellTempLow    = extractInt(pLineStart, 27);
      g_stack.batts[ix].cellTempHigh   = extractInt(pLineStart, 34);
      g_stack.batts[ix].cellVoltLow    = extractInt(pLineStart, 41);
      g_stack.batts[ix].cellVoltHigh   = extractInt(pLineStart, 48);
      g_stack.batts[ix].soc            = extractInt(pLineStart, 91);

      // Summation for entire stack
      g_stack.batteryCount++;
      g_stack.currentDC += g_stack.batts[ix].current;   // total mA
      g_stack.avgVoltage += g_stack.batts[ix].voltage;  // total mV
      socAvg += g_stack.batts[ix].soc;

      // Determine overall state (alarm, charge, discharge, etc.)
      if(!g_stack.batts[ix].isNormal()){ alarmCnt++; }
      else if(g_stack.batts[ix].isCharging())   { chargeCnt++; }
      else if(g_stack.batts[ix].isDischarging()){ dischargeCnt++; }
      else if(g_stack.batts[ix].isIdle())       { idleCnt++; }
      else { alarmCnt++; }

      // For the first battery, store initial lowest/highest values
      if(g_stack.batteryCount == 1)
      {
        socLow = g_stack.batts[ix].soc;
        tempLow  = g_stack.batts[ix].cellTempLow;
        tempHigh = g_stack.batts[ix].cellTempHigh;
      }
      else
      {
        if(socLow > g_stack.batts[ix].soc)             socLow = g_stack.batts[ix].soc;
        if(tempHigh < g_stack.batts[ix].cellTempHigh)  tempHigh = g_stack.batts[ix].cellTempHigh;
        if(tempLow > g_stack.batts[ix].cellTempLow)    tempLow = g_stack.batts[ix].cellTempLow;
      }
    }
  }

  // Compute average voltage by dividing the total by batteryCount (still in mV)
  g_stack.avgVoltage /= g_stack.batteryCount;

  // Overall stack SoC, if all are charging then it uses average, else lowest
  g_stack.soc = socLow;

  // Decide how to pick the main temperature
  if(tempHigh > 15000) g_stack.temp = tempHigh; // focusing on the warmest cell
  else                 g_stack.temp = tempLow;  // or the coldest

  // Decide baseState for the entire stack
  if(alarmCnt > 0)
  {
    strcpy(g_stack.baseState, "Alarm!");
  }
  else if(chargeCnt == g_stack.batteryCount)
  {
    strcpy(g_stack.baseState, "Charge");
    g_stack.soc = (int)(socAvg / g_stack.batteryCount);
  }
  else if(dischargeCnt == g_stack.batteryCount)
  {
    strcpy(g_stack.baseState, "Dischg");
  }
  else if(idleCnt == g_stack.batteryCount)
  {
    strcpy(g_stack.baseState, "Idle");
  }
  else
  {
    strcpy(g_stack.baseState, "Balance");
  }

  return true;
}



bool parse2(const char* pStr)
{
 return true;
}  

/**
 * prepareJsonOutput
 * 
 * Creates a JSON string describing the main battery data:
 * - soc (percentage)
 * - temp (mC)
 * - currentDC (mA)
 * - avgVoltage (mV)
 * - baseState (string)
 * - batteryCount (int)
 * - powerDC (W) -> from getPowerDC() 
 * - estPowerAC (W) -> from getEstPowerAc()
 * - isNormal (bool)
 *
 * The resulting JSON is put into pBuff with max length buffSize.
 */
void prepareJsonOutput(char* pBuff, int buffSize)
{
  memset(pBuff, 0, buffSize);
  snprintf(pBuff, buffSize-1, 
    "{\"soc\": %d, \"temp\": %d, \"currentDC\": %ld, \"avgVoltage\": %ld, \"baseState\": \"%s\", \"batteryCount\": %d, \"powerDC\": %ld, \"estPowerAC\": %ld, \"isNormal\": %s}",
    g_stack.soc, 
    g_stack.temp, 
    g_stack.currentDC, 
    g_stack.avgVoltage, 
    g_stack.baseState, 
    g_stack.batteryCount, 
    g_stack.getPowerDC(), 
    g_stack.getEstPowerAc(),
    g_stack.isNormal() ? "true" : "false");
}

/**
 * Main loop: 
 * - Runs mqttLoop() if enabled
 * - Handles OTA updates
 * - Processes incoming web requests
 * - Reads any unexpected data on Serial
 */
void loop() {
#ifdef ENABLE_MQTT
  mqttLoop();
#endif
  ArduinoOTA.handle();
  server.handleClient();

  int bytesAv = Serial.available();
  if(bytesAv > 0)
  {
    if(bytesAv > 63)
    {
      bytesAv = 63;
    }

    char buff[64+4] = "RCV:";
    if(Serial.readBytes(buff+4, bytesAv) > 0)
    {
      digitalWrite(LED_BUILTIN, LOW);
      delay(5);
      digitalWrite(LED_BUILTIN, HIGH); //HIGH = off
      Log(buff);
    }
  }
}

#ifdef ENABLE_MQTT
#define ABS_DIFF(a, b) (a > b ? a-b : b-a)

/**
 * publishSensorDiscovery
 * Publishes a Home Assistant autodiscovery config for a given sensor.
 */

 void publishSensorDiscovery(const char* sensorId,
                            const char* sensorName,
                            const char* unitOfMeasurement,
                            const char* deviceClass,
                            const char* stateTopic,
                            const char* valueTemplate,
                            bool forceRetain)
{
  StaticJsonDocument<512> doc;

  doc["name"]               = sensorName;
  doc["state_topic"]        = stateTopic;
  doc["unique_id"]          = String(WIFI_HOSTNAME) + "_" + sensorId;
  doc["availability_topic"] = String(MQTT_TOPIC_ROOT) + "availability";
  doc["payload_available"]  = "online";
  doc["payload_not_available"] = "offline";

  if(unitOfMeasurement && strlen(unitOfMeasurement) > 0) {
    doc["unit_of_measurement"] = unitOfMeasurement;
  }
  if(deviceClass && strlen(deviceClass) > 0) {
    doc["device_class"] = deviceClass;
  }
  if(valueTemplate && strlen(valueTemplate) > 0) {
    doc["value_template"] = valueTemplate;
  }

  // device info
  JsonObject dev = doc.createNestedObject("device");
  dev["identifiers"]   = String("esp8266-" + String(WIFI_HOSTNAME));
  dev["manufacturer"]  = "Pylontech";
  dev["model"]         = "ESP8266 Battery Monitor";
  dev["name"]          = String(WIFI_HOSTNAME);

  // Costruisci il topic di discovery
  //String configTopic = "homeassistant/sensor/";
  String configTopic = String(HA_DISCOVERY_SENSOR_PREFIX);
  configTopic += String(WIFI_HOSTNAME) + "_" + sensorId;
  configTopic += "/config";

  // Serializziamo il JSON in un buffer
  char buffer[512];
  size_t n = serializeJson(doc, buffer);
  // NOTA: se superi 512 byte di doc, potresti dover aumentare dimensione

  // --- LOG DEBUG ---
  // discovery topic
  {
    char debugBuff[512];
    snprintf(debugBuff, sizeof(debugBuff),
             "Discovery topic: %s", configTopic.c_str());
    Log(debugBuff);
  }

  // payload
  {
    char debugBuff[512];
    snprintf(debugBuff, sizeof(debugBuff),
             "Discovery payload (length %u): %s", 
             (unsigned)n, buffer);
    Log(debugBuff);
  }
  // --- LOG DEBUG ---

  // Publish discovery msg 
  bool ok = mqttClient.publish(
               configTopic.c_str(),
               (const uint8_t*)buffer, // cast necessario
               n,                      // lunghezza
               forceRetain             // retained
             );

  // log publish
  if(ok) {
    Log("publishSensorDiscovery: OK");
  } else {
    Log("publishSensorDiscovery: FAILED");
  }
}

/**
 * Publishes Home Assistant autodiscovery entries for the main battery sensors.
 */
void publishHomeAssistantDiscovery()
{
  publishSensorDiscovery("soc",
                        "Pylontech Battery SoC",
                        "%",
                        "battery",
                        String(MQTT_TOPIC_ROOT + String("soc")).c_str(),
                        nullptr,
                        true);

  publishSensorDiscovery("temp",
                        "Pylontech Temperature",
                        "°C",
                        "temperature",
                        String(MQTT_TOPIC_ROOT + String("temp")).c_str(),
                        nullptr,
                        true);

  publishSensorDiscovery("currentDC",
                        "Pylontech Battery Current",
                        "mA",
                        "current",
                        String(MQTT_TOPIC_ROOT + String("currentDC")).c_str(),
                        nullptr,
                        true);

  publishSensorDiscovery("powerDC",
                        "Pylontech DC Power",
                        "W", // The getPowerDC() function returns an approximate 
                             // power value in Watts.
                        "power",
                        String(MQTT_TOPIC_ROOT + String("getPowerDC")).c_str(),
                        nullptr,
                        true);

  publishSensorDiscovery("powerIN",
                        "Pylontech Power IN",
                        "W", // Also published in Watts
                        "power",
                        String(MQTT_TOPIC_ROOT + String("powerIN")).c_str(),
                        nullptr,
                        true);

  publishSensorDiscovery("powerOUT",
                        "Pylontech Power OUT",
                        "W", // Also in Watts
                        "power",
                        String(MQTT_TOPIC_ROOT + String("powerOUT")).c_str(),
                        nullptr,
                        true);


 for(int ix = 0; ix < MAX_PYLON_BATTERIES; ix++)
 {
   // CHARGING
   publishSensorDiscovery(
     (String(ix) + "_charging").c_str(),               // sensorId univoco
     ("Battery " + String(ix) + " Charging").c_str(),  // nome leggibile
     "",        // unità di misura (nessuna)
     "",        // device_class (vuoto, o "power", o "battery", ecc. se vuoi)
     (MQTT_TOPIC_ROOT + String(ix) + "/charging").c_str(), // state_topic
     nullptr,   // value_template (non serve)
     true       // forceRetain
   );

   // DISCHARGING
   publishSensorDiscovery(
     (String(ix) + "_discharging").c_str(),
     ("Battery " + String(ix) + " Discharging").c_str(),
     "",
     "",
     (MQTT_TOPIC_ROOT + String(ix) + "/discharging").c_str(),
     nullptr,
     true
   );

   // IDLE
   publishSensorDiscovery(
     (String(ix) + "_idle").c_str(),
     ("Battery " + String(ix) + " Idle").c_str(),
     "",
     "",
     (MQTT_TOPIC_ROOT + String(ix) + "/idle").c_str(),
     nullptr,
     true
   );

   // STATE
   publishSensorDiscovery(
     (String(ix) + "_state").c_str(),
     ("Battery " + String(ix) + " State").c_str(),
     "",         // no unit
     "",         // no device_class
     (MQTT_TOPIC_ROOT + String(ix) + "/state").c_str(),
     nullptr,    
     true
   );
 }


}

/**
 * mqtt_publish_f
 * Publishes a float value if it differs by more than minDiff or if forced.
 */
void mqtt_publish_f(const char* topic, float newValue, float oldValue, float minDiff, bool force)
{
  char szTmp[16] = "";
  snprintf(szTmp, 15, "%.2f", newValue);
  if(force || ABS_DIFF(newValue, oldValue) > minDiff)
  {
    mqttClient.publish(topic, szTmp, false);
  }
}

/**
 * mqtt_publish_i
 * Publishes an integer value if changed or forced.
 */
void mqtt_publish_i(const char* topic, int newValue, int oldValue, int minDiff, bool force)
{
  char szTmp[16] = "";
  snprintf(szTmp, 15, "%d", newValue);
  if(force || ABS_DIFF(newValue, oldValue) > minDiff)
  {
    mqttClient.publish(topic, szTmp, false);
  }
}

/**
 * mqtt_publish_s
 * Publishes a string if changed or forced.
 */
void mqtt_publish_s(const char* topic, const char* newValue, const char* oldValue, bool force)
{
  if(force || strcmp(newValue, oldValue) != 0)
  {
    mqttClient.publish(topic, newValue, false);
  }
}

/**
 * pushBatteryDataToMqtt
 * 
 * Sends the battery stack data to MQTT. 
 * Key calculations are in getPowerDC(), powerIN(), powerOUT(), all in units of Watts (W).
 * If you want kW, you'd have to divide by 1000.
 * For kWh, you'd integrate over time.
 */
void pushBatteryDataToMqtt(const batteryStack& lastSentData, bool forceUpdate)
{
  // SoC is a percentage (int or float).
  mqtt_publish_f(MQTT_TOPIC_ROOT "soc",          
                 g_stack.soc,                
                 lastSentData.soc,                
                 0, 
                 forceUpdate);

  // Temperature is stored in milli-deg Celsius, so dividing by 1000.0 for degrees Celsius.
  mqtt_publish_f(MQTT_TOPIC_ROOT "temp",         
                 (float)g_stack.temp/1000.0, 
                 (float)lastSentData.temp/1000.0, 
                 0.1, 
                 forceUpdate);

  // Current in mA
  mqtt_publish_i(MQTT_TOPIC_ROOT "currentDC",    
                 g_stack.currentDC,          
                 lastSentData.currentDC,          
                 1, 
                 forceUpdate);

  // Estimated AC power in W (approx) after inverter losses
  mqtt_publish_i(MQTT_TOPIC_ROOT "estPowerAC",   
                 g_stack.getEstPowerAc(),    
                 lastSentData.getEstPowerAc(),   
                 10, 
                 forceUpdate);

  // Number of present batteries
  mqtt_publish_i(MQTT_TOPIC_ROOT "battery_count",
                 g_stack.batteryCount,       
                 lastSentData.batteryCount,       
                 0, 
                 forceUpdate);

  // Base state (string): "Charge", "Dischg", etc.
  mqtt_publish_s(MQTT_TOPIC_ROOT "base_state",   
                 g_stack.baseState,          
                 lastSentData.baseState,          
                 forceUpdate);

  // 1 if everything is normal, 0 otherwise
  mqtt_publish_i(MQTT_TOPIC_ROOT "is_normal",    
                 g_stack.isNormal() ? 1 : 0,   
                 lastSentData.isNormal() ? 1 : 0,  
                 0, 
                 forceUpdate);

  // getPowerDC() returns approximate power in W (long).
  mqtt_publish_i(MQTT_TOPIC_ROOT "getPowerDC",   
                 g_stack.getPowerDC(),       
                 lastSentData.getPowerDC(),       
                 1, 
                 forceUpdate);

  // powerIN() returns W (float) if currentDC > 0
  mqtt_publish_f(MQTT_TOPIC_ROOT "powerIN",      
                 g_stack.powerIN(),          
                 lastSentData.powerIN(),          
                 1, 
                 forceUpdate);

  // powerOUT() returns W (float) if currentDC < 0
  mqtt_publish_f(MQTT_TOPIC_ROOT "powerOUT",     
                 g_stack.powerOUT(),         
                 lastSentData.powerOUT(),         
                 1, 
                 forceUpdate);



  // Per-battery details (e.g., voltage, current, SOC)
  for (int ix = 0; ix < g_stack.batteryCount; ix++) {
    char ixBuff[50];
    // Voltage in mW -> dividing by 1000.0 = V
    String ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/voltage";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_f(ixBuff, 
                   g_stack.batts[ix].voltage / 1000.0, 
                   lastSentData.batts[ix].voltage / 1000.0, 
                   0, 
                   forceUpdate);

    // Current in mA -> dividing by 1000.0 = A
    ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/current";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_f(ixBuff, 
                   g_stack.batts[ix].current / 1000.0, 
                   lastSentData.batts[ix].current / 1000.0, 
                   0, 
                   forceUpdate);

    // SoC is an integer percentage
    ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/soc";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_i(ixBuff, 
                   g_stack.batts[ix].soc, 
                   lastSentData.batts[ix].soc, 
                   0, 
                   forceUpdate);

    // Additional battery details can be published the same way...
  }

  for (int ix = 0; ix < g_stack.batteryCount; ix++) {
   char ixBuff[50];
   String ixBattStr;

   // 1) CHARGING -> 1 se sta caricando, 0 altrimenti
   ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/charging";
   ixBattStr.toCharArray(ixBuff, 50);
   mqtt_publish_i(
     ixBuff, 
     g_stack.batts[ix].isCharging() ? 1 : 0,
     lastSentData.batts[ix].isCharging() ? 1 : 0,
     0,     // minDiff
     forceUpdate
   );

   // 2) DISCHARGING
   ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/discharging";
   ixBattStr.toCharArray(ixBuff, 50);
   mqtt_publish_i(
     ixBuff, 
     g_stack.batts[ix].isDischarging() ? 1 : 0,
     lastSentData.batts[ix].isDischarging() ? 1 : 0,
     0, 
     forceUpdate
   );

   // 3) IDLE
   ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/idle";
   ixBattStr.toCharArray(ixBuff, 50);
   mqtt_publish_i(
     ixBuff, 
     g_stack.batts[ix].isIdle() ? 1 : 0,
     lastSentData.batts[ix].isIdle() ? 1 : 0,
     0, 
     forceUpdate
   );

   // 4) STATE -> testo: "Idle", "Charging", "Discharging"
   ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/state";
   ixBattStr.toCharArray(ixBuff, 50);
   // costruiamo la stringa di stato
   const char* newState =
     g_stack.batts[ix].isIdle()        ? "Idle" :
     g_stack.batts[ix].isCharging()    ? "Charging" :
     g_stack.batts[ix].isDischarging() ? "Discharging" :
                                         "Unknown";

   const char* oldState =
     lastSentData.batts[ix].isIdle()        ? "Idle" :
     lastSentData.batts[ix].isCharging()    ? "Charging" :
     lastSentData.batts[ix].isDischarging() ? "Discharging" :
                                              "Unknown";

   mqtt_publish_s(
     ixBuff, 
     newState,
     oldState,
     forceUpdate
   );
 }

}

/**
 * mqttLoop
 * 
 * Responsible for:
 * 1. Connecting to MQTT if not connected
 * 2. Periodically sending battery data (every MQTT_PUSH_FREQ_SEC seconds)
 * 3. Invoking Home Assistant discovery
 */
void mqttLoop()
{
  static unsigned long g_lastConnectionAttempt = 0;
  const char* topicLastWill = MQTT_TOPIC_ROOT "availability";

  // Connect to MQTT if not connected (and not tried recently)
  if (!mqttClient.connected() && 
      (g_lastConnectionAttempt == 0 || os_getCurrentTimeSec() - g_lastConnectionAttempt > 60)) {
    if(mqttClient.connect(WIFI_HOSTNAME, MQTT_USER, MQTT_PASSWORD, topicLastWill, 1, true, "offline"))
    {
      Log("Connected to MQTT server: " MQTT_SERVER);
      mqttClient.publish(topicLastWill, "online", true);

      // Publish Home Assistant discovery upon connection
      publishHomeAssistantDiscovery();
      Log("Publishing autodiscovery...");
    }
    else
    {
      Log("Failed to connect to MQTT server.");
    }
    g_lastConnectionAttempt = os_getCurrentTimeSec();
  }

  // If connected, gather data from the battery and publish
  static unsigned long g_lastDataSent = 0;
  if(mqttClient.connected() && 
     os_getCurrentTimeSec() - g_lastDataSent > MQTT_PUSH_FREQ_SEC &&
     sendCommandAndReadSerialResponse("pwr") == true)
  {
    static batteryStack lastSentData;
    static unsigned int callCnt = 0;

    parsePwrResponse(g_szRecvBuff);

    // Every 20 cycles, force an update of all fields
    bool forceUpdate = (callCnt % 20 == 0);
    pushBatteryDataToMqtt(lastSentData, forceUpdate);

    callCnt++;
    g_lastDataSent = os_getCurrentTimeSec();
    memcpy(&lastSentData, &g_stack, sizeof(batteryStack));
  }

  mqttClient.loop();
}
#endif //ENABLE_MQTT
