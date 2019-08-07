#include "utils.h"
#include "publishable.h"
#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <PubSubClient.h>

PowerSupply psu(Serial2);
const uint8_t VMEASURE_PIN = 36;
float inVolt_ = 0, wh_ = 0;
double setpoint_ = 0, pgain_ = 0.1;
int collapses_ = 0; //collapses, reset every.. minute?
int printPeriod_ = 1000, pubPeriod_ = 12000; //TODO configurable other periods
float vadjust_ = 105.0;
bool autoStart_ = true;
String wifiap, wifipass;
String mqttServ, mqttUser, mqttPass, mqttFeed;

WebServer server(80);
WiFiClient espClient;
PubSubClient psClient(espClient);
Publishable pub;

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(10); //very fast, need to keep the ctrl loop running
  delay(100);
  uint64_t chipid = ESP.getEfuseMac();
  Serial.printf("startup, ID %08X %04X\n", chipid, (uint16_t)(chipid >> 32));
  Serial2.begin(4800, SERIAL_8N1, 16, 17, false, 1000);
  analogSetCycles(32);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
    Serial.println("wifi event");
  });
  pub.add("wifiap",     wifiap).hide().pref();
  pub.add("wifipass", wifipass).hide().pref();
  pub.add("mqttServ", mqttServ).hide().pref();
  pub.add("mqttUser", mqttUser).hide().pref();
  pub.add("mqttPass", mqttPass).hide().pref();
  pub.add("mqttFeed", mqttFeed).hide().pref();
  pub.add("outputEN",[](String s){ if (s.length()) psu.enableOutput(s == "on"); return String(psu.outEn_); });
  pub.add("outvolt", [](String s){ if (s.length()) psu.setVoltage(s.toFloat()); return String(psu.outVolt_); });
  pub.add("outcurr", [](String s){ if (s.length()) psu.setCurrent(s.toFloat()); return String(psu.outCurr_); });
  pub.add("outpower",[](String s){ return String(psu.outVolt_ * psu.outCurr_); });
  pub.add("pgain",      pgain_          ).pref();
  pub.add("setpoint",   setpoint_       ).pref();
  pub.add("vadjust",    vadjust_        ).pref();
  pub.add("autostart",  autoStart_      ).pref();
  pub.add("printperiod",printPeriod_    ).pref();
  pub.add("pubperiod",  pubPeriod_      ).pref();
  pub.add("involt",  inVolt_);
  pub.add("wh",      wh_    ); //TODO load the old value from mqtt?
  pub.add("connect",[](String s){ pubsubConnect(); return "connected"; }).hide();
  pub.add("disconnect",[](String s){ psClient.disconnect(); WiFi.disconnect(); return "dissed"; }).hide();
  pub.add("restart",[](String s){ ESP.restart(); return ""; }).hide();
  pub.add("clear",[](String s){ pub.clearPrefs(); return "cleared"; }).hide();

  server.on("/", HTTP_ANY, []() {
    Serial.println("got req " + server.uri() + " -> " + server.hostHeader());
    String ret;
    for (int i = 0; i < server.args(); i++)
      ret += pub.handleSet(server.argName(i), server.arg(i)) + "\n";
    server.sendHeader("Connection", "close");
    if (! ret.length()) ret = pub.toJson();
    server.send(200, "application/json", ret.c_str());
  });
  pub.loadPrefs();
  // wifi & mqtt is connected by pubsubConnect below

  xTaskCreate(publishTask, "publish", 10000, NULL, 1, NULL); //fn, name, stack size, parameter, priority, handle
  Serial.println("finished setup");
}

void wifiConnect() {
  if (! WiFi.isConnected()) {
    if (wifiap.length() && wifipass.length()) {
      WiFi.begin(wifiap.c_str(), wifipass.c_str());
      uint64_t chipid = ESP.getEfuseMac();
      String hostname = str("mpptESP-%02X", chipid & 0xff);
      WiFi.setHostname(hostname.c_str());
      if (WiFi.waitForConnectResult() == WL_CONNECTED) {
        Serial.println("Wifi connected! hostname: " + hostname);
        MDNS.begin("mppt");
        MDNS.addService("http", "tcp", 80);
        pubsubConnect();
        server.begin();
      }
    } else Serial.println("no wifiap or wifipass set!");
  }
}

void pubsubConnect() {
  if (WiFi.isConnected() && !psClient.connected()) {
    if (mqttServ.length() && mqttFeed.length()) {
      Serial.println("Connecting MQTT to " + mqttUser + "@" + mqttServ);
      psClient.setServer(mqttServ.c_str(), 1883); //TODO split serv:port
      if (psClient.connect("MPPT", mqttUser.c_str(), mqttPass.c_str()))
        Serial.println("PubSub connect success! " + psClient.state());
      else Serial.println("PubSub connect ERROR! " + psClient.state());
    } else Serial.println("no MQTT user / pass / server / feed set up!");
  } else Serial.printf("can't pub connect, wifi %d pub %d\n", WiFi.isConnected(), psClient.connected());
}

