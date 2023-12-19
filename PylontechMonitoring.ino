#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <circular_log.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <ESP8266TimerInterrupt.h>

//+++ START CONFIGURATION +++

//IMPORTANT: Specify your WIFI settings:
#define WIFI_SSID "*** your ssid ***"
#define WIFI_PASS "*** your wifi pass ***"
#define WIFI_HOSTNAME "PylontechBattery"

//IMPORTANT: Uncomment this line if you want to enable MQTT (and fill correct MQTT_ values below):
#define ENABLE_MQTT

// Set offset time in seconds to adjust for your timezone, for example:
// GMT +1 = 3600
// GMT +1 = 7200
// GMT +8 = 28800
// GMT -1 = -3600
// GMT 0 = 0
#define GMT 7200

//NOTE 1: if you want to change what is pushed via MQTT - edit function: pushBatteryDataToMqtt.
//NOTE 2: MQTT_TOPIC_ROOT is where battery will push MQTT topics. For example "soc" will be pushed to: "home/grid_battery/soc"
#define MQTT_SERVER        "*** your MQTT server ***"
#define MQTT_PORT          1883
#define MQTT_USER          "*** your MQTT username ***"
#define MQTT_PASSWORD      "*** your MQTT password ***"
#define MQTT_TOPIC_ROOT    "pylontech/sensor/grid_battery/"  //this is where mqtt data will be pushed
#define MQTT_PUSH_FREQ_SEC 2  //maximum mqtt update frequency in seconds


// Set your Static IP address
IPAddress local_IP(*** your IP ***);
// Set your Gateway IP address
IPAddress gateway(*** your router ip ***);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(*** your router ip ***);   //optional

//set http Authentication
const char* www_username = "admin";
const char* www_password = "password";

//+++   END CONFIGURATION +++

#ifdef ENABLE_MQTT
#include <PubSubClient.h>
WiFiClient espClient;
PubSubClient mqttClient(espClient);
#endif //ENABLE_MQTT

//text response
char g_szRecvBuff[7000];

const long utcOffsetInSeconds = GMT;
char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
// Define NTP Client to get time
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);


ESP8266WebServer server(80);
circular_log<7000> g_log;
bool ntpTimeReceived = false;
int g_baudRate = 0;

void Log(const char* msg)
{
  g_log.Log(msg);
}

//Define Interrupt Timer to Calculate Power meter every second (kWh)
#define USING_TIM_DIV1 true                                             // for shortest and most accurate timer
ESP8266Timer ITimer;
bool setInterval(unsigned long interval, timer_callback callback);      // interval (in microseconds)
#define TIMER_INTERVAL_MS 1000

//Global Variables for the Power Meter - accessible from the calculating interrupt und from main
unsigned long powerIN = 0;       //WS gone in to the BAttery
unsigned long powerOUT = 0;      //WS gone out of the Battery
//Global Variables for the Power Meter - Überlauf
unsigned long powerINWh = 0;       //WS gone in to the BAttery
unsigned long powerOUTWh = 0;      //WS gone out of the Battery
void setup() {
  
  memset(g_szRecvBuff, 0, sizeof(g_szRecvBuff)); //clean variable
  
  pinMode(LED_BUILTIN, OUTPUT); 
  digitalWrite(LED_BUILTIN, HIGH);//high is off
  
  // put your setup code here, to run once:
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false); //our credentialss are hardcoded, so we don't need ESP saving those each boot (will save on flash wear)
  WiFi.hostname(WIFI_HOSTNAME);
  WiFi.config(local_IP, gateway, subnet, primaryDNS);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  for(int ix=0; ix<10; ix++)
  {
    Log("Wait for WIFI Connection");
    if(WiFi.status() == WL_CONNECTED)
    {
      Log("Wifi Connected.");
      break;
    }

    delay(1000);
  }

  ArduinoOTA.setHostname(WIFI_HOSTNAME);
  ArduinoOTA.begin();
  server.on("/", handleRoot);
  server.on("/log", handleLog);
  server.on("/req", handleReq);
  server.on("/jsonOut", handleJsonOut);
  server.on("/reboot", [](){
    if (!server.authenticate(www_username, www_password)) {
      return server.requestAuthentication();
    }
    ESP.restart();
  });
  
  server.begin(); 
  
  timeClient.begin();

  
