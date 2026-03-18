#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "Adafruit_SHT31.h"
#include "esp_sleep.h"
#include <time.h>
#include <SD.h>
#include <WebServer.h>
#include <ESPmDNS.h>

// ================== 引脚定义 ==================
#define SCLK 8
#define MOSI 9
#define OLED_RES  10
#define OLED_DC   20
#define OLED_CS   21
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define MISO 3
#define SD_CS 5

#define SHT_SDA 2
#define SHT_SCL 0
#define SHT31_ADDR 0x44

#define TOUCH_PIN 1 // TTP223 触摸引脚
#define LED_PIN 7

#define ALARM_TEMP 30.0f // 温度报警阈值

// ================== WiFi & NTP ==================
const char* ssid     = "EiHei-WiFi";
const char* password = "19896664";
const char* mdnsName = "myhome"; // 局域网访问地址: myhome.local

const char* ntpServer = "ntp2.aliyun.com";
const long gmtOffset_sec = 8 * 3600; // UTC+8
const int daylightOffset_sec = 0;

// ================== 全局对象 ==================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RES, OLED_CS);
Adafruit_SHT31 sht31;
WebServer server(80);

// FreeRTOS 句柄
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t timeSyncTaskHandle = NULL;
TaskHandle_t touchTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t webTaskHandle = NULL;
SemaphoreHandle_t displayMutex = NULL;

// 状态与数据全局变量 (用于任务间通信)
volatile bool alarmActive = false;
bool sdInitialized = false;
bool logStarted = false;
String logFileName = "";

float globalTemp = NAN;
float globalHum = NAN;

// ================== WebServer 响应函数 ==================
String getTimeString() {
  struct tm t;
  if (!getLocalTime(&t)) return "--:--:--";
  char b[16];
  strftime(b, sizeof(b), "%H:%M:%S", &t);
  return String(b);
}

String getDateString() {
  struct tm t;
  if (!getLocalTime(&t)) return "";
  char b[64];
  strftime(b, sizeof(b), "%Y-%m-%d", &t);
  return String(b);
}

String getTempWeb() {
  if (isnan(globalTemp)) return "--.- °C";
  char b[16];
  snprintf(b, sizeof(b), "%.1f °C", globalTemp);
  return String(b);
}

String getHumWeb() {
  if (isnan(globalHum)) return "--.- %RH";
  char b[20];
  snprintf(b, sizeof(b), "%.1f %%RH", globalHum);
  return String(b);
}

