/*
Domoticz_openterm_handler, based on  OpenTherm Master Communication Example from Ihor Melnyk
By: Arnold Kamminga
Date: May 14th , 2021
Uses the WiFiManager from tzapu to configure wifi connection
Uses the OpenTherm library from ihormelyk to communicate with boiler
Open serial monitor at 115200 baud to see output.
Hardware Connections (OpenTherm Adapter (http://ihormelnyk.com/pages/OpenTherm) to Arduino/ESP8266):
-OT1/OT2 = Boiler X1/X2
-VCC = 5V or 3.3V
-GND = GND
-IN  = Arduino (3) / ESP8266 (5g) Input Pin
*/


#include <Arduino.h>              // duh...
#include <OpenTherm.h>            //https://github.com/ihormelnyk/opentherm_library
#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino
#include <ESP8266WebServer.h>     //Local Webserver for receiving http commands
#include <ESP8266mDNS.h>          // To respond on hostname as well
#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic
#include <OneWire.h>              // One Wire bus
#include <DallasTemperature.h>    // temperature sensors
#include <ArduinoOTA.h>           // OTA updates
#include <ArduinoJson.h>          // make JSON payloads
#include <PubSubClient.h>         // MQTT library
#include "config.h"                 // Set Configuration
// #include <SPIFFS.h>               // Filesystem


// vars to manage boiler
bool enableCentralHeating = false;
bool enableHotWater = true;
bool enableCooling = false;
float boiler_SetPoint = 0;
float dhw_SetPoint = 65;

// device names for mqtt autodiscovery
const char Boiler_Temperature_Name[] = "Boiler_Temperature";   
const char DHW_Temperature_Name[] = "DHW_Temperature";   
const char Return_Temperature_Name[] = "Return_Temperature";   
const char Thermostat_Temperature_Name[] = "Thermostat_Temperature";   
const char Outside_Temperature_Name[] = "Outside_Temperature";   
const char FlameActive_Name[] = "FlameActive";   
const char FaultActive_Name[] = "FaultActive";   
const char DiagnosticActive_Name[] = "DiagnosticActive";   
const char CoolingActive_Name[] = "CoolingActive";   
const char CentralHeatingActive_Name[] = "CentralHeatingActive";   
const char HotWaterActive_Name[] = "HotWaterActive";  
const char EnableCooling_Name[] = "EnableCooling";   
const char EnableCentralHeating_Name[] = "EnableCentralHeating";   
const char EnableHotWater_Name[] = "EnableHotWater";   
const char Boiler_Setpoint_Name[] = "Boiler_Setpoint";
const char DHW_Setpoint_Name[] = "DHW_Setpoint";
const char Modulation_Name[] = "Modulation";
const char Pressure_Name[] = "Pressure";
const char FaultCode_Name[] = "Faultcode";

// Current Temp on Thermostat
float currentTemperature = 0;

// return values from boiler
float dhw_Temperature = 0;
float boiler_Temperature = 0;
float return_Temperature = 0;
float outside_Temperature = 0;
float modulation = 0;
float pressure = 0;
float flowrate = 0;
OpenThermResponseStatus responseStatus;
bool CentralHeating = false;
bool HotWater = false;
bool Cooling = false;
bool Flame = false;
bool Fault = false;
bool Diagnostic = false;
unsigned char FaultCode=65;


// reported vars to mqtt, make sure all are initialized on unexpected values, so they are sent the 1st time
float mqtt_boiler_Temperature=100;
float mqtt_dhw_Temperature=100;
float mqtt_return_Temperature=100;
float mqtt_outside_Temperature=100;
float mqtt_currentTemperature=100;
bool mqtt_CentralHeating=true;
bool mqtt_HotWater=true;
bool mqtt_Cooling=true;
bool mqtt_Flame=true;
bool mqtt_Fault=true;
bool mqtt_Diagnostic=true;
float mqtt_modulation=101;
bool mqtt_enable_HotWater=false;
bool mqtt_enable_CentralHeating=true;
bool mqtt_enable_Cooling=true;
float mqtt_boiler_setpoint=1;
float mqtt_dhw_setpoint=0;
float mqtt_pressure=3; 
unsigned char mqtt_FaultCode=65;

// ot actions (main will loop through these actions in this order)
enum OTCommand { SetBoilerStatus, 
                 SetBoilerTemp, 
                 SetDHWTemp, 
                 GetBoilerTemp,  
                 GetDHWTemp, 
                 GetReturnTemp, 
                 GetOutsideTemp, 
                 GetPressure, 
                 GetFlowRate,
                 GetFaultCode, 
                 GetThermostatTemp 
               } OpenThermCommand ;

// vars for program logic
const char compile_date[] = __DATE__ " " __TIME__;  // Make sure we can output compile date
unsigned long t_heartbeat=millis()-heartbeatTickInMillis; // last heartbeat timestamp, init on previous heartbeat, so processing start right away
unsigned long t_last_mqtt_command=millis()-MQTTTimeoutInMillis; // last MQTT command timestamp. init on previous timeout value, so processing start right away
unsigned long t_last_http_command=millis()-HTTPTimeoutInMillis; // last HTTP command timestamp. init on previous timeout value, so processing start right away
unsigned long t_last_mqtt_discovery=millis()-MQTTDiscoveryHeartbeatInMillis; // last mqqt discovery timestamp
bool OTAUpdateInProgress=false;

// objects to be used by program
ESP8266WebServer server(httpport);   //Web server object. Will be listening in port 80 (default for HTTP)
OneWire oneWire(OneWireBus);  // for OneWire Bus
DallasTemperature sensors(&oneWire); // for the temp sensor on one wire bus
OpenTherm ot(inPin, outPin); // for communication with Opentherm adapter
WiFiClient espClient;  // Needed for MQTT
PubSubClient MQTT(espClient); // MQTT client


void ICACHE_RAM_ATTR handleInterrupt() {
    ot.handleInterrupt();
}

void SendHTTP(String command, String result) {
    String message = "{\n  \"InterfaceVersion\":2,\n  \"Command\":\""+command+"\",\n  \"Result\":\""+result+"\""+getSensors()+"\n}";
    server.send(200, "application/json", message);       //Response to the HTTP request
}

void handleResetWifiCredentials() {
  Serial.println("Resetting Wifi Credentials");
  WiFiManager wifiManager;
  wifiManager.resetSettings();
  SendHTTP("ResetWifiCredentials","OK");
  delay(500);

  Serial.println("Trigger watchdog to reset wemos");
  wdt_disable();
  wdt_enable(WDTO_15MS);
  while (1) {}
}



