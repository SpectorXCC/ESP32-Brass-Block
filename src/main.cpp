#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "Adafruit_SHT31.h"
#include "esp_sleep.h"
#include <time.h>
#include <SD.h> // <--- 添加：SD 卡支持

#define SCLK 8
#define MOSI 9
#define OLED_RES  10
#define OLED_DC   20
#define OLED_CS   21
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define MISO 3    // <--- 添加：SD 卡 MISO 引脚（示例）
#define SD_CS 5   // <--- 添加：SD 卡片选引脚（示例）

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &SPI,
  OLED_DC,
  OLED_RES,
  OLED_CS
);

#define SHT_SDA 2
#define SHT_SCL 0
#define SHT31_ADDR 0x44

#define TOUCH_PIN 1 // TTP223 触摸引脚
#define LED_PIN 7

#define ALARM_TEMP 30.0f // 温度报警阈值

// WiFi
const char* ssid     = "EiHei-WiFi";
const char* password = "19896664";

// NTP
const char* ntpServer = "ntp2.aliyun.com";
const long gmtOffset_sec = 8 * 3600; // UTC+8
const int daylightOffset_sec = 0;

Adafruit_SHT31 sht31;

// FreeRTOS objects
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t timeSyncTaskHandle = NULL;
TaskHandle_t touchTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;
SemaphoreHandle_t displayMutex = NULL;

volatile bool alarmActive = false; // 报警状态标志

// SD / logging 全局
bool sdInitialized = false;
bool logStarted = false;
String logFileName = "";

// Touch ISR: notify touch task
void IRAM_ATTR touchISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (touchTaskHandle) vTaskNotifyGiveFromISR(touchTaskHandle, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken == pdTRUE) {
    portYIELD_FROM_ISR();
  }
}

// WiFi + NTP sync (same logic as before)
void connectWiFiAndSyncTime() {
  WiFi.begin(ssid, password);
  uint8_t retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(300);
    if (displayMutex) {
      if (xSemaphoreTake(displayMutex, 0) == pdTRUE) {
        display.println(".");
        display.display();
        xSemaphoreGive(displayMutex);
      }
    }
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    if (displayMutex) {
      if (xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
        display.println("Syncing time...");
        display.display();
        xSemaphoreGive(displayMutex);
      }
    }

    // 等待时间同步（最多 ~10s）
    time_t now = 0;
    int retryTime = 0;
    while ((now = time(NULL)) < 1000000000 && retryTime < 20) {
      delay(500);
      retryTime++;
    }

    if (displayMutex) {
      if (xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
        if (now >= 1000000000) display.println("Time Synced");
        else display.println("Time Sync Failed");
        display.display();
        xSemaphoreGive(displayMutex);
      }
    }
    delay(800);
  } else {
    if (displayMutex) {
      if (xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
        display.println("WiFi FAILED!");
        display.display();
        xSemaphoreGive(displayMutex);
      }
    }
    delay(800);
  }
}

// Enter deep sleep (called from touch task)
void enterDeepSleep() {
  // disable touch interrupt
  detachInterrupt(digitalPinToInterrupt(TOUCH_PIN));

  if (displayMutex) {
    if (xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
      display.clearDisplay();
      display.setCursor(20, 25);
      display.setTextSize(2);
      display.println("SLEEPING");
      display.display();
      xSemaphoreGive(displayMutex);
    }
  }
  delay(500);
  display.ssd1306_command(SSD1306_DISPLAYOFF);

  // 配置 GPIO 唤醒（高电平唤醒）
  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}

// Touch task: waits for notify from ISR, then enters deep sleep
void touchTask(void* pvParameters) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // wait indefinitely
    // debounce / ensure press released before sleeping
    delay(20);
    // Wait until the sensor pin is low (not pressed) to avoid immediate wake
    // If TTP223 stays high while finger on it, wait until release then sleep
    while (digitalRead(TOUCH_PIN) == HIGH) vTaskDelay(pdMS_TO_TICKS(10));
    enterDeepSleep();
  }
}

