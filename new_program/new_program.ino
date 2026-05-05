#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>

// --- 状态枚举 ---
enum SystemStatus { free_state, WARNING, WATERING }; 
SystemStatus currentStatus = free_state;

// --- 引脚定义 ---
#define BUZZER_PIN     PB0
#define RELAY_PIN      PA8
#define LIGHT_PIN      PA0
const int soilPins[4] = {PA1, PA2, PA3, PA4};

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// --- 键盘配置 ---
const byte ROWS = 5; 
const byte COLS = 4; 
char keys[ROWS][COLS] = {
  {'F', 'G', '#', '*'}, 
  {'1', '2', '3', 'U'}, 
  {'4', '5', '6', 'D'}, 
  {'7', '8', '9', 'E'}, 
  {'L', '0', 'R', 'S'}  // S: Ent, L: Stop, R: Manual Run, G: Mode
};
byte rowPins[ROWS] = {PB15, PB14, PB13, PB12, PB11}; 
byte colPins[COLS] = {PB4, PB3, PB9, PB8}; 
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// --- 系统变量 ---
int IRRIGATION_THRESHOLD = 30;     // 湿度阈值
int WATER_DURATION_MIN = 10;       // 喷水持续时长（分钟）
int INTERVAL_HOUR = 24;            // 喷水间隔周期（小时）- 新增

int systemMode = 1;                // 1:湿度, 2:定时, 3:湿度+定时, 4:停止
int settingStep = 0;               // 设置步骤: 0:阈值, 1:间隔小时, 2:时长

unsigned long warningStartTime = 0;
unsigned long wateringStartTime = 0;
unsigned long dailyTimer = 0;      
unsigned long lastProcessTime = 0;

const unsigned long WARNING_MS = 20 * 1000L;  // 预警20s
unsigned long last_BUZZER_time = 0;
bool flag_Buzzer = false;

int DRY_VALUE = 1022;
int WET_VALUE = 480;
bool isSettingMode = false;
String inputBuffer = "";
int soilPercentages[4];
int avgMoisture = 0;

void setup() {
  // 禁用 JTAG 释放 PB3, PB4
  RCC->APB2ENR |= RCC_APB2ENR_AFIOEN; 
  AFIO->MAPR |= AFIO_MAPR_SWJ_CFG_JTAGDISABLE; 

  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  dailyTimer = millis(); 
}

void loop() {
  char key = keypad.getKey();
  if (key) handleKeypad(key);

  if (millis() - lastProcessTime >= 300) {
    lastProcessTime = millis();
    readSensors();
    checkIrrigationLogic(); 
    
    if (!isSettingMode) updateDisplayMonitor();
    else updateDisplaySetting();
  }
}

// --- 核心逻辑 ---

void startProcess() {
  if (currentStatus == free_state) {
    currentStatus = WARNING;
    warningStartTime = millis();
    last_BUZZER_time = millis();
    flag_Buzzer = true;
    digitalWrite(BUZZER_PIN, HIGH);
  }
}

void stopProcess() {
  currentStatus = free_state;
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  // 注意：如果是定时触发结束，重置计时器在 checkIrrigationLogic 里处理更精准
}

void checkIrrigationLogic() {
  unsigned long now = millis();
  
  if (systemMode == 4) { stopProcess(); return; }

  // 1. 触发判断 (空闲态)
  if (currentStatus == free_state) {
    // 自动计算毫秒间隔，支持用户修改 INTERVAL_HOUR
    unsigned long targetIntervalMs = (unsigned long)INTERVAL_HOUR * 3600 * 1000L;
    
    bool sensorTrigger = ((systemMode == 1 || systemMode == 3) && avgMoisture < IRRIGATION_THRESHOLD);
    bool timerTrigger = ((systemMode == 2 || systemMode == 3) && (now - dailyTimer >= targetIntervalMs));
    
    if (sensorTrigger || timerTrigger) {
        startProcess();
        return;
    }
  }

  // 2. 状态机流转
  if (currentStatus == WARNING) {
    if (now - last_BUZZER_time >= 500) { 
      last_BUZZER_time = now;
      flag_Buzzer = !flag_Buzzer;
      digitalWrite(BUZZER_PIN, flag_Buzzer ? HIGH : LOW);
    }

    if (now - warningStartTime >= WARNING_MS) {
      digitalWrite(BUZZER_PIN, LOW); 
      digitalWrite(RELAY_PIN, HIGH);
      currentStatus = WATERING;
      wateringStartTime = now;
    }
  } 
  else if (currentStatus == WATERING) {
    bool shouldStop = false;

    if(systemMode == 1) { // 纯湿度模式
      if(avgMoisture >= (IRRIGATION_THRESHOLD + 5)) shouldStop = true;
    }
    else if (systemMode == 2) { // 纯定时模式
      if (now - wateringStartTime >= (unsigned long)WATER_DURATION_MIN * 60 * 1000L) {
        shouldStop = true;
        dailyTimer = now; // 任务完成后更新定时器起点
      }
    }
    else if (systemMode == 3) { // 混合模式
      bool timeReached = (now - wateringStartTime >= (unsigned long)WATER_DURATION_MIN * 60 * 1000L);
      bool moistureReached = (avgMoisture >= IRRIGATION_THRESHOLD);
      
      if (timeReached) {
        if(moistureReached) {
          shouldStop = true;
          dailyTimer = now; 
        } else {
          // 如果时间到了湿度还没够，重置 wateringStartTime 继续喷下一个周期（或者你可以让它一直喷）
          // 这里保持 RELAY 为 HIGH，等待湿度达标
        }
      }
    }

    if (shouldStop) stopProcess();
  }
}