void handleGetSensors() {
  Serial.println("Getting the sensors");
  SendHTTP("GetSensors","OK");
}

void handleCommand() {
  String Statustext="Unknown Command";

  // we received a command, so someone is comunicating
  t_last_http_command=millis();

  // blink the LED, so we can see a command was sent
  digitalWrite(LED_BUILTIN, HIGH);    // turn the LED off , to indicate we are executing a command

  // for Debugging purposes
  Serial.println("Handling Command: Number of args received: "+server.args());
  for (int i = 0; i < server.args(); i++) {
    Serial.println ("Argument "+String(i)+" -> "+server.argName(i)+": "+server.arg(i));

  } 

  // Set DHWTemp
  if (server.arg("DHWTemperature")!="") {
    Serial.println("Setting dhw temp to "+server.arg("DHWTemperature"));
    dhw_SetPoint=server.arg("DHWTemperature").toFloat();
    Statustext="OK";
  }

  // Set Boiler Temp
  if (server.arg("BoilerTemperature")!="") {
    Serial.println("Setting boiler temp to "+server.arg("BoilerTemperature"));
    boiler_SetPoint=server.arg("BoilerTemperature").toFloat();
    Statustext="OK";
  }

  // Enable/Disable Cooling
  if (server.arg("Cooling")!="") {
    Statustext="OK";
    if (server.arg("Cooling").equalsIgnoreCase("On")) {
      Serial.println("Enabling Cooling");
      enableCooling= true;
    } else {
      Serial.println("Disabling Cooling");
      enableCooling=false;
    }
  }

  // Enable/Disable Central Heating
  if (server.arg("CentralHeating")!="") {
    Statustext="OK";
    if (server.arg("CentralHeating").equalsIgnoreCase("On")) {
      Serial.println("Enabling Central Heating");
      enableCentralHeating=true;
    } else {
      Serial.println("Disabling Central Heating");
      enableCentralHeating=false;
    }
  }

  // Enable/Disable HotWater
  if (server.arg("HotWater")!="") {
    Statustext="OK";
    if (server.arg("HotWater").equalsIgnoreCase("On")) {
      Serial.println("Enabling Domestic Hot Water");
      enableHotWater=true;
    } else {
      Serial.println("Disabling Domestic Hot Water");
      enableHotWater=false;
    }
  }

  digitalWrite(LED_BUILTIN, LOW);    // turn the LED on , to indicate we executed the command

  SendHTTP("SetDHWTemp",Statustext);
}

String getSensors() { //Handler

  String message;

  // Add Compile Date
  message = ",\n  \"CompileDate\": \"" + String(compile_date) + "\"";

  // Add uptime
  long seconds=millis()/1000;
  int secs = seconds % 60;
  int mins = (seconds/60) % 60;
  int hrs = (seconds/3600) % 24;
  int days = (seconds/(3600*24)); 
  message += ",\n  \"uptime\": \"" + String(days)+" days, "+String(hrs)+" hours, "+String(mins)+" minutes, "+String(secs)+" seconds" + "\"";

  // Add Opentherm Status
  message += ",\n  \"OpenThermStatus\":";
  if (responseStatus == OpenThermResponseStatus::SUCCESS) {
      message += "\"OK\"";

  } else if (responseStatus == OpenThermResponseStatus::NONE) {
      message += "\"OpenTherm is not initialized\"";
  } else if (responseStatus == OpenThermResponseStatus::INVALID) {
      message += "\"Invalid response\"";
  } else if (responseStatus == OpenThermResponseStatus::TIMEOUT) {
      message += "\"Response timeout, is the boiler connected?\"";
  } else {
      message += "\"Unknown Status\"";
  }

  // Report if we are receiving commands
  if (millis()-t_last_http_command<HTTPTimeoutInMillis) {
    message += ",\n  \"ControlledBy\": \"HTTP\""; // HTTP overrules MQTT so check HTTP 1st
  } else if (millis()-t_last_mqtt_command<MQTTTimeoutInMillis) {
    message += ",\n  \"ControlledBy\": \"MQTT\""; // then check if MQTT command was given
  } else {
    message += ",\n  \"ControlledBy\": \"None\""; // no commands given lately
  }
  
  // Add MQTT Connection status 
  message += ",\n  \"MQTTconnected\": " + String(MQTT.connected() ? "true" : "false");
  message += ",\n  \"MQTTstate\": " + String(MQTT.state());

  // Add BoilerManagementVars
  message += ",\n  \"EnableCentralHeating\": " + String(enableCentralHeating ? "\"on\"" : "\"off\"");
  message += ",\n  \"EnableHotWater\": " + String(enableHotWater ? "\"on\"" : "\"off\"");
  message += ",\n  \"EnableCooling\": " + String(enableCooling ? "\"on\"" : "\"off\"");
  message +=",\n  \"BoilerSetpoint\": " + (String(boiler_SetPoint));
  message +=",\n  \"DHWSetpoint\": " + (String(dhw_SetPoint));

  
  // Add BoilerStatus
  message += ",\n  \"CentralHeating\": " + String(CentralHeating ? "\"on\"" : "\"off\"");
  message += ",\n  \"HotWater\": " + String(HotWater ? "\"on\"" : "\"off\"");
  message += ",\n  \"Cooling\": " + String(Cooling ? "\"on\"" : "\"off\"");
  message += ",\n  \"Flame\": " + String(Flame ? "\"on\"" : "\"off\"");
  message += ",\n  \"Fault\": " + String(Fault ? "\"on\"" : "\"off\"");
  message += ",\n  \"Diagnostic\": " + String(Diagnostic ? "\"on\"" : "\"off\"");

   // Add boiler sensors
  message +=",\n  \"BoilerTemperature\": " + (String)boiler_Temperature;
  message +=",\n  \"DhwTemperature\": " + String(dhw_Temperature);    
  message +=",\n  \"ReturnTemperature\": " + String(return_Temperature);
  message +=",\n  \"OutsideTemperature\": " + String(outside_Temperature);
  message +=",\n  \"Modulation\": " + String(modulation);
  message +=",\n  \"Pressure\": " + String(pressure);
  message +=",\n  \"Flowrate\": " + String(flowrate);
  message +=",\n  \"FaultCode\": " + String(FaultCode);

  // Add Temp Sensor value
  message +=",\n  \"ThermostatTemperature\": " + (String(currentTemperature+ThermostatTemperatureCalibration));

  return message;
}

