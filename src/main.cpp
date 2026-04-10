#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "Adafruit_SHT31.h"
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
//const char* mdnsName = "myhome"; // 局域网访问地址: myhome.local

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
volatile bool isLowPowerMode = false; // 新增：软休眠状态标志位
bool sdInitialized = false;
bool logStarted = false;
String logFileName = "";

float globalTemp = NAN;
float globalHum = NAN;

// ================== 数据分析缓冲区与显示模式 ==================
enum DisplayMode { REALTIME_MODE, ANALYSIS_MODE };
volatile DisplayMode currentMode = REALTIME_MODE;

#define BUFFER_SIZE 60 // 存储过去60次采样
float tempHistory[BUFFER_SIZE];
float humHistory[BUFFER_SIZE];
int bufferIndex = 0;
bool bufferFull = false;

// ================== 自动休眠逻辑变量 ==================
float lastStableTemp = 0.0;
float lastStableHum = 0.0;
unsigned long lastChangeTime = 0; 
const unsigned long AUTO_SLEEP_TIMEOUT = 30000; // 30秒无变化则休眠

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
  
  display.setCursor(8, 2);
  display.println("--- 60s ANALYSIS ---");

  display.setCursor(0, 18);
  display.printf("T-Max:%.1f Min:%.1f", maxT, minT);
  display.setCursor(0, 30);
  display.printf("T-Avg:%.1f C", avgT);

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
.panel{ padding:18px; border-radius:18px; background: rgba(255,255,255,0.04); border:1px solid var(--border); transition: 0.3s; }
.title{ font-size:.85rem; letter-spacing:.08em; text-transform:uppercase; color:var(--muted); }
.temp{ margin-top:10px; font-size:clamp(4.2rem,9vw,6.8rem); font-weight:700; opacity:.92; transition: 0.3s; }
.chip{ display:inline-block; margin-top:12px; padding:8px 12px; border-radius:999px; font-weight:600; background: rgba(255,255,255,0.05); border:1px solid var(--border); opacity:.85; transition: 0.3s; }
.time{ margin-top:14px; font-size:clamp(2.1rem,5vw,3rem); font-weight:600; opacity:.85; transition: 0.3s; }
.date{ margin-top:8px; font-size:1rem; color:var(--muted); }
.kv{ display:flex; justify-content:space-between; padding:10px 12px; margin-top:10px; border-radius:14px; background: rgba(255,255,255,0.04); border:1px solid var(--border); }
.k{ color:var(--muted); } .v{ font-weight:600; opacity:.9; }
.footer{ margin-top:14px; font-size:.9rem; color:var(--muted); }
.v-input { background:none; border:none; border-bottom:1px solid var(--border); color:white; width:50px; text-align:right; font-weight:600; outline:none; }
#btn-save { margin-top:15px; width:100%; padding:10px; border-radius:12px; background:rgba(255,255,255,0.1); color:white; border:1px solid var(--border); cursor:pointer; transition:0.3s; }
#btn-save:hover { background:rgba(255,255,255,0.2); }
#btn-power { margin-top:10px; width:100%; padding:10px; border-radius:12px; background:rgba(255,255,255,0.1); color:white; border:1px solid var(--border); cursor:pointer; transition:0.3s; }
#btn-power:hover { background:rgba(255,255,255,0.2); }
</style>
</head>
<body>
<div class="wrap">
  <div class="card">
    <div class="panel" id="main-panel">
      <div class="title">室内环境监测系统</div>
      <div class="temp" id="temp">--.- °C</div>
      <div class="chip" id="hum">--.- %RH</div>
      <div class="time" id="time">--:--:--</div>
      <div class="date" id="date"></div>
    </div>

    <div class="panel">
      <div class="title">预设中心 (Setup)</div>
      <div style="margin-top:10px;">
        <div class="kv"><div class="k">温度上限</div><input type="number" id="in_th" class="v-input"></div>
        <div class="kv"><div class="k">温度下限</div><input type="number" id="in_tl" class="v-input"></div>
        <div class="kv"><div class="k">湿度上限</div><input type="number" id="in_hh" class="v-input"></div>
        <div class="kv"><div class="k">湿度下限</div><input type="number" id="in_hl" class="v-input"></div>
        <button onclick="save()" id="btn-save">应用更改</button>
        <button onclick="togglePower()" id="btn-power">进入低功耗</button>
      </div>
    </div>

  </div>
</div>

<script>
let firstLoad = true;

async function togglePower(){
  try{
    await fetch('/power?action=toggle');
    // 小延迟后刷新状态（让设备有时间切换）
    setTimeout(update, 300);
  }catch(e){ console.log("Power toggle error", e); }
}

