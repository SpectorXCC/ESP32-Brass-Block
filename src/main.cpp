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

// 报警阈值（全局变量，可在 Web 端修改）
float threshHighT = 50.0;
float threshLowT  = 0.0;
float threshHighH = 80.0;
float threshLowH  = 30.0;

// ================== WiFi & NTP ==================
const char* ssid     = "EiHei-WiFi";
const char* password = "19896664";
const char* mdnsName = "myhome"; // 局域网访问地址: myhome.local

const char* ntpServer = "ntp2.aliyun.com";
const long gmtOffset_sec = 8 * 3600; // UTC+8
const int daylightOffset_sec = 0;

// ================== 全局对象与 FreeRTOS 句柄 ==================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RES, OLED_CS);
Adafruit_SHT31 sht31;
WebServer server(80);

TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t timeSyncTaskHandle = NULL;
TaskHandle_t touchTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t webTaskHandle = NULL;
SemaphoreHandle_t displayMutex = NULL;

// 状态与数据全局变量
volatile bool alarmActive = false;
bool sdInitialized = false;
bool logStarted = false;
String logFileName = "";

float globalTemp = NAN;
float globalHum = NAN;

// ================== 数据分析缓冲区与显示模式 ==================
enum DisplayMode { REALTIME_MODE, ANALYSIS_MODE };
volatile DisplayMode currentMode = REALTIME_MODE;

#define BUFFER_SIZE 60 // 存储过去60次采样（即过去60秒的数据）
float tempHistory[BUFFER_SIZE];
float humHistory[BUFFER_SIZE];
int bufferIndex = 0;
bool bufferFull = false;

// ================== 屏幕显示绘制函数 ==================
void drawRealTimePage(float t, float h, const char* hhmm) {
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
}

void drawAnalysisPage() {
  int count = bufferFull ? BUFFER_SIZE : bufferIndex;
  
  // 如果刚刚开机，还没有收集到数据
  if (count == 0) {
    display.clearDisplay();
    display.drawRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, SSD1306_WHITE);
    display.setCursor(15, 28);
    display.setTextSize(1);
    display.print("Collecting Data...");
    display.display();
    return;
  }

  float maxT = -99.0, minT = 99.0, sumT = 0;
  float maxH = -99.0, minH = 99.0, sumH = 0;

  // 遍历缓冲区计算极值和总和
  for (int i = 0; i < count; i++) {
    if (tempHistory[i] > maxT) maxT = tempHistory[i];
    if (tempHistory[i] < minT) minT = tempHistory[i];
    sumT += tempHistory[i];

    if (humHistory[i] > maxH) maxH = humHistory[i];
    if (humHistory[i] < minH) minH = humHistory[i];
    sumH += humHistory[i];
  }
  
  float avgT = sumT / count;
  float avgH = sumH / count;

  display.clearDisplay();
  display.setTextSize(1);
  
  // 标题
  display.setCursor(8, 2);
  display.println("--- 60s ANALYSIS ---");

  // 温度统计
  display.setCursor(0, 18);
  display.printf("T-Max:%.1f Min:%.1f", maxT, minT);
  display.setCursor(0, 30);
  display.printf("T-Avg:%.1f C", avgT);

  // 湿度统计
  display.setCursor(0, 44);
  display.printf("H-Max:%.1f Min:%.1f", maxH, minH);
  display.setCursor(0, 56);
  display.printf("H-Avg:%.1f %%", avgH);

  display.display();
}

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