void handleGetInfo()
{
  Serial.println("GetInfo");
  StaticJsonDocument<512> json;
  char buf[512];
  json["heap"] = ESP.getFreeHeap();
  json["sketchsize"] = ESP.getSketchSize();
  json["sketchspace"] = ESP.getFreeSketchSpace();
  json["cpufrequency"] = ESP.getCpuFreqMHz();
  json["chipid"] = ESP.getChipId();
  json["sdkversion"] = ESP.getSdkVersion();
  json["bootversion"] = ESP.getBootVersion();
  json["bootmode"] = ESP.getBootMode();
  json["flashid"] = ESP.getFlashChipId();
  json["flashspeed"] = ESP.getFlashChipSpeed();
  json["flashsize"] = ESP.getFlashChipRealSize();
  json["resetreason"] = ESP.getResetReason();
  json["resetinfo"] = ESP.getResetInfo();
  json["freeheap"] = ESP.getFreeHeap();
  if (MQTT.connected())
  {
    json["MQTTconnected"] = true;
  } else {
    json["MQTTconnected"] = false;
  }
  json["mqttstate"] = MQTT.state();

  long seconds=millis()/1000;
  int secs = seconds % 60;
  int mins = (seconds/60) % 60;
  int hrs = (seconds/3600) % 24;
  int days = (seconds/(3600*24)); 
  json["uptime"] = String(days)+" days, "+String(hrs)+" hours, "+String(mins)+" minutes, "+String(secs)+" seconds";

  serializeJson(json, buf); 
  server.send(200, "application/json", buf);       //Response to the HTTP request
}

void handleGetConfig()
{
  Serial.println("GetConfig");
  StaticJsonDocument<512> json;
  char buf[512];

  // gpio config
  json["inpin"] = inPin;
  json["outpin"] = outPin;
  json["temppin"] = OneWireBus;

  // mqtt config
  json["usemqtt"] = usemqtt;
  json["mqttserver"] = mqttserver;
  json["mqttport"] = mqttport;
  json["usemqttauthentication"] = usemqttauthentication;
  json["mqttuser"] = mqttuser;
  json["mqttpass"] = "*****"; // This is the only not allowed password, password will only be saved if it is not 5 stars
  json["mqttretained"] = mqttpersistence;  

  // Add some General status info 
  if (MQTT.connected())
  {
    json["MQTTconnected"] = true;
  } else {
    json["MQTTconnected"] = false;
  }
  json["MQTTState"] = MQTT.state();

  json["heap"] = ESP.getFreeHeap();
  long seconds=millis()/1000;
  int secs = seconds % 60;
  int mins = (seconds/60) % 60;
  int hrs = (seconds/3600) % 24;
  int days = (seconds/(3600*24)); 
  json["uptime"] = String(days)+" days, "+String(hrs)+" hours, "+String(mins)+" minutes, "+String(secs)+" seconds";

  serializeJson(json, buf); 
  server.send(200, "application/json", buf);       //Response to the HTTP request
}

void handleSaveConfig() {
  Serial.println("handleSaveConfig");
  String Message;
  
  // for Debugging purposes
  Serial.println("Handling Command: Number of args received: "+server.args());
  for (int i = 0; i < server.args(); i++) {
    Serial.println ("Argument "+String(i)+" -> "+server.argName(i)+": "+server.arg(i));
  } 

  // try to deserialize
  StaticJsonDocument<1024> json;
  DeserializationError error = deserializeJson(json, server.arg("plain"));
  if (error) {
    Message=String("Invalid JSON: ")+error.f_str();
    server.send(500, "text/plain", Message.c_str());
    return;
  } else {
    //save the custom parameters to FS
    Serial.println("saving config");

    File configFile = SPIFFS.open(CONFIGFILE, "w");
    if (!configFile) {
      server.send(500, "text/plain", "failed to open config file for writing");
      return;
    } else {
      // if password was not changed: leave old password in file
      if (json["mqttpass"]=="*****") {
        json["mqttpass"]=mqttpass;
      }
      
      serializeJson(json, configFile);
      configFile.close();

      // dump to response as well
      // serializeJson(json, Message);
      // server.send(200, "text/plain", Message.c_str());
      server.send(200, "text/plain", "New Config Saved");
      delay(500); // wait for server send to finish
      ESP.restart(); // restart
      
      return;
      //end save
    }
  }
}

void handleRemoveConfig() {
  Serial.println("handleRemoveConfig");
  
  if (SPIFFS.exists(CONFIGFILE)) {
    Serial.println("Config file existst, removing configfile");
    SPIFFS.remove(CONFIGFILE);
    server.send(200, "text/plain", "Config file removed");
    delay(500); // wait for server send to finish
    ESP.restart(); // restart
  } else {
    server.send(200, "text/plain", "No confile file present to remove");
  }
  return;
}

void handleReset() {
  Serial.println("handleReset");
  
  server.send(200, "text/plain", "Device Reset");
  delay(500); // wait for server send to finish
  ESP.restart(); // restart

  return;
}

bool endsWith(const char* what, const char* withwhat)
{
    int l1 = strlen(withwhat);
    int l2 = strlen(what);
    if (l1 > l2)
        return 0;

    return strcmp(withwhat, what + (l2 - l1)) == 0;
}


bool serveFile(const char url[])
{
  Serial.printf("serveFile(): %s\n\rE",url);

  char path[50];
  
  if (url[strlen(url)-1]=='/') {
    sprintf (path,"%sindex.html",url);
  } else {
    sprintf(path,"%s",url);
  }
  if (SPIFFS.exists(path))
  {
    File file = SPIFFS.open(path, "r");
    if (server.hasArg("download")) server.streamFile(file, "application/octet-stream");
    else if (endsWith(path,".htm") or endsWith(path,".html")) server.streamFile(file, "text/html");
    else if (endsWith(path,".css") ) server.streamFile(file, "text/css");
    else if (endsWith(path,".png") ) server.streamFile(file, "image/png");
    else if (endsWith(path,".gif") ) server.streamFile(file, "image/gif");
    else if (endsWith(path,".jpg") ) server.streamFile(file, "image/jpeg");
    else if (endsWith(path,".ico") ) server.streamFile(file, "image/x-icon");
    else if (endsWith(path,".xml") ) server.streamFile(file, "text/xml");
    else if (endsWith(path,".pdf") ) server.streamFile(file, "application./x-pdf");
    else if (endsWith(path,".zip") ) server.streamFile(file, "application./x-zip");
    else if (endsWith(path,".gz") ) server.streamFile(file, "application./x-gzip");
    else server.streamFile(file, "text/plain");
    file.close();
    return true;
  }
  return false;
}


