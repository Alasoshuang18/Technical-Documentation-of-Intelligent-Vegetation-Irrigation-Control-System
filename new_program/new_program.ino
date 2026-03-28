#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>

// --- 状态枚举 ---
enum SystemStatus { free_state, WARNING, WATERING }; 
SystemStatus currentStatus = free_state;

// --- 引脚定义 ---
#define BUZZER_PIN    PB0
#define RELAY_PIN     PA8
#define LIGHT_PIN     PA0
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
int IRRIGATION_THRESHOLD = 30;
int WATER_DURATION_MIN = 30;       // 喷水时长（分钟）
int systemMode = 1;                // 1:湿度, 2:定时, 3:湿度+定时, 4:停止
int settingStep = 0;               // 设置步骤: 0:阈值, 1:时长

unsigned long warningStartTime = 0;
unsigned long wateringStartTime = 0;
unsigned long dailyTimer = 0;      // 模拟每日定时
unsigned long lastProcessTime = 0;

const unsigned long MS_PER_DAY = 24 * 3600 * 1000L; // 一天毫秒数
const unsigned long WARNING_MS = 60 * 1000L;        // 预警1分钟

unsigned long last_BUZZER_time = 0;
bool flag_Buzzer = false;

int DRY_VALUE = 1022;
int WET_VALUE = 480;
bool isSettingMode = false;
String inputBuffer = "";
int soilPercentages[4];
int avgMoisture = 0;
unsigned long now = millis();

void setup() 
{
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
  dailyTimer = millis(); // 初始化启动计时
}

void loop() 
{
  char key = keypad.getKey();
  if (key) handleKeypad(key);

  // 每300ms处理一次逻辑和显示，保证系统流畅
  if (millis() - lastProcessTime >= 300)
   {
      lastProcessTime = millis();
      readSensors();
      checkIrrigationLogic(); 
    
      if (!isSettingMode) updateDisplayMonitor();
      else updateDisplaySetting();
  }
}

// --- 核心逻辑控制 ---


//以下两个函数都是在空闲状态下才能执行
void startProcess() 
{
  // 只有在空闲状态下才能启动新流程
  if (currentStatus == free_state) 
  {
    currentStatus = WARNING;
    warningStartTime = millis();
    last_BUZZER_time = millis();
    flag_Buzzer = true;
    digitalWrite(BUZZER_PIN, HIGH); // 开启预警蜂鸣器
    Serial.println("Status: WARNING - Buzzer On");
  }
}

void stopProcess() 
{
  currentStatus = free_state;
  digitalWrite(RELAY_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  Serial.println("Status: free_state - All Off");
}

void checkIrrigationLogic() 
{
  now = millis();
  // 模式4为强制停止模式
  if (systemMode == 4) { stopProcess(); return; }
  // 1. 触发判断 (仅在空闲状态 free_state 检查)
  if (currentStatus == free_state)
   {
    bool sensorTrigger = ((systemMode == 1 || systemMode == 3) && avgMoisture < IRRIGATION_THRESHOLD);//avgMoisture是平均湿度
    bool timerTrigger = ((systemMode == 2 || systemMode == 3) && (now - dailyTimer >= MS_PER_DAY));
    
    if (sensorTrigger || timerTrigger) 
    {
        startProcess();
        return;
    }
  }

  // 2. 状态机自动流转 也是定时器开始进行判断工作
  if (currentStatus == WARNING) 
  {
    if (now - last_BUZZER_time >= 500)// 500ms 翻转一次
    { 
      last_BUZZER_time = now;
      flag_Buzzer = !flag_Buzzer;
      digitalWrite(BUZZER_PIN, flag_Buzzer ? HIGH : LOW);
    }

    // 检查预警是否结束（60秒）
    if (now - warningStartTime >= WARNING_MS) 
    {
      digitalWrite(BUZZER_PIN, LOW); // 预警结束，关闭蜂鸣器
      digitalWrite(RELAY_PIN, HIGH);
      currentStatus = WATERING;
      wateringStartTime = now;
      Serial.println("Status: WATERING - Relay On");
    }
  } 
  //固定时间喷水的，比如每天喷1次，时长一样
  //后期做改动，将第一个变为 纯湿度传感器 不受时间控制
    else if (currentStatus == WATERING)
    {
      bool flag = false;//检测是否达到阈值停止
      if(systemMode == 1)
      {
        if(IRRIGATION_THRESHOLD + 5 <= avgMoisture) //加点误差，防止测出来的值不稳经常去开关继电器
        {
            flag = true;
        }
      }

    else if (systemMode == 2) 
    {
      if (now - wateringStartTime >= (unsigned long)WATER_DURATION_MIN * 60 * 1000L) 
      {
        flag = true;
        dailyTimer = now; // 定时结束，开始重新计时浇水时间
      }
    }

    else if (systemMode == 3) 
    {
      bool timeReached = (now - wateringStartTime >= (unsigned long)WATER_DURATION_MIN * 60 * 1000L);
      bool moistureReached = (avgMoisture >= IRRIGATION_THRESHOLD);
      
      if (timeReached) 
      {
        if(!moistureReached)
        {
            wateringStartTime = now;
        }
        else 
        {
          stopProcess();
          flag = true;
          dailyTimer = now; // 任务彻底完成，重置 从现在开始计时
        }
      }
    }
    if (flag) 
    {
      stopProcess();
    }
  }
}

void handleKeypad(char key) 
{
  if (key == 'L') { stopProcess(); return; } // 一键停水
  if (key == 'R') { startProcess(); return; } // 一键喷水
  
  if (key == 'G') 
  { // 模式切换
    systemMode++; 
    if (systemMode > 4) systemMode = 1; 
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
      if (settingStep == 0) {
        if (inputBuffer.length() > 0) IRRIGATION_THRESHOLD = inputBuffer.toInt();
        inputBuffer = "";
        settingStep = 1; 
      } else {
        if (inputBuffer.length() > 0) WATER_DURATION_MIN = inputBuffer.toInt();
        isSettingMode = false;
      }
    }
  } 
  else if (isSettingMode && key >= '0' && key <= '9') {
    if (inputBuffer.length() < 3) inputBuffer += key;
  }
}

// --- 传感器读取与显示 ---

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
  
  display.print("MOD:"); display.print(systemMode);
  display.print(" LIM:"); display.print(IRRIGATION_THRESHOLD);
  display.print("% T:"); display.print(WATER_DURATION_MIN); display.println("m");

  display.setCursor(0, 12);
  display.print("AVG:"); display.print(avgMoisture); display.print("%  ST:");
  if (currentStatus == free_state) display.print("free");
  else if (currentStatus == WARNING) display.print("WARN");
  else display.print("WTR");

  for(int i=0; i<4; i++) {
    display.setCursor((i%2)*64, 25 + (i/2)*10);
    display.print("S"); display.print(i+1); display.print(":");
    display.print(soilPercentages[i]); display.print("%");
  }
  
  display.setCursor(0, 52);
  display.print("G:Mode L:Stop R:Run");
  display.display();
}

void updateDisplaySetting() {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("--- SETTING MODE ---");
  if (settingStep == 0) {
    display.println("Set Threshold (0-99%):");
  } else {
    display.println("Set Duration (min):");
  }
  display.setTextSize(2);
  display.setCursor(30, 30);
  display.print(inputBuffer); display.print("_");
  display.setTextSize(1);
  display.setCursor(0, 55);
  display.print("S:Next/Save  E:Exit");
  display.display();
}