#include "exposureTask.h"
#include "sharedState.h"
#include <Arduino.h>
#include <cfloat>

// =============================================================================
// TWA DOSE LIMITS (For Raw Physical Concentrations Only)
// =============================================================================
// Format: { "Name", dose ceiling in (unit·hours) }
// VOC and NOx are removed from here and handled via the Harm Factor model.
struct ExposureLimitDef {
  const char *name;
  float doseCeiling;
};

const ExposureLimitDef LIMITS[] = {
    {"PM1", 80.0f},    // 10 µg/m³ × 8hr
    {"PM2.5", 120.0f}, // 15 µg/m³ × 8hr
    {"PM4", 280.0f},   // 35 µg/m³ × 8hr
    {"PM10", 360.0f},  // 45 µg/m³ × 8hr
    {"CO2", 32000.0f}, // 4000 ppm × 8hr
};

const size_t NUM_RAW_POLLUTANTS = sizeof(LIMITS) / sizeof(ExposureLimitDef);

// =============================================================================
// HARM-FACTOR FUNCTIONS (VOC Index & NOx Index → Normalized dose rate)
// =============================================================================
// Both share the same ceiling: 8 harm·hours
// (= 8 hours at harm factor 1.0, or 2 hours at harm factor 4.0, etc.)
static constexpr float HARM_CEILING = 8.0f;

static float piecewiseLerp(float x, const float xs[], const float ys[], int n) {
  if (x <= xs[0])
    return ys[0];
  if (x >= xs[n - 1])
    return ys[n - 1];
  for (int i = 0; i < n - 1; i++) {
    if (x >= xs[i] && x < xs[i + 1]) {
      float t = (x - xs[i]) / (xs[i + 1] - xs[i]);
      return ys[i] + t * (ys[i + 1] - ys[i]);
    }
  }
  return ys[n - 1];
}

// VOC Index breakpoints
float vocHarmFactor(float idx) {
  static const float xs[] = {0, 100, 150, 200, 300, 400, 500};
  static const float ys[] = {0.0, 0.0, 1.0, 4.0, 16.0, 32.0, 64.0};
  return piecewiseLerp(idx, xs, ys, 7);
}

// NOx Index breakpoints
float noxHarmFactor(float idx) {
  static const float xs[] = {1, 20, 50, 100, 200, 300, 500};
  static const float ys[] = {0.0, 0.0, 1.0, 4.0, 16.0, 32.0, 64.0};
  return piecewiseLerp(idx, xs, ys, 7);
}

