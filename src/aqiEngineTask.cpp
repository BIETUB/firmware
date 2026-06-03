#include "aqiEngineTask.h"
#include "sharedState.h"

#include <Arduino.h>
#include <algorithm>

const unsigned long WARMUP_DURATION_MS = 120000;

struct AQIPoint {
  float sensorValue;
  float aqiValue;
};

struct PollutantDefinition {
  const char *name;
  const AQIPoint *curve;
  size_t curveSize;
  float value;
};

const AQIPoint PM1_CURVE[] = {{1, 1}, {6, 2}, {10, 3}, {15, 4}, {20, 5}, {25, 6}, {30, 7}, {35, 8}, {55, 9}, {75, 10}};
const AQIPoint PM25_CURVE[] = {{1, 1}, {8, 2}, {12, 3}, {20, 4}, {27, 5}, {35, 6}, {42, 7}, {50, 8}, {75, 9}, {100, 10}};
const AQIPoint PM4_CURVE[] = {{15, 1}, {20, 2}, {35, 3}, {40, 4}, {45, 5}, {50, 6}, {55, 7}, {60, 8}, {85, 9}, {115, 10}};
const AQIPoint PM10_CURVE[] = {{15, 1}, {30, 2}, {45, 3}, {55, 4}, {63, 5}, {70, 6}, {80, 7}, {90, 8}, {115, 9}, {150, 10}};
const AQIPoint CO2_CURVE[] = {{300, 1}, {400, 2}, {800, 3}, {950, 4}, {1000, 5}, {1500, 6}, {2000, 7}, {3000, 8}, {4000, 9}, {5000, 10}};
const AQIPoint VOC_CURVE[] = {{50, 1}, {100, 2}, {125, 3}, {150, 4}, {175, 5}, {225, 6}, {250, 7}, {300, 8}, {400, 9}, {500, 10}};
const AQIPoint NOX_CURVE[] = {{0, 1}, {1, 2}, {10, 3}, {20, 4}, {50, 5}, {75, 6}, {100, 7}, {250, 8}, {350, 9}, {500, 10}};

float mapFloat(float x, float inMin, float inMax, float outMin, float outMax) {
  if (inMin == inMax)
    return outMax;

  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

float calculateIndex(float value, const AQIPoint *curve, size_t count) {
  if (count < 2)
    return 1.0f;

  if (value <= curve[0].sensorValue)
    return curve[0].aqiValue;

  for (size_t i = 1; i < count; i++) {
    if (value <= curve[i].sensorValue) {
      return mapFloat(value, curve[i - 1].sensorValue, curve[i].sensorValue,
                      curve[i - 1].aqiValue, curve[i].aqiValue);
    }
  }

  return curve[count - 1].aqiValue;
}

void aqiEngineTask(void *pvParameters) {
  for (;;) {
    unsigned long now = millis();

    DeviceState localState;

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {

      // Warmup handling
      if (now < WARMUP_DURATION_MS) {
        state.isWarmingUp = true;
        state.warmupSeconds = (WARMUP_DURATION_MS - now) / 1000;

        state.customAQIFloat = 1.0f;
        state.customAQI = 1;
        strcpy(state.worstPollutant, "Warming Up...");

        xSemaphoreGive(stateMutex);

        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }

      // Warmup just completed
      if (state.isWarmingUp) {
        Serial.println(
            "\n[Engine] *** WARMUP COMPLETE! Actuators Engaged. ***\n");

        state.isWarmingUp = false;
        state.warmupSeconds = 0;
      }

      localState = state;
      xSemaphoreGive(stateMutex);

    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // Debug override still works after warmup
    if (localState.debugOverride) {
      if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {
        state.customAQIFloat = localState.debugAQIValue;
        state.customAQI = round(localState.debugAQIValue);
        strcpy(state.worstPollutant, "DEBUG OVERRIDE");
        xSemaphoreGive(stateMutex);
      }

      Serial.printf("[Engine] OVERRIDE ACTIVE! Forcing AQI to: %.2f\n",
                    localState.debugAQIValue);

      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    // Time-based mode is handled entirely by exposureTask — skip AQI
    if (localState.currentMode == MODE_TIME_BASED) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    float finalAQIFloat = 1.0f;
    int finalAQI = 1;
    const char *worstPollutant = "None";

    PollutantDefinition pollutants[] = {
        {"PM1", PM1_CURVE, sizeof(PM1_CURVE) / sizeof(AQIPoint),
         localState.pm1},

        {"PM2.5", PM25_CURVE, sizeof(PM25_CURVE) / sizeof(AQIPoint),
         localState.pm2_5},

        {"PM4", PM4_CURVE, sizeof(PM4_CURVE) / sizeof(AQIPoint),
         localState.pm4},

        {"PM10", PM10_CURVE, sizeof(PM10_CURVE) / sizeof(AQIPoint),
         localState.pm10},

        {"CO2", CO2_CURVE, sizeof(CO2_CURVE) / sizeof(AQIPoint),
         static_cast<float>(localState.co2)},

        {"VOC", VOC_CURVE, sizeof(VOC_CURVE) / sizeof(AQIPoint),
         localState.voc_indx},

        {"NOx", NOX_CURVE, sizeof(NOX_CURVE) / sizeof(AQIPoint),
         localState.nox_indx}};

    const size_t pollutantCount =
        sizeof(pollutants) / sizeof(PollutantDefinition);

    for (size_t i = 0; i < pollutantCount; i++) {
      float score = calculateIndex(pollutants[i].value, pollutants[i].curve,
                                   pollutants[i].curveSize);

      if (score > finalAQIFloat) {
        finalAQIFloat = score;
        worstPollutant = pollutants[i].name;
      }
    }

    finalAQI = round(finalAQIFloat);
    finalAQI = constrain(finalAQI, 1, 10);

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {
      state.customAQIFloat = finalAQIFloat;
      state.customAQI = finalAQI;
      strcpy(state.worstPollutant, worstPollutant);

      xSemaphoreGive(stateMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}