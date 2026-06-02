#include "sensorTask.h"
#include "sharedState.h"
#include <Arduino.h>
#include <SensirionI2cSen66.h>
#include <Wire.h>

void sensorTask(void *pvParameters) {

  SensirionI2cSen66 sen66;
  sen66.begin(Wire, SEN66_I2C_ADDR_6B);
  sen66.deviceReset();

  vTaskDelay(pdMS_TO_TICKS(100));

  sen66.startContinuousMeasurement();
  Serial.println("[SensorTask] SEN66 started continuous measurement.");
  vTaskDelay(pdMS_TO_TICKS(1000));

  for (;;) {
    float massConcentrationPm1p0;
    float massConcentrationPm2p5;
    float massConcentrationPm4p0;
    float massConcentrationPm10p0;
    float humidity;
    float temperature;
    float vocIndex;
    float noxIndex;
    uint16_t co2;

    int16_t error = sen66.readMeasuredValues(
        massConcentrationPm1p0, massConcentrationPm2p5, massConcentrationPm4p0,
        massConcentrationPm10p0, humidity, temperature, vocIndex, noxIndex,
        co2);

    if (error == 0) {

      if (xSemaphoreTake(stateMutex, portMAX_DELAY)) {

        state.pm1 = massConcentrationPm1p0;
        state.pm2_5 = massConcentrationPm2p5;
        state.pm4 = massConcentrationPm4p0;
        state.pm10 = massConcentrationPm10p0;
        state.co2 = co2;
        state.voc_indx = vocIndex;
        state.nox_indx = noxIndex;
        state.humidity = humidity;
        state.temperature = temperature;

        xSemaphoreGive(stateMutex);
      }

    } else {
      Serial.printf("[SensorTask] Error reading SEN66: %d\n", error);
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