void handleNotFound()
{
  // first, try to serve the requested file from flash
  if (!serveFile(server.uri().c_str()))
  {
    // create 404 message if no file was found for this URI
    String message = "File Not Found\n\nURI: "+server.uri() + "\nMethod: "+ ( server.method() == HTTP_GET ? "GET" : "POST" ) + "\nArguments "+server.args()+"\n";
    
    for (uint8_t i = 0; i < server.args(); i++)
    {
      message += server.argName(i)+"="+server.arg(i)+"\n";
    }
    
    server.send(404, "text/plain", message.c_str());
  }
}

void readConfig()
{
  if (SPIFFS.exists(CONFIGFILE)) {
    //file exists, reading and loading
    Serial.println("reading config file");
    File configFile = SPIFFS.open(CONFIGFILE, "r");
    if (configFile) {
      Serial.println("opened config file");
      size_t size = configFile.size();
      // Allocate a buffer to store contents of the file.
      std::unique_ptr<char[]> buf(new char[size]);

      configFile.readBytes(buf.get(), size);

      DynamicJsonDocument json(1024);
      auto deserializeError = deserializeJson(json, buf.get());
      serializeJson(json, Serial);
      if ( ! deserializeError ) {
        Serial.println("\nparsed json");
        usemqtt=json["usemqtt"];
        usemqttauthentication=json["usemqttauthentication"];
        mqttserver=json["mqttserver"].as<String>();
        mqttport=json["mqttport"];
        mqttuser=json["mqttuser"].as<String>();
        mqttpass=json["mqttpass"].as<String>();
        mqttpersistence=json["mqttretained"];
        inPin=json["inpin"];
        outPin=json["outpin"];
        OneWireBus=json["temppin"];
      } else {
        Serial.println("failed to load json config");
      }
      configFile.close();
    }
  }
}

void setup()
{
  // start Serial port for logging purposes
  Serial.begin(115200);
  Serial.println("\nDomEspHelper, compile date "+String(compile_date));

  //read configuration from FS json
  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    readConfig();
  } else {
    Serial.println("failed to mount FS");
  }
  //end read
  
  // Handle Wifi connection by wifi manager
  WiFiManager wifiManager;
  wifiManager.setHostname(host.c_str());
  wifiManager.autoConnect("Thermostat");

  if (MDNS.begin(host.c_str())) {
    Serial.println("MDNS responder started");
  }
  // Log IP adress
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());  //Print the local IP to access the server

  // Register commands on webserver
  server.on("/ResetWifiCredentials", handleResetWifiCredentials);
  server.on("/GetSensors",handleGetSensors);
  server.on("/info", handleGetInfo);
  server.on("/getconfig", handleGetConfig);
  server.on("/saveconfig", handleSaveConfig);
  server.on("/removeconfig", handleRemoveConfig);
  server.on("/reset", handleReset);
  server.on("/command", handleCommand);   
  server.onNotFound(handleNotFound);

  // Initialize OTA
  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname(host.c_str());

  // No authentication by default
  // ArduinoOTA.setPassword(host); // Disable for data upload

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");

  ArduinoOTA.onStart([]() {
    if (ArduinoOTA.getCommand() == U_FLASH) {
      Serial.println("OTA: Start updating sketch...");
    } else { // U_FS
      Serial.println("OTA: Start updating filesystem...");
    }
    OTAUpdateInProgress=true;
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
    OTAUpdateInProgress=false;
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA: Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("OTA: Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("OTA: Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("OTA: Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("OTA: Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("OTA: End Failed");
    }
    OTAUpdateInProgress=false;
  });
  
  ArduinoOTA.begin();
  
  //Start the server
  server.begin();   
  Serial.println("Opentherm Helper is waiting for commands");   

  //Init DS18B20 sensor
  sensors.begin();

  // Init builtin led
  pinMode(LED_BUILTIN, OUTPUT);

  // Start OpenTherm
  ot.begin(handleInterrupt);

  // MQTT
  MQTT.setServer(mqttserver.c_str(), mqttport); // server details
  MQTT.setBufferSize(1024); // discovery messages are longer than default max buffersize(!)
  MQTT.setCallback(MQTTcallback); // listen to callbacks
}

// not defined  in opentherm lib, so declaring local
float getOutsideTemperature() {
  unsigned long response = ot.sendRequest(ot.buildRequest(OpenThermRequestType::READ, OpenThermMessageID::Toutside, 0));
  return ot.isValidResponse(response) ? ot.getFloat(response) : 0;
}

float getDHWFlowrate() {
  unsigned long response = ot.sendRequest(ot.buildRequest(OpenThermMessageType::READ_DATA, OpenThermMessageID::DHWFlowRate, 0));
  return ot.isValidResponse(response) ? ot.getFloat(response) : 0;
}

