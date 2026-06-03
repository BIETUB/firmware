#include "actuatorTasks.h"
#include "sharedState.h"
#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

// --- NeoPixel Configuration ---
const int NEOPIXEL_PIN = 13;
const int NUM_PIXELS = 12;

Adafruit_NeoPixel ring(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

void ledTask(void *pvParameters) {
  ring.begin();
  ring.setBrightness(40);
  ring.clear();
  ring.show();

  int lastPrintedAqi = -1;
  int lastPrintedLeds = -1;

  for (;;) {
    DeviceState localState;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50))) {
      localState = state;
      xSemaphoreGive(stateMutex);
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // --- 1. WARMUP ANIMATION: Smooth Breathing Cyan ---
    if (localState.isWarmingUp) {
      if (millis() % 2000 < 50 && lastPrintedAqi != -2) {
        Serial.printf("[LED Task] Warming Up (%ds)... Cyan Breathe\n",
                      localState.warmupSeconds);
        lastPrintedAqi = -2;
      }

      int intensity = (millis() / 5) % 512;
      if (intensity > 255)
        intensity = 511 - intensity;

      int cyanVal = map(intensity, 0, 255, 10, 150);
      ring.fill(ring.Color(0, cyanVal, cyanVal));
      ring.show();

      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }

    // =========================================================================
    // TIME-BASED MODE: Countdown Ring
    // =========================================================================
    if (localState.currentMode == MODE_TIME_BASED) {
      float pct = localState.exposurePercent;

      // LEDs count down: 12 at 0%, 0 at 100%
      int activeLEDs = 12 - round(pct / 100.0f * 12.0f);
      if (activeLEDs < 0)
        activeLEDs = 0;
      if (activeLEDs > 12)
        activeLEDs = 12;

      ring.clear();

      if (pct >= 100.0f) {
        // EXPIRED: All LEDs flash red
        if (millis() % 500 < 250) {
          ring.fill(ring.Color(255, 0, 0));
        }
      } else if (activeLEDs > 0) {
        // Color shifts based on exposure percentage
        uint32_t color;
        if (pct < 50.0f) {
          color = ring.Color(0, 255, 0); // Green — plenty of time
        } else if (pct < 75.0f) {
          color = ring.Color(200, 255, 0); // Amber — getting there
        } else if (pct < 90.0f) {
          color = ring.Color(255, 100, 0); // Orange — wrapping up
        } else {
          color = ring.Color(255, 0, 0); // Red — leave soon
        }

        for (int i = 0; i < activeLEDs; i++) {
          ring.setPixelColor(i, color);
        }
      }

      ring.show();

      if (activeLEDs != lastPrintedLeds) {
        Serial.printf("[LED Task] Exposure Mode: %.1f%% — %d/%d LEDs active\n",
                      pct, activeLEDs, NUM_PIXELS);
        lastPrintedLeds = activeLEDs;
      }

      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }

    // =========================================================================
    // LEVEL MODE: Original fill-up ring based on AQI
    // =========================================================================
    int aqi = round(localState.customAQIFloat);
    if (aqi < 1)
      aqi = 1;
    if (aqi > 10)
      aqi = 10;

    if (aqi != lastPrintedAqi) {
      Serial.printf("[LED Task] AQI Level %d. Updating NeoPixel Ring.\n", aqi);
      lastPrintedAqi = aqi;
    }

    ring.clear();

    if (aqi == 10) {
      if (millis() % 500 < 250) {
        ring.fill(ring.Color(255, 0, 0));
      } else {
        ring.clear();
      }
    } else {
      int activePixels = map(aqi, 1, 10, 1, NUM_PIXELS);

      uint32_t color;
      if (aqi < 4) {
        color = ring.Color(0, 255, 0);
      } else if (aqi >= 4 && aqi < 6) {
        color = ring.Color(200, 255, 0);
      } else if (aqi >= 6 && aqi < 8) {
        color = ring.Color(255, 100, 0);
      } else {
        color = ring.Color(255, 0, 0);
      }

      for (int i = 0; i < activePixels; i++) {
        ring.setPixelColor(i, color);
      }
    }

    ring.show();

    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// =============================================================================
// SERVO WARNING TASK (Replaces Motor Task — debug serial only for now)
// =============================================================================

void servoTask(void *pvParameters) {
  int lastPrintedState = -1;
  unsigned long lastSweepTime = 0;

  for (;;) {
    DeviceState localState;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50))) {
      localState = state;
      xSemaphoreGive(stateMutex);
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (localState.isWarmingUp) {
      if (millis() % 2000 < 100) {
        Serial.printf("[Servo Task] System Warming Up (%ds remaining)...\n",
                      localState.warmupSeconds);
      }
      lastPrintedState = 0;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    unsigned long now = millis();

    // =========================================================================
    // TIME-BASED MODE: Escalating servo sweep warnings
    // =========================================================================
    if (localState.currentMode == MODE_TIME_BASED) {
      float pct = localState.exposurePercent;
      unsigned long sweepInterval = 0;

      if (pct >= 100.0f) {
        sweepInterval = 500; // Continuous rapid sweeping
      } else if (pct >= 90.0f) {
        sweepInterval = 3000; // Every 3 seconds
      } else if (pct >= 75.0f) {
        sweepInterval = 10000; // Every 10 seconds
      } else {
        // Below 75% — servo idle
        if (lastPrintedState != 1) {
          Serial.println("[Servo Task] Exposure <75% — Servo idle.");
          lastPrintedState = 1;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        continue;
      }

      if (now - lastSweepTime >= sweepInterval) {
        Serial.printf(
            "[Servo Task] SWEEP! (0->180->0) | Exposure: %.1f%% | "
            "Interval: %lums\n",
            pct, sweepInterval);
        // TODO: Actual servo write when pin is assigned
        // servo.write(180); delay(300); servo.write(0);
        lastSweepTime = now;
        lastPrintedState = -1; // Reset so idle message prints when returning
      }

      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // =========================================================================
    // LEVEL MODE: Servo sweep based on AQI severity
    // =========================================================================
    int aqi = round(localState.customAQIFloat);
    unsigned long sweepInterval = 0;

    if (aqi >= 10) {
      sweepInterval = 500; // Continuous at max danger
    } else if (aqi >= 8) {
      sweepInterval = 3000;
    } else if (aqi >= 6) {
      sweepInterval = 10000;
    } else {
      // AQI < 6 — servo idle
      if (lastPrintedState != 2) {
        Serial.printf("[Servo Task] AQI %d — Servo idle.\n", aqi);
        lastPrintedState = 2;
      }
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (now - lastSweepTime >= sweepInterval) {
      Serial.printf("[Servo Task] SWEEP! (0->180->0) | AQI: %d | "
                    "Interval: %lums\n",
                    aqi, sweepInterval);
      // TODO: Actual servo write when pin is assigned
      lastSweepTime = now;
      lastPrintedState = -1;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// =============================================================================
// DIFFUSER TASK
// =============================================================================

const int DIFFUSER_PIN = 23;

const unsigned long ON_PULSE_TIME = 0.5 * 1000;
const unsigned long OFF_PULSE_TIME = 2 * 1000;

const unsigned long DIFFUSER_DURATION = 10 * 1000;

enum DiffuserState {
  STATE_IDLE,
  STATE_PULSE_ON,
  STATE_RUNNING,
  STATE_PULSE_OFF
};

void diffuserTask(void *pvParameters) {
  pinMode(DIFFUSER_PIN, OUTPUT);
  digitalWrite(DIFFUSER_PIN, HIGH); // Start HIGH (Diffuser Button NOT pressed)

  DiffuserState currentState = STATE_IDLE;
  int lastAqi = 0;
  bool exposureFired = false; // Only fire once per exposure session
  unsigned long stateTimestamp = 0;

  Serial.println("[Diffuser Task] Pulse State Machine ready.");

  for (;;) {
    DeviceState localState;
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50))) {
      localState = state;
      xSemaphoreGive(stateMutex);
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    if (localState.isWarmingUp) {
      digitalWrite(DIFFUSER_PIN, HIGH);
      currentState = STATE_IDLE;
      lastAqi = 0;
      exposureFired = false;

      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    unsigned long now = millis();

    // =========================================================================
    // TIME-BASED MODE: Fire diffuser when exposure hits 100%
    // =========================================================================
    if (localState.currentMode == MODE_TIME_BASED) {

      // Reset the one-shot flag when session resets
      if (localState.exposurePercent < 1.0f) {
        exposureFired = false;
      }

      switch (currentState) {

      case STATE_IDLE:
        if (localState.exposurePercent >= 100.0f && !exposureFired) {
          Serial.println("[Diffuser Task] EXPOSURE LIMIT REACHED! Starting ON "
                         "Pulse...");
          digitalWrite(DIFFUSER_PIN, LOW);
          stateTimestamp = now;
          currentState = STATE_PULSE_ON;
          exposureFired = true;
        }
        break;

      case STATE_PULSE_ON:
        if (now - stateTimestamp >= ON_PULSE_TIME) {
          Serial.println(
              "[Diffuser Task] ON Pulse finished. Diffuser is now running.");
          digitalWrite(DIFFUSER_PIN, HIGH);
          stateTimestamp = now;
          currentState = STATE_RUNNING;
        }
        break;

      case STATE_RUNNING: {
        unsigned long elapsed = now - stateTimestamp;
        if (elapsed % 2000 < 100) {
          long remaining = (long)DIFFUSER_DURATION - (long)elapsed;
          if (remaining < 0)
            remaining = 0;
          Serial.printf(
              "[Diffuser Task] Diffuser running... Time remaining: %ld ms\n",
              remaining);
        }
        if (elapsed >= DIFFUSER_DURATION) {
          Serial.println(
              "[Diffuser Task] Runtime expired! Starting OFF Pulse...");
          digitalWrite(DIFFUSER_PIN, LOW);
          stateTimestamp = now;
          currentState = STATE_PULSE_OFF;
        }
      } break;

      case STATE_PULSE_OFF:
        if (now - stateTimestamp >= OFF_PULSE_TIME) {
          Serial.println(
              "[Diffuser Task] OFF Pulse finished. Returning to IDLE.");
          digitalWrite(DIFFUSER_PIN, HIGH);
          currentState = STATE_IDLE;
        }
        break;
      }

      lastAqi = 0;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // =========================================================================
    // LEVEL MODE: Original AQI-threshold trigger
    // =========================================================================
    int aqi = round(localState.customAQIFloat);

    switch (currentState) {

    case STATE_IDLE:
      if (aqi >= 8 && lastAqi < 8) {
        Serial.println("[Diffuser Task] AQI Trigger! Starting 1s ON Pulse...");

        digitalWrite(DIFFUSER_PIN, LOW);
        stateTimestamp = now;
        currentState = STATE_PULSE_ON;
      }
      break;

    case STATE_PULSE_ON:
      if (now - stateTimestamp >= ON_PULSE_TIME) {
        Serial.println(
            "[Diffuser Task] ON Pulse finished. Diffuser is now running.");

        digitalWrite(DIFFUSER_PIN, HIGH);
        stateTimestamp = now;
        currentState = STATE_RUNNING;
      }
      break;

    case STATE_RUNNING: {
      unsigned long elapsed = now - stateTimestamp;

      // Only print every ~2 seconds
      if (elapsed % 2000 < 100) {
        // Calculate remaining time safely using signed longs
        long remaining = (long)DIFFUSER_DURATION - (long)elapsed;
        if (remaining < 0)
          remaining = 0; // Clamp to zero if it slightly overshoots

        Serial.printf(
            "[Diffuser Task] Diffuser running... Time remaining: %ld ms\n",
            remaining);
      }

      if (elapsed >= DIFFUSER_DURATION) {
        Serial.println(
            "[Diffuser Task] Runtime expired! Starting 3s OFF Pulse...");
        digitalWrite(DIFFUSER_PIN, LOW);
        stateTimestamp = now;
        currentState = STATE_PULSE_OFF;
      }
    } break;

    case STATE_PULSE_OFF:
      if (now - stateTimestamp >= OFF_PULSE_TIME) {
        Serial.println(
            "[Diffuser Task] OFF Pulse finished. Returning to IDLE.");

        digitalWrite(DIFFUSER_PIN, HIGH);
        currentState = STATE_IDLE;
      }
      break;
    }

    lastAqi = aqi;

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}