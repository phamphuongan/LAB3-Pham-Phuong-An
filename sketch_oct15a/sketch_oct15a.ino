
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ================= PIN =================
#define MQ2_PIN       34
#define ONE_WIRE_BUS  4
#define FAN_PIN       25
#define PUMP_PIN      26
#define BUZZER_PIN    27

// ================= NGUONG =================
const int GAS_THRESHOLD = 2000;
const float TEMP_THRESHOLD = 50.0;

const int BUZZER_FREQ = 2000;
const int BUZZER_RESOLUTION = 8;

// ================= WIFI =================
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// ================= MQTT =================
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// ================= TOPIC DU LIEU =================
const char* topic_gas = "viu/fire/gas";
const char* topic_temp = "viu/fire/temperature";
const char* topic_alarm = "viu/fire/alarm";

// ================= TOPIC TRANG THAI =================
const char* topic_fan_state = "viu/fire/fan/state";
const char* topic_pump_state = "viu/fire/pump/state";
const char* topic_mode_state = "viu/fire/mode";

// ================= TOPIC NHAN LENH =================
const char* topic_mode_cmd = "viu/fire/mode/set";
const char* topic_fan_cmd = "viu/fire/fan/set";
const char* topic_pump_cmd = "viu/fire/pump/set";

// ================= CAM BIEN =================
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ================= CAU TRUC DU LIEU =================
typedef struct {
  int gasValue;
  float temperature;
  bool tempValid;
} SensorData;

typedef struct {
  bool fan;
  bool pump;
  bool alarm;
  bool autoMode;
} DeviceStatus;

// ================= FREERTOS QUEUE =================
QueueHandle_t dataQueue;
QueueHandle_t alertQueue;
QueueHandle_t statusQueue;

// ================= TRANG THAI DUNG CHUNG =================
bool autoMode = true;
bool manualFan = false;
bool manualPump = false;

// Bao ve bien dung chung giua cac Core
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// ================= TRANG THAI THIET BI =================
bool fanState = false;
bool pumpState = false;
bool alarmState = false;

// ================= THOI GIAN =================
const unsigned long PUBLISH_INTERVAL = 2000;

// =================================================
// HAM DIEU KHIEN THIET BI
// Chi Task canh bao duoc dieu khien GPIO
// =================================================

void setFan(bool state) {
  fanState = state;
  digitalWrite(FAN_PIN, state ? HIGH : LOW);
}

void setPump(bool state) {
  pumpState = state;
  digitalWrite(PUMP_PIN, state ? HIGH : LOW);
}

void setAlarm(bool state) {
  alarmState = state;

  if (state) {
    ledcWriteTone(BUZZER_PIN, BUZZER_FREQ);
  } else {
    ledcWriteTone(BUZZER_PIN, 0);
  }
}

// =================================================
// MQTT CALLBACK
// Chi cap nhat lenh, khong dieu khien GPIO truc tiep
// =================================================