void handleOpenTherm()
{
  
  switch (OpenThermCommand)
  {
    case SetBoilerStatus:
    {
      // enable/disable heating, hotwater, heating and get status from opentherm connection and boiler (if it can be reached)
      unsigned long response = ot.setBoilerStatus(enableCentralHeating, enableHotWater, enableCooling);
      responseStatus = ot.getLastResponseStatus();
      if (responseStatus == OpenThermResponseStatus::SUCCESS) {
        // Yes we have a connection, update statuses
        Serial.println("Central Heating: " + String(ot.isCentralHeatingActive(response) ? "on" : "off"));
        Serial.println("Hot Water: " + String(ot.isHotWaterActive(response) ? "on" : "off"));
        Serial.println("Cooling: " + String(ot.isCoolingActive(response) ? "on" : "off"));
        Serial.println("Flame: " + String(ot.isFlameOn(response) ? "on" : "off"));
        Flame=ot.isFlameOn(response);
        CentralHeating=ot.isCentralHeatingActive(response);
        HotWater=ot.isHotWaterActive(response);
        Cooling=ot.isCoolingActive(response);
        Fault=ot.isFault(response);
        Diagnostic=ot.isDiagnostic(response);

        // modulation is reported on the switches, so make sure we have modulation value as well
        modulation = ot.getModulation();

        // Check if we have to send to MQTT
        if (MQTT.connected()) {
          if (Flame!=mqtt_Flame){ // value changed
            UpdateMQTTSwitch(FlameActive_Name,Flame);
            mqtt_Flame=Flame;
          }
          if (Fault!=mqtt_Fault){ // value changed
            UpdateMQTTSwitch(FaultActive_Name,Fault);
            mqtt_Fault=Fault;
          }
          if (Diagnostic!=mqtt_Diagnostic){ // value changed
            UpdateMQTTSwitch(DiagnosticActive_Name,Diagnostic);
            mqtt_Diagnostic=Diagnostic;
          }
          if (enableCentralHeating!=mqtt_enable_CentralHeating) // value changed
          {
            UpdateMQTTSwitch(EnableCentralHeating_Name,enableCentralHeating);
            mqtt_enable_CentralHeating=enableCentralHeating;
          }
          if (enableCooling!=mqtt_enable_Cooling) // value changed
          {
            UpdateMQTTSwitch(EnableCooling_Name,enableCooling);
            mqtt_enable_Cooling=enableCooling;
          }
          if (enableHotWater!=mqtt_enable_HotWater) // value changed
          {
            UpdateMQTTSwitch(EnableHotWater_Name,enableHotWater);
            mqtt_enable_HotWater=enableHotWater;
          }
          if (Cooling!=mqtt_Cooling){ // value changed
            UpdateMQTTSwitch(CoolingActive_Name,Cooling);
            mqtt_Cooling=Cooling;
          }
          if (CentralHeating!=mqtt_CentralHeating or modulation!=mqtt_modulation){ // Update switch when on/off or modulation changed
            if (CentralHeating) {
              UpdateMQTTDimmer(CentralHeatingActive_Name,CentralHeating,modulation);
            } else {
              UpdateMQTTDimmer(CentralHeatingActive_Name,CentralHeating,0);
            }
            mqtt_CentralHeating=CentralHeating;
          }
          if (HotWater!=mqtt_HotWater or modulation!=mqtt_modulation){ // Update switch when on/off or modulation changed
            if (HotWater) {
              UpdateMQTTDimmer(HotWaterActive_Name,HotWater,modulation);
            } else {
              UpdateMQTTDimmer(HotWaterActive_Name,HotWater,0);
            }
            mqtt_HotWater=HotWater;
          }
          if (modulation!=mqtt_modulation){ // value changed
            UpdateMQTTPercentageSensor(Modulation_Name,modulation);
            mqtt_modulation=modulation; // remember mqtt modulation value
          }
        }
        // Execute the next command in the next call
        OpenThermCommand = SetBoilerTemp;
      } else if (responseStatus == OpenThermResponseStatus::NONE) {
          Serial.println("Opentherm Error: OpenTherm is not initialized");
      } else if (responseStatus == OpenThermResponseStatus::INVALID) {
          Serial.println("Opentherm Error: Invalid response " + String(response, HEX));
      } else if (responseStatus == OpenThermResponseStatus::TIMEOUT) {
          Serial.println("Opentherm Error: Response timeout");
      }  else {
          Serial.println("Opentherm Error: unknown error");
      }
      break;
    }
    
    case SetBoilerTemp:
    {
      // set setpoint
      ot.setBoilerTemperature(boiler_SetPoint);

      // check if we have to send MQTT message
      if (MQTT.connected()) {
        if ((boiler_SetPoint-mqtt_boiler_setpoint)>=0.1 or (boiler_SetPoint-mqtt_boiler_setpoint)<=-0.1){ // value changed
          UpdateMQTTSetpoint(Boiler_Setpoint_Name,boiler_SetPoint);
          mqtt_boiler_setpoint=boiler_SetPoint;
        }
      }

      OpenThermCommand = SetDHWTemp;
      break;
    }

    case SetDHWTemp:
    {
      ot.setDHWSetpoint(dhw_SetPoint);

      // check if we have to send MQTT message
      if (MQTT.connected()) {
        if ((dhw_SetPoint-mqtt_dhw_setpoint)>=0.1 or (dhw_SetPoint-mqtt_dhw_setpoint)<=-0.1){ // value changed
          UpdateMQTTSetpoint(DHW_Setpoint_Name,dhw_SetPoint);
          mqtt_dhw_setpoint=dhw_SetPoint;
        }
      }

      OpenThermCommand = GetBoilerTemp;
      break;
    }

    case GetBoilerTemp:
    {
      boiler_Temperature = ot.getBoilerTemperature();

      // Check if we have to send to MQTT
      if (MQTT.connected()) {
        float delta = mqtt_boiler_Temperature-boiler_Temperature;
        if (delta<-0.1 or delta>0.1){ // value changed
          UpdateMQTTTemperatureSensor(Boiler_Temperature_Name,boiler_Temperature);
          mqtt_boiler_Temperature=boiler_Temperature;
        }
      }

      OpenThermCommand = GetDHWTemp;
      break;
    }

    case GetDHWTemp:
    {
      dhw_Temperature = ot.getDHWTemperature();

      // Check if we have to send to MQTT
      if (MQTT.connected()) {
        float delta = mqtt_dhw_Temperature-dhw_Temperature;
        if (delta<-0.1 or delta>0.1){ // value changed
          UpdateMQTTTemperatureSensor(DHW_Temperature_Name,dhw_Temperature);
          mqtt_dhw_Temperature=dhw_Temperature;
        }
      }

      OpenThermCommand = GetReturnTemp;
      break;
    }
      
    case GetReturnTemp:
    {
      return_Temperature = ot.getReturnTemperature();

      // Check if we have to send to MQTT
      if (MQTT.connected()) {
        float delta = mqtt_return_Temperature-return_Temperature;
        if (delta<-0.1 or delta>0.1){ // value changed
          UpdateMQTTTemperatureSensor(Return_Temperature_Name,return_Temperature);
          mqtt_return_Temperature=return_Temperature;
        }
      }

      OpenThermCommand = GetOutsideTemp;
      break;
    }
      
    case GetOutsideTemp:
    {
      outside_Temperature = getOutsideTemperature();
      
      // Check if we have to send to MQTT
      if (MQTT.connected()) {
        float delta = mqtt_outside_Temperature-outside_Temperature;
        if (delta<-0.1 or delta>0.1){ // value changed
          UpdateMQTTTemperatureSensor(Outside_Temperature_Name,outside_Temperature);
          mqtt_outside_Temperature=outside_Temperature;
        }
      }

      OpenThermCommand = GetPressure;
      break;
    }
      
    case GetPressure: 
    {
      pressure = ot.getPressure();

      // Check if we have to send to MQTT
      if (MQTT.connected()) {
        float delta = mqtt_pressure-pressure;
        if (delta<-0.01 or delta>0.01){ // value changed
          UpdateMQTTPressureSensor(Pressure_Name,pressure);
          mqtt_pressure=pressure;
        }
      }

      OpenThermCommand = GetFlowRate;
      break;
    }

    case GetFlowRate: 
    {
      flowrate = getDHWFlowrate();

      /* Check if we have to send to MQTT
      if (MQTT.connected()) {
        float delta = mqtt_flowrate-flowrate;
        if (delta<-0.01 or delta>0.01){ // value changed
          UpdateMQTTPressureSensor(Pressure_Name,pressure);
          mqtt_pressure=pressure;
        }
      } */

      OpenThermCommand = GetFaultCode;
      break;
    }
 
    case GetFaultCode:
    {
      FaultCode = ot.getFault();

      // Check if we have to send to MQTT
      if (MQTT.connected()) {
        if (FaultCode!=mqtt_FaultCode){ // value changed
          UpdateMQTTFaultCodeSensor(FaultCode_Name,FaultCode);
          mqtt_FaultCode=FaultCode;
        }
      }

      
      OpenThermCommand=GetThermostatTemp;
      break;
    }

    case GetThermostatTemp:
    {
      sensors.requestTemperatures(); // Send the command to get temperature readings 
      currentTemperature = sensors.getTempCByIndex(0);

      // Check if we have to send to MQTT
      if (MQTT.connected()) {
        float delta = mqtt_currentTemperature-currentTemperature;
        if (delta<-0.1 or delta>0.1){ // value changed
          UpdateMQTTTemperatureSensor(Thermostat_Temperature_Name,currentTemperature);
          mqtt_currentTemperature=currentTemperature;
        }
      }

      OpenThermCommand=SetBoilerStatus;
      
      break;
    }

  }
}

