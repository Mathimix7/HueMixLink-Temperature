/*
  HUEMIXLINK V3 - PLUGGED-IN TEMPERATURE SENSOR WITH OLED
  Supports: ESP32
*/

#include "HueMixLink.h"
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <Adafruit_SHT31.h>
#include <math.h>
#include <ariblk16.h> 
#include <font8x8.h>
#include <Ticker.h>
#include <time.h>
#include <sys/time.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include "mbedtls/sha256.h"

#define PIN_RESET 14
#define PIN_LED 18
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22

#define LED_ACTIVE_HIGH HIGH

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR_PRIMARY 0x3C
#define OLED_ADDR_SECONDARY 0x3D
#define OLED_RESET -1

#define SHT31_ADDR 0x44

#define DEV_PLUGIN_MARKER 0xFE
#define PLUGIN_DEVICE_TYPE 0x00
#define PKT_TEMP_EVENT 0x55
#define PKT_TIME_REQUEST 0x56
#define PKT_TIME_RESPONSE 0x57

// 550e8400-e29b-41d4-a716-446655440000
static const uint8_t PLUGIN_UUID[16] = {
  0x55, 0x0e, 0x84, 0x00, 0xe2, 0x9b, 0x41, 0xd4,
  0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00
};

#define HELLO_INTERVAL_MS 5000
#define SENSOR_READ_INTERVAL_MS 2000
#define DISPLAY_INTERVAL_MS 1000
#define RESET_HOLD_MS 5000

Preferences prefs;
Adafruit_SH1106G display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
Adafruit_SH1106G displaySec(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
Adafruit_SHT31 sht31 = Adafruit_SHT31();

#define PKT_TEMP_CONFIG 0x58
#define PKT_TEMP_SECONDARY 0x59

// Configuration settings
uint8_t config_day_h = 7;
uint8_t config_day_m = 0;
uint8_t config_day_bright = 100;
uint8_t config_night_h = 21;
uint8_t config_night_m = 0;
uint8_t config_night_bright = 25;
char config_prim_name[6] = "";
uint8_t config_sec_enabled = 0;
char config_sec_name[6] = "";
uint8_t config_sec_mac[6] = {0};
uint32_t config_report_interval = 600;

// Secondary reading state (Core 0 writes)
volatile float sec_temp_c = NAN;
volatile float sec_humidity_pct = NAN;
volatile int sec_battery_percentage = 0;
volatile bool sec_data_new_pending = false;

// State tracking managed exclusively by Core 1 (main loop)
unsigned long sec_data_last_seen = 0;
bool sec_data_valid = false;

uint32_t HOME_ID = 0;
Payload_GatewayList gateways;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;

float currentTempC = NAN;
float currentHumidityPct = NAN;
float tempSum = 0.0f;
float humiditySum = 0.0f;
uint16_t sampleCount = 0;
uint16_t battery_mv = 0;

unsigned long lastHelloMs = 0;
unsigned long lastSensorReadMs = 0;
unsigned long lastDisplayMs = 0;
unsigned long lastTimeRequestMs = 0;
volatile bool ackReceived = false;
volatile bool helloRequestPending = false;
volatile bool pendingPairReading = false;

uint32_t lastMidnightSyncDay = 0;
bool timeSynced = false;
unsigned long lastTimeSyncAttemptMs = 0;
uint32_t lastReportBoundary = 0;

// --- LED BREATHING (for OTA) ---
#define LED_PWM_FREQ 5000
#define LED_PWM_RESOLUTION 8
Ticker breathingTicker;
int breathingDirection = 1;
int breathingBrightness = 0;
bool breathingActive = false;

// --- OTA STATE MACHINE ---
enum OtaState { OTA_IDLE, OTA_RECEIVING, OTA_VALIDATING, OTA_COMPLETE };
OtaState otaState = OTA_IDLE;
const esp_partition_t *update_partition = nullptr;
esp_ota_handle_t update_handle = 0;
mbedtls_sha256_context sha256_ctx;
uint32_t expected_firmware_size = 0;
uint32_t received_bytes = 0;
uint16_t expected_chunk_index = 0;
uint8_t expected_sha256[32];
unsigned long last_ota_activity = 0;
unsigned long ota_wake_time = 0;
bool ota_mode = false;

bool displayReady = false;
bool displaySecReady = false;
bool sensorReady = false;
bool showFahrenheit = false;
unsigned long lastUnitToggleMs = 0;

static void setLed(bool on) {
  digitalWrite(PIN_LED, on ? LED_ACTIVE_HIGH : !LED_ACTIVE_HIGH);
}

static void blinkLed(int times, int onMs, int offMs) {
  for (int i = 0; i < times; i++) {
    setLed(true);
    delay(onMs);
    setLed(false);
    delay(offMs);
  }
}

static void updateBreathing() {
  breathingBrightness += breathingDirection * 10;
  if (breathingBrightness >= 255) {
    breathingBrightness = 255;
    breathingDirection = -1;
  } else if (breathingBrightness <= 0) {
    breathingBrightness = 0;
    breathingDirection = 1;
  }

  if (LED_ACTIVE_HIGH) {
    ledcWrite(PIN_LED, breathingBrightness);
  } else {
    ledcWrite(PIN_LED, 255 - breathingBrightness);
  }
}

static void startLedBreathing() {
  ledcAttach(PIN_LED, LED_PWM_FREQ, LED_PWM_RESOLUTION);
  breathingBrightness = 0;
  breathingDirection = 1;
  breathingTicker.attach_ms(30, updateBreathing);
  breathingActive = true;
}

static void stopLedBreathing() {
  if (!breathingActive) return;
  breathingTicker.detach();
  ledcDetach(PIN_LED);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, !LED_ACTIVE_HIGH);
  breathingActive = false;
}

static void drawWifiIcon(Adafruit_SH1106G &disp, int x, int y) {
  disp.drawPixel(x + 0, y + 3, SH110X_WHITE);
  disp.drawPixel(x + 1, y + 2, SH110X_WHITE);
  disp.drawPixel(x + 2, y + 1, SH110X_WHITE);
  disp.drawLine(x + 3, y + 0, x + 10, y + 0, SH110X_WHITE);
  disp.drawPixel(x + 11, y + 1, SH110X_WHITE);
  disp.drawPixel(x + 12, y + 2, SH110X_WHITE);
  disp.drawPixel(x + 13, y + 3, SH110X_WHITE);
  disp.drawPixel(x + 2, y + 5, SH110X_WHITE);
  disp.drawPixel(x + 3, y + 4, SH110X_WHITE);
  disp.drawLine(x + 4, y + 3, x + 9, y + 3, SH110X_WHITE);
  disp.drawPixel(x + 10, y + 4, SH110X_WHITE);
  disp.drawPixel(x + 11, y + 5, SH110X_WHITE);
  disp.drawPixel(x + 4, y + 7, SH110X_WHITE);
  disp.drawPixel(x + 5, y + 6, SH110X_WHITE);
  disp.drawLine(x + 6, y + 6, x + 8, y + 6, SH110X_WHITE);
  disp.drawPixel(x + 9, y + 7, SH110X_WHITE);
  disp.fillRect(x + 6, y + 9, 2, 2, SH110X_WHITE);
}

