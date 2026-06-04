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
    if (request->url() == "/" || request->url() == "/data" ||
        request->url() == "/history" || request->url().startsWith("/api/")) {
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

  if (MDNS.begin("bietub")) {
    Serial.println("[WebTask] mDNS started: http://bietub.local");
  }

  // =========================================================================
  // /data — Live metric data (mode-aware)
  // =========================================================================
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
    json +=
        "\"mode\":\"" +
        String(localState.currentMode == MODE_LEVEL ? "level" : "timeBased") +
        "\",";
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

    // Exposure fields — only included when in time-based mode
    if (localState.currentMode == MODE_TIME_BASED) {
      json +=
          ",\"timeRemaining\":" + String(localState.timeRemainingMinutes, 1);
      json += ",\"exposurePercent\":" + String(localState.exposurePercent, 1);
      json += ",\"limitingPollutant\":\"" +
              String(localState.limitingPollutant) + "\"";
      json += ",\"sessionSeconds\":" + String(localState.totalExposureSeconds);
    }

    json += "}";

    req->send(200, "application/json", json);
  });

  // =========================================================================
  // /api/mode — Switch operating mode
  // =========================================================================
  server.on("/api/mode", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!req->hasParam("mode")) {
      req->send(400, "text/plain", "Missing 'mode' parameter");
      return;
    }

    String modeStr = req->getParam("mode")->value();

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {
      if (modeStr == "level") {
        state.currentMode = MODE_LEVEL;
        state.exposureActive = false;
        Serial.println("[WebTask] Mode switched to: LEVEL");
      } else if (modeStr == "timeBased") {
        state.currentMode = MODE_TIME_BASED;
        // exposureTask will detect the mode change and reset accumulators
        state.exposureActive = false;
        Serial.println("[WebTask] Mode switched to: TIME-BASED");
      } else {
        xSemaphoreGive(stateMutex);
        req->send(400, "text/plain",
                  "Invalid mode. Use 'level' or 'timeBased'");
        return;
      }
      xSemaphoreGive(stateMutex);
      req->send(200, "text/plain", "Mode updated");
    } else {
      req->send(500, "text/plain", "Mutex busy");
    }
  });

  // =========================================================================
  // /api/reset-exposure — Reset exposure session
  // =========================================================================
  server.on("/api/reset-exposure", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {
      state.exposureResetRequested = true;
      xSemaphoreGive(stateMutex);
      Serial.println("[WebTask] Exposure reset requested from dashboard.");
      req->send(200, "text/plain", "Exposure session reset");
    } else {
      req->send(500, "text/plain", "Mutex busy");
    }
  });

  // =========================================================================
  // /config — Debug override (unchanged)
  // =========================================================================
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

  // =========================================================================
  // /api/set-time — Time sync (unchanged)
  // =========================================================================
  server.on("/api/set-time", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (req->hasParam("epoch")) {
      unsigned long phoneTime = req->getParam("epoch")->value().toInt();

      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {
        historyLogs.timeOffset = phoneTime - (millis() / 1000);
        historyLogs.isTimeSynced = true;
        xSemaphoreGive(stateMutex);

        Serial.println(
            "[WebTask] Timeline calibrated via dashboard attachment.");
        req->send(200, "text/plain", "Time Synced");
      } else {
        req->send(500, "text/plain", "Mutex Busy");
      }
    } else {
      req->send(400, "text/plain", "Missing epoch token");
    }
  });

  // =========================================================================
  // /history — History data (unchanged)
  // =========================================================================
  server.on("/history", HTTP_GET, [](AsyncWebServerRequest *req) {
    String json = "[";

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(200))) {
      int count = historyLogs.count;
      int head = historyLogs.head;
      int startIndex = (count == MAX_LOG_ENTRIES) ? head : 0;

      for (int i = 0; i < count; i++) {
        int index = (startIndex + i) % MAX_LOG_ENTRIES;
        LogEntry entry = historyLogs.buffer[index];

        // Apply retroactive calculations for true unix timestamps
        unsigned long retroActiveTime =
            entry.uptimeSeconds + historyLogs.timeOffset;

        json += "{";
        json += "\"t\":" + String(retroActiveTime) + ",";
        json += "\"aqi\":" + String(entry.aqi) + ",";
        json += "\"pm25\":" + String(entry.pm2_5, 2) + ",";
        json += "\"pm10\":" + String(entry.pm10, 2) + ",";
        json += "\"co2\":" + String(entry.co2, 2) + ",";
        json += "\"voc\":" + String(entry.voc_indx, 2);
        json += "}";

        if (i < count - 1)
          json += ",";
      }
      xSemaphoreGive(stateMutex);
    }

    json += "]";
    req->send(200, "application/json", json);
  });

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

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