String CommandTopic(const char* DeviceName){
  return String(host)+String("/light/")+String(DeviceName)+String("/set");
}

String SetpointCommandTopic(const char* DeviceName){
  return String(host)+"/climate/"+String(DeviceName)+"/cmd_temp";
}

void MQTTcallback(char* topic, byte* payload, unsigned int length) {
  // we received a mqtt callback, so someone is comunicating
  t_last_mqtt_command=millis();

  if (millis()-t_last_http_command>HTTPTimeoutInMillis) { // only execute mqtt commands if not commanded by http
    // get vars from callback
    String topicstr=String(topic);
    char payloadstr[256];
    strncpy(payloadstr,(char *)payload,length);
    payloadstr[length]='\0';
  
    // decode payload
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, payloadstr);
  
    if (error) {
      MQTT.publish("log/topic",topicstr.c_str());
      MQTT.publish("log/payload",payloadstr);
      MQTT.publish("log/length",String(length).c_str());
      MQTT.publish("log/error","Deserialisation failed");
    } else {
      if (topicstr.equals(CommandTopic(EnableHotWater_Name))) {
        // we have a match
        if (String(doc["state"]).equals("ON")){
          enableHotWater=true;
        } else if (String(doc["state"]).equals("OFF")) {
          enableHotWater=false;
        }
      } else if (topicstr.equals(CommandTopic(EnableCooling_Name))) {
        // we have a match
        if (String(doc["state"]).equals("ON")){
          enableCooling=true;
        } else if (String(doc["state"]).equals("OFF")) {
          enableCooling=false;
        } 
      } else if (topicstr.equals(CommandTopic(EnableCentralHeating_Name))) {
        // we have a match
        if (String(doc["state"]).equals("ON")){
          enableCentralHeating=true;
        } else if (String(doc["state"]).equals("OFF")) {
          enableCentralHeating=false;
        }
      } else if (topicstr.equals(SetpointCommandTopic(Boiler_Setpoint_Name))) {
        // we have a match, log what we see..
        boiler_SetPoint=String(payloadstr).toFloat();
      } else if (topicstr.equals(SetpointCommandTopic(DHW_Setpoint_Name))) {
        // we have a match, log what we see..
        dhw_SetPoint=String(payloadstr).toFloat();
      } else {
        MQTT.publish("log/topic",topicstr.c_str());
        MQTT.publish("log/payload",payloadstr);
        MQTT.publish("log/length",String(length).c_str());
        MQTT.publish("log/command","unknown topic");
      }
    }
  }

}

void reconnect()
{
  if (usemqtt) {
    if (!MQTT.connected()) {
      Serial.print("Attempting MQTT connection...");
      bool mqttconnected;
      if (usemqttauthentication) {
        mqttconnected = MQTT.connect(host.c_str(), mqttuser.c_str(), mqttpass.c_str());
      } else {
        mqttconnected = MQTT.connect(host.c_str());
      }
      if (mqttconnected) {
        Serial.println("Connect succeeded");
        PublishAllMQTTSensors();      
      } else {
        Serial.print("failed, rc=");
        Serial.print(MQTT.state());
      }
    }
  }
}

void PublishMQTTDimmer(const char* uniquename)
{
  Serial.println("UpdateMQTTDimmer");
  StaticJsonDocument<512> json;

  // Construct JSON config message
  json["name"] = uniquename;
  json["unique_id"] = host+"_"+uniquename;
  json["cmd_t"] = host+"/light/"+String(uniquename)+"/set";
  json["stat_t"] = host+"/light/"+String(uniquename)+"/state";
  json["schema"] = "json";
  json["brightness"] = true;

  JsonObject dev = json.createNestedObject("dev");
  String MAC = WiFi.macAddress();
  MAC.replace(":", "");
  dev["ids"] = MAC;
  dev["name"] = host;
  dev["sw"] = String(host)+"_"+String(__DATE__)+"_"+String(__TIME__);
  dev["mdl"] = "d1_mini";
  dev["mf"] = "espressif";

  char conf[512];
  serializeJson(json, conf);  // conf now contains the json

  // Publish config message
  MQTT.publish((String(mqttautodiscoverytopic)+"/light/"+host+"/"+String(uniquename)+"/config").c_str(),conf,mqttpersistence);

}

void UpdateMQTTDimmer(const char* uniquename, bool Value, float Mod)
{
  Serial.println("UpdateMQTTDimmer");
  StaticJsonDocument<512> json;

  // Construct JSON config message
  json["state"]=Value ? "ON" : "OFF";
  if (Value and Mod==0) { // Workaround for homekit not being able to show dimmer with value on and brightness 0
    json["brightness"]=3;     
  } else {
    json["brightness"]=int(Mod*255/100);
  }
  char state[128];
  serializeJson(json, state);  // state now contains the json

  // publish state message
  MQTT.publish((host+"/light/"+String(uniquename)+"/state").c_str(),state,mqttpersistence);
}