void handleRoot() {
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 温湿度监测系统</title>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@300;400;600;700&display=swap" rel="stylesheet">
<style>
:root{ --glass: rgba(255,255,255,0.045); --glass2: rgba(255,255,255,0.07); --border: rgba(255,255,255,0.08); --text: rgba(255,255,255,0.88); --muted: rgba(255,255,255,0.55); }
* { box-sizing: border-box; }
html,body{ height:100%; margin:0; font-family: Inter, system-ui, sans-serif; color: var(--text); }
body{ background: radial-gradient(900px 600px at 20% 10%, rgba(120,120,255,0.12), transparent 60%), radial-gradient(900px 600px at 80% 90%, rgba(255,120,200,0.10), transparent 60%), linear-gradient(135deg, #0b1020, #0e1628); }
.wrap{ height:100%; display:grid; place-items:center; padding:24px; }
.card{ width:min(880px,100%); display:grid; grid-template-columns: 1.2fr 0.8fr; gap:16px; padding:18px; border-radius:24px; background: linear-gradient(180deg,var(--glass2),var(--glass)); border:1px solid var(--border); backdrop-filter: blur(10px); }
@media(max-width:820px){ .card{grid-template-columns:1fr;} }
.panel{ padding:18px; border-radius:18px; background: rgba(255,255,255,0.04); border:1px solid var(--border); }
.title{ font-size:.85rem; letter-spacing:.08em; text-transform:uppercase; color:var(--muted); }
.temp{ margin-top:10px; font-size:clamp(4.2rem,9vw,6.8rem); font-weight:700; opacity:.92; }
.chip{ display:inline-block; margin-top:12px; padding:8px 12px; border-radius:999px; font-weight:600; background: rgba(255,255,255,0.05); border:1px solid var(--border); opacity:.85; }
.time{ margin-top:14px; font-size:clamp(2.1rem,5vw,3rem); font-weight:600; opacity:.85; }
.date{ margin-top:8px; font-size:1rem; color:var(--muted); }
.kv{ display:flex; justify-content:space-between; padding:10px 12px; margin-top:10px; border-radius:14px; background: rgba(255,255,255,0.04); border:1px solid var(--border); }
.k{ color:var(--muted); } .v{ font-weight:600; opacity:.9; }
.footer{ margin-top:14px; font-size:.9rem; color:var(--muted); }
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <div class="panel">
      <div class="title">室内环境监测系统</div>
      <div class="temp" id="temp">--.- °C</div>
      <div class="chip" id="hum">--.- %RH</div>
      <div class="time" id="time">--:--:--</div>
      <div class="date" id="date"></div>
    </div>
    <div class="panel">
      <div class="title">实时详细数据</div>
      <div class="kv"><div class="k">温度</div><div class="v" id="t2"></div></div>
      <div class="kv"><div class="k">湿度</div><div class="v" id="h2"></div></div>
      <div class="kv"><div class="k">时间</div><div class="v" id="time2"></div></div>
      <div class="kv"><div class="k">日期</div><div class="v" id="date2"></div></div>
    </div>
  </div>
</div>
<script>
async function update(){
  try{
    const t = await (await fetch('/temp')).text();
    const h = await (await fetch('/hum')).text();
    const ti = await (await fetch('/time')).text();
    const d = await (await fetch('/date')).text();
    temp.textContent = t; hum.textContent = h;
    time.textContent = ti; date.textContent = d;
    t2.textContent = t; h2.textContent = h;
    time2.textContent = ti; date2.textContent = d;
  }catch(e){}
}
update();
setInterval(update,1000);
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", page);
}

// ================== 中断与休眠逻辑 ==================
void IRAM_ATTR touchISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (touchTaskHandle) vTaskNotifyGiveFromISR(touchTaskHandle, &xHigherPriorityTaskWoken);
  if (xHigherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
}

void enterDeepSleep() {
  detachInterrupt(digitalPinToInterrupt(TOUCH_PIN));
  if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
    display.clearDisplay();
    display.setCursor(20, 25);
    display.setTextSize(2);
    display.println("SLEEPING");
    display.display();
    xSemaphoreGive(displayMutex);
  }
  delay(500);
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}

// ================== FreeRTOS 任务 ==================

void touchTask(void* pvParameters) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    delay(20);
    while (digitalRead(TOUCH_PIN) == HIGH) vTaskDelay(pdMS_TO_TICKS(10));
    enterDeepSleep();
  }
}

void ledTask(void* pvParameters) {
  bool state = LOW;
  for (;;) {
    if (alarmActive) {
      state = !state;
      digitalWrite(LED_PIN, state ? HIGH : LOW);
      vTaskDelay(pdMS_TO_TICKS(200));
    } else {
      digitalWrite(LED_PIN, LOW);
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
}

void connectWiFiAndSyncTime() {
  WiFi.begin(ssid, password);
  uint8_t retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 20) {
    delay(300);
    if (displayMutex && xSemaphoreTake(displayMutex, 0) == pdTRUE) {
      display.println(".");
      display.display();
      xSemaphoreGive(displayMutex);
    }
    retry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    
    // 初始化并开启 Web Server
    MDNS.begin(mdnsName);
    server.on("/", handleRoot);
    server.on("/time", []() { server.send(200, "text/plain", getTimeString()); });
    server.on("/date", []() { server.send(200, "text/plain", getDateString()); });
    server.on("/temp", []() { server.send(200, "text/plain", getTempWeb()); });
    server.on("/hum",  []() { server.send(200, "text/plain", getHumWeb()); });
    server.begin();
    
    if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
      display.println("WiFi & Web Ready");
      display.display();
      xSemaphoreGive(displayMutex);
    }
    
    // 等待时间同步
    time_t now = 0;
    int retryTime = 0;
    while ((now = time(NULL)) < 1000000000 && retryTime < 20) {
      delay(500);
      retryTime++;
    }
    delay(800);
  } else {
    if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
      display.println("WiFi FAILED!");
      display.display();
      xSemaphoreGive(displayMutex);
    }
    delay(800);
  }
}

void timeSyncTask(void* pvParameters) {
  connectWiFiAndSyncTime();
  vTaskDelete(NULL); // 运行一次后删除自身，释放内存
}

// Web Server 处理任务
void webServerTask(void* pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      server.handleClient();
    }
    vTaskDelay(pdMS_TO_TICKS(20)); // 让出 CPU 避免 Watchdog 复位，保持系统流畅
  }
}

