#pragma once
#include <Arduino.h>

enum operatingMode { MODE_LEVEL, MODE_TIME_BASED };

const int MAX_LOG_ENTRIES = 60;

struct LogEntry {
  unsigned long uptimeSeconds;
  int aqi;
  int pm2_5;
  int pm10;
  int co2;
  int voc_indx;
};

struct HistoryData {
  bool isTimeSynced = false;
  unsigned long timeOffset = 0;
  LogEntry buffer[MAX_LOG_ENTRIES];
  int head = 0;
  int count = 0;
};

struct DeviceState {
  float pm1;
  float pm2_5;
  float pm4;
  float pm10;
  float co2;
  float voc_indx = 100;
  float nox_indx = 1;
  float humidity;
  float temperature;

  operatingMode currentMode = MODE_LEVEL;
  int customAQI = 1;
  float customAQIFloat = 1.0;
  char worstPollutant[20] = "None";

  bool debugOverride = false;
  float debugAQIValue = 1.0f;

  bool isWarmingUp = true;
  int warmupSeconds = 120;

  bool isTimeSynced = false;
  unsigned long timeOffset = 0;

  // --- Exposure Mode State ---
  bool exposureActive = false;
  bool exposureResetRequested = false;
  float timeRemainingMinutes = 0.0f;
  float exposurePercent = 0.0f;
  char limitingPollutant[20] = "None";

  float dosePM1 = 0.0f;
  float dosePM25 = 0.0f;
  float dosePM4 = 0.0f;
  float dosePM10 = 0.0f;
  float doseCO2 = 0.0f;
  float doseVOC = 0.0f;
  float doseNOx = 0.0f;

  unsigned long exposureStartTime = 0;
  unsigned long totalExposureSeconds = 0;
};

extern DeviceState state;
extern HistoryData historyLogs;
extern SemaphoreHandle_t stateMutex;