void PublishMQTTSwitch(const char* uniquename, bool controllable)
{
  Serial.println("PublishMQTTSwitch");
  StaticJsonDocument<512> json;

  // Construct JSON config message
  json["name"] = uniquename;
  json["unique_id"] = host+"_"+uniquename;
  json["cmd_t"] = host+"/light/"+String(uniquename)+"/set";
  json["stat_t"] = host+"/light/"+String(uniquename)+"/state";

  JsonObject dev = json.createNestedObject("dev");
  String MAC = WiFi.macAddress();
  MAC.replace(":", "");
  dev["ids"] = MAC;
  dev["name"] = host;
  dev["sw"] = String(host)+"_"+String(__DATE__)+"_"+String(__TIME__);
  dev["mdl"] = "d1_mini";
  dev["mf"] = "espressif";


  char conf[512];
  serializeJson(json, conf);  // conf now contains the json

  // Publish config message
  MQTT.publish((String(mqttautodiscoverytopic)+"/light/"+host+"/"+String(uniquename)+"/config").c_str(),conf,mqttpersistence);

  // subscribe if need to listen to commands
  if (controllable) {
    MQTT.subscribe((host+"/light/"+String(uniquename)+"/set").c_str());
  }
}

void UpdateMQTTSwitch(const char* uniquename, bool Value)
{
  Serial.println("UpdateMQTTSwitch");
  // publish state message
  MQTT.publish((host+"/light/"+String(uniquename)+"/state").c_str(),Value?"ON":"OFF",mqttpersistence);
}

void PublishMQTTTemperatureSensor(const char* uniquename)
{
  Serial.println("PublishMQTTTemperatureSensor");
  StaticJsonDocument<768> json;

  // Create message
  char conf[768];
  json["value_template"] =  "{{ value_json.value }}";
  json["device_class"] = "temperature";
  json["unit_of_measurement"] = "°C";
  json["state_topic"] = host+"/sensor/"+String(uniquename)+"/state";
  json["json_attributes_topic"] = host+"/sensor/"+String(uniquename)+"/state";
  json["name"] = uniquename;
  json["unique_id"] = host+"_"+uniquename;

  JsonObject dev = json.createNestedObject("dev");
  String MAC = WiFi.macAddress();
  MAC.replace(":", "");
  dev["ids"] = MAC;
  dev["name"] = host;
  dev["sw"] = String(host)+"_"+String(__DATE__)+"_"+String(__TIME__);
  dev["mdl"] = "d1_mini";
  dev["mf"] = "espressif";

  serializeJson(json, conf);  // buf now contains the json 

  // publsh the Message
  MQTT.publish((String(mqttautodiscoverytopic)+"/sensor/"+host+"/"+String(uniquename)+"/config").c_str(),conf,mqttpersistence);
}

void UpdateMQTTTemperatureSensor(const char* uniquename, float temperature)
{
  Serial.println("UpdateMQTTTemperatureSensor");
  StaticJsonDocument<128> json;

  // Create message
  char state[128];
  json["value"] =  temperature;
  serializeJson(json, state);  // buf now contains the json 
  // char charVal[10];
  // dtostrf(temperature,4,1,charVal); 
  MQTT.publish((host+"/sensor/"+String(uniquename)+"/state").c_str(),state,mqttpersistence);
}

void PublishMQTTPressureSensor(const char* uniquename)
{
  Serial.println("PublishMQTTPressureSensor");
  StaticJsonDocument<512> json;

  // Create message
  char conf[512];
  json["value_template"] =  "{{ value_json.value }}";
  json["device_class"] = "pressure";
  json["unit_of_measurement"] = "bar";
  json["state_topic"] = host+"/sensor/"+String(uniquename)+"/state";
  json["json_attributes_topic"] = host+"/sensor/"+String(uniquename)+"/state";
  json["name"] = uniquename;
  json["unique_id"] = host+"_"+uniquename;

  JsonObject dev = json.createNestedObject("dev");
  String MAC = WiFi.macAddress();
  MAC.replace(":", "");
  dev["ids"] = MAC;
  dev["name"] = host;
  dev["sw"] = String(host)+"_"+String(__DATE__)+"_"+String(__TIME__);
  dev["mdl"] = "d1_mini";
  dev["mf"] = "espressif";

  serializeJson(json, conf);  // buf now contains the json 

  // publsh the Message
  MQTT.publish((String(mqttautodiscoverytopic)+"/sensor/"+host+"/"+String(uniquename)+"/config").c_str(),conf,mqttpersistence);
}

void UpdateMQTTPressureSensor(const char* uniquename, float pressure)
{
  Serial.println("UpdateMQTTPressureSensor");
  // Create message
  char state[128];
  StaticJsonDocument<128> json;
  json["value"] =  pressure;
  serializeJson(json, state);  // buf now contains the json 
  // char charVal[10];
  // dtostrf(pressure,4,1,charVal); 
  MQTT.publish((host+"/sensor/"+String(uniquename)+"/state").c_str(),state,mqttpersistence);
}


void PublishMQTTPercentageSensor(const char* uniquename)
{
  Serial.println("PublishMQTTPercentageSensor");
  StaticJsonDocument<512> json;

  // Create message
  char conf[512];
  json["value_template"] =  "{{ value_json.value }}";
  json["device_class"] = "power_factor";
  json["unit_of_measurement"] = "%";
  json["state_topic"] = host+"/sensor/"+String(uniquename)+"/state";
  json["json_attributes_topic"] = host+"/sensor/"+String(uniquename)+"/state";
  json["name"] = uniquename;
  json["unique_id"] = host+"_"+uniquename;

  JsonObject dev = json.createNestedObject("dev");
  String MAC = WiFi.macAddress();
  MAC.replace(":", "");
  dev["ids"] = MAC;
  dev["name"] = host;
  dev["sw"] = String(host)+"_"+String(__DATE__)+"_"+String(__TIME__);
  dev["mdl"] = "d1_mini";
  dev["mf"] = "espressif";

  serializeJson(json, conf);  // buf now contains the json 

  // publsh the Message
  MQTT.publish((String(mqttautodiscoverytopic)+"/sensor/"+host+"/"+String(uniquename)+"/config").c_str(),conf,mqttpersistence);
}

void UpdateMQTTPercentageSensor(const char* uniquename, float percentage)
{
  Serial.println("UpdateMQTTPercentageSensor");
  // char charVal[10];
  // dtostrf(percentage,4,1,charVal); 
   StaticJsonDocument<128> json;

  // Create message
  char data[128];
  json["value"]=percentage;
  serializeJson(json,data);
 
  MQTT.publish((host+"/sensor/"+String(uniquename)+"/state").c_str(),data,mqttpersistence);
}