void sensorTask(void* pvParameters) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(1000);
  for (;;) {
    vTaskDelayUntil(&lastWake, period);

    float t = sht31.readTemperature();
    float h = sht31.readHumidity();

    if (!isnan(t) && !isnan(h)) {
        globalTemp = t;
        globalHum = h;
        alarmActive = (t > ALARM_TEMP); // 简单的温度报警
    }

    time_t now = time(NULL);
    struct tm timeinfo;
    char timeStamp[20] = "--:--:--";
    char hhmm[6] = "--:--";
    if (now >= 1000000000) {
      if (localtime_r(&now, &timeinfo)) {
        strftime(timeStamp, sizeof(timeStamp), "%Y-%m-%d %H:%M:%S", &timeinfo);
        strftime(hhmm, sizeof(hhmm), "%H:%M", &timeinfo);
      }
    } else {
      snprintf(timeStamp, sizeof(timeStamp), "ms:%lu", millis());
    }

    if (sdInitialized && !logStarted) {
      char fname[32];
      if (now >= 1000000000) {
        if (localtime_r(&now, &timeinfo)) strftime(fname, sizeof(fname), "/log_%Y%m%d_%H%M%S.csv", &timeinfo);
        else snprintf(fname, sizeof(fname), "/log_%lu.csv", millis());
      } else {
        snprintf(fname, sizeof(fname), "/log_%lu.csv", millis());
      }
      logFileName = String(fname);
      File f = SD.open(logFileName.c_str(), FILE_WRITE);
      if (f) {
        f.println("timestamp,temperature,humidity");
        f.close();
        logStarted = true;
      }
    }

    if (sdInitialized && logStarted) {
      File f = SD.open(logFileName.c_str(), FILE_APPEND);
      if (f) {
        f.print(timeStamp); f.print(",");
        if (!isnan(t)) f.print(t, 1); else f.print("NaN"); f.print(",");
        if (!isnan(h)) f.print(h, 1); else f.print("NaN"); f.println();
        f.close();
      }
    }

    if (!isnan(t) && !isnan(h)) {
      if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
        display.clearDisplay();
        display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
        display.setCursor(90, 10);
        display.setTextSize(1);
        display.print(hhmm);
        display.setCursor(10, 10);
        display.setTextSize(1);
        display.print("TEMP:");
        display.setCursor(10, 22);
        display.setTextSize(2);
        display.print(t, 1); display.print(" C");
        display.setCursor(10, 42);
        display.setTextSize(1);
        display.print("HUMIDITY:");
        display.setCursor(70, 42);
        display.print(h, 1); display.print("%");
        display.display();
        xSemaphoreGive(displayMutex);
      }

      // 串口输出
      //Serial.printf("%s, Temp: %.1f C, Humidity: %.1f%%\n", timeStamp, t, h);
    }
  }
}

// ================== 主程序 ==================
void setup() {
  Serial.begin(115200);
  Wire.begin(SHT_SDA, SHT_SCL);
  SPI.begin(SCLK, MISO, MOSI, SD_CS);

  if (!SD.begin(SD_CS)) Serial.println("SD init failed");
  else sdInitialized = true;

  if(!display.begin(SSD1306_SWITCHCAPVCC)) for(;;);
  display.setRotation(2);
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("Init Systems...");
  display.display();

  displayMutex = xSemaphoreCreateMutex();

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
    while(digitalRead(TOUCH_PIN) == HIGH) delay(10);
  }

  if (!sht31.begin(SHT31_ADDR)) {
    Serial.println("Couldn't find SHT31");
    for(;;) vTaskDelay(portMAX_DELAY);
  }

  pinMode(TOUCH_PIN, INPUT_PULLDOWN);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // 挂载任务
  xTaskCreate(ledTask, "LedTask", 1024, NULL, 2, &ledTaskHandle);
  xTaskCreate(touchTask, "TouchTask", 2048, NULL, 2, &touchTaskHandle);
  attachInterrupt(digitalPinToInterrupt(TOUCH_PIN), touchISR, RISING);

  xTaskCreate(timeSyncTask, "TimeSync", 4096, NULL, 1, &timeSyncTaskHandle);

  // 创建传感器读取任务
  xTaskCreate(sensorTask, "SensorTask", 4096, NULL, 1, &sensorTaskHandle);
  
  // 创建 WebServer 守护任务
  xTaskCreate(webServerTask, "WebTask", 4096, NULL, 1, &webTaskHandle);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}