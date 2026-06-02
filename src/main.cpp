#include "actuatorTasks.h"
#include "aqiEngineTask.h"
#include "loggerTask.h"
#include "monitorTask.h"
#include "sensorTask.h"
#include "sharedState.h"
#include "webTask.h"
#include <Arduino.h>
#include <Wire.h>

DeviceState state;
HistoryData historyLogs;
SemaphoreHandle_t stateMutex;

void setup() {

  Serial.begin(115200);
  Wire.begin();

  Serial.println("Starting ESP32 Air Quality Monitor...");

  stateMutex = xSemaphoreCreateMutex();

  xTaskCreate(sensorTask, "SensorTask", 4096, NULL, 1, NULL);
  xTaskCreate(monitorTask, "MonitorTask", 2048, NULL, 1, NULL);
  xTaskCreate(webTask, "webTask", 4096, NULL, 1, NULL);
  xTaskCreate(aqiEngineTask, "AqiEngine", 3072, NULL, 1, NULL);
  xTaskCreate(ledTask, "LEDTask", 2048, NULL, 1, NULL);
  xTaskCreate(motorTask, "MotorTask", 2048, NULL, 1, NULL);
  xTaskCreate(diffuserTask, "DiffuserTask", 2048, NULL, 1, NULL);
  xTaskCreate(loggerTask, "LoggerTask", 2048, NULL, 1, NULL);

  Serial.println("Tasks launched successfully!");
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }