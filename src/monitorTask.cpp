#include "monitorTask.h"
#include "sharedState.h"
#include <Arduino.h>

void monitorTask(void *pvParameters) {
  for (;;) {
    DeviceState localState;

    if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {
      localState = state;
      xSemaphoreGive(stateMutex);
    }

    Serial.println("\n--- Current Environmental State ---");
    Serial.printf("PM1.0: %5.2f | PM2.5: %5.2f | PM10: %5.2f µg/m³\n",
                  localState.pm1, localState.pm2_5, localState.pm10);

    Serial.printf("Temp:  %5.2f°C | Hum:   %5.2f%% \n", localState.temperature,
                  localState.humidity);

    Serial.printf("VOC:   %5.2f   | NOx:   %5.2f   | CO2: %.0f ppm\n",
                  localState.voc_indx, localState.nox_indx, localState.co2);
    Serial.println("-----------------------------------");

    Serial.printf("[AQI] %.2f (%d) | Worst: %s\n", localState.customAQIFloat,
                  localState.customAQI, localState.worstPollutant);

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}