/* 新增样式 */
.v-input { background:none; border:none; border-bottom:1px solid var(--border); color:white; width:50px; text-align:right; font-weight:600; outline:none; }
#btn-save { margin-top:15px; width:100%; padding:10px; border-radius:12px; background:rgba(255,255,255,0.1); color:white; border:1px solid var(--border); cursor:pointer; transition:0.3s; }
#btn-save:hover { background:rgba(255,255,255,0.2); }
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

    <!-- 右侧面板已被替换为：预设中心（Setup） -->
    <div class="panel">
      <div class="title">预设中心 (Setup)</div>
      <div style="margin-top:10px;">
        <div class="kv"><div class="k">温度上限</div><input type="number" id="in_th" class="v-input"></div>
        <div class="kv"><div class="k">温度下限</div><input type="number" id="in_tl" class="v-input"></div>
        <div class="kv"><div class="k">湿度上限</div><input type="number" id="in_hh" class="v-input"></div>
        <div class="kv"><div class="k">湿度下限</div><input type="number" id="in_hl" class="v-input"></div>
        <button onclick="save()" id="btn-save">应用更改</button>
      </div>
    </div>

  </div>
</div>

<script>
let firstLoad = true;

async function update(){
  try{
    const res = await fetch('/data');
    const d = await res.json();

    // 主面板显示
    const elTemp = document.getElementById('temp');
    const elHum = document.getElementById('hum');
    const elTime = document.getElementById('time');
    const elDate = document.getElementById('date');
    if(elTemp) elTemp.textContent = d.temp;
    if(elHum) elHum.textContent = d.hum;
    if(elTime) elTime.textContent = d.time;
    if(elDate) elDate.textContent = d.date;

    // 首次加载时填充输入框（阈值）
    if(firstLoad){
      if (d.tH !== undefined) document.getElementById('in_th').value = d.tH;
      if (d.tL !== undefined) document.getElementById('in_tl').value = d.tL;
      if (d.hH !== undefined) document.getElementById('in_hh').value = d.hH;
      if (d.hL !== undefined) document.getElementById('in_hl').value = d.hL;
      firstLoad = false;
    }
  }catch(e){
    console.log("Fetch error", e);
  }
}

async function save(){
  const th = document.getElementById('in_th').value;
  const tl = document.getElementById('in_tl').value;
  const hh = document.getElementById('in_hh').value;
  const hl = document.getElementById('in_hl').value;
  
  const btn = document.getElementById('btn-save');
  btn.textContent = "保存中...";
  
  await fetch(`/set?th=${encodeURIComponent(th)}&tl=${encodeURIComponent(tl)}&hh=${encodeURIComponent(hh)}&hl=${encodeURIComponent(hl)}`);
  
  btn.textContent = "已应用 ✓";
  setTimeout(() => { btn.textContent = "应用更改"; }, 2000);
}

update();
setInterval(update, 2000);
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

  // 睡前礼貌断开 Wi-Fi，让路由器释放连接状态，避免唤醒后 TCP 丢包卡顿
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100); 

  esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
  esp_deep_sleep_start();
}

// ================== FreeRTOS 任务 ==================

void touchTask(void* pvParameters) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(20)); // 简单防抖

    if (digitalRead(TOUCH_PIN) == HIGH) {
      uint32_t pressTime = millis();
      bool isLongPress = false;

      while (digitalRead(TOUCH_PIN) == HIGH) {
        if (millis() - pressTime > 1500) { // 设定长按时间为 1.5 秒
          isLongPress = true;
          break; 
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // 让出 CPU 避免看门狗复位
      }

      if (isLongPress) {
        // 【长按逻辑：休眠】
        while (digitalRead(TOUCH_PIN) == HIGH) {
          vTaskDelay(pdMS_TO_TICKS(50));
        }
        enterDeepSleep();
      } else {
        // 【短按逻辑：切换页面】
        currentMode = (currentMode == REALTIME_MODE) ? ANALYSIS_MODE : REALTIME_MODE;
        
        // 强制立即刷新屏幕，提供更快的触觉反馈
        if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
            if (currentMode == ANALYSIS_MODE) {
                drawAnalysisPage();
            } else {
                display.clearDisplay();
                display.setCursor(20, 28);
                display.setTextSize(1);
                display.print("Loading UI...");
                display.display();
                // 具体的实时数据将在 sensorTask 下一个1秒周期内迅速画出
            }
            xSemaphoreGive(displayMutex);
        }
      }
    }
    xTaskNotifyStateClear(NULL);
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
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
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
    
    MDNS.begin(mdnsName);
    server.on("/", handleRoot);
    server.on("/data", []() {
      String json = "{";
      json += "\"temp\":\"" + getTempWeb() + "\",";
      json += "\"hum\":\"" + getHumWeb() + "\",";
      json += "\"time\":\"" + getTimeString() + "\",";
      json += "\"date\":\"" + getDateString() + "\",";
      json += "\"tH\":" + String(threshHighT, 1) + ",";
      json += "\"tL\":" + String(threshLowT, 1) + ",";
      json += "\"hH\":" + String(threshHighH, 1) + ",";
      json += "\"hL\":" + String(threshLowH, 1);
      json += "}";
      server.send(200, "application/json", json);
    });

    // 新增 /set 路径，接收网页发来的新参数
    server.on("/set", HTTP_GET, []() {
      if (server.hasArg("th")) threshHighT = server.arg("th").toFloat();
      if (server.hasArg("tl")) threshLowT  = server.arg("tl").toFloat();
      if (server.hasArg("hh")) threshHighH = server.arg("hh").toFloat();
      if (server.hasArg("hl")) threshLowH  = server.arg("hl").toFloat();
      
      server.send(200, "text/plain", "OK");
    });
    server.begin();
    
    if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
      display.println("WiFi & Web Ready");
      display.display();
      xSemaphoreGive(displayMutex);
    }
    
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
  vTaskDelete(NULL); 
}

void webServerTask(void* pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      server.handleClient();
    }
    vTaskDelay(pdMS_TO_TICKS(2)); 
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
        // 全维度报警判断：任一条件不满足即触发
        alarmActive = (t > threshHighT || t < threshLowT || 
                  h > threshHighH || h < threshLowH);

        // 核心更新：将数据推入数据分析的环形缓冲区
        tempHistory[bufferIndex] = t;
        humHistory[bufferIndex] = h;
        bufferIndex++;
        if (bufferIndex >= BUFFER_SIZE) {
            bufferIndex = 0;
            bufferFull = true;
        }
    }

    // 时间处理与 SD 卡写入逻辑保持不变...
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

    if (sdInitialized && logStarted && !isnan(t) && !isnan(h)) {
      File f = SD.open(logFileName.c_str(), FILE_APPEND);
      if (f) {
        f.print(timeStamp); f.print(",");
        f.print(t, 1); f.print(",");
        f.print(h, 1); f.println();
        f.close();
      }
    }

    // 根据当前模式更新屏幕
    if (!isnan(t) && !isnan(h)) {
      if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
        if (currentMode == REALTIME_MODE) {
          drawRealTimePage(t, h, hhmm);
        } else if (currentMode == ANALYSIS_MODE) {
          drawAnalysisPage();
        }
        xSemaphoreGive(displayMutex);
      }
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

  // 修复后的深度休眠唤醒逻辑
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
    pinMode(TOUCH_PIN, INPUT_PULLDOWN);

    uint32_t wakeTime = millis();
    bool validWakeup = true;
    
    // 伪长按确认机制 (1.5秒)
    while (millis() - wakeTime < 1500) { 
      if (digitalRead(TOUCH_PIN) == LOW) {
        validWakeup = false; 
        break; 
      }
      delay(20);
    }

    if (!validWakeup) {
      // 误触则立刻滚回去继续睡
      esp_deep_sleep_enable_gpio_wakeup(1ULL << TOUCH_PIN, ESP_GPIO_WAKEUP_GPIO_HIGH);
      esp_deep_sleep_start();
    }

    // 确认是人为长按唤醒，显示启动画面
    if (xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
      display.clearDisplay();
      display.setCursor(10, 20);
      display.setTextSize(1);
      display.println("Waking up...");
      display.display();
      xSemaphoreGive(displayMutex);
    }
    
    // 等待手指松开
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
  xTaskCreate(sensorTask, "SensorTask", 4096, NULL, 1, &sensorTaskHandle);
  xTaskCreate(webServerTask, "WebTask", 4096, NULL, 1, &webTaskHandle);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}