static void drawTemperatureIcon(Adafruit_SH1106G &disp, int x, int y, int fill) {
  // Static Frame
  disp.drawLine(x + 3, y + 0, x + 5, y + 0, SH110X_WHITE);
  disp.drawLine(x + 2, y + 1, x + 2, y + 8, SH110X_WHITE);
  disp.drawLine(x + 6, y + 1, x + 6, y + 8, SH110X_WHITE);
  disp.drawPixel(x + 1, y + 9, SH110X_WHITE);
  disp.drawPixel(x + 7, y + 9, SH110X_WHITE);
  disp.drawPixel(x + 1, y + 10, SH110X_WHITE);
  disp.drawPixel(x + 7, y + 10, SH110X_WHITE);
  disp.drawPixel(x + 0, y + 11, SH110X_WHITE);
  disp.drawPixel(x + 8, y + 11, SH110X_WHITE);
  disp.drawPixel(x + 0, y + 12, SH110X_WHITE);
  disp.drawPixel(x + 8, y + 12, SH110X_WHITE);
  disp.drawPixel(x + 1, y + 13, SH110X_WHITE);
  disp.drawPixel(x + 7, y + 13, SH110X_WHITE);
  disp.drawPixel(x + 2, y + 14, SH110X_WHITE);
  disp.drawPixel(x + 6, y + 14, SH110X_WHITE);
  disp.drawLine(x + 3, y + 15, x + 5, y + 15, SH110X_WHITE);
  
  // Wind Representation
  disp.drawLine(x + 8, y + 2, x + 10, y + 2, SH110X_WHITE);
  disp.drawLine(x + 8, y + 4, x + 9, y + 4, SH110X_WHITE);
  disp.drawLine(x + 8, y + 6, x + 10, y + 6, SH110X_WHITE);

  // Corrected Fill Logic
  fill = constrain(fill, 0, 14);
  for (int i = 0; i < fill; i++) {
    int yy = y + 14 - i;
    int x_start, x_end;

    if (i == 0) { // Row y+14
      x_start = x + 3; x_end = x + 5;
    } else if (i == 1 || i == 4 || i == 5) { // Rows y+13, y+10, y+9
      x_start = x + 2; x_end = x + 6;
    } else if (i == 2 || i == 3) { // Rows y+12, y+11
      x_start = x + 1; x_end = x + 7;
    } else { // Rows y+8 up to y+1 (i >= 6)
      x_start = x + 3; x_end = x + 5;
    }

    disp.drawLine(x_start, yy, x_end, yy, SH110X_WHITE);
  }
}

static void drawHumidityIcon(Adafruit_SH1106G &disp, int x, int y) {
  disp.drawPixel(x + 7, y + 0, SH110X_WHITE);
  for (int i = 1; i <= 7; i++) {
    disp.drawPixel(x + 7 - i, y + i, SH110X_WHITE);
    disp.drawPixel(x + 7 + i, y + i, SH110X_WHITE);
  }
  disp.drawLine(x + 0, y + 7, x + 0, y + 11, SH110X_WHITE);
  disp.drawLine(x + 14, y + 7, x + 14, y + 11, SH110X_WHITE);
  disp.drawPixel(x + 1, y + 12, SH110X_WHITE);
  disp.drawPixel(x + 13, y + 12, SH110X_WHITE);
  disp.drawPixel(x + 2, y + 13, SH110X_WHITE);
  disp.drawPixel(x + 12, y + 13, SH110X_WHITE);
  disp.drawPixel(x + 3, y + 14, SH110X_WHITE);
  disp.drawPixel(x + 11, y + 14, SH110X_WHITE);
  disp.drawLine(x + 4, y + 15, x + 10, y + 15, SH110X_WHITE);
  disp.fillRect(x + 4, y + 7, 2, 2, SH110X_WHITE);
  disp.drawLine(x + 5, y + 11, x + 9, y + 7, SH110X_WHITE);
  disp.fillRect(x + 9, y + 10, 2, 2, SH110X_WHITE);
}

static void drawBatteryIcon(Adafruit_SH1106G &disp, int x, int y, int percentage) {
  disp.drawRect(x, y, 15, 10, SH110X_WHITE);
  
  disp.drawRect(x + 15, y + 3, 2, 4, SH110X_WHITE);

  if (percentage == 255) { // Not supported battery device
    // disp.drawLine(x + 2, y + 9, x + 13, y - 2, SH110X_WHITE);
    disp.fillRect(x + 6, y + 2, 3, 6, SH110X_WHITE); // Center block of the plug
    disp.drawPixel(x + 5, y + 3, SH110X_WHITE);       // Rounded top-left shoulder
    disp.drawPixel(x + 5, y + 6, SH110X_WHITE);       // Rounded bottom-left shoulder

    // 3. Base / Wire Guard (the small cylinder where the cord meets the plug)
    disp.fillRect(x + 4, y + 4, 1, 2, SH110X_WHITE);

    // 4. Cable / Wire
    disp.drawFastHLine(x + 2, y + 4, 2, SH110X_WHITE);
    disp.drawFastHLine(x + 2, y + 5, 2, SH110X_WHITE);

    // 5. Two Prongs (pointing right)
    disp.drawFastHLine(x + 9, y + 3, 3, SH110X_WHITE); // Top prong
    disp.drawFastHLine(x + 9, y + 6, 3, SH110X_WHITE); // Bottom prong
    return;
  }

  percentage = constrain(percentage, 0, 100);

  int fill_level = 0;
  if (percentage > 75) {
    fill_level = 4;
  } else if (percentage > 50) {
    fill_level = 3;
  } else if (percentage > 25) {
    fill_level = 2;
  } else if (percentage > 5) {
    fill_level = 1;
  }

  if (fill_level >= 1) disp.fillRect(x + 2,  y + 2, 2, 6, SH110X_WHITE);
  if (fill_level >= 2) disp.fillRect(x + 5,  y + 2, 2, 6, SH110X_WHITE);
  if (fill_level >= 3) disp.fillRect(x + 8,  y + 2, 2, 6, SH110X_WHITE);
  if (fill_level >= 4) disp.fillRect(x + 11, y + 2, 2, 6, SH110X_WHITE);
}

void drawText(Adafruit_SH1106G &disp, int x, int y, const char* text, uint16_t color) {
  int i = 0;
  while (text[i] != '\0') {
    char c = text[i];
    if (c >= 32 && c <= 127) {
      int fontIndex = (c - 32) * 8;
      for (int row = 0; row < 8; row++) {
        unsigned char rowByte = pgm_read_byte(&font8x8[fontIndex + row]);
        for (int col = 0; col < 8; col++) {
          if (rowByte & (0x80 >> col)) {
            disp.drawPixel(x + (i * 8) + col, y + row, color);
          }
        }
      }
    }
    i++;
  }
}