uint32_t lastV = 0, lastpub = 20000, lastLog_ = 0;
uint32_t lastPSUpdate_ = 0, lastPSUadjust_ = 1000, lastCollapseReset_ = 0;
double newDesiredCurr_ = 0;
bool needsQuickAdj_ = false;
String logme;

void applyAdjustment() {
  if (newDesiredCurr_ > 0) {
    if (psu.setCurrent(newDesiredCurr_))
      logme += str("[adjusting %0.1fA (from %0.1fA)] ", newDesiredCurr_ - psu.outCurr_, psu.outCurr_);
    else Serial.println("error setting current");
    psu.readCurrent();
    if (needsQuickAdj_)
      needsQuickAdj_ = false;
    printStatus();
  }
  newDesiredCurr_ = 0;
}

void loop() {
  uint32_t now = millis();
  if ((now - lastV) >= 200) {
    int analogval = analogRead(VMEASURE_PIN);
    inVolt_ = analogval * 3.3 * (vadjust_ / 3.3) / 4096.0;
    if (setpoint_ > 0 && psu.outEn_) { //corrections enabled
      double error = inVolt_ - setpoint_;
      double dcurr = constrain(error * pgain_, -3, 1); //limit ramping speed
      if (error > 0.3 || (-error > 0.2)) { //adjustment deadband, more sensitive when needing to ramp down
        newDesiredCurr_ = psu.outCurr_ + dcurr;
        if (error < 0.6) { //ramp down, quick!
          logme += "[QUICK] ";
          needsQuickAdj_ = true;
        }
      }
    }
    lastV = millis();
  }
  if ((now - lastPSUadjust_) >= (needsQuickAdj_? 500 : 5000)) {
    if (setpoint_ > 0) {
      if (psu.outEn_)
        applyAdjustment();
      if (psu.outEn_ && inVolt_ < (setpoint_  * 3 / 4)) { //collapse detection
        newDesiredCurr_ = psu.outCurr_ / 3;
        ++collapses_;
        Serial.printf("collapsed! %0.1fV set recovery to %0.1fA\n", inVolt_ ,newDesiredCurr_);
        psu.enableOutput((psu.outEn_ = false));
        psu.setCurrent(newDesiredCurr_);
      } else if (autoStart_ && !psu.outEn_ && inVolt_ > (setpoint_ * 1.02)) {
        Serial.println("restoring from collapse");
        psu.enableOutput(true);
      }
    }
    lastPSUadjust_ = now;
  }
  if ((now - lastLog_) >= printPeriod_) {
    printStatus();
    lastLog_ = now;
  }
  if ((now - lastPSUpdate_) >= 5000) {
    bool res = psu.doUpdate();
    if (res) wh_ += psu.outVolt_ * psu.outCurr_ * (now - lastPSUpdate_) / 1000.0 / 60 / 60;
    else {
      Serial.println("psu update fail");
      psu.flush();
      //psu.debug_ = true;
    }
    lastPSUpdate_ = now;
  }
}

void publishTask(void*) {
  wifiConnect();
  while (psu.outVolt_ == 0)
    delay(1);
  Serial.println("starting publish task");
  delay(1); //wait for the full PSU poll to complete
  while (true) {
    uint32_t now = millis();
    if ((now - lastpub) >= (psu.outEn_? pubPeriod_ : pubPeriod_ * 3)) { //slow-down when not enabled
      if (psClient.connected()) {
        int wins = 0;
        auto pubs = pub.items();
        for (auto i : pubs)
          if (!i->hidden_)
            wins += psClient.publish((mqttFeed + "/" + i->key).c_str(), i->toString().c_str(), true)? 1 : 0;
        logme += str("[published %d] ", wins);
      } else {
        logme += "[pub disconnected] ";
        wifiConnect();
        pubsubConnect();
      }
      lastpub = now;
    }
    if ((now - lastCollapseReset_) >= 60000) {
      psClient.publish((mqttFeed + "/collapses").c_str(), String(collapses_).c_str(), true);
      Serial.println("published collapses: " + String(collapses_));
      collapses_ = 0;
      lastCollapseReset_ = now;
    }
    psClient.loop();
    pub.poll(&Serial);
    server.handleClient();
    delay(1);
  }
}

void printStatus() {
  Serial.println(str("%0.1fVin -> %0.2fWh <%0.1fV out %0.1fA %den> ", inVolt_, wh_, psu.outVolt_, psu.outCurr_, psu.outEn_) + logme);
  logme = "";
}