#ifdef ENABLE_MQTT
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
#endif

  Log("Boot event");

  //Setup timer for interrupt energy meter counter
  ITimer.attachInterruptInterval(TIMER_INTERVAL_MS * 1000, EnergyMeterTimerHandler);
  
}

void handleLog()
{
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  server.send(200, "text/html", g_log.c_str());
}

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

void waitForSerial()
{
  for(int ix=0; ix<150;ix++)
  {
    if(Serial.available()) break;
    delay(10);
  }
}

int readFromSerial()
{
  memset(g_szRecvBuff, 0, sizeof(g_szRecvBuff));
  int recvBuffLen = 0;
  bool foundTerminator = true;
  
  waitForSerial();
  
  while(Serial.available())
  {
    char szResponse[256] = "";
    const int readNow = Serial.readBytesUntil('>', szResponse, sizeof(szResponse)-1); //all commands terminate with "$$\r\n\rpylon>" (no new line at the end)
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
        strcat(g_szRecvBuff, ">"); //readBytesUntil will skip this, so re-add
        foundTerminator = true;
        break; //found end of the string
      }

      if(strstr(g_szRecvBuff, "Press [Enter] to be continued,other key to exit"))
      {
        //we need to send new line character so battery continues the output
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

  //wake up console and try again:
  wakeUpConsole();

  if(pszCommand[0] != '\0')
  {
    Serial.write(pszCommand);
  }
  Serial.write("\n");

  return readFromSerial() > 0;
}

void handleReq()
{
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
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



void handleJsonOut()
{
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  if(sendCommandAndReadSerialResponse("pwr") == false)
  {
    server.send(500, "text/plain", "Failed to get response to 'pwr' command");
    return;
  }

  parsePwrResponse(g_szRecvBuff);
  prepareJsonOutput(g_szRecvBuff, sizeof(g_szRecvBuff));
  server.send(200, "application/json", g_szRecvBuff);
}

void handleRoot() {
  if (!server.authenticate(www_username, www_password)) {
    return server.requestAuthentication();
  }
  timeClient.update(); //get ntp datetime
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
  //Get a time structure
  struct tm *ptm = gmtime ((time_t *)&epochTime); 
  int currentMonth = ptm->tm_mon+1;

  static char szTmp[9500] = "";  
  long timezone= GMT / 3600;
  snprintf(szTmp, sizeof(szTmp)-1, "<html><b>Pylontech Battery</b><br>Time GMT: %s (%s %d)<br>Uptime: %02d:%02d:%02d.%02d<br><br>free heap: %u<br>Wifi RSSI: %d<BR>Wifi SSID: %s", 
            formattedTime, "GMT ", timezone,
            (int)days, (int)hours, (int)minutes, (int)val, 
            ESP.getFreeHeap(), WiFi.RSSI(), WiFi.SSID().c_str());

  strncat(szTmp, "<BR><a href='/log'>Runtime log</a><HR>", sizeof(szTmp)-1);
  strncat(szTmp, "<form action='/req' method='get'>Command:<input type='text' name='code'/><input type='submit'><a href='/req?code=pwr'>Power</a> | <a href='/req?code=help'>Help</a> | <a href='/req?code=log'>Event Log</a> | <a href='/req?code=time'>Time</a><br>", sizeof(szTmp)-1);
  strncat(szTmp, "<textarea rows='80' cols='180'>", sizeof(szTmp)-1);
  strncat(szTmp, g_szRecvBuff, sizeof(szTmp)-1);
  strncat(szTmp, "</textarea></form>", sizeof(szTmp)-1);
  strncat(szTmp, "</html>", sizeof(szTmp)-1);
  //send page
  server.send(200, "text/html", szTmp);
}

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
  
  //millis will wrap each 50 days, as we are interested only in seconds, let's keep the wrap counter
  return (wrapCnt*4294967) + seconds;
}

void wakeUpConsole()
{
  switchBaud(1200);

  //byte wakeUpBuff[] = {0x7E, 0x32, 0x30, 0x30, 0x31, 0x34, 0x36, 0x38, 0x32, 0x43, 0x30, 0x30, 0x34, 0x38, 0x35, 0x32, 0x30, 0x46, 0x43, 0x43, 0x33, 0x0D};
  //Serial.write(wakeUpBuff, sizeof(wakeUpBuff));
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

#define MAX_PYLON_BATTERIES 8

struct pylonBattery
{
  bool isPresent;
  long  soc;     //Coulomb in %
  long  voltage; //in mW
  long  current; //in mA, negative value is discharge
  long  tempr;   //temp of case or BMS?
  long  cellTempLow;
  long  cellTempHigh;
  long  cellVoltLow;
  long  cellVoltHigh;
  char baseState[9];    //Charge | Dischg | Idle
  char voltageState[9]; //Normal
  char currentState[9]; //Normal
  char tempState[9];    //Normal
  char time[20];        //2019-06-08 04:00:29
  char b_v_st[9];       //Normal  (battery voltage?)
  char b_t_st[9];       //Normal  (battery temperature?)

  bool isCharging()    const { return strcmp(baseState, "Charge")   == 0; }
  bool isDischarging() const { return strcmp(baseState, "Dischg")   == 0; }
  bool isIdle()        const { return strcmp(baseState, "Idle")     == 0; }
  bool isBalancing()   const { return strcmp(baseState, "Balance")  == 0; }
  

  bool isNormal() const
  {
    if(isCharging()    == false &&
       isDischarging() == false &&
       isIdle()        == false &&
       isBalancing()   == false)
    {
      return false; //base state looks wrong!
    }

    return  strcmp(voltageState, "Normal") == 0 &&
            strcmp(currentState, "Normal") == 0 &&
            strcmp(tempState,    "Normal") == 0 &&
            strcmp(b_v_st,       "Normal") == 0 &&
            strcmp(b_t_st,       "Normal") == 0 ;
  }
};

struct batteryStack
{
  int batteryCount;
  int soc;  //in %, if charging: average SOC, otherwise: lowest SOC
  int temp; //in mC, if highest temp is > 15C, this will show the highest temp, otherwise the lowest
  long currentDC;    //mAh current going in or out of the battery
  long avgVoltage;    //in mV
  char baseState[9];  //Charge | Dischg | Idle | Balance | Alarm!
  
  pylonBattery batts[MAX_PYLON_BATTERIES];

  bool isNormal() const
  {
    for(int ix=0; ix<MAX_PYLON_BATTERIES; ix++)
    {
      if(batts[ix].isPresent && 
         batts[ix].isNormal() == false)
      {
        return false;
      }
    }

    return true;
  }

  //in Wh
  long getPowerDC() const
  {
    return (long)(((double)currentDC/1000.0)*((double)avgVoltage/1000.0));
  }


  // power in kWh in charge
  float getpowerIN() const
  {
    return ((float)powerIN/3600000) + ((float)powerINWh/3600);
  }
  
  // power in kWh in discharge
  float getpowerOUT() const
  {
    return ((float)powerOUT/3600000) + ((float)powerOUTWh/3600);
  }

/*
  //Wh estimated current on AC side (taking into account Sofar ME3000SP losses)
  long getEstPowerAc() const
  {
    double powerDC = (double)getPowerDC();
    if(powerDC == 0)
    {
      return 0;
    }
    else if(powerDC < 0)
    {
      //we are discharging, on AC side we will see less power due to losses
      if(powerDC < -1000)
      {
        return (long)(powerDC*0.94);
      }
      else if(powerDC < -600)
      {
        return (long)(powerDC*0.90);
      }
      else
      {
        return (long)(powerDC*0.87);
      }
    }
    else
    {
      //we are charging, on AC side we will have more power due to losses
      if(powerDC > 1000)
      {
        return (long)(powerDC*1.06);
      }
      else if(powerDC > 600)
      {
        return (long)(powerDC*1.1);
      }
      else
      {
        return (long)(powerDC*1.13);
      }
    }
  }
  */
};

batteryStack g_stack;


long extractInt(const char* pStr, int pos)
{
  return atol(pStr+pos);
}

void extractStr(const char* pStr, int pos, char* strOut, int strOutSize)
{
  strOut[strOutSize-1] = '\0';
  strncpy(strOut, pStr+pos, strOutSize-1);
  strOutSize--;
  
  
  //trim right
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

/* Output has mixed \r and \r\n
pwr

@

Power Volt   Curr   Tempr  Tlow   Thigh  Vlow   Vhigh  Base.St  Volt.St  Curr.St  Temp.St  Coulomb  Time                 B.V.St   B.T.St  

1     49735  -1440  22000  19000  19000  3315   3317   Dischg   Normal   Normal   Normal   93%      2019-06-08 04:00:30  Normal   Normal  

....   

8     -      -      -      -      -      -      -      Absent   -        -        -        -        -                    -        -       

Command completed successfully

$$

pylon
*/
bool parsePwrResponse(const char* pStr)
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

  memset(&g_stack, 0, sizeof(g_stack));

  for(int ix=0; ix<MAX_PYLON_BATTERIES; ix++)
  {
    char szToFind[32] = "";
    snprintf(szToFind, sizeof(szToFind)-1, "\r\r\n%d     ", ix+1);

    const char* pLineStart = strstr(pStr, szToFind);
    if(pLineStart == NULL)
    {
      return false;
    }

    pLineStart += 3; //move past \r\r\n

    extractStr(pLineStart, 55, g_stack.batts[ix].baseState, sizeof(g_stack.batts[ix].baseState));
    if(strcmp(g_stack.batts[ix].baseState, "Absent") == 0)
    {
      g_stack.batts[ix].isPresent = false;
    }
    else
    {
      g_stack.batts[ix].isPresent = true;
      extractStr(pLineStart, 64, g_stack.batts[ix].voltageState, sizeof(g_stack.batts[ix].voltageState));
      extractStr(pLineStart, 73, g_stack.batts[ix].currentState, sizeof(g_stack.batts[ix].currentState));
      extractStr(pLineStart, 82, g_stack.batts[ix].tempState, sizeof(g_stack.batts[ix].tempState));
      extractStr(pLineStart, 100, g_stack.batts[ix].time, sizeof(g_stack.batts[ix].time));
      extractStr(pLineStart, 121, g_stack.batts[ix].b_v_st, sizeof(g_stack.batts[ix].b_v_st));
      extractStr(pLineStart, 130, g_stack.batts[ix].b_t_st, sizeof(g_stack.batts[ix].b_t_st));
      g_stack.batts[ix].voltage = extractInt(pLineStart, 6);
      g_stack.batts[ix].current = extractInt(pLineStart, 13);
      g_stack.batts[ix].tempr   = extractInt(pLineStart, 20);
      g_stack.batts[ix].cellTempLow    = extractInt(pLineStart, 27);
      g_stack.batts[ix].cellTempHigh   = extractInt(pLineStart, 34);
      g_stack.batts[ix].cellVoltLow    = extractInt(pLineStart, 41);
      g_stack.batts[ix].cellVoltHigh   = extractInt(pLineStart, 48);
      g_stack.batts[ix].soc            = extractInt(pLineStart, 91);

      //////////////////////////////// Post-process ////////////////////////
      g_stack.batteryCount++;
      g_stack.currentDC += g_stack.batts[ix].current;
      g_stack.avgVoltage += g_stack.batts[ix].voltage;
      socAvg += g_stack.batts[ix].soc;

      if(g_stack.batts[ix].isNormal() == false){ alarmCnt++; }
      else if(g_stack.batts[ix].isCharging()){chargeCnt++;}
      else if(g_stack.batts[ix].isDischarging()){dischargeCnt++;}
      else if(g_stack.batts[ix].isIdle()){idleCnt++;}
      else{ alarmCnt++; } //should not really happen!

      if(g_stack.batteryCount == 1)
      {
        socLow = g_stack.batts[ix].soc;
        tempLow  = g_stack.batts[ix].cellTempLow;
        tempHigh = g_stack.batts[ix].cellTempHigh;
      }
      else
      {
        if(socLow > g_stack.batts[ix].soc){socLow = g_stack.batts[ix].soc;}
        if(tempHigh < g_stack.batts[ix].cellTempHigh){tempHigh = g_stack.batts[ix].cellTempHigh;}
        if(tempLow > g_stack.batts[ix].cellTempLow){tempLow = g_stack.batts[ix].cellTempLow;}
      }
      
    }
  }

  //now update stack state:
  g_stack.avgVoltage /= g_stack.batteryCount;
  g_stack.soc = socLow;

  if(tempHigh > 15000) //15C
  {
    g_stack.temp = tempHigh; //in the summer we highlight the warmest cell
  }
  else
  {
    g_stack.temp = tempLow; //in the winter we focus on coldest cell
  }

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

void prepareJsonOutput(char* pBuff, int buffSize)
{
  memset(pBuff, 0, buffSize);
  snprintf(pBuff, buffSize-1, "{\"soc\": %d, \"temp\": %d, \"currentDC\": %ld, \"avgVoltage\": %ld, \"baseState\": \"%s\", \"batteryCount\": %d, \"powerDC\": %ld, \"estPowerAC\": %ld, \"isNormal\": %s}", g_stack.soc, 
                                                                                                                                                                                                            g_stack.temp, 
                                                                                                                                                                                                            g_stack.currentDC, 
                                                                                                                                                                                                            g_stack.avgVoltage, 
                                                                                                                                                                                                            g_stack.baseState, 
                                                                                                                                                                                                            g_stack.batteryCount, 
                                                                                                                                                                                                            g_stack.getPowerDC(), 
                                                                                                                                                                                                            /*g_stack.getEstPowerAc(),*/
                                                                                                                                                                                                            g_stack.isNormal() ? "true" : "false");
}



//Define ISR to calculate WattSeconds gone in and out of the Battery
void EnergyMeterTimerHandler()
{
//  onlineTime += 1;

  if(powerOUT > 4000000000)
  {
    powerOUTWh = powerOUT/3600;
    powerOUT = 0;
    Log("Copy powerOUT to Wh and reset Ws counter");
  }
  if(powerIN > 4000000000)
  {
    powerINWh = powerIN/3600;
    powerIN = 0;
    Log("Copy powerIN to Wh and reset Ws counter");
  }
  
  long power = g_stack.getPowerDC();
  if(power == 0)
  {
    return;
  }
  else if(power < 0)
  {
    powerOUT += abs(power);
  }
  else
  {
    powerIN += power;
  } 
}




void loop() {
#ifdef ENABLE_MQTT
  mqttLoop();
#endif
  
  ArduinoOTA.handle();
  server.handleClient();

  //if there are bytes availbe on serial here - it's unexpected
  //when we send a command to battery, we read whole response
  //if we get anything here anyways - we will log it
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
      digitalWrite(LED_BUILTIN, HIGH);//high is off

      Log(buff);
    }
  }
}

#ifdef ENABLE_MQTT
#define ABS_DIFF(a, b) (a > b ? a-b : b-a)
void mqtt_publish_f(const char* topic, float newValue, float oldValue, float minDiff, bool force)
{
  char szTmp[16] = "";
  snprintf(szTmp, 15, "%.2f", newValue);
  if(force || ABS_DIFF(newValue, oldValue) > minDiff)
  {
    mqttClient.publish(topic, szTmp, false);
  }
}

void mqtt_publish_i(const char* topic, int newValue, int oldValue, int minDiff, bool force)
{
  char szTmp[16] = "";
  snprintf(szTmp, 15, "%d", newValue);
  if(force || ABS_DIFF(newValue, oldValue) > minDiff)
  {
    mqttClient.publish(topic, szTmp, false);
  }
}

void mqtt_publish_s(const char* topic, const char* newValue, const char* oldValue, bool force)
{
  if(force || strcmp(newValue, oldValue) != 0)
  {
    mqttClient.publish(topic, newValue, false);
  }
}

void pushBatteryDataToMqtt(const batteryStack& lastSentData, bool forceUpdate /* if true - we will send all data regardless if it's the same */)
{
  mqtt_publish_f(MQTT_TOPIC_ROOT "soc",          g_stack.soc,                lastSentData.soc,                0, forceUpdate);
  mqtt_publish_f(MQTT_TOPIC_ROOT "temp",         (float)g_stack.temp/1000.0, (float)lastSentData.temp/1000.0, 0, forceUpdate);
  mqtt_publish_i(MQTT_TOPIC_ROOT "currentDC",    g_stack.currentDC,          lastSentData.currentDC,          1, forceUpdate);
 // mqtt_publish_i(MQTT_TOPIC_ROOT "estPowerAC",   g_stack.getEstPowerAc(),    lastSentData.getEstPowerAc(),   10, forceUpdate);
  mqtt_publish_i(MQTT_TOPIC_ROOT "battery_count",g_stack.batteryCount,       lastSentData.batteryCount,       0, forceUpdate);
  mqtt_publish_s(MQTT_TOPIC_ROOT "base_state",   g_stack.baseState,          lastSentData.baseState            , forceUpdate);
  mqtt_publish_i(MQTT_TOPIC_ROOT "is_normal",    g_stack.isNormal() ? 1:0,   lastSentData.isNormal() ? 1:0,   0, forceUpdate);
  mqtt_publish_i(MQTT_TOPIC_ROOT "getPowerDC",   g_stack.getPowerDC(),       lastSentData.getPowerDC(),       1, forceUpdate);
  mqtt_publish_f(MQTT_TOPIC_ROOT "powerIN",      g_stack.getpowerIN(),          lastSentData.getpowerIN(),          1, forceUpdate);
  mqtt_publish_f(MQTT_TOPIC_ROOT "powerOUT",     g_stack.getpowerOUT(),         lastSentData.getpowerOUT(),         1, forceUpdate);

  // publishing details
  for (int ix = 0; ix < g_stack.batteryCount; ix++) {
    char ixBuff[50];
    String ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/voltage";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_f(ixBuff, g_stack.batts[ix].voltage / 1000.0, lastSentData.batts[ix].voltage / 1000.0, 0, forceUpdate);
    ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/current";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_f(ixBuff, g_stack.batts[ix].current / 1000.0, lastSentData.batts[ix].current / 1000.0, 0, forceUpdate);
    ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/soc";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_i(ixBuff, g_stack.batts[ix].soc, lastSentData.batts[ix].soc, 0, forceUpdate);
    ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/charging";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_i(ixBuff, g_stack.batts[ix].isCharging()?1:0, lastSentData.batts[ix].isCharging()?1:0, 0, forceUpdate);
    ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/discharging";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_i(ixBuff, g_stack.batts[ix].isDischarging()?1:0, lastSentData.batts[ix].isDischarging()?1:0, 0, forceUpdate);
    ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/idle";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_i(ixBuff, g_stack.batts[ix].isIdle()?1:0, lastSentData.batts[ix].isIdle()?1:0, 0, forceUpdate);
    ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/state";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_s(ixBuff, g_stack.batts[ix].isIdle()?"Idle":g_stack.batts[ix].isCharging()?"Charging":g_stack.batts[ix].isDischarging()?"Discharging":"", lastSentData.batts[ix].isIdle()?"Idle":lastSentData.batts[ix].isCharging()?"Charging":lastSentData.batts[ix].isDischarging()?"Discharging":"", forceUpdate);
    ixBattStr = MQTT_TOPIC_ROOT + String(ix) + "/temp";
    ixBattStr.toCharArray(ixBuff, 50);
    mqtt_publish_f(ixBuff, (float)g_stack.batts[ix].tempr/1000.0, (float)lastSentData.batts[ix].tempr/1000.0, 0.1, forceUpdate);
  }
} 

void mqttLoop()
{
  //if we have problems with connecting to mqtt server, we will attempt to re-estabish connection each 1minute (not more than that)
  static unsigned long g_lastConnectionAttempt = 0;

  //first: let's make sure we are connected to mqtt
  const char* topicLastWill = MQTT_TOPIC_ROOT "availability";
  if (!mqttClient.connected() && (g_lastConnectionAttempt == 0 || os_getCurrentTimeSec() - g_lastConnectionAttempt > 60)) {
    if(mqttClient.connect(WIFI_HOSTNAME, MQTT_USER, MQTT_PASSWORD, topicLastWill, 1, true, "offline"))
    {
      Log("Connected to MQTT server: " MQTT_SERVER);
      mqttClient.publish(topicLastWill, "online", true);
    }
    else
    {
      Log("Failed to connect to MQTT server.");
    }

    g_lastConnectionAttempt = os_getCurrentTimeSec();
  }

  //next: read data from battery and send via MQTT (but only once per MQTT_PUSH_FREQ_SEC seconds)
  static unsigned long g_lastDataSent = 0;
  if(mqttClient.connected() && 
     os_getCurrentTimeSec() - g_lastDataSent > MQTT_PUSH_FREQ_SEC &&
     sendCommandAndReadSerialResponse("pwr") == true)
  {
    static batteryStack lastSentData; //this is the last state we sent to MQTT, used to prevent sending the same data over and over again
    static unsigned int callCnt = 0;
    
    parsePwrResponse(g_szRecvBuff);

    bool forceUpdate = (callCnt % 20 == 0); //push all the data every 20th call
    pushBatteryDataToMqtt(lastSentData, forceUpdate);
    
    callCnt++;
    g_lastDataSent = os_getCurrentTimeSec();
    memcpy(&lastSentData, &g_stack, sizeof(batteryStack));
  }
  
  mqttClient.loop();
}

#endif //ENABLE_MQTT