static void renderStatus(const char *line1, const char *line2 = nullptr) {
  if (!displayReady) return;
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(line1);
  if (line2) {
    display.setCursor(0, 16);
    display.println(line2);
  }
  display.display();
}

static uint32_t getCurrentEpochSeconds();
static void requestTimeSync();

static void renderSingleDisplay(Adafruit_SH1106G &disp, bool isReady, const char *name, float temp_c, float humidity_pct, const char *timeBuf, uint8_t contrast, int battery_pct = -1) {
  if (!isReady) return;

  disp.clearDisplay();
  disp.setContrast(contrast);
  disp.setTextColor(SH110X_WHITE);
  disp.drawLine(0, 14, 127, 14, SH110X_WHITE);

  disp.setTextSize(1);
  disp.setCursor(0, 0);
  
  if (HOME_ID != 0 && &disp == &display) {
    drawWifiIcon(disp, 114, 0);
  }

  if (battery_pct >= 0) {
    drawBatteryIcon(disp, 110, 0, battery_pct);
  }

  drawText(disp, 43, 1, timeBuf, SH110X_WHITE);

  if (strlen(name) > 0) {
    drawText(disp, 0, 1, name, SH110X_WHITE);
  }

  int fill = 0;
  if (!isnan(temp_c)) {
    fill = constrain((int)roundf(temp_c / 2.5f), 0, 14);
  }
  drawTemperatureIcon(disp, 30, 20, fill);
  
  disp.setFont(&ariblk16);
  disp.setTextColor(SH110X_WHITE);
  disp.setCursor(45, 33);
  
  if (isnan(temp_c)) {
    disp.printf("N/A %c", showFahrenheit ? 'F' : 'C');
  } else {
    float displayTempConv = showFahrenheit ? (temp_c * 9.0f / 5.0f + 32.0f) : temp_c;
    disp.printf("%.1f %c", displayTempConv, showFahrenheit ? 'F' : 'C');
  }

  drawHumidityIcon(disp, 27, 40);
  disp.setCursor(45, 54);
  if (isnan(humidity_pct)) {
    disp.print("N/A %");
  } else { 
    disp.printf("%.1f%%", humidity_pct);
  }
  disp.setFont();

  disp.display();
}

static void renderSecondaryInactive(Adafruit_SH1106G &disp, bool isReady) {
  if (!isReady) return;
  disp.clearDisplay();
  disp.display();
}

static void renderOtaStatus() {
  // Reuse the normal display layout, just replace wifi icon with "OTA"
  if (!displayReady && !displaySecReady) return;

  if (millis() - lastUnitToggleMs >= 5000) {
    showFahrenheit = !showFahrenheit;
    lastUnitToggleMs = millis();
  }

  uint8_t contrast = 255;
  char timeBuf[6] = "--:--";

  if (timeSynced) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    
    if (localtime_r(&now, &timeinfo)) {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
      
      // Calculate and apply display contrast schedule
      int current_minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
      int day_start_minutes = config_day_h * 60 + config_day_m;
      int night_start_minutes = config_night_h * 60 + config_night_m;
      
      bool is_day = false;
      if (day_start_minutes < night_start_minutes) {
        is_day = (current_minutes >= day_start_minutes && current_minutes < night_start_minutes);
      } else {
        is_day = (current_minutes >= day_start_minutes || current_minutes < night_start_minutes);
      }
      
      uint8_t current_brightness = is_day ? config_day_bright : config_night_bright;
      contrast = (current_brightness * 255) / 100;
    }

    uint32_t currentDay = (uint32_t)(now / 86400);
    if (currentDay != lastMidnightSyncDay) {
      lastMidnightSyncDay = currentDay;
      requestTimeSync();
    }
  }

  // Primary display
  if (displayReady) {
    display.clearDisplay();
    display.setContrast(contrast);
    display.setTextColor(SH110X_WHITE);
    display.drawLine(0, 14, 127, 14, SH110X_WHITE);

    display.setTextSize(1);
    display.setCursor(0, 0);

    // "OTA" where wifi icon was
    drawText(display, 104, 0, "OTA", SH110X_WHITE);
  
    // CENTER TEXT IN THE MIDDLE OF SCRREN
    char progressBuf[10];
    snprintf(progressBuf, sizeof(progressBuf), "%d%%", (int)roundf(((float)received_bytes * 100.0) / ((float)expected_firmware_size)));
    int textWidth = strlen(progressBuf) * 6;
    int xPosition = (128 - textWidth) / 2;
    drawText(display, xPosition, 1, progressBuf, SH110X_WHITE);

    if (strlen(config_prim_name) > 0) {
      drawText(display, 0, 1, config_prim_name, SH110X_WHITE);
    }

    int fill = 0;
    if (!isnan(currentTempC)) {
      fill = constrain((int)roundf(currentTempC / 2.5f), 0, 14);
    }
    drawTemperatureIcon(display, 30, 20, fill);

    display.setFont(&ariblk16);
    display.setCursor(45, 33);
    if (isnan(currentTempC)) {
      display.printf("N/A %c", showFahrenheit ? 'F' : 'C');
    } else {
      float displayTempConv = showFahrenheit ? (currentTempC * 9.0f / 5.0f + 32.0f) : currentTempC;
      display.printf("%.1f %c", displayTempConv, showFahrenheit ? 'F' : 'C');
    }

    drawHumidityIcon(display, 27, 40);
    display.setCursor(45, 54);
    if (isnan(currentHumidityPct)) {
      display.print("N/A %");
    } else {
      display.printf("%.1f%%", currentHumidityPct);
    }

    display.setFont();
    display.display();
  }

  // 2. Secondary Display (same as normal)
  if (displaySecReady) {
    if (!config_sec_enabled) {
      renderSecondaryInactive(displaySec, displaySecReady);
    } else {
      float temp = sec_data_valid ? sec_temp_c : NAN;
      float humid = sec_data_valid ? sec_humidity_pct : NAN;
      float battery = sec_data_valid ? sec_battery_percentage : -1;
      renderSingleDisplay(displaySec, displaySecReady, config_sec_name, temp, humid, timeBuf, contrast, battery);
    }
  }
}


static void renderDisplay() {
  if (!displayReady && !displaySecReady) return;

  if (millis() - lastUnitToggleMs >= 5000) {
    showFahrenheit = !showFahrenheit;
    lastUnitToggleMs = millis();
  }

  char timeBuf[6] = "--:--";
  uint8_t contrast = 255;

  if (timeSynced) {
    time_t now;
    time(&now);
    struct tm timeinfo;
    
    if (localtime_r(&now, &timeinfo)) {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
      
      // Calculate and apply display contrast schedule
      int current_minutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
      int day_start_minutes = config_day_h * 60 + config_day_m;
      int night_start_minutes = config_night_h * 60 + config_night_m;
      
      bool is_day = false;
      if (day_start_minutes < night_start_minutes) {
        is_day = (current_minutes >= day_start_minutes && current_minutes < night_start_minutes);
      } else {
        is_day = (current_minutes >= day_start_minutes || current_minutes < night_start_minutes);
      }
      
      uint8_t current_brightness = is_day ? config_day_bright : config_night_bright;
      contrast = (current_brightness * 255) / 100;
    }

    uint32_t currentDay = (uint32_t)(now / 86400);
    if (currentDay != lastMidnightSyncDay) {
      lastMidnightSyncDay = currentDay;
      requestTimeSync();
    }
  }

  // 1. Render Primary Display (always local sensor data)
  renderSingleDisplay(display, displayReady, config_prim_name, currentTempC, currentHumidityPct, timeBuf, contrast);

  // 2. Render Secondary Display
  if (displaySecReady) {
    if (!config_sec_enabled) {
      renderSecondaryInactive(displaySec, displaySecReady);
    } else {
      float temp = sec_data_valid ? sec_temp_c : NAN;
      float humid = sec_data_valid ? sec_humidity_pct : NAN;
      float battery = sec_data_valid ? sec_battery_percentage : -1;
      renderSingleDisplay(displaySec, displaySecReady, config_sec_name, temp, humid, timeBuf, contrast, battery);
    }
  }
}

