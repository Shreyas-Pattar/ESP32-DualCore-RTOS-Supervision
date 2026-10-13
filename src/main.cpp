#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_idf_version.h"

#define WDT_TIMEOUT_SECONDS 3

typedef struct {
    uint32_t timestamp_ms;
    uint16_t sensor_raw;
    uint32_t execution_cycles;
    uint8_t  core_id;
} TelemetryPacket_t;

QueueHandle_t telemetryQueue;
SemaphoreHandle_t printMutex;
TaskHandle_t sensorTaskHandle = NULL;

void CheckBootReason(void) {
    esp_reset_reason_t reason = esp_reset_reason();
    Serial.println("\n============================================");
    Serial.print("[SYSTEM BOOT] Reset Reason: ");
    switch (reason) {
        case ESP_RST_POWERON:
            Serial.println("POWER-ON RESET (Normal cold boot)");
            break;
        case ESP_RST_TASK_WDT:
            Serial.println("CRITICAL FAULT: TASK WATCHDOG TIMEOUT!");
            Serial.println("System recovered automatically from task starvation.");
            break;
        case ESP_RST_PANIC:
            Serial.println("EXCEPTION / PANIC RESET");
            break;
        default:
            Serial.printf("Code %d\n", reason);
            break;
    }
    Serial.println("============================================\n");
}

void TaskSensorAcquisition(void *pvParameters) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(50); // 20 Hz
    TelemetryPacket_t packet;
    uint32_t loop_count = 0;

    esp_task_wdt_add(NULL);

    for (;;) {
        uint32_t t_start = xthal_get_ccount();

        packet.timestamp_ms = millis();
        packet.sensor_raw = (uint16_t)(esp_random() & 0x0FFF);
        packet.core_id = xPortGetCoreID();

        for (volatile int i = 0; i < 1500; i++) { __asm__ __volatile__("nop"); }

        packet.execution_cycles = xthal_get_ccount() - t_start;
        xQueueSend(telemetryQueue, &packet, (TickType_t)0);

        esp_task_wdt_reset();

        loop_count++;
        // Trigger deadlock after ~10 seconds (200 cycles * 50ms)
        if (loop_count >= 200) {
            if (xSemaphoreTake(printMutex, portMAX_DELAY) == pdTRUE) {
                Serial.println("\n[FAULT INJECTION] Inducing artificial task deadlock...");
                Serial.println("[FAULT INJECTION] SensorTask halted. Watchdog starvation starting!");
                xSemaphoreGive(printMutex);
            }
            while (1) {
                // Infinite busy-wait locks CPU Core 0 without yielding
            }
        }

        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void TaskTelemetryLogger(void *pvParameters) {
    TelemetryPacket_t rx_packet;

    for (;;) {
        if (xQueueReceive(telemetryQueue, &rx_packet, portMAX_DELAY) == pdTRUE) {
            if (xSemaphoreTake(printMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                Serial.printf("[T=%06u ms | Core %u] Sample: 0x%03X | Cost: %u cycles\n",
                              rx_packet.timestamp_ms,
                              rx_packet.core_id,
                              rx_packet.sensor_raw,
                              rx_packet.execution_cycles);
                xSemaphoreGive(printMutex);
            }
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    CheckBootReason();

    /* Framework version abstraction */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    // ESP-IDF v5.x (Wokwi / Arduino Core 3.x)
    esp_task_wdt_config_t twdt_config = {
        .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_task_wdt_init(&twdt_config);
#else
    // ESP-IDF v4.x (PlatformIO Stable Core 2.x)
    esp_task_wdt_init(WDT_TIMEOUT_SECONDS, true);
#endif

    printMutex = xSemaphoreCreateMutex();
    telemetryQueue = xQueueCreate(16, sizeof(TelemetryPacket_t));

    xTaskCreatePinnedToCore(TaskSensorAcquisition, "SensorAcq", 4096, NULL, 2, &sensorTaskHandle, 0);
    xTaskCreatePinnedToCore(TaskTelemetryLogger, "TelemLog", 4096, NULL, 1, NULL, 1);
}

void loop() {
    vTaskDelete(NULL);
}