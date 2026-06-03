#include "exposureTask.h"
#include "sharedState.h"
#include <Arduino.h>
#include <cfloat>

// =============================================================================
// TWA DOSE LIMITS — Tweak these based on your research
// =============================================================================
// Format: { "Name", dose ceiling in (unit·hours) }
// Dose ceiling = recommended TWA concentration × 8-hour reference window.
// e.g., PM2.5: 35 µg/m³ × 8hr = 280 µg·hr

struct ExposureLimitDef {
  const char *name;
  float doseCeiling; // unit·hours (µg·hr, ppm·hr, or index·hr)
};

const ExposureLimitDef LIMITS[] = {
    {"PM1", 80.0f},     // 10 µg/m³ × 8hr
    {"PM2.5", 280.0f},  // 35 µg/m³ × 8hr
    {"PM4", 400.0f},    // 50 µg/m³ × 8hr
    {"PM10", 1200.0f},  // 150 µg/m³ × 8hr
    {"CO2", 40000.0f},  // 5000 ppm × 8hr
    {"VOC", 2000.0f},   // 250 index × 8hr
    {"NOx", 1200.0f},   // 150 index × 8hr
};

const size_t NUM_POLLUTANTS = sizeof(LIMITS) / sizeof(ExposureLimitDef);

// Sampling interval for dose integration
const unsigned long EXPOSURE_SAMPLE_MS = 10000; // 10 seconds
const float DT_HOURS = EXPOSURE_SAMPLE_MS / 3600000.0f; // 10s in hours

void resetDoseAccumulators(DeviceState &s) {
  s.dosePM1 = 0.0f;
  s.dosePM25 = 0.0f;
  s.dosePM4 = 0.0f;
  s.dosePM10 = 0.0f;
  s.doseCO2 = 0.0f;
  s.doseVOC = 0.0f;
  s.doseNOx = 0.0f;

  s.exposurePercent = 0.0f;
  s.timeRemainingMinutes = 0.0f;
  s.totalExposureSeconds = 0;
  s.exposureStartTime = millis();
  strcpy(s.limitingPollutant, "None");

  Serial.println("[Exposure] Session reset — all dose accumulators zeroed.");
}

void exposureTask(void *pvParameters) {

  bool wasActive = false;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(EXPOSURE_SAMPLE_MS));

    DeviceState localState;

    if (!xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {
      continue;
    }

    // Skip if not in time-based mode or still warming up
    if (state.currentMode != MODE_TIME_BASED || state.isWarmingUp) {
      wasActive = false;
      xSemaphoreGive(stateMutex);
      continue;
    }

    // Handle session start (mode just switched to time-based)
    if (!wasActive || state.exposureResetRequested) {
      resetDoseAccumulators(state);
      state.exposureActive = true;
      state.exposureResetRequested = false;
      wasActive = true;
      xSemaphoreGive(stateMutex);
      continue; // Skip this tick — start clean on the next one
    }

    localState = state;
    xSemaphoreGive(stateMutex);

    // =========================================================================
    // DOSE ACCUMULATION: dose += concentration × dt
    // =========================================================================
    localState.dosePM1 += localState.pm1 * DT_HOURS;
    localState.dosePM25 += localState.pm2_5 * DT_HOURS;
    localState.dosePM4 += localState.pm4 * DT_HOURS;
    localState.dosePM10 += localState.pm10 * DT_HOURS;
    localState.doseCO2 += localState.co2 * DT_HOURS;
    localState.doseVOC += localState.voc_indx * DT_HOURS;
    localState.doseNOx += localState.nox_indx * DT_HOURS;

    // =========================================================================
    // TIME REMAINING CALCULATION
    // =========================================================================
    // For each pollutant: remaining_time = (ceiling - accumulated) / current_rate
    // Overall time = minimum across all pollutants

    float doses[] = {localState.dosePM1,  localState.dosePM25,
                     localState.dosePM4,  localState.dosePM10,
                     localState.doseCO2,  localState.doseVOC,
                     localState.doseNOx};

    float concentrations[] = {localState.pm1,      localState.pm2_5,
                              localState.pm4,      localState.pm10,
                              localState.co2,      localState.voc_indx,
                              localState.nox_indx};

    float minTimeRemainingHours = FLT_MAX;
    float maxExposurePercent = 0.0f;
    const char *limiting = "None";

    for (size_t i = 0; i < NUM_POLLUTANTS; i++) {
      float remainingDose = LIMITS[i].doseCeiling - doses[i];

      // Exposure percentage for this pollutant
      float pct = (doses[i] / LIMITS[i].doseCeiling) * 100.0f;
      if (pct > maxExposurePercent) {
        maxExposurePercent = pct;
      }

      // Time remaining at current concentration
      if (concentrations[i] > 0.01f) {
        float timeHours = remainingDose / concentrations[i];
        if (timeHours < 0.0f)
          timeHours = 0.0f;

        if (timeHours < minTimeRemainingHours) {
          minTimeRemainingHours = timeHours;
          limiting = LIMITS[i].name;
        }
      }
    }

    // Clamp values
    if (maxExposurePercent > 100.0f)
      maxExposurePercent = 100.0f;

    float timeRemainingMin = (minTimeRemainingHours == FLT_MAX)
                                 ? 999.0f
                                 : minTimeRemainingHours * 60.0f;
    if (timeRemainingMin < 0.0f)
      timeRemainingMin = 0.0f;

    unsigned long sessionSeconds =
        (millis() - localState.exposureStartTime) / 1000;

    // =========================================================================
    // WRITE RESULTS BACK TO SHARED STATE
    // =========================================================================
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {
      state.dosePM1 = localState.dosePM1;
      state.dosePM25 = localState.dosePM25;
      state.dosePM4 = localState.dosePM4;
      state.dosePM10 = localState.dosePM10;
      state.doseCO2 = localState.doseCO2;
      state.doseVOC = localState.doseVOC;
      state.doseNOx = localState.doseNOx;

      state.exposurePercent = maxExposurePercent;
      state.timeRemainingMinutes = timeRemainingMin;
      state.totalExposureSeconds = sessionSeconds;
      strcpy(state.limitingPollutant, limiting);

      xSemaphoreGive(stateMutex);
    }

    // Debug logging
    Serial.printf("[Exposure] Session: %lus | Exposure: %.1f%% | "
                  "Time Left: %.1fmin | Limiting: %s\n",
                  sessionSeconds, maxExposurePercent, timeRemainingMin,
                  limiting);

    Serial.printf("[Exposure]   Doses -> PM1:%.2f PM2.5:%.2f PM4:%.2f "
                  "PM10:%.2f CO2:%.2f VOC:%.2f NOx:%.2f\n",
                  localState.dosePM1, localState.dosePM25, localState.dosePM4,
                  localState.dosePM10, localState.doseCO2, localState.doseVOC,
                  localState.doseNOx);
  }
}
