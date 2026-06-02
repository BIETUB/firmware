#include "loggerTask.h"
#include "sharedState.h"
#include <Arduino.h>

const unsigned long LOG_INTERVAL_MS =
    10000; // Change to 300000 (5 mins) for long term trends later

void loggerTask(void *pvParameters) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(LOG_INTERVAL_MS));

    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100))) {

      // Only record trend indicators if the primary startup period passed
      if (!state.isWarmingUp) {

        LogEntry newEntry;
        newEntry.uptimeSeconds = millis() / 1000;
        newEntry.aqi = round(state.customAQIFloat);
        newEntry.pm2_5 = state.pm2_5;
        newEntry.pm10 = state.pm10;
        newEntry.co2 = state.co2;
        newEntry.voc_indx = state.voc_indx;

        // Push values into the isolated history architecture safely
        historyLogs.buffer[historyLogs.head] = newEntry;
        historyLogs.head = (historyLogs.head + 1) % MAX_LOG_ENTRIES;

        if (historyLogs.count < MAX_LOG_ENTRIES) {
          historyLogs.count++;
        }

        Serial.printf("[Logger] Data logged to RAM history. Entries: %d/%d\n",
                      historyLogs.count, MAX_LOG_ENTRIES);
      }

      xSemaphoreGive(stateMutex);
    }
  }
}