static void saveGateways() {
  prefs.putBytes("gw", &gateways, sizeof(gateways));
}

static void addGateway(const uint8_t *mac) {
  for (int i = 0; i < gateways.count; i++) {
    if (memcmp(gateways.macs[i], mac, 6) == 0) return;
  }
  if (gateways.count < MAX_GATEWAYS) {
    memcpy(gateways.macs[gateways.count], mac, 6);
    gateways.count++;
    saveGateways();
    Serial.printf("[TEMP-OLED] Added gateway, count=%d\n", gateways.count);
  }
}

static void ensurePeer(const uint8_t *mac) {
  if (!esp_now_is_peer_exist(mac)) {
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_err_t res = esp_now_add_peer(&peerInfo);
    Serial.printf("[TEMP-OLED] add_peer %02X:%02X:%02X:%02X:%02X:%02X -> %d\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], res);
  }
}

static bool sendPacketToTarget(const uint8_t *targetMac, HueMixLinkPacket &pkt) {
  ensurePeer(targetMac);
  return esp_now_send(targetMac, reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
}

static bool sendPacketToGateways(HueMixLinkPacket &pkt) {
  if (gateways.count == 0) {
    Serial.printf("[TEMP-OLED] No gateways for packet 0x%02X\n", pkt.type);
    return false;
  }

  for (int i = 0; i < gateways.count; i++) {
    if (sendPacketToTarget(gateways.macs[i], pkt)) {
      if (i > 0) {
        uint8_t tempMac[6];
        memcpy(tempMac, gateways.macs[i], 6);
        for (int j = i; j > 0; j--) memcpy(gateways.macs[j], gateways.macs[j - 1], 6);
        memcpy(gateways.macs[0], tempMac, 6);
        saveGateways();
      }
      return true;
    }
  }

  Serial.printf("[TEMP-OLED] Failed to queue packet 0x%02X to any gateway\n", pkt.type);
  return false;
}

static bool sendPacketToGatewaysWithAck(HueMixLinkPacket &pkt) {
  if (gateways.count == 0) {
    Serial.printf("[TEMP-OLED][ACK] No gateways for packet 0x%02X\n", pkt.type);
    return false;
  }

  ackReceived = false;
  int successfulGatewayIndex = -1;
  Serial.printf("[TEMP-OLED][ACK] Sending packet 0x%02X to %d gateway(s)\n", pkt.type, gateways.count);

  for (int i = 0; i < gateways.count; i++) {
    Serial.printf("[TEMP-OLED][ACK] Attempt %d/%d via %02X:%02X:%02X:%02X:%02X:%02X\n",
                  i + 1, gateways.count,
                  gateways.macs[i][0], gateways.macs[i][1], gateways.macs[i][2],
                  gateways.macs[i][3], gateways.macs[i][4], gateways.macs[i][5]);

    ensurePeer(gateways.macs[i]);
    esp_err_t sendResult = esp_now_send(gateways.macs[i], reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
    if (sendResult != ESP_OK) {
      Serial.printf("[TEMP-OLED][ACK] esp_now_send failed on gateway index %d: %d\n", i, sendResult);
      continue;
    }

    unsigned long waitStart = millis();
    while (millis() - waitStart < 150 && !ackReceived) delay(1);

    if (ackReceived) {
      Serial.printf("[TEMP-OLED][ACK] ACK received from gateway index %d\n", i);
      successfulGatewayIndex = i;
      break;
    }
    Serial.printf("[TEMP-OLED][ACK] No ACK from gateway index %d\n", i);
  }

  if (successfulGatewayIndex < 0) {
    Serial.printf("[TEMP-OLED][ACK] Packet 0x%02X not acknowledged\n", pkt.type);
    return false;
  }

  if (successfulGatewayIndex > 0) {
    Serial.printf("[TEMP-OLED][ACK] Promoting gateway index %d to front\n", successfulGatewayIndex);
    uint8_t tempMac[6];
    memcpy(tempMac, gateways.macs[successfulGatewayIndex], 6);
    for (int j = successfulGatewayIndex; j > 0; j--) memcpy(gateways.macs[j], gateways.macs[j - 1], 6);
    memcpy(gateways.macs[0], tempMac, 6);
    saveGateways();
  }

  return true;
}

static void fillHelloPacket(HueMixLinkPacket &pkt) {
  memset(&pkt, 0, sizeof(HueMixLinkPacket));
  pkt.type = PKT_HELLO;
  pkt.msgID = 0;
  WiFi.macAddress(pkt.sourceMAC);
  pkt.payload.raw[0] = DEV_PLUGIN_MARKER;
  pkt.payload.raw[1] = 0;
  memcpy(&pkt.payload.raw[2], PLUGIN_UUID, 16);
  pkt.payload.raw[18] = PLUGIN_DEVICE_TYPE;

  uint8_t major = 0, minor = 0, patch = 0;
#ifdef FIRMWARE_VERSION
  sscanf(FIRMWARE_VERSION, "%hhu.%hhu.%hhu", &major, &minor, &patch);
#endif
  pkt.payload.raw[19] = major;
  pkt.payload.raw[20] = minor;
  pkt.payload.raw[21] = patch;
  pkt.payload.raw[22] = 0;
  pkt.signature = calculateHash(pkt.payload.raw, 185, HOME_ID);
}

static void fillTemperaturePacket(HueMixLinkPacket &pkt, float temperatureC, float humidityPct) {
  memset(&pkt, 0, sizeof(HueMixLinkPacket));
  pkt.type = PKT_TEMP_EVENT;
  pkt.msgID = 0;
  WiFi.macAddress(pkt.sourceMAC);

  int16_t temp_x10 = (int16_t)lroundf(temperatureC * 10.0f);
  uint16_t humidity_x10 = isnan(humidityPct) ? 0xFFFF : (uint16_t)lroundf(humidityPct * 10.0f);

  memcpy(&pkt.payload.raw[0], &temp_x10, sizeof(temp_x10));
  memcpy(&pkt.payload.raw[2], &humidity_x10, sizeof(humidity_x10));
  memcpy(&pkt.payload.raw[4], &battery_mv, sizeof(battery_mv));
  pkt.payload.raw[6] = 0x01;  // plugged-in/display variant
  pkt.signature = calculateHash(pkt.payload.raw, 185, HOME_ID);
}

static void fillTimeRequestPacket(HueMixLinkPacket &pkt) {
  memset(&pkt, 0, sizeof(HueMixLinkPacket));
  pkt.type = PKT_TIME_REQUEST;
  pkt.msgID = 0;
  WiFi.macAddress(pkt.sourceMAC);
  pkt.signature = calculateHash(pkt.payload.raw, 185, HOME_ID);
}

// ============================================================
// OTA FUNCTIONS
// ============================================================

static void abortOta(const char* reason) {
  Serial.printf("[OTA] ABORT: %s\n", reason);
  if (update_handle) {
    esp_ota_abort(update_handle);
    update_handle = 0;
  }
  otaState = OTA_IDLE;
  expected_chunk_index = 0;
  received_bytes = 0;
  ota_mode = false;
  stopLedBreathing();
  blinkLed(3, 100, 100);
}

static void handleOtaNotify(HueMixLinkPacket* pkt) {
  if (otaState != OTA_IDLE) {
    Serial.println("[OTA] Already in OTA, ignoring duplicate NOTIFY");
    return;
  }

  Serial.println("[OTA] NOTIFY received, starting OTA...");
  ota_mode = true;
  ota_wake_time = millis();

  expected_firmware_size = pkt->payload.otaNotify.firmware_size;
  memcpy(expected_sha256, pkt->payload.otaNotify.sha256_hash, 32);

  update_partition = esp_ota_get_next_update_partition(NULL);
  if (!update_partition) {
    Serial.println("[OTA] No update partition available");
    abortOta("No partition");
    return;
  }

  Serial.printf("[OTA] Starting update: %u bytes\n", expected_firmware_size);

  esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
  if (err != ESP_OK) {
    Serial.printf("[OTA] Begin failed: %d\n", err);
    abortOta("Begin failed");
    return;
  }

  mbedtls_sha256_init(&sha256_ctx);
  mbedtls_sha256_starts(&sha256_ctx, 0);

  otaState = OTA_RECEIVING;
  expected_chunk_index = 0;
  received_bytes = 0;
  last_ota_activity = millis();

  Serial.println("[OTA] Ready to receive chunks");
  startLedBreathing();

  // Send OTA_READY response with actual firmware size to confirm readiness
  HueMixLinkPacket ready;
  memset(&ready, 0, sizeof(HueMixLinkPacket));
  ready.type = PKT_OTA_READY;
  WiFi.macAddress(ready.sourceMAC);
  memset(ready.targetMAC, 0xFF, 6);
  ready.payload.otaReady.firmware_size = expected_firmware_size;
  ready.signature = calculateHash(ready.payload.raw, 185, HOME_ID);

  bool sent = false;
  for (int i = 0; i < gateways.count; i++) {
    ensurePeer(gateways.macs[i]);
    if (esp_now_send(gateways.macs[i], (uint8_t*)&ready, sizeof(ready)) == ESP_OK) {
      sent = true;
      Serial.printf("[OTA] Sent OTA_READY response via gateway %d\n", i);
      if (i > 0) {
        uint8_t tempMac[6];
        memcpy(tempMac, gateways.macs[i], 6);
        for (int j = i; j > 0; j--) {
          memcpy(gateways.macs[j], gateways.macs[j-1], 6);
        }
        memcpy(gateways.macs[0], tempMac, 6);
        saveGateways();
      }
      break;
    }
  }

  if (!sent) {
    Serial.println("[OTA] Failed to send OTA_READY to any gateway");
  }
}

static void handleOtaChunk(HueMixLinkPacket* pkt) {
  if (otaState != OTA_RECEIVING) return;

  last_ota_activity = millis();

  uint16_t chunk_idx = pkt->payload.otaChunk.chunk_index;
  uint8_t data_len = pkt->payload.otaChunk.data_len;

  if (chunk_idx < expected_chunk_index) {
    Serial.printf("[OTA] Ignoring duplicate chunk %d (already at %d)\n", chunk_idx, expected_chunk_index);
    return;
  }

  if (chunk_idx != expected_chunk_index) {
    Serial.printf("[OTA] Ignoring out-of-order chunk %d (expecting %d)\n", chunk_idx, expected_chunk_index);
    return;
  }

  esp_err_t err = esp_ota_write(update_handle, pkt->payload.otaChunk.data, data_len);
  if (err != ESP_OK) {
    Serial.printf("[OTA] Write failed at chunk %d: %d\n", chunk_idx, err);
    abortOta("Write failed");
    return;
  }

  mbedtls_sha256_update(&sha256_ctx, pkt->payload.otaChunk.data, data_len);

  received_bytes += data_len;
  expected_chunk_index++;

  if (chunk_idx % 50 == 0) {
    Serial.printf("[OTA] Progress: %u / %u bytes (%.1f%%)\n",
      received_bytes, expected_firmware_size,
      (received_bytes * 100.0) / expected_firmware_size);
  }
}

static void handleOtaComplete(HueMixLinkPacket* pkt) {
  if (otaState != OTA_RECEIVING) return;

  Serial.println("[OTA] COMPLETE received, validating...");
  otaState = OTA_VALIDATING;

  uint8_t calculated_sha256[32];
  mbedtls_sha256_finish(&sha256_ctx, calculated_sha256);
  mbedtls_sha256_free(&sha256_ctx);

  if (memcmp(calculated_sha256, expected_sha256, 32) != 0) {
    Serial.println("[OTA] SHA256 MISMATCH!");
    abortOta("SHA256 mismatch");
    return;
  }

  Serial.println("[OTA] SHA256 verified!");

  esp_err_t err = esp_ota_end(update_handle);
  if (err != ESP_OK) {
    Serial.printf("[OTA] End failed: %d\n", err);
    abortOta("End failed");
    return;
  }
  update_handle = 0;

  err = esp_ota_set_boot_partition(update_partition);
  if (err != ESP_OK) {
    Serial.printf("[OTA] Set boot partition failed: %d\n", err);
    abortOta("Set boot failed");
    return;
  }

  Serial.println("[OTA] UPDATE SUCCESSFUL! Rebooting...");
  otaState = OTA_COMPLETE;

  stopLedBreathing();
  blinkLed(10, 100, 100);
  ESP.restart();
}

static void handleOtaAbort(HueMixLinkPacket* pkt) {
  Serial.println("[OTA] ABORT received from server");
  abortOta("Server abort");
}

static void handleOtaCheckpointReq(HueMixLinkPacket* pkt) {
  if (otaState != OTA_RECEIVING) return;

  uint16_t last_chunk = (expected_chunk_index > 0) ? (expected_chunk_index - 1) : 0;
  Serial.printf("[OTA] Checkpoint request - last chunk received: %d\n", last_chunk);

  HueMixLinkPacket ack;
  memset(&ack, 0, sizeof(HueMixLinkPacket));
  ack.type = PKT_OTA_CHUNK_ACK;
  WiFi.macAddress(ack.sourceMAC);
  memset(ack.targetMAC, 0, 6);
  ack.msgID = 0;
  ack.payload.otaChunkAck.last_chunk_index = last_chunk;
  ack.signature = calculateHash(ack.payload.raw, 185, HOME_ID);

  for (int i = 0; i < gateways.count; i++) {
    ensurePeer(gateways.macs[i]);
    if (esp_now_send(gateways.macs[i], (uint8_t*)&ack, sizeof(ack)) == ESP_OK) {
      Serial.printf("[OTA] Sent checkpoint ACK via gateway %d: last_chunk=%d\n", i, last_chunk);
      if (i > 0) {
        uint8_t tempMac[6];
        memcpy(tempMac, gateways.macs[i], 6);
        for (int j = i; j > 0; j--) {
          memcpy(gateways.macs[j], gateways.macs[j-1], 6);
        }
        memcpy(gateways.macs[0], tempMac, 6);
        saveGateways();
      }
      break;
    }
  }
}

static uint32_t getCurrentEpochSeconds() {
  if (!timeSynced) return 0;
  time_t now;
  time(&now);
  return (uint32_t)now;
}

static bool sendTimeRequest() {
  HueMixLinkPacket pkt;
  fillTimeRequestPacket(pkt);
  Serial.println("[TEMP-OLED] Sending time request");
  lastTimeRequestMs = millis();
  return sendPacketToGatewaysWithAck(pkt);
}

static void requestTimeSync() {
  if (HOME_ID == 0) return;
  if (sendTimeRequest()) {
    Serial.println("[TEMP-OLED] Time request queued");
  } else {
    Serial.println("[TEMP-OLED] Time request failed");
  }
}

static bool sendHello(bool broadcastIfUnpaired) {
  HueMixLinkPacket pkt;
  fillHelloPacket(pkt);

  if (HOME_ID == 0 && broadcastIfUnpaired) {
    Serial.println("[TEMP-OLED] Broadcasting HELLO");
    ensurePeer(broadcastAddress);
    return esp_now_send(broadcastAddress, reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
  }

  Serial.println("[TEMP-OLED] Sending HELLO to gateway list");
  return sendPacketToGateways(pkt);
}

static bool readSensor() {
  if (!sensorReady) return false;

  float t = sht31.readTemperature();
  float h = sht31.readHumidity();
  if (isnan(t) || isnan(h)) {
    Serial.println("[TEMP-OLED] Failed to read SHT31D");
    return false;
  }

  currentTempC = t;
  currentHumidityPct = h;

  // Only accumulate samples for averaging if paired and synchronized
  if (HOME_ID != 0 && timeSynced) {
    tempSum += t;
    humiditySum += h;
    if (sampleCount < 60000) sampleCount++;
  } else {
    // Keep sum values empty when offline/unpaired to prevent precision errors
    tempSum = 0.0f;
    humiditySum = 0.0f;
    sampleCount = 0;
  }
  
  Serial.printf("[TEMP-OLED] SHT31D %.2fC %.2f%% samples=%u\n", t, h, sampleCount);
  return true;
}

static bool sendTemperatureReading() {
  if (sampleCount == 0) {
    Serial.println("[TEMP-OLED] No sensor samples to send");
    return false;
  }

  float avgTemp = tempSum / sampleCount;
  float avgHumidity = humiditySum / sampleCount;
  HueMixLinkPacket pkt;
  fillTemperaturePacket(pkt, avgTemp, avgHumidity);

  Serial.printf("[TEMP-OLED] Reporting avg %.1fC %.1f%% from %u sample(s)\n", avgTemp, avgHumidity, sampleCount);
  bool delivered = sendPacketToGatewaysWithAck(pkt);
  Serial.printf("[TEMP-OLED][ACK] Temperature packet %s\n", delivered ? "delivered" : "not acknowledged");

  if (delivered) {
    tempSum = 0.0f;
    humiditySum = 0.0f;
    sampleCount = 0;
  }
  return delivered;
}

static void updateGatewayList(const Payload_GatewayList *newList) {
  bool membershipChanged = false;

  if (gateways.count != newList->count) {
    membershipChanged = true;
  } else {
    // Check if every gateway in the new list already exists in our local list
    for (int i = 0; i < newList->count; i++) {
      bool found = false;
      for (int j = 0; j < gateways.count; j++) {
        if (memcmp(gateways.macs[j], newList->macs[i], 6) == 0) {
          found = true;
          break;
        }
      }
      if (!found) {
        membershipChanged = true;
        break;
      }
    }
  }

  if (membershipChanged) {
    memcpy(&gateways, newList, sizeof(Payload_GatewayList));
    saveGateways();
    Serial.printf("[TEMP-OLED] Gateway membership updated, new count=%d\n", gateways.count);
  } else {
    Serial.println("[TEMP-OLED] Gateway list received, but membership is unchanged. Preserving sorted order.");
  }
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(HueMixLinkPacket)) return;

  HueMixLinkPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  const uint8_t *mac = info->src_addr;

  if (pkt.type == PKT_PAIR_CONFIRM) {
    const uint32_t sig = calculateHash(pkt.payload.raw, 185, 0);
    if (pkt.signature != sig) {
      Serial.println("[TEMP-OLED] Rejected pair confirm with invalid signature");
      return;
    }

    if (HOME_ID == 0) {
      HOME_ID = pkt.payload.pair.newHomeID;
      prefs.putUInt("hid", HOME_ID);
      Serial.printf("[TEMP-OLED] Paired! HOME_ID=0x%08X\n", HOME_ID);
      blinkLed(2, 100, 100);
      pendingPairReading = true;
      helloRequestPending = true;
    } else {
      Serial.println("[TEMP-OLED] Pair confirm received while already paired");
      helloRequestPending = true;
    }
    addGateway(mac);
    requestTimeSync();
    lastHelloMs = 0;
  } else if (pkt.type == PKT_TIME_RESPONSE) {
     if (HOME_ID != 0) {
      uint32_t expectedSig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expectedSig) {
        Serial.println("[TEMP-OLED] Rejected time response with invalid signature");
        return;
      }
    }

    uint32_t epochSeconds = 0;
    int32_t tzOffsetSeconds = 0;
    
    // Parse UTC time (bytes 0-3) and timezone offset in seconds (bytes 4-7)
    memcpy(&epochSeconds, &pkt.payload.raw[0], sizeof(epochSeconds));
    memcpy(&tzOffsetSeconds, &pkt.payload.raw[4], sizeof(tzOffsetSeconds));
    
    if (epochSeconds == 0) {
      Serial.println("[TEMP-OLED] Received invalid epoch in time response");
      return;
    }

    // Convert offset in seconds to POSIX GMT format (West is +, East is -)
    int32_t tzHoursWest = -tzOffsetSeconds / 3600;
    int32_t tzMinutesWest = abs(tzOffsetSeconds % 3600) / 60;
    
    char tzStr[32];
    if (tzMinutesWest == 0) {
      snprintf(tzStr, sizeof(tzStr), "GMT%+d", tzHoursWest);
    } else {
      snprintf(tzStr, sizeof(tzStr), "GMT%+d:%02d", tzHoursWest, tzMinutesWest);
    }
    
    // Apply timezone dynamically to system
    setenv("TZ", tzStr, 1);
    tzset();

    timeSynced = true;
    lastMidnightSyncDay = epochSeconds / 86400;

    // Synchronize internal system clock
    struct timeval tv = { .tv_sec = (time_t)epochSeconds, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    Serial.printf("[TEMP-OLED] Time synced: epoch=%u offset=%d (%s)\n", epochSeconds, tzOffsetSeconds, tzStr);
  } else if (pkt.type == PKT_ACK_TO_BTN) {
    Serial.printf("[TEMP-OLED][ACK] Received PKT_ACK_TO_BTN from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (HOME_ID != 0) {
      uint32_t expectedSig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expectedSig) {
        Serial.println("[TEMP-OLED][ACK] Rejected ACK with invalid signature");
        return;
      }
    }
    if (pkt.payload.gwList.count > 0) {
      updateGatewayList(&pkt.payload.gwList);
    } else {
      Serial.println("[TEMP-OLED][ACK] ACK contained empty gateway list");
    }
    ackReceived = true;
  } else if (pkt.type == PKT_GW_LIST_UPD) {
    if (HOME_ID != 0) {
      uint32_t expectedSig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expectedSig) {
        Serial.println("[TEMP-OLED] Rejected gateway list with invalid signature");
        return;
      }
    }
    if (pkt.payload.gwList.count > 0) {
      updateGatewayList(&pkt.payload.gwList);
    } else {
      Serial.println("[TEMP-OLED] Gateway list update contained no gateways");
    }
  } else if (pkt.type == PKT_TEMP_CONFIG) {
    if (HOME_ID != 0) {
      uint32_t expectedSig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expectedSig) {
        Serial.println("[TEMP-OLED] Rejected config with invalid signature");
        return;
      }
    }
    
    uint8_t day_h = pkt.payload.raw[0];
    uint8_t day_m = pkt.payload.raw[1];
    uint8_t day_bright = pkt.payload.raw[2];
    uint8_t night_h = pkt.payload.raw[3];
    uint8_t night_m = pkt.payload.raw[4];
    uint8_t night_bright = pkt.payload.raw[5];
    
    char prim_name[6];
    memcpy(prim_name, &pkt.payload.raw[6], 5);
    prim_name[5] = '\0';
    
    uint8_t sec_enabled = pkt.payload.raw[11];
    
    char sec_name[6];
    memcpy(sec_name, &pkt.payload.raw[12], 5);
    sec_name[5] = '\0';
    
    uint8_t sec_mac[6];
    memcpy(sec_mac, &pkt.payload.raw[17], 6);
    
    // Read report interval from bytes 23-26 (uint32 LE)
    uint32_t report_interval;
    memcpy(&report_interval, &pkt.payload.raw[23], sizeof(report_interval));

    Serial.printf("[TEMP-OLED] Received config: Day=%02d:%02d (%d%%), Night=%02d:%02d (%d%%), Primary='%s', Sec=%d '%s', Interval=%u\n",
                  day_h, day_m, day_bright, night_h, night_m, night_bright, prim_name, sec_enabled, sec_name, report_interval);
                 
    config_day_h = day_h;
    config_day_m = day_m;
    config_day_bright = day_bright;
    config_night_h = night_h;
    config_night_m = night_m;
    config_night_bright = night_bright;
    strcpy(config_prim_name, prim_name);
    config_sec_enabled = sec_enabled;
    strcpy(config_sec_name, sec_name);
    memcpy(config_sec_mac, sec_mac, 6);
    
    if (report_interval >= 60 && report_interval <= 86400) {
      config_report_interval = report_interval;
    }

    prefs.putUChar("day_h", day_h);
    prefs.putUChar("day_m", day_m);
    prefs.putUChar("day_bright", day_bright);
    prefs.putUChar("night_h", night_h);
    prefs.putUChar("night_m", night_m);
    prefs.putUChar("night_bright", night_bright);
    prefs.putString("prim_name", prim_name);
    prefs.putUChar("sec_enabled", sec_enabled);
    prefs.putString("sec_name", sec_name);
    prefs.putBytes("sec_mac", sec_mac, 6);
    prefs.putUInt("rpt_int", config_report_interval);
  } else if (pkt.type == PKT_TEMP_SECONDARY) {
    if (HOME_ID != 0) {
      uint32_t expectedSig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expectedSig) {
        Serial.println("[TEMP-OLED] Rejected secondary reading with invalid signature");
        return;
      }
    }
    
    int16_t temp_x10;
    uint16_t humidity_x10;
    memcpy(&temp_x10, &pkt.payload.raw[0], sizeof(temp_x10));
    memcpy(&humidity_x10, &pkt.payload.raw[2], sizeof(humidity_x10));
    
    sec_temp_c = temp_x10 / 10.0f;
    sec_humidity_pct = (humidity_x10 == 0xFFFF) ? NAN : (humidity_x10 / 10.0f);
    sec_battery_percentage = pkt.payload.raw[4];
    
    // Signal to main loop that new data has arrived
    sec_data_new_pending = true;
    
    Serial.printf("[TEMP-OLED] Received secondary reading: %.1f C, %.1f %%, Battery: %d%%\n", sec_temp_c, sec_humidity_pct, sec_battery_percentage);
  }

  // OTA packet handling - verify signatures first
  if (pkt.type == PKT_OTA_NOTIFY || pkt.type == PKT_OTA_CHUNK ||
      pkt.type == PKT_OTA_CHECKPOINT_REQ || pkt.type == PKT_OTA_COMPLETE ||
      pkt.type == PKT_OTA_ABORT) {
    if (HOME_ID != 0) {
      uint32_t expected_sig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expected_sig) {
        Serial.printf("[TEMP-OLED] SECURITY: Invalid OTA signature. Expected 0x%08X, got 0x%08X\n", expected_sig, pkt.signature);
        return;
      }
    }
  }

  if (pkt.type == PKT_OTA_NOTIFY) {
    handleOtaNotify(&pkt);
  } else if (pkt.type == PKT_OTA_CHUNK) {
    handleOtaChunk(&pkt);
  } else if (pkt.type == PKT_OTA_CHECKPOINT_REQ) {
    handleOtaCheckpointReq(&pkt);
  } else if (pkt.type == PKT_OTA_COMPLETE) {
    handleOtaComplete(&pkt);
  } else if (pkt.type == PKT_OTA_ABORT) {
    handleOtaAbort(&pkt);
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n--- TEMP OLED SENSOR BOOT ---");

  pinMode(PIN_LED, OUTPUT);
  setLed(false);
  pinMode(PIN_RESET, INPUT_PULLDOWN);

  prefs.begin("huemixlink", false);
  HOME_ID = prefs.getUInt("hid", 0);

  memset(&gateways, 0, sizeof(gateways));
  prefs.getBytes("gw", &gateways, sizeof(gateways));

  config_day_h = prefs.getUChar("day_h", 7);
  config_day_m = prefs.getUChar("day_m", 0);
  config_day_bright = prefs.getUChar("day_bright", 100);
  config_night_h = prefs.getUChar("night_h", 21);
  config_night_m = prefs.getUChar("night_m", 0);
  config_night_bright = prefs.getUChar("night_bright", 25);
  
  String pName = prefs.getString("prim_name", "");
  strncpy(config_prim_name, pName.c_str(), sizeof(config_prim_name) - 1);
  config_prim_name[sizeof(config_prim_name) - 1] = '\0';
  
  config_sec_enabled = prefs.getUChar("sec_enabled", 0);
  
  String sName = prefs.getString("sec_name", "");
  strncpy(config_sec_name, sName.c_str(), sizeof(config_sec_name) - 1);
  config_sec_name[sizeof(config_sec_name) - 1] = '\0';
  
  prefs.getBytes("sec_mac", config_sec_mac, 6);
  config_report_interval = prefs.getUInt("rpt_int", 600);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  displayReady = display.begin(OLED_ADDR_PRIMARY, true);
  if (displayReady) {
    display.setRotation(2);
    display.clearDisplay();
    display.display();
    renderStatus("HueMix Temp", "Starting...");
  } else {
    Serial.println("[TEMP-OLED] Primary SH1106 display (0x3C) not found");
  }

  displaySecReady = displaySec.begin(OLED_ADDR_SECONDARY, true);
  if (displaySecReady) {
    displaySec.setRotation(2);
    displaySec.clearDisplay();
    displaySec.display();
  } else {
    Serial.println("[TEMP-OLED] Secondary SH1106 display (0x3D) not found");
  }

  sensorReady = sht31.begin(SHT31_ADDR);
  Serial.printf("[TEMP-OLED] SHT31D %s\n", sensorReady ? "ready" : "not found");
  if (!sensorReady) renderStatus("SHT31D failed", "Check I2C wiring");

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[TEMP-OLED] ESP-NOW init failed");
    renderStatus("ESP-NOW failed");
    delay(1000);
    ESP.restart();
  }

  esp_now_register_recv_cb(OnDataRecv);
  memset(&peerInfo, 0, sizeof(peerInfo));

  readSensor();
  renderDisplay();

  Serial.printf("[TEMP-OLED] HOME_ID: 0x%08X\n", HOME_ID);
  Serial.printf("[TEMP-OLED] Gateways: %d\n", gateways.count);

  // Send HELLO after OTA reboot to confirm new version
  if (HOME_ID != 0 && gateways.count > 0) {
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_SW) {
      Serial.println("[TEMP-OLED] Software reset detected - sending HELLO with new version");
      delay(100);
      sendHello(false);
      delay(200);
    }
  }

  blinkLed(5, 100, 100);

  if (HOME_ID != 0) {
    sendHello(false);
    requestTimeSync();
    lastTimeSyncAttemptMs = millis();
  }

  lastHelloMs = millis();
  lastSensorReadMs = millis();
  lastDisplayMs = millis();
}