void mqttCallback(char* topic, byte* payload,
                  unsigned int length) {

  String message = "";

  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  message.trim();
  message.toUpperCase();

  Serial.print("MQTT nhan [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  portENTER_CRITICAL(&mux);

  if (strcmp(topic, topic_mode_cmd) == 0) {

    if (message == "AUTO") {
      autoMode = true;
    }

    else if (message == "MANUAL") {
      autoMode = false;
    }
  }

  else if (strcmp(topic, topic_fan_cmd) == 0) {

    if (message == "ON") {
      manualFan = true;
    }

    else if (message == "OFF") {
      manualFan = false;
    }
  }

  else if (strcmp(topic, topic_pump_cmd) == 0) {

    if (message == "ON") {
      manualPump = true;
    }

    else if (message == "OFF") {
      manualPump = false;
    }
  }

  portEXIT_CRITICAL(&mux);
}

// =================================================
// TASK 1: DOC DU LIEU CAM BIEN - CORE 1
// =================================================

void TaskReadData(void *pvParameters) {

  SensorData data;

  TickType_t lastWakeTime = xTaskGetTickCount();

  while (1) {

    // Doc gia tri MQ2
    data.gasValue = analogRead(MQ2_PIN);
    // Doc nhiet do DS18B20
    sensors.requestTemperatures();
    data.temperature = sensors.getTempCByIndex(0);
    data.tempValid =
      (data.temperature != DEVICE_DISCONNECTED_C);
    // Hien thi Serial
    Serial.println("========== CAM BIEN ==========");
    Serial.print("Gas ADC: ");
    Serial.println(data.gasValue);

    Serial.print("Nhiet do: ");

    if (data.tempValid) {
      Serial.print(data.temperature);
      Serial.println(" C");
    } else {
      Serial.println("Loi cam bien!");
    }
    // Gui du lieu vao dataQueue
    xQueueOverwrite(dataQueue, &data);
    // Gui du lieu moi nhat vao alertQueue
    xQueueOverwrite(alertQueue, &data);

    // Chu ky doc 1 giay
    vTaskDelayUntil(&lastWakeTime,
                    pdMS_TO_TICKS(1000));
  }
}

// =================================================
// TASK 2: TRUYEN DU LIEU MQTT - CORE 0
// =================================================

void TaskPublishData(void *pvParameters) {

  SensorData sensorData;
  DeviceStatus status;
  DeviceStatus lastSentStatus;

  bool haveSensorData = false;
  bool haveStatus = false;
  bool forceStatePublish = false;

  unsigned long lastPublish = 0;
  unsigned long lastWiFiAttempt = 0;
  unsigned long lastMQTTAttempt = 0;

  // Cau hinh MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(512);
  client.setKeepAlive(60);

  while (1) {

    // ========== KET NOI WIFI ==========
    if (WiFi.status() != WL_CONNECTED) {

      if (millis() - lastWiFiAttempt >= 5000) {

        lastWiFiAttempt = millis();

        Serial.println("Dang ket noi WiFi...");

        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid, password);
      }
    }

    // ========== KET NOI MQTT ==========
    if (WiFi.status() == WL_CONNECTED) {

      if (!client.connected()) {

        if (millis() - lastMQTTAttempt >= 3000) {

          lastMQTTAttempt = millis();

          Serial.println("Dang ket noi MQTT...");

          String clientId = "ESP32-FireGuard-";
          clientId += String((uint32_t)esp_random(), HEX);

          if (client.connect(clientId.c_str())) {

            Serial.println("MQTT da ket noi!");

            client.subscribe(topic_mode_cmd);
            client.subscribe(topic_fan_cmd);
            client.subscribe(topic_pump_cmd);

            forceStatePublish = true;

          } else {

            Serial.print("MQTT loi: ");
            Serial.println(client.state());
          }
        }
      }

      if (client.connected()) {
        client.loop();
      }
    }

    // ========== NHAN DU LIEU CAM BIEN ==========
    if (xQueueReceive(dataQueue, &sensorData, 0)
        == pdTRUE) {

      haveSensorData = true;
    }

    // ========== NHAN TRANG THAI THIET BI ==========
    if (xQueueReceive(statusQueue, &status, 0)
        == pdTRUE) {

      haveStatus = true;
    }

    // ========== GUI DU LIEU LEN MQTT ==========
    if (client.connected() &&
        millis() - lastPublish >= PUBLISH_INTERVAL) {

      lastPublish = millis();

      if (haveSensorData) {

        char gasBuffer[12];
        char tempBuffer[16];

        snprintf(gasBuffer, sizeof(gasBuffer),
                 "%d", sensorData.gasValue);

        snprintf(tempBuffer, sizeof(tempBuffer),
                 "%.2f", sensorData.temperature);

        client.publish(topic_gas, gasBuffer);

        if (sensorData.tempValid) {
          client.publish(topic_temp, tempBuffer);
        }

        Serial.println("Da gui du lieu cam bien len MQTT!");
      }
    }

    // ========== GUI TRANG THAI THIET BI ==========
    if (client.connected() && haveStatus) {

      bool changed = forceStatePublish;

      if (!forceStatePublish) {

        changed =
          (status.fan != lastSentStatus.fan) ||
          (status.pump != lastSentStatus.pump) ||
          (status.alarm != lastSentStatus.alarm) ||
          (status.autoMode != lastSentStatus.autoMode);
      }

      if (changed) {

        client.publish(topic_fan_state,
                       status.fan ? "ON" : "OFF");

        client.publish(topic_pump_state,
                       status.pump ? "ON" : "OFF");

        client.publish(topic_mode_state,
                       status.autoMode ? "AUTO" : "MANUAL");

        const char* alarmMessage = "NORMAL";

        if (status.alarm) {
          alarmMessage = "CANH BAO";
        }

        client.publish(topic_alarm, alarmMessage);

        lastSentStatus = status;
        forceStatePublish = false;

        Serial.println("Da cap nhat trang thai len Web!");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// =================================================
// TASK 3: CANH BAO VA DIEU KHIEN - CORE 1
// =================================================

void TaskAlert(void *pvParameters) {

  SensorData data;

  bool haveData = false;

  DeviceStatus status;

  // Trang thai ban dau
  status.fan = false;
  status.pump = false;
  status.alarm = false;
  status.autoMode = true;

  while (1) {

    // Nhan du lieu tu alertQueue
    if (xQueueReceive(alertQueue, &data,
                      pdMS_TO_TICKS(100)) == pdTRUE) {

      haveData = true;
    }

    // Doc lenh AUTO/MANUAL an toan
    bool currentMode;
    bool currentManualFan;
    bool currentManualPump;

    portENTER_CRITICAL(&mux);

    currentMode = autoMode;
    currentManualFan = manualFan;
    currentManualPump = manualPump;

    portEXIT_CRITICAL(&mux);

    // Kiem tra nguong canh bao
    bool gasExceeded = false;
    bool tempExceeded = false;

    if (haveData) {

      gasExceeded =
        (data.gasValue >= GAS_THRESHOLD);

      tempExceeded =
        (data.tempValid &&
         data.temperature >= TEMP_THRESHOLD);
    }

    // Xac dinh canh bao
    bool currentAlarm =
      gasExceeded || tempExceeded;

    // Dieu khien theo che do
    bool newFan;
    bool newPump;

    if (currentMode) {

      // CHE DO AUTO
      newFan = gasExceeded;
      newPump = tempExceeded;

    } else {

      // CHE DO MANUAL
      newFan = currentManualFan;
      newPump = currentManualPump;
    }

    // Dieu khien GPIO
    setFan(newFan);
    setPump(newPump);

    // Buzzer van hoat dong theo canh bao cam bien
    setAlarm(currentAlarm);

    // Cap nhat Queue trang thai
    status.fan = fanState;
    status.pump = pumpState;
    status.alarm = alarmState;
    status.autoMode = currentMode;

    xQueueOverwrite(statusQueue, &status);

    // Hien thi trang thai
    Serial.print("Alarm: ");
    Serial.print(alarmState ? "ON" : "OFF");

    Serial.print(" | Mode: ");
    Serial.print(currentMode ? "AUTO" : "MANUAL");

    Serial.print(" | Fan: ");
    Serial.print(fanState ? "ON" : "OFF");

    Serial.print(" | Pump: ");
    Serial.println(pumpState ? "ON" : "OFF");
  }
}

// =================================================
// SETUP
// =================================================

void setup() {

  Serial.begin(115200);

  // Khoi tao GPIO
  pinMode(MQ2_PIN, INPUT);
  pinMode(FAN_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);

  // Khoi tao PWM buzzer
  if (!ledcAttach(BUZZER_PIN, BUZZER_FREQ,
                  BUZZER_RESOLUTION)) {

    Serial.println("Loi khoi tao buzzer!");
  }

  digitalWrite(FAN_PIN, LOW);
  digitalWrite(PUMP_PIN, LOW);

  ledcWriteTone(BUZZER_PIN, 0);

  // Khoi tao ADC
  analogReadResolution(12);

  // Khoi tao cam bien nhiet do
  sensors.begin();

  // ========== TAO QUEUE ==========
  dataQueue = xQueueCreate(1, sizeof(SensorData));

  alertQueue = xQueueCreate(1, sizeof(SensorData));

  statusQueue = xQueueCreate(1, sizeof(DeviceStatus));

  if (dataQueue == NULL ||
      alertQueue == NULL ||
      statusQueue == NULL) {

    Serial.println("Loi tao Queue!");

    while (1) {
      delay(1000);
    }
  }

  Serial.println("==============================");
  Serial.println("HE THONG BAO CHAY FREE RTOS");
  Serial.println("==============================");

  // ========== TAO TASK DOC DATA ==========
  xTaskCreatePinnedToCore(
    TaskReadData,
    "TaskReadData",
    4096,
    NULL,
    2,
    NULL,
    1
  );

  // ========== TAO TASK TRUYEN DATA ==========
  xTaskCreatePinnedToCore(
    TaskPublishData,
    "TaskPublishData",
    8192,
    NULL,
    2,
    NULL,
    0
  );

  // ========== TAO TASK CANH BAO ==========
  xTaskCreatePinnedToCore(
    TaskAlert,
    "TaskAlert",
    4096,
    NULL,
    3,
    NULL,
    1
  );
}

// =================================================
// LOOP
// =================================================

void loop() {

  // Tat ca cong viec duoc thuc hien boi FreeRTOS
  vTaskDelay(portMAX_DELAY);
}
