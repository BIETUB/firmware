#include "actuatorTasks.h"
#include "sharedState.h"
#include <Arduino.h>

const int LED_PIN_1 = 13;
const int LED_PIN_2 = 12;
const int LED_PIN_3 = 14;
const int LED_PIN_4 = 27;

void ledTask(void *pvParameters) {
  int lastPrintedAqi = -1;

  pinMode(LED_PIN_1, OUTPUT);
  pinMode(LED_PIN_2, OUTPUT);
  pinMode(LED_PIN_3, OUTPUT);
  pinMode(LED_PIN_4, OUTPUT);
  digitalWrite(LED_PIN_1, LOW);
  digitalWrite(LED_PIN_2, LOW);
  digitalWrite(LED_PIN_3, LOW);
  digitalWrite(LED_PIN_4, LOW);

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
        Serial.printf("[LED Task] System Warming Up (%ds remaining)... "
                      "Displaying Soft Cyan Pulse.\n",
                      localState.warmupSeconds);
      }
      lastPrintedAqi = -1;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Convert the float to an integer level (1 to 10)
    int aqi = round(localState.customAQIFloat);

    // Only print if the level actually changed to avoid spamming the console
    if (aqi != lastPrintedAqi) {
      if (aqi < 4) {
        Serial.println("[LED Task] AQI Safe. LEDs: ALL GREEN (Static)");
        digitalWrite(LED_PIN_1, HIGH);
        digitalWrite(LED_PIN_2, LOW);
        digitalWrite(LED_PIN_3, LOW);
        digitalWrite(LED_PIN_4, LOW);
      } else if (aqi >= 4 && aqi < 6) {
        Serial.println("[LED Task] AQI Level 4+. LEDs: First few active (Slow "
                       "blinking animation)");
        digitalWrite(LED_PIN_1, HIGH);
        digitalWrite(LED_PIN_2, HIGH);
        digitalWrite(LED_PIN_3, LOW);
        digitalWrite(LED_PIN_4, LOW);
      } else if (aqi >= 6 && aqi < 8) {
        Serial.println("[LED Task] AQI Level 6+. LEDs: Middle segment active "
                       "(Orange alert pattern)");
        digitalWrite(LED_PIN_1, HIGH);
        digitalWrite(LED_PIN_2, HIGH);
        digitalWrite(LED_PIN_3, HIGH);
        digitalWrite(LED_PIN_4, LOW);
      } else if (aqi >= 8 && aqi < 10) {
        Serial.println("[LED Task] AQI Level 8+. LEDs: Upper segment active "
                       "(Red alert pattern)");
        digitalWrite(LED_PIN_1, HIGH);
        digitalWrite(LED_PIN_2, HIGH);
        digitalWrite(LED_PIN_3, HIGH);
        digitalWrite(LED_PIN_4, HIGH);
      } else if (aqi >= 10) {
        Serial.println("[LED Task] AQI Level 10! LEDs: ALL ACTIVE (Intense red "
                       "flashing animation!)");
        digitalWrite(LED_PIN_1, HIGH);
        digitalWrite(LED_PIN_2, HIGH);
        digitalWrite(LED_PIN_3, HIGH);
        digitalWrite(LED_PIN_4, HIGH);
      }
      lastPrintedAqi = aqi;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void motorTask(void *pvParameters) {
  int lastPrintedSpeed = -1;

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
        Serial.printf("[Motor Task] System Warming Up (%ds remaining)... ",
                      localState.warmupSeconds);
      }
      lastPrintedSpeed = 0;
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    int aqi = round(localState.customAQIFloat);
    int currentTargetSpeed = 0; // 0% to 100%

    // Calculate intended speed based on your rules
    if (aqi < 6) {
      currentTargetSpeed = 0; // Level 1-5: Motor Off
    } else if (aqi >= 6 && aqi < 8) {
      currentTargetSpeed = 35; // Level 6-7: Motor Spinning (Slowly)
    } else if (aqi >= 8 && aqi < 10) {
      currentTargetSpeed = 70; // Level 8-9: Motor Speeds Up
    } else if (aqi >= 10) {
      currentTargetSpeed = 100; // Level 10: Motor Max Speed
    }

    // Only print when target speed changes
    if (currentTargetSpeed != lastPrintedSpeed) {
      Serial.printf(
          "[Motor Task] AQI Level is %d -> Adjusting Fan PWM to: %d%%\n", aqi,
          currentTargetSpeed);
      lastPrintedSpeed = currentTargetSpeed;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

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

      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    int aqi = round(localState.customAQIFloat);
    unsigned long now = millis();

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

    case STATE_RUNNING:
      if ((now - stateTimestamp) % 2000 < 100) {
        Serial.printf(
            "[Diffuser Task] Diffuser running... Time remaining: %lu ms\n",
            DIFFUSER_DURATION - (now - stateTimestamp));
      }

      if (now - stateTimestamp >= DIFFUSER_DURATION) {
        Serial.println(
            "[Diffuser Task] Runtime expired! Starting 3s OFF Pulse...");

        digitalWrite(DIFFUSER_PIN, LOW);
        stateTimestamp = now;
        currentState = STATE_PULSE_OFF;
      }
      break;

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