// LED 报警任务：alarmActive 为 true 时快速翻转
void ledTask(void* pvParameters) {
  bool state = LOW;
  for (;;) {
    if (alarmActive) {
      state = !state;
      digitalWrite(LED_PIN, state ? HIGH : LOW);
      vTaskDelay(pdMS_TO_TICKS(200)); // 快速翻转：200ms 周期
    } else {
      digitalWrite(LED_PIN, LOW);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

// Time sync task: runs once on startup if required, then deletes itself
void timeSyncTask(void* pvParameters) {
  connectWiFiAndSyncTime();
  vTaskDelete(NULL);
}

// Sensor & display task: runs every 1s
void sensorTask(void* pvParameters) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000);
  for (;;) {
    vTaskDelayUntil(&lastWake, period);

    float t = sht31.readTemperature();
    float h = sht31.readHumidity();

    // 更新报警状态：温度超过阈值触发
    if (!isnan(t)) {
      alarmActive = (t > ALARM_TEMP);
    }

    // 获取当前时间与格式化字符串（用于显示与日志）
    time_t now = time(NULL);
    struct tm timeinfo;
    char timeStamp[20] = "--:--:--";
    char hhmm[6] = "--:--";
    if (now >= 1000000000) {
      time_t localNow = now + gmtOffset_sec; // 将 UTC 转成本地时间
      if (localtime_r(&now, &timeinfo)) {
        strftime(timeStamp, sizeof(timeStamp), "%Y-%m-%d %H:%M:%S", &timeinfo);
        strftime(hhmm, sizeof(hhmm), "%H:%M", &timeinfo);
      }
    } else {
      snprintf(timeStamp, sizeof(timeStamp), "ms:%lu", millis());
    }

    // 创建日志文件（如果尚未创建）
    if (sdInitialized && !logStarted) {
      char fname[32];
      if (now >= 1000000000) {
        if (localtime_r(&now, &timeinfo)) {
          strftime(fname, sizeof(fname), "/log_%Y%m%d_%H%M%S.csv", &timeinfo);
        } else {
          snprintf(fname, sizeof(fname), "/log_%lu.csv", millis());
        }
      } else {
        snprintf(fname, sizeof(fname), "/log_%lu.csv", millis());
      }
      logFileName = String(fname);
      File f = SD.open(logFileName.c_str(), FILE_WRITE);
      if (f) {
        f.println("timestamp,temperature,humidity");
        f.close();
        logStarted = true;
        Serial.printf("Log file created: %s\n", logFileName.c_str());
      } else {
        Serial.println("Failed to create log file");
      }
    }

    // 写入 CSV（每秒一次，如果可用）
    if (sdInitialized && logStarted) {
      File f = SD.open(logFileName.c_str(), FILE_APPEND);
      if (f) {
        f.print(timeStamp);
        f.print(",");
        if (!isnan(t)) f.print(t, 1); else f.print("NaN");
        f.print(",");
        if (!isnan(h)) f.print(h, 1); else f.print("NaN");
        f.println();
        f.close();
      } else {
        Serial.println("SD open append failed");
      }
    }

    // OLED 显示：保留原有 时间 + 温湿度 布局
    if (!isnan(t) && !isnan(h)) {
      if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);

        // 时间 (HH:MM)
        display.setCursor(90, 10);
        display.setTextSize(1);
        display.print(hhmm);

        // 温度
        display.setCursor(10, 10);
        display.setTextSize(1);
        display.print("TEMP:");
        display.setCursor(10, 22);
        display.setTextSize(2);
        display.print(t, 1);
        display.print(" C");

        // 湿度
        display.setCursor(10, 42);
        display.setTextSize(1);
        display.print("HUMIDITY:");
        display.setCursor(70, 42);
        display.print(h, 1);
        display.print("%");

        display.display();
        xSemaphoreGive(displayMutex);
      }

      // 串口输出
      //Serial.printf("%s, Temp: %.1f C, Humidity: %.1f%%\n", timeStamp, t, h);
    } else {
      if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.setCursor(0,0);
        display.println("Sensor Read Fail");
        display.display();
        xSemaphoreGive(displayMutex);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  Wire.begin(SHT_SDA, SHT_SCL);

  SPI.begin(SCLK, MISO, MOSI, SD_CS);

  if (!SD.begin(SD_CS)) {
      Serial.println("SD init failed");
  } else {
      sdInitialized = true;
  }

  if(!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println(F("OLED allocation failed"));
    for(;;);
  }
  display.setRotation(2);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("Init Systems...");
  display.display();

  // Create display mutex
  displayMutex = xSemaphoreCreateMutex();

  // Handle wake-from-deep-sleep
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    if (xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
      display.clearDisplay();
      display.setCursor(10, 20);
      display.setTextSize(1);
      display.println("Waking up...");
      display.display();
      xSemaphoreGive(displayMutex);
    }
    pinMode(TOUCH_PIN, INPUT_PULLDOWN);
    // 防连触：等手指离开
    while(digitalRead(TOUCH_PIN) == HIGH) {
      delay(10);
    }
  }

  // Init sensor
  if (!sht31.begin(SHT31_ADDR)) {
    Serial.println("Couldn't find SHT31");
    if (xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
      display.setCursor(0, 10);
      display.println("SHT31 Error!");
      display.display();
      xSemaphoreGive(displayMutex);
    }
    for(;;) vTaskDelay(portMAX_DELAY);
  }

  // Setup touch pin & ISR + touch task
  pinMode(TOUCH_PIN, INPUT_PULLDOWN);
  // 初始化 LED 引脚并创建任务
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  xTaskCreate(ledTask, "LedTask", 1024, NULL, 2, &ledTaskHandle);
  xTaskCreate(touchTask, "TouchTask", 2048, NULL, 2, &touchTaskHandle);
  attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), touchISR, RISING);

  // If not wake-from-deep-sleep, create time sync task to sync NTP once
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_GPIO) {
    xTaskCreate(timeSyncTask, "TimeSync", 4096, NULL, 1, &timeSyncTaskHandle);
  }

  // Create sensor task (periodic display update)
  xTaskCreate(sensorTask, "SensorTask", 4096, NULL, 1, &sensorTaskHandle);
}

void loop() {
  // empty: work handled by FreeRTOS tasks
  vTaskDelay(portMAX_DELAY);
}