void handleKeypad(char key) {
  if (key == 'L') { stopProcess(); return; }
  if (key == 'R') { startProcess(); return; }
  
  if (key == 'G') {
    systemMode++; 
    if (systemMode > 4) systemMode = 1; 
    dailyTimer = millis(); // 切换模式时重置计时
    return; 
  }

  if (key == 'F') { 
    isSettingMode = true; 
    settingStep = 0;
    inputBuffer = ""; 
  } 
  else if (key == 'E') { 
    isSettingMode = false; 
  } 
  else if (key == 'S') { 
    if (isSettingMode) {
      if (settingStep == 0) { // 设置湿度阈值
        if (inputBuffer.length() > 0) IRRIGATION_THRESHOLD = inputBuffer.toInt();
        inputBuffer = "";
        settingStep = 1; 
      } 
      else if (settingStep == 1) { // 设置间隔小时 (新增)
        if (inputBuffer.length() > 0) INTERVAL_HOUR = inputBuffer.toInt();
        inputBuffer = "";
        settingStep = 2;
      }
      else if (settingStep == 2) { // 设置喷水时长
        if (inputBuffer.length() > 0) WATER_DURATION_MIN = inputBuffer.toInt();
        isSettingMode = false;
        settingStep = 0;
      }
    }
  } 
  else if (isSettingMode && key >= '0' && key <= '9') {
    if (inputBuffer.length() < 3) inputBuffer += key;
  }
}

// --- 显示与传感器 ---

void readSensors() {
  long totalSum = 0;
  for (int i = 0; i < 4; i++) {
    int currentAdc = analogRead(soilPins[i]);
    int p = map(currentAdc, DRY_VALUE, WET_VALUE, 0, 100);
    soilPercentages[i] = constrain(p, 0, 100);
    totalSum += soilPercentages[i];
  }
  avgMoisture = totalSum / 4;
}

void updateDisplayMonitor() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  
  display.print("M:"); display.print(systemMode);
  display.print(" H:"); display.print(INTERVAL_HOUR); // 显示间隔小时
  display.print(" T:"); display.print(WATER_DURATION_MIN); display.println("m");

  display.setCursor(0, 12);
  display.print("AVG:"); display.print(avgMoisture); display.print("% TH:"); display.print(IRRIGATION_THRESHOLD);

  for(int i=0; i<4; i++) {
    display.setCursor((i%2)*64, 25 + (i/2)*10);
    display.print("S"); display.print(i+1); display.print(":");
    display.print(soilPercentages[i]); display.print("%");
  }
  
  display.setCursor(0, 52);
  if (currentStatus == free_state) display.print("State: FREE");
  else if (currentStatus == WARNING) display.print("State: !!WARN!!");
  else display.print("State: WATERING");
  
  display.display();
}

void updateDisplaySetting() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0,0);
  display.println("--- SETTING MODE ---");
  display.setCursor(0, 15);
  
  if (settingStep == 0) {
    display.println("1. Set Humidity %:");
    display.print("Current: "); display.println(IRRIGATION_THRESHOLD);
  } else if (settingStep == 1) {
    display.println("2. Set Interval (H):");
    display.print("Current: "); display.println(INTERVAL_HOUR);
  } else {
    display.println("3. Set Duration (M):");
    display.print("Current: "); display.println(WATER_DURATION_MIN);
  }
  
  display.setTextSize(2);
  display.setCursor(40, 35);
  display.print(inputBuffer); display.print("_");
  
  display.setTextSize(1);
  display.setCursor(0, 55);
  display.print("S:Next  E:Exit");
  display.display();
}