// =============================================================================
// CORE TASK ARCHITECTURE
// =============================================================================
// Sampling interval for dose integration
const unsigned long EXPOSURE_SAMPLE_MS = 10000;         // 10 seconds
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
  float sumPM1 = 0, sumPM25 = 0, sumPM4 = 0, sumPM10 = 0, sumCO2 = 0,
        sumVOC = 0, sumNOx = 0;
  int sampleCount = 0;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000)); // Sample every 1 second

    DeviceState localState;

    if (!xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {
      continue;
    }

    // Skip if not in time-based mode or still warming up
    if (state.currentMode != MODE_TIME_BASED || state.isWarmingUp) {
      wasActive = false;
      xSemaphoreGive(stateMutex);
      sumPM1 = sumPM25 = sumPM4 = sumPM10 = sumCO2 = sumVOC = sumNOx = 0.0f;
      sampleCount = 0;
      continue;
    }

    // Handle session start (mode just switched to time-based)
    if (!wasActive || state.exposureResetRequested) {
      resetDoseAccumulators(state);
      state.exposureActive = true;
      state.exposureResetRequested = false;
      wasActive = true;
      xSemaphoreGive(stateMutex);
      sumPM1 = sumPM25 = sumPM4 = sumPM10 = sumCO2 = sumVOC = sumNOx = 0.0f;
      sampleCount = 0;
      continue;
    }

    localState = state;
    xSemaphoreGive(stateMutex);

    // Accumulate 1-second samples
    sumPM1 += localState.pm1;
    sumPM25 += localState.pm2_5;
    sumPM4 += localState.pm4;
    sumPM10 += localState.pm10;
    sumCO2 += localState.co2;
    sumVOC += localState.voc_indx;
    sumNOx += localState.nox_indx;
    sampleCount++;

    // Only process dose and remaining time every 10 seconds
    if (sampleCount < 10) {
      continue;
    }

    float avgPM1 = sumPM1 / 10.0f;
    float avgPM25 = sumPM25 / 10.0f;
    float avgPM4 = sumPM4 / 10.0f;
    float avgPM10 = sumPM10 / 10.0f;
    float avgCO2 = sumCO2 / 10.0f;
    float avgVOC = sumVOC / 10.0f;
    float avgNOx = sumNOx / 10.0f;

    sumPM1 = sumPM25 = sumPM4 = sumPM10 = sumCO2 = sumVOC = sumNOx = 0.0f;
    sampleCount = 0;

    // =========================================================================
    // DOSE ACCUMULATION (Linear for PM/CO2, Harm Factor for VOC/NOx)
    // =========================================================================
    localState.dosePM1 += avgPM1 * DT_HOURS;
    localState.dosePM25 += avgPM25 * DT_HOURS;
    localState.dosePM4 += avgPM4 * DT_HOURS;
    localState.dosePM10 += avgPM10 * DT_HOURS;
    localState.doseCO2 += avgCO2 * DT_HOURS;

    // Calculate harm factors from the raw indices
    float harmVOC = vocHarmFactor(avgVOC);
    float harmNOx = noxHarmFactor(avgNOx);

    // Accumulate unitless "harm hours"
    localState.doseVOC += harmVOC * DT_HOURS;
    localState.doseNOx += harmNOx * DT_HOURS;

    // =========================================================================
    // TIME REMAINING & PERCENTAGE CALCULATION (SPLIT APPROACH)
    // =========================================================================
    float minTimeRemainingHours = FLT_MAX;
    float maxExposurePercent = 0.0f;
    const char *limiting = "None";

    // --- PART 1: PM and CO2 (Raw Concentration Model) ---
    struct RawPollutant {
      float dose;
      float concentration;
      float ceiling;
      const char *name;
    };
    RawPollutant rawPollutants[] = {
        {localState.dosePM1, avgPM1, LIMITS[0].doseCeiling, LIMITS[0].name},
        {localState.dosePM25, avgPM25, LIMITS[1].doseCeiling, LIMITS[1].name},
        {localState.dosePM4, avgPM4, LIMITS[2].doseCeiling, LIMITS[2].name},
        {localState.dosePM10, avgPM10, LIMITS[3].doseCeiling, LIMITS[3].name},
        {localState.doseCO2, avgCO2, LIMITS[4].doseCeiling, LIMITS[4].name},
    };

    for (auto &p : rawPollutants) {
      float pct = (p.dose / p.ceiling) * 100.0f;
      if (pct > maxExposurePercent)
        maxExposurePercent = pct;

      if (p.concentration > 0.01f) {
        float t = (p.ceiling - p.dose) / p.concentration;
        if (t < 0.0f)
          t = 0.0f;
        if (t < minTimeRemainingHours) {
          minTimeRemainingHours = t;
          limiting = p.name;
        }
      }
    }

    // --- PART 2: VOC and NOx (Harm-Factor Model) ---
    struct IndexPollutant {
      float dose;
      float harmFactor;
      const char *name;
    };
    IndexPollutant indexPollutants[] = {
        {localState.doseVOC, harmVOC, "VOC"},
        {localState.doseNOx, harmNOx, "NOx"},
    };

    for (auto &p : indexPollutants) {
      // Compare against the strict 8.0 harm ceiling
      float pct = (p.dose / HARM_CEILING) * 100.0f;
      if (pct > maxExposurePercent)
        maxExposurePercent = pct;

      if (p.harmFactor > 0.01f) {
        float t = (HARM_CEILING - p.dose) / p.harmFactor;
        if (t < 0.0f)
          t = 0.0f;
        if (t < minTimeRemainingHours) {
          minTimeRemainingHours = t;
          limiting = p.name;
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

    // Note: VOC and NOx doses are now in "harm·hours", not raw index·hours!
    Serial.printf("[Exposure] Doses -> PM1:%.2f PM2.5:%.2f PM4:%.2f "
                  "PM10:%.2f CO2:%.2f VOC:%.2f NOx:%.2f\n",
                  localState.dosePM1, localState.dosePM25, localState.dosePM4,
                  localState.dosePM10, localState.doseCO2, localState.doseVOC,
                  localState.doseNOx);
  }
}