void PublishMQTTFaultCodeSensor(const char* uniquename)
{
  Serial.println("PublishMQTTFaultCodeSensor");
  StaticJsonDocument<512> json;

  // Create message
  char conf[512];
  json["value_template"] =  "{{ value_json.value }}";
  // json["device_class"] = "None";
  json["unit_of_measurement"] = "";
  json["state_topic"] = host+"/sensor/"+String(uniquename)+"/state";
  json["json_attributes_topic"] = host+"/sensor/"+String(uniquename)+"/state";
  json["name"] = uniquename;
  json["unique_id"] = host+"_"+uniquename;

  JsonObject dev = json.createNestedObject("dev");
  String MAC = WiFi.macAddress();
  MAC.replace(":", "");
  dev["ids"] = MAC;
  dev["name"] = host;
  dev["sw"] = String(host)+"_"+String(__DATE__)+"_"+String(__TIME__);
  dev["mdl"] = "d1_mini";
  dev["mf"] = "espressif";

  serializeJson(json, conf);  // buf now contains the json 

  // publsh the Message
  MQTT.publish((String(mqttautodiscoverytopic)+"/sensor/"+host+"/"+String(uniquename)+"/config").c_str(),conf,mqttpersistence);
}

void UpdateMQTTFaultCodeSensor(const char* uniquename, unsigned char FaultCode)
{
  Serial.println("UpdateMQTTFaultCodeSensor");
    // Create message
  char state[128];
  StaticJsonDocument<128> json;
  json["value"] =  FaultCode;
  serializeJson(json, state);  // buf now contains the json 

  MQTT.publish((host+"/sensor/"+String(uniquename)+"/state").c_str(),state,mqttpersistence);
}



void PublishMQTTSetpoint(const char* uniquename)
{
  Serial.println("PublishMQTTSetpoint");
  StaticJsonDocument<512> json;

  // Create message
  char conf[512];
  json["name"] = uniquename;
  json["unique_id"] = host+"_"+uniquename;
  json["temp_cmd_t"] = host+"/climate/"+String(uniquename)+"/cmd_temp";
  json["temp_stat_t"] = host+"/climate/"+String(uniquename)+"/state";
  json["temp_stat_tpl"] = "{{value_json.seltemp}}";
  serializeJson(json, conf);  // buf now contains the json 

  // publsh the Message
  MQTT.publish((String(mqttautodiscoverytopic)+"/climate/"+host+"/"+String(uniquename)+"/config").c_str(),conf,mqttpersistence);
  MQTT.subscribe((host+"/climate/"+String(uniquename)+"/cmd_temp").c_str());
}

void UpdateMQTTSetpoint(const char* uniquename, float temperature)
{
  Serial.println("UpdateMQTTSetpoint");
  // char charVal[10];
  // dtostrf(temperature,4,1,charVal);
  StaticJsonDocument<128> json;

  // Construct JSON config message
  json["seltemp"] = temperature;

  char value[128];
  serializeJson(json, value);  // conf now contains the json

  MQTT.publish((host+"/climate/"+String(uniquename)+"/state").c_str(),value,mqttpersistence);

  // MQTT.publish((host+"/climate/"+String(uniquename)+"/state").c_str(),charVal,mqttpersistence);
}




void PublishAllMQTTSensors()
{
  Serial.println("PublishAllMQTTSensors()");
  // Sensors
  PublishMQTTTemperatureSensor(Boiler_Temperature_Name);
  PublishMQTTTemperatureSensor(DHW_Temperature_Name);
  PublishMQTTTemperatureSensor(Return_Temperature_Name);
  PublishMQTTTemperatureSensor(Thermostat_Temperature_Name);
  PublishMQTTTemperatureSensor(Outside_Temperature_Name);
  PublishMQTTPressureSensor(Pressure_Name);
  PublishMQTTPercentageSensor(Modulation_Name);
  PublishMQTTFaultCodeSensor(FaultCode_Name);

  // On/off sensors telling state
  PublishMQTTSwitch(FlameActive_Name,false);
  PublishMQTTSwitch(FaultActive_Name,false);
  PublishMQTTSwitch(DiagnosticActive_Name,false);
  PublishMQTTSwitch(CoolingActive_Name,false);
  PublishMQTTDimmer(CentralHeatingActive_Name);
  PublishMQTTDimmer(HotWaterActive_Name);
  t_last_mqtt_discovery=millis();

  // Switches to control the boiler
  PublishMQTTSwitch(EnableCentralHeating_Name,true);
  PublishMQTTSwitch(EnableCooling_Name,true);
  PublishMQTTSwitch(EnableHotWater_Name,true);

  // Publish setpoints
  PublishMQTTSetpoint(Boiler_Setpoint_Name);
  PublishMQTTSetpoint(DHW_Setpoint_Name);
}

void loop()
{
  // handle OTA
  ArduinoOTA.handle();

  // don't do anything if we are doing if OTA upgrade is in progress
  if (!OTAUpdateInProgress) {
    if (millis()-t_heartbeat>heartbeatTickInMillis) {
      //reset tick time
      t_heartbeat=millis();

      // (Re)connect MQTT
      if (!MQTT.connected()) {
        // We are no connected, so try to reconnect
        reconnect();
      } else {
        // we are connected, check if we have to resend discovery info
        if (millis()-t_last_mqtt_discovery>MQTTDiscoveryHeartbeatInMillis)
        {
          PublishAllMQTTSensors();
        }
      }
      
      if (millis()-t_last_mqtt_command>MQTTTimeoutInMillis and millis()-t_last_http_command>HTTPTimeoutInMillis) {
          // No commands given for too long a time, so do nothing
          Serial.print("."); // Just print a dot, so we can see the software in still running
          digitalWrite(LED_BUILTIN, HIGH);    // switch the LED off, to indicate we lost connection

          // Switch off Heating and Cooling since there is no one controlling it
          enableCentralHeating=false;
          enableCooling=false;
          boiler_SetPoint=10;
      } else {
          digitalWrite(LED_BUILTIN, LOW);    // switch the LED on , to indicate we have connection
      }

      // handle MDNS
      MDNS.update();
    }

    // handle openthem commands
    handleOpenTherm();

    //Handle incoming webrequests
    server.handleClient();

    // handle MQTT
    MQTT.loop();

  }
} 
