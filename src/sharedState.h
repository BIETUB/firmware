#pragma once
#include <Arduino.h>

enum operatingMode { MODE_LEVEL, MODE_TIME_BASED };

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
};

extern DeviceState state;
extern SemaphoreHandle_t stateMutex;