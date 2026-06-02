#include "webTask.h"
#include "sharedState.h"
#include <Arduino.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <WiFi.h>

const char *AP_SSID = "BIETUB";
const char *AP_PASSWORD = "";
const byte DNS_PORT = 53;

AsyncWebServer server(80);
DNSServer dnsServer;

class CaptiveRequestHandler : public AsyncWebHandler {
public:
  CaptiveRequestHandler() {}
  virtual ~CaptiveRequestHandler() {}

  bool canHandle(AsyncWebServerRequest *request) override {
    if (request->url() == "/" || request->url() == "/data") {
      return false;
    }
    return true;
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    request->redirect("http://192.168.4.1/");
  }
};

void webTask(void *pvParameters) {

  // 1. Mount LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("[WebTask] Error mounting LittleFS!");
  } else {
    Serial.println("[WebTask] LittleFS mounted successfully.");
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.printf("\n[WebTask] AP Started. IP: %s\n",
                WiFi.softAPIP().toString().c_str());

  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());

  if (MDNS.begin("beitub")) {
    Serial.println("[WebTask] mDNS started: http://beitub.local");
  }

  // 1. SPECIFIC ROUTES FIRST: Live Data API Endpoint
  // 1. Live Data API Endpoint (Updated to include debug fields)
  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *req) {
    DeviceState localState;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {
      localState = state;
      xSemaphoreGive(stateMutex);
    } else {
      req->send(500, "text/plain", "Server busy");
      return;
    }

    String json = "{";
    json += "\"worst\":\"" + String(localState.worstPollutant) + "\",";
    json += "\"aqi\":" + String(localState.customAQIFloat, 2) + ",";
    json += "\"pm1\":" + String(localState.pm1, 2) + ",";
    json += "\"pm25\":" + String(localState.pm2_5, 2) + ",";
    json += "\"pm4\":" + String(localState.pm4, 2) + ",";
    json += "\"pm10\":" + String(localState.pm10, 2) + ",";
    json += "\"co2\":" + String(localState.co2) + ",";
    json += "\"voc\":" + String(localState.voc_indx, 2) + ",";
    json += "\"nox\":" + String(localState.nox_indx, 2) + ",";
    json += "\"temp\":" + String(localState.temperature, 2) + ",";
    json += "\"hum\":" + String(localState.humidity, 2) + ",";
    json += "\"debugOverride\":" +
            String(localState.debugOverride ? "true" : "false") + ",";
    json += "\"debugAQI\":" + String(localState.debugAQIValue, 2) + ",";
    json +=
        "\"isWarmingUp\":" + String(localState.isWarmingUp ? "true" : "false") +
        ",";
    json += "\"warmupTime\":" + String(localState.warmupSeconds);
    json += "}";

    req->send(200, "application/json", json);
  });

  // 2. NEW ROUTE: Receive slider configuration adjustments
  // Example call: /config?override=true&aqi=7.5
  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *req) {
    bool nextOverride = false;
    float nextAqi = 1.0f;

    if (req->hasParam("override")) {
      nextOverride = (req->getParam("override")->value() == "true");
    }
    if (req->hasParam("aqi")) {
      nextAqi = req->getParam("aqi")->value().toFloat();
    }

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {
      state.debugOverride = nextOverride;
      state.debugAQIValue = nextAqi;
      xSemaphoreGive(stateMutex);
      req->send(200, "text/plain", "Config updated");
    } else {
      req->send(500, "text/plain", "Bridge busy");
    }
  });

  // 3. CATCH-ALL ROUTE: Serve static dashboard files from LittleFS
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // 3. Captive Portal & 404 Handlers remain at the very bottom
  server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);

  server.onNotFound([](AsyncWebServerRequest *req) {
    req->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("[WebTask] Web server running!");

  for (;;) {
    dnsServer.processNextRequest();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}