void loop() {
  unsigned long now = millis();

  // Process any pending secondary sensor updates from ESP-NOW (Core 0) safely on Core 1
  if (sec_data_new_pending) {
    sec_data_last_seen = now;
    sec_data_valid = true;
    sec_data_new_pending = false;
  }

  // ================================================================
  // OTA TIMEOUT HANDLING
  // ================================================================
  if (ota_mode && otaState == OTA_RECEIVING) {
    if (millis() - last_ota_activity > 30000) {
      Serial.println("[OTA] No chunk received for 30s, aborting");
      abortOta("Chunk timeout");
    }
    if (millis() - ota_wake_time > 600000) {
      Serial.println("[OTA] Overall timeout (10 min), aborting");
      abortOta("Overall timeout");
    }
  }

  // ================================================================
  // BUTTON HANDLING
  // ================================================================
  if (digitalRead(PIN_RESET) == HIGH) {
    unsigned long holdStart = millis();

    while (digitalRead(PIN_RESET) == HIGH) {
      if (millis() - holdStart > RESET_HOLD_MS) {
        Serial.println("[TEMP-OLED] Factory reset");
        stopLedBreathing();
        renderStatus("Factory reset", "Rebooting...");
        prefs.clear();
        prefs.end();
        delay(200);
        ESP.restart();
      }
      delay(10);
    }

    // Button released
    unsigned long holdDuration = millis() - holdStart;
    if (holdDuration > 50) {
      if (ota_mode) {
        Serial.println("[TEMP-OLED] Button press during OTA ignored");
      } else if (HOME_ID == 0) {
        Serial.println("[TEMP-OLED] Reset button pressed (unpaired). Sending HELLO");
        renderStatus("Searching...", "Sending HELLO");
        sendHello(true);
        blinkLed(1, 200, 0);
      } else {
        Serial.println("[TEMP-OLED] Reset button pressed. Sending HELLO");
        sendHello(false);
      }
    }
  }

  // ================================================================
  // SENSOR READING (skip during OTA for performance)
  // ================================================================
  if (!ota_mode && now - lastSensorReadMs >= SENSOR_READ_INTERVAL_MS) {
    readSensor();
    lastSensorReadMs = now;
  }

  // ================================================================
  // TIME SYNC RETRY
  // Retry every 30 seconds until successful.
  // ================================================================
  if (!ota_mode && HOME_ID != 0 && !timeSynced && now - lastTimeSyncAttemptMs >= 30000) {
    requestTimeSync();
    lastTimeSyncAttemptMs = now;
  }

  // ================================================================
  // NORMAL OPERATION (skip when in OTA mode)
  // ================================================================
  if (!ota_mode && HOME_ID != 0) {
    if (helloRequestPending) {
      if (sendHello(false)) Serial.println("[TEMP-OLED] HELLO sent on server request");
      requestTimeSync();
      helloRequestPending = false;
      lastHelloMs = now;
    }

    // First temperature reading after pairing (runs after HELLO above on the same loop)
    if (pendingPairReading) {
      readSensor();
      if (!isnan(currentTempC)) {
        Serial.println("[TEMP-OLED] Sending initial reading after pair");
        tempSum = currentTempC;
        humiditySum = currentHumidityPct;
        sampleCount = 1;
        sendTemperatureReading();
      }
      pendingPairReading = false;
    }

    if (timeSynced) {
      uint32_t epochNow = getCurrentEpochSeconds();
      uint32_t currentBoundary = (epochNow / config_report_interval) * config_report_interval;
      if (currentBoundary != lastReportBoundary) {
        sendTemperatureReading();
        lastReportBoundary = currentBoundary;
      }
    }
  }

  // ================================================================
  // DISPLAY
  // ================================================================
  if (now - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
    if (ota_mode) {
      renderOtaStatus();
    } else {
      renderDisplay();
    }
    lastDisplayMs = now;
  }

  // Invalidate secondary data after 15 minutes of no updates
  if (sec_data_valid && (now - sec_data_last_seen > 900000)) {
    sec_data_valid = false;
  }

  delay(10);
}