async function update(){
  try{
    const res = await fetch('/data');
    const d = await res.json();

    const elTemp = document.getElementById('temp');
    const elHum = document.getElementById('hum');
    const elTime = document.getElementById('time');
    const elDate = document.getElementById('date');

    const btnPower = document.getElementById('btn-power');

    // 无论是否休眠，首次加载都要填充阈值输入框
    if(firstLoad){
      if (d.tH !== undefined) document.getElementById('in_th').value = d.tH;
      if (d.tL !== undefined) document.getElementById('in_tl').value = d.tL;
      if (d.hH !== undefined) document.getElementById('in_hh').value = d.hH;
      if (d.hL !== undefined) document.getElementById('in_hl').value = d.hL;
      firstLoad = false;
    }

    // 根据设备是否处于休眠状态(isIdle)更新 UI
    if(d.isIdle) {
      if(elTemp) elTemp.style.opacity = "0.2";
      if(elHum) elHum.style.opacity = "0.2";
      if(elTime) { elTime.style.color = "var(--muted)"; elTime.textContent = "待机休眠中..."; }
    } else {
      if(elTemp) { elTemp.style.opacity = "1"; elTemp.textContent = d.temp; }
      if(elHum) { elHum.style.opacity = "1"; elHum.textContent = d.hum; }
      if(elTime) { elTime.style.color = "var(--text)"; elTime.textContent = d.time; }
      if(elDate) elDate.textContent = d.date;
    }
    
    if(btnPower){
      btnPower.textContent = d.isIdle ? "退出低功耗" : "进入低功耗";
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
setInterval(update, 200);
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

// 新增：进入软休眠
void enterLowPower() {
  if (isLowPowerMode) return;
  isLowPowerMode = true;
  
  if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
    display.clearDisplay();
    display.setCursor(20, 25);
    display.setTextSize(2);
    display.println("STANDBY");
    display.display();
    xSemaphoreGive(displayMutex);
  }
  delay(600); // 留出时间给用户看提示
  
  if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
    display.ssd1306_command(SSD1306_DISPLAYOFF); // 关屏极大地节省电流
    xSemaphoreGive(displayMutex);
  }
  
  WiFi.setSleep(true); // 启用 WiFi Modem Sleep
}

// 新增：退出软休眠
void exitLowPower() {
  if (!isLowPowerMode) return;
  isLowPowerMode = false;
  
  // 恢复 WiFi 性能
  WiFi.setSleep(false);
  
  // 唤醒屏幕
  if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    display.clearDisplay();
    display.setCursor(10, 28);
    display.setTextSize(2);
    display.print("WAKING UP");
    display.display();
    xSemaphoreGive(displayMutex);
  }

  // 关键：重置自动休眠的相关参考点
  lastChangeTime = millis(); 
  lastStableTemp = globalTemp; // 以当前值为新基准，防止唤醒后立即又满足自动休眠条件
  lastStableHum = globalHum;

  //Serial.println("<<< 系统已唤醒");
}

// ================== FreeRTOS 任务 ==================

void touchTask(void* pvParameters) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(20)); // 防抖

    if (digitalRead(TOUCH_PIN) == HIGH) {
      // --- 关键修改：重置自动休眠计时器 ---
      lastChangeTime = millis(); 
      
      uint32_t pressTime = millis();
      bool isLongPress = false;

      while (digitalRead(TOUCH_PIN) == HIGH) {
        if (millis() - pressTime > 1500) {
          isLongPress = true;
          break; 
        }
        vTaskDelay(pdMS_TO_TICKS(20)); 
      }

      if (isLongPress) {
        // 【长按：手动强制切换】
        while (digitalRead(TOUCH_PIN) == HIGH) vTaskDelay(pdMS_TO_TICKS(50));
        
        if (isLowPowerMode) exitLowPower();
        else enterLowPower();
        
      } else {
        // 【短按】
        if (isLowPowerMode) {
          // 如果处于休眠状态，短按直接唤醒
          exitLowPower();
          //Serial.println(">>> 手动触摸唤醒");
        } else {
          // 如果已经是清醒状态，短按切换显示模式
          currentMode = (currentMode == REALTIME_MODE) ? ANALYSIS_MODE : REALTIME_MODE;
          
          // 立即刷新屏幕反馈
          if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
            display.clearDisplay();
            display.setCursor(20, 28);
            display.print(currentMode == ANALYSIS_MODE ? "Switching to Chart" : "Switching to RT");
            display.display();
            xSemaphoreGive(displayMutex);
          }
        }
      }
    }
    xTaskNotifyStateClear(NULL);
  }
}

void ledTask(void* pvParameters) {
  bool state = LOW;
  for (;;) {
    if (alarmActive && !isLowPowerMode) { // 休眠时不闪报警灯（根据需求可改）
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
  WiFi.setSleep(false); // 启动时保持最高性能
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
    
    //MDNS.begin(mdnsName);
    server.on("/", handleRoot);
    server.on("/data", []() {
    // 分配一块 256 字节的局部数组（在栈上，自动回收，无碎片）
    char jsonBuf[256];
    
    // 提取出当前值，避免多次调用函数
    float currentT = globalTemp; 
    float currentH = globalHum;
    
    snprintf(jsonBuf, sizeof(jsonBuf),
             "{\"temp\":\"%.1f °C\",\"hum\":\"%.1f %%RH\",\"time\":\"%s\",\"date\":\"%s\",\"tH\":%.1f,\"tL\":%.1f,\"hH\":%.1f,\"hL\":%.1f,\"isIdle\":%s}",
             currentT, currentH,
             getTimeString().c_str(), getDateString().c_str(),
             threshHighT, threshLowT, threshHighH, threshLowH,
             isLowPowerMode ? "true" : "false");

    server.send(200, "application/json", jsonBuf);
    });

    server.on("/set", HTTP_GET, []() {
      if (server.hasArg("th")) threshHighT = server.arg("th").toFloat();
      if (server.hasArg("tl")) threshLowT  = server.arg("tl").toFloat();
      if (server.hasArg("hh")) threshHighH = server.arg("hh").toFloat();
      if (server.hasArg("hl")) threshLowH  = server.arg("hl").toFloat();
      server.send(200, "text/plain", "OK");
    });

    server.on("/power", HTTP_GET, []() {
      if (server.hasArg("action")) {
        String a = server.arg("action");
        if (a == "toggle") {
          if (isLowPowerMode) exitLowPower();
          else enterLowPower();
        } else if (a == "enter") {
          enterLowPower();
        } else if (a == "exit") {
          exitLowPower();
        }
      }
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
      snprintf(timeStamp, sizeof(timeStamp), "ms:%lu", (unsigned long)millis());
    }

    if (!isnan(t) && !isnan(h)) {
      globalTemp = t;
      globalHum = h;

      // --- 自动休眠/唤醒核心逻辑 ---
      float diffT = fabsf(t - lastStableTemp);
      float diffH = fabsf(h - lastStableHum);

      if (lastChangeTime == 0) lastChangeTime = millis();

      if (diffT >= 1.0f || diffH >= 2.0f) {
        // 检测到变化 -> 活跃
        lastStableTemp = t;
        lastStableHum = h;
        lastChangeTime = millis();

        if (isLowPowerMode) {
          exitLowPower(); // 自动唤醒
          //Serial.println(">>> 检测到环境变化，自动唤醒");
        }
      } else {
        // 数据稳定，检查是否进入软休眠
        if (!isLowPowerMode && (millis() - lastChangeTime > AUTO_SLEEP_TIMEOUT)) {
          // 只有在 30 秒内 既没有数据大幅波动，也没有人摸过按键，才准休眠
          enterLowPower();
          //Serial.println(">>> 环境长期稳定，自动进入节能模式");
        }
      }
      // ----------------------------

      // 报警逻辑
      alarmActive = (t > threshHighT || t < threshLowT || h > threshHighH || h < threshLowH);

      // 环形缓冲区更新
      tempHistory[bufferIndex] = t;
      humHistory[bufferIndex] = h;
      bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
      if (bufferIndex == 0) bufferFull = true;
    }

    // SD 卡文件创建（使用 epoch 秒命名文件）和写入
    if (sdInitialized && !logStarted && now >= 1000000000) {
        char fname[32];
        // 严格使用 Epoch 秒命名
        snprintf(fname, sizeof(fname), "/%lu.csv", (unsigned long)now);
        
        logFileName = String(fname);
        File f = SD.open(logFileName.c_str(), FILE_WRITE);
        if (f) {
            f.println("timestamp,temperature,humidity");
            f.close();
            logStarted = true; 
            //Serial.printf("SD卡记录启动，文件名: %s\n", fname);
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

    // 屏幕刷新（休眠模式下跳过）
    if (!isLowPowerMode && !isnan(t) && !isnan(h)) {
      if (displayMutex && xSemaphoreTake(displayMutex, portMAX_DELAY) == pdTRUE) {
        if (currentMode == REALTIME_MODE) drawRealTimePage(t, h, hhmm);
        else if (currentMode == ANALYSIS_MODE) drawAnalysisPage();
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
  display.println("Ciallo World!");
  display.println("Init Systems...");
  display.display();

  displayMutex = xSemaphoreCreateMutex();

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
  // ================== 系统启动完成 ==================
  //Serial.println("\n**************************************");
  //Serial.println("Ciallo World! (∠・ω< )⌒☆");
  //Serial.println("Embedded System Initialized Successfully.");
  //Serial.println("**************************************\n");
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}