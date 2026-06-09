/*
  HUEMIXLINK V3 - BATTERY TEMPERATURE SENSOR (DEEP SLEEP VARIANT)
  Supports: ESP32
*/

#include "HueMixLink.h"
#include <Preferences.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>
#include <esp_adc_cal.h>
#include <esp_sleep.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include "mbedtls/sha256.h"
#include <Ticker.h>
#include <time.h>
#include <sys/time.h>
#include <Adafruit_SHT31.h>

#define PIN_RESET 14
#define PIN_RESET_ALT 27
#define PIN_LED 18
#define PIN_I2C_SDA 21
#define PIN_I2C_SCL 22
#define PIN_BATTERY 35
#define PIN_SENSOR_POWER 17
#define PIN_BAT_TYPE 23     // Battery type detect: HIGH=CR123A, LOW=Li-Ion

#define LED_ACTIVE_HIGH HIGH

#define SHT31_ADDR 0x44

#define DEV_PLUGIN_MARKER 0xFE
#define PLUGIN_DEVICE_TYPE 0x01  // 0x01 indicates battery version
#define PKT_TEMP_EVENT 0x55
#define PKT_TIME_REQUEST 0x56
#define PKT_TIME_RESPONSE 0x57
#define PKT_TEMP_CONFIG 0x58

// Double-tap detection: 1 second window (in microseconds for timer sleep)
#define DOUBLE_TAP_WINDOW_US 1000000

// 550e8400-e29b-41d4-a716-446655440000
static const uint8_t PLUGIN_UUID[16] = {
  0x55, 0x0e, 0x84, 0x00, 0xe2, 0x9b, 0x41, 0xd4,
  0xa7, 0x16, 0x44, 0x66, 0x55, 0x44, 0x00, 0x00
};

// Keep state in RTC slow memory to survive deep sleep
RTC_DATA_ATTR bool rtc_timeSynced = false;
RTC_DATA_ATTR uint32_t rtc_lastMidnightSyncDay = 0;
RTC_DATA_ATTR int32_t rtc_tzOffsetSeconds = 0;
RTC_DATA_ATTR char rtc_tzStr[32] = "GMT0";
RTC_DATA_ATTR bool rtc_waitingForDoubleTap = false;
RTC_DATA_ATTR uint32_t rtc_lastSyncEpoch = 0;
RTC_DATA_ATTR uint32_t rtc_reportIntervalSeconds = 600;

static inline bool isResetPressed() {
  return digitalRead(PIN_RESET) == HIGH || digitalRead(PIN_RESET_ALT) == HIGH;
}

// --- LED BREATHING (for OTA) ---
#define LED_PWM_FREQ 5000
#define LED_PWM_RESOLUTION 8
Ticker breathingTicker;
int breathingDirection = 1;
int breathingBrightness = 0;
bool breathingActive = false;

Preferences prefs;
Adafruit_SHT31 sht31 = Adafruit_SHT31();

uint32_t HOME_ID = 0;
Payload_GatewayList gateways;
uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
esp_now_peer_info_t peerInfo;
esp_adc_cal_characteristics_t adc_chars_battery;

float currentTempC = NAN;
float currentHumidityPct = NAN;
uint16_t battery_mv = 0;

volatile bool ackReceived = false;
volatile bool timeResponseReceived = false;
volatile bool pairingConfirmed = false;

// --- OTA STATE MACHINE ---
enum OtaState { OTA_IDLE, OTA_WAITING_NOTIFY, OTA_RECEIVING, OTA_VALIDATING, OTA_COMPLETE };
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

// ============================================================
// LED FUNCTIONS
// ============================================================

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

// ============================================================
// GATEWAY MANAGEMENT
// ============================================================

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
    Serial.printf("[TEMP-BATTERY] Added gateway, count=%d\n", gateways.count);
  }
}

static void ensurePeer(const uint8_t *mac) {
  if (!esp_now_is_peer_exist(mac)) {
    memcpy(peerInfo.peer_addr, mac, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_err_t res = esp_now_add_peer(&peerInfo);
    Serial.printf("[TEMP-BATTERY] add_peer %02X:%02X:%02X:%02X:%02X:%02X -> %d\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], res);
  }
}

static void updateGatewayList(const Payload_GatewayList *newList) {
  // Merge strategy: preserve local gateway ordering (successful gateways stay at front)
  // while adding new gateways and removing stale ones from the server's authoritative list.
  Payload_GatewayList merged;
  merged.count = 0;

  // First: keep existing gateways that are still in the server's list (preserves local order)
  for (int i = 0; i < gateways.count && merged.count < MAX_GATEWAYS; i++) {
    bool stillExists = false;
    for (int j = 0; j < newList->count; j++) {
      if (memcmp(gateways.macs[i], newList->macs[j], 6) == 0) {
        stillExists = true;
        break;
      }
    }
    if (stillExists) {
      memcpy(merged.macs[merged.count], gateways.macs[i], 6);
      merged.count++;
    }
  }

  // Then: append any new gateways from the server that we don't have yet
  for (int i = 0; i < newList->count && merged.count < MAX_GATEWAYS; i++) {
    bool isNew = true;
    for (int j = 0; j < merged.count; j++) {
      if (memcmp(newList->macs[i], merged.macs[j], 6) == 0) {
        isNew = false;
        break;
      }
    }
    if (isNew) {
      memcpy(merged.macs[merged.count], newList->macs[i], 6);
      merged.count++;
    }
  }

  // Check if anything actually changed
  bool changed = (gateways.count != merged.count);
  if (!changed) {
    for (int i = 0; i < merged.count; i++) {
      if (memcmp(gateways.macs[i], merged.macs[i], 6) != 0) {
        changed = true;
        break;
      }
    }
  }

  if (changed) {
    memcpy(&gateways, &merged, sizeof(Payload_GatewayList));
    saveGateways();
    Serial.printf("[TEMP-BATTERY] Gateway list updated, count=%d\n", gateways.count);
    for (int i = 0; i < gateways.count; i++) {
      Serial.printf("[TEMP-BATTERY]   [%d] %02X:%02X:%02X:%02X:%02X:%02X\n", i,
        gateways.macs[i][0], gateways.macs[i][1], gateways.macs[i][2],
        gateways.macs[i][3], gateways.macs[i][4], gateways.macs[i][5]);
    }
  }
}

// ============================================================
// PACKET SENDING
// ============================================================

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
    Serial.printf("[TEMP-BATTERY][ACK] No gateways for packet 0x%02X\n", pkt.type);
    return false;
  }

  ackReceived = false;
  int successfulGatewayIndex = -1;
  Serial.printf("[TEMP-BATTERY][ACK] Sending packet 0x%02X to %d gateway(s)\n", pkt.type, gateways.count);

  for (int i = 0; i < gateways.count; i++) {
    Serial.printf("[TEMP-BATTERY][ACK] Attempt %d/%d via %02X:%02X:%02X:%02X:%02X:%02X\n",
                  i + 1, gateways.count,
                  gateways.macs[i][0], gateways.macs[i][1], gateways.macs[i][2],
                  gateways.macs[i][3], gateways.macs[i][4], gateways.macs[i][5]);

    ensurePeer(gateways.macs[i]);
    esp_err_t sendResult = esp_now_send(gateways.macs[i], reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt));
    if (sendResult != ESP_OK) {
      Serial.printf("[TEMP-BATTERY][ACK] esp_now_send failed on gateway index %d: %d\n", i, sendResult);
      continue;
    }

    unsigned long waitStart = millis();
    while (millis() - waitStart < 150 && !ackReceived) {
      delay(1);
    }

    if (ackReceived) {
      Serial.printf("[TEMP-BATTERY][ACK] ACK received from gateway index %d\n", i);
      successfulGatewayIndex = i;
      break;
    }
    Serial.printf("[TEMP-BATTERY][ACK] No ACK from gateway index %d\n", i);
  }

  if (successfulGatewayIndex < 0) {
    Serial.printf("[TEMP-BATTERY][ACK] Packet 0x%02X not acknowledged\n", pkt.type);
    return false;
  }

  if (successfulGatewayIndex > 0) {
    Serial.printf("[TEMP-BATTERY][ACK] Promoting gateway index %d to front\n", successfulGatewayIndex);
    uint8_t tempMac[6];
    memcpy(tempMac, gateways.macs[successfulGatewayIndex], 6);
    for (int j = successfulGatewayIndex; j > 0; j--) {
      memcpy(gateways.macs[j], gateways.macs[j - 1], 6);
    }
    memcpy(gateways.macs[0], tempMac, 6);
    saveGateways();
  }

  return true;
}

// ============================================================
// BATTERY & SENSOR
// ============================================================

static void getBatteryVoltage() {
  analogSetPinAttenuation(PIN_BATTERY, ADC_2_5db);
  uint32_t raw = 0;
  for(int i = 0; i < 10; i++) {
    raw += analogRead(PIN_BATTERY);
    delay(5);
  }
  raw /= 10;

  uint32_t voltage = esp_adc_cal_raw_to_voltage(raw, &adc_chars_battery);
  battery_mv = (voltage * 1300) / 300;

  Serial.printf("[BATTERY] Raw ADC: %u, ADC voltage: %u mV, Calculated battery: %u mV\n", raw, voltage, battery_mv);
}

static bool readSensor() {
  float t = sht31.readTemperature();
  float h = sht31.readHumidity();
  if (isnan(t) || isnan(h)) {
    Serial.println("[TEMP-BATTERY] Failed to read SHT31D");
    return false;
  }

  currentTempC = t;
  currentHumidityPct = h;

  Serial.printf("[TEMP-BATTERY] SHT31D %.2fC %.2f%%\n", t, h);
  return true;
}

// ============================================================
// PACKET BUILDERS
// ============================================================

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
  uint16_t humidity_x10 = (isnan(humidityPct) || humidityPct < 0.0f) ? 0xFFFF : (uint16_t)lroundf(humidityPct * 10.0f);

  memcpy(&pkt.payload.raw[0], &temp_x10, sizeof(temp_x10));
  memcpy(&pkt.payload.raw[2], &humidity_x10, sizeof(humidity_x10));
  memcpy(&pkt.payload.raw[4], &battery_mv, sizeof(battery_mv));

  uint8_t flags = 0x02;  // battery/deepsleep variant
  if (digitalRead(PIN_BAT_TYPE) == HIGH) {
    flags |= PLATFORM_FLAG_BATTERY_CR123A;
  }
  pkt.payload.raw[6] = flags;

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
  if (otaState != OTA_WAITING_NOTIFY) {
    Serial.println("[OTA] Not in OTA mode");
    return;
  }

  Serial.println("[OTA] NOTIFY received");

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
  ready.payload.otaReady.battery_mv = battery_mv;
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

static bool sendHello(bool broadcastIfUnpaired);
static bool requestTimeSync();

// ============================================================
// ESP-NOW RECEIVE CALLBACK
// ============================================================

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(HueMixLinkPacket)) return;

  HueMixLinkPacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  const uint8_t *mac = info->src_addr;

  if (pkt.type == PKT_PAIR_CONFIRM) {
    const uint32_t sig = calculateHash(pkt.payload.raw, 185, 0);
    if (pkt.signature != sig) {
      Serial.println("[TEMP-BATTERY] Rejected pair confirm with invalid signature");
      return;
    }

    if (HOME_ID == 0) {
      HOME_ID = pkt.payload.pair.newHomeID;
      prefs.putUInt("hid", HOME_ID);
      Serial.printf("[TEMP-BATTERY] Paired! HOME_ID=0x%08X\n", HOME_ID);
      pairingConfirmed = true;
    } else {
      Serial.println("[TEMP-BATTERY] Pair confirm received while already paired, re-syncing");
      sendHello(false);
      requestTimeSync();
    }
    addGateway(mac);
  } else if (pkt.type == PKT_TIME_RESPONSE) {
    if (HOME_ID != 0) {
      uint32_t expectedSig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expectedSig) {
        Serial.println("[TEMP-BATTERY] Rejected time response with invalid signature");
        return;
      }
    }

    uint32_t epochSeconds = 0;
    int32_t tzOffsetSeconds = 0;

    memcpy(&epochSeconds, &pkt.payload.raw[0], sizeof(epochSeconds));
    memcpy(&tzOffsetSeconds, &pkt.payload.raw[4], sizeof(tzOffsetSeconds));

    if (epochSeconds == 0) {
      Serial.println("[TEMP-BATTERY] Received invalid epoch in time response");
      return;
    }

    int32_t tzHoursWest = -tzOffsetSeconds / 3600;
    int32_t tzMinutesWest = abs(tzOffsetSeconds % 3600) / 60;

    if (tzMinutesWest == 0) {
      snprintf(rtc_tzStr, sizeof(rtc_tzStr), "GMT%+d", tzHoursWest);
    } else {
      snprintf(rtc_tzStr, sizeof(rtc_tzStr), "GMT%+d:%02d", tzHoursWest, tzMinutesWest);
    }

    rtc_timeSynced = true;
    rtc_tzOffsetSeconds = tzOffsetSeconds;
    rtc_lastMidnightSyncDay = epochSeconds / 86400;
    rtc_lastSyncEpoch = epochSeconds;

    struct timeval tv = { .tv_sec = (time_t)epochSeconds, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    setenv("TZ", rtc_tzStr, 1);
    tzset();

    timeResponseReceived = true;
    Serial.printf("[TEMP-BATTERY] Time synced: epoch=%u offset=%d (%s)\n", epochSeconds, tzOffsetSeconds, rtc_tzStr);
  } else if (pkt.type == PKT_ACK_TO_BTN) {
    Serial.printf("[TEMP-BATTERY][ACK] Received PKT_ACK_TO_BTN from %02X:%02X:%02X:%02X:%02X:%02X\n",
                  mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (HOME_ID != 0) {
      uint32_t expectedSig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expectedSig) {
        Serial.println("[TEMP-BATTERY][ACK] Rejected ACK with invalid signature");
        return;
      }
    }
    if (pkt.payload.gwList.count > 0) {
      updateGatewayList(&pkt.payload.gwList);
    }
    ackReceived = true;
  } else if (pkt.type == PKT_GW_LIST_UPD) {
    if (HOME_ID != 0) {
      uint32_t expectedSig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expectedSig) {
        Serial.println("[TEMP-BATTERY] Rejected gateway list with invalid signature");
        return;
      }
    }
    if (pkt.payload.gwList.count > 0) {
      updateGatewayList(&pkt.payload.gwList);
    }
  } else if (pkt.type == PKT_TEMP_CONFIG) {
    if (HOME_ID != 0) {
      uint32_t expectedSig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expectedSig) {
        Serial.println("[TEMP-BATTERY] Rejected config with invalid signature");
        return;
      }
    }

    // Read report interval from bytes 23-26 (uint32 LE, after display config fields)
    uint32_t interval;
    memcpy(&interval, &pkt.payload.raw[23], sizeof(interval));
    if (interval >= 60 && interval <= 86400) {
      rtc_reportIntervalSeconds = interval;
      prefs.putUInt("rpt_int", rtc_reportIntervalSeconds);
      Serial.printf("[TEMP-BATTERY] Config received: report interval set to %u seconds (%u min)\n", interval, interval / 60);
    } else if (interval > 0) {
      Serial.printf("[TEMP-BATTERY] Config received but interval %u out of range (60-86400), ignoring\n", interval);
    } else {
      Serial.println("[TEMP-BATTERY] Config received with no interval change (0 = keep current)");
    }
  }

  // OTA packet handling - verify signatures first
  if (pkt.type == PKT_OTA_NOTIFY || pkt.type == PKT_OTA_CHUNK ||
      pkt.type == PKT_OTA_CHECKPOINT_REQ || pkt.type == PKT_OTA_COMPLETE ||
      pkt.type == PKT_OTA_ABORT) {
    if (HOME_ID != 0) {
      uint32_t expected_sig = calculateHash(pkt.payload.raw, 185, HOME_ID);
      if (pkt.signature != expected_sig) {
        Serial.printf("[TEMP-BATTERY] SECURITY: Invalid OTA signature. Expected 0x%08X, got 0x%08X\n", expected_sig, pkt.signature);
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

// ============================================================
// NETWORK HELPERS
// ============================================================

static bool requestTimeSync() {
  HueMixLinkPacket pkt;
  fillTimeRequestPacket(pkt);
  Serial.println("[TEMP-BATTERY] Sending time request");
  timeResponseReceived = false;
  return sendPacketToGatewaysWithAck(pkt);
}

static bool sendHello(bool broadcastIfUnpaired) {
  HueMixLinkPacket pkt;
  fillHelloPacket(pkt);

  if (HOME_ID == 0 && broadcastIfUnpaired) {
    Serial.println("[TEMP-BATTERY] Broadcasting HELLO");
    ensurePeer(broadcastAddress);
    return esp_now_send(broadcastAddress, reinterpret_cast<uint8_t *>(&pkt), sizeof(pkt)) == ESP_OK;
  }

  Serial.println("[TEMP-BATTERY] Sending HELLO to gateway list");
  return sendPacketToGateways(pkt);
}

static bool sendTemperatureReading() {
  HueMixLinkPacket pkt;
  fillTemperaturePacket(pkt, currentTempC, currentHumidityPct);

  Serial.printf("[TEMP-BATTERY] Reporting %.1fC %.1f%%\n", currentTempC, currentHumidityPct);
  bool delivered = sendPacketToGatewaysWithAck(pkt);
  Serial.printf("[TEMP-BATTERY][ACK] Temperature packet %s\n", delivered ? "delivered" : "not acknowledged");
  return delivered;
}

static void waitForConfigWindow() {
  unsigned long start = millis();
  Serial.println("[TEMP-BATTERY] Listening for pending config...");
  while (millis() - start < 2000) {
    delay(10);
  }
  Serial.println("[TEMP-BATTERY] Config window closed");
}

// ============================================================
// SLEEP FUNCTIONS
// ============================================================

static void goToSleep(uint32_t sleepSeconds) {
  Serial.printf("[TEMP-BATTERY] Entering deep sleep for %u seconds...\n", sleepSeconds);
  Serial.flush();

  digitalWrite(PIN_SENSOR_POWER, HIGH);
  pinMode(PIN_SENSOR_POWER, INPUT);
  pinMode(PIN_LED, INPUT);
  WiFi.mode(WIFI_OFF);
  btStop();

  esp_sleep_enable_timer_wakeup((uint64_t)sleepSeconds * 1000000ULL);
  esp_sleep_enable_ext1_wakeup((1ULL << PIN_RESET) | (1ULL << PIN_RESET_ALT), ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

static void goToSleepReset() {
  Serial.println("[TEMP-BATTERY] Sleeping until reset button is pressed...");
  Serial.flush();

  digitalWrite(PIN_SENSOR_POWER, HIGH);
  pinMode(PIN_SENSOR_POWER, INPUT);
  pinMode(PIN_LED, INPUT);
  WiFi.mode(WIFI_OFF);
  btStop();

  esp_sleep_enable_ext1_wakeup((1ULL << PIN_RESET) | (1ULL << PIN_RESET_ALT), ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

static void goToDoubleTapWaitSleep() {
  Serial.println("[TEMP-BATTERY] Sleeping briefly, waiting for possible second tap...");
  Serial.flush();

  digitalWrite(PIN_SENSOR_POWER, HIGH);
  pinMode(PIN_SENSOR_POWER, INPUT);
  pinMode(PIN_LED, INPUT);
  WiFi.mode(WIFI_OFF);
  btStop();

  // Short sleep: 1 second timer + EXT1 wakeup on any reset button
  esp_sleep_enable_timer_wakeup(DOUBLE_TAP_WINDOW_US);
  esp_sleep_enable_ext1_wakeup((1ULL << PIN_RESET) | (1ULL << PIN_RESET_ALT), ESP_EXT1_WAKEUP_ANY_HIGH);
  esp_deep_sleep_start();
}

// ============================================================
// OTA MODE ENTRY
// ============================================================

static void enterOtaMode() {
  Serial.println("[OTA] Entering OTA mode...");
  ota_mode = true;
  otaState = OTA_WAITING_NOTIFY;
  ota_wake_time = millis();
  last_ota_activity = millis();

  blinkLed(3, 200, 200);

  // Send OTA_READY announcement to gateways
  HueMixLinkPacket ready;
  memset(&ready, 0, sizeof(HueMixLinkPacket));
  ready.type = PKT_OTA_READY;
  WiFi.macAddress(ready.sourceMAC);
  memset(ready.targetMAC, 0xFF, 6);
  ready.payload.otaReady.firmware_size = 0;  // 0 indicates "ready for OTA, no firmware yet"
  ready.payload.otaReady.battery_mv = battery_mv;
  ready.signature = calculateHash(ready.payload.raw, 185, HOME_ID);

  bool sent = false;
  for (int i = 0; i < gateways.count; i++) {
    ensurePeer(gateways.macs[i]);
    if (esp_now_send(gateways.macs[i], (uint8_t*)&ready, sizeof(ready)) == ESP_OK) {
      sent = true;
      Serial.printf("[OTA] Sent OTA_READY announcement via gateway %d\n", i);
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

uint32_t calculateSleepSeconds() {
  uint32_t interval = rtc_reportIntervalSeconds;
  if (interval < 60) interval = 600;

  uint32_t sleepSeconds = interval;
  if (rtc_timeSynced) {
    time_t now = time(NULL);
    uint32_t secondsSinceBoundary = now % interval;
    sleepSeconds = interval - secondsSinceBoundary;

    Serial.printf("[TEMP-BATTERY] Time synced. Unix=%lu, sleeping %u seconds to next %u-sec boundary\n", now, sleepSeconds, interval);
  } else {
    Serial.printf("[TEMP-BATTERY] Time not synced. Sleeping for %u seconds\n", interval);
  }

  return sleepSeconds;
}

// ============================================================
// FACTORY RESET
// ============================================================

static void factoryReset() {
  Serial.println("[TEMP-BATTERY] Factory reset initiated...");
  blinkLed(10, 50, 50);
  prefs.clear();
  HOME_ID = 0;
  gateways.count = 0;
  rtc_timeSynced = false;
  rtc_waitingForDoubleTap = false;
  Serial.println("[TEMP-BATTERY] Reset complete, restarting...");
  ESP.restart();
}

// ============================================================
// SETUP
// ============================================================

void setup() {
  Serial.begin(115200);
  Serial.println("\n--- TEMP BATTERY SENSOR BOOT ---");

  pinMode(PIN_LED, OUTPUT);
  setLed(false);
  pinMode(PIN_RESET, INPUT_PULLDOWN);
  pinMode(PIN_RESET_ALT, INPUT_PULLDOWN);
  pinMode(PIN_BAT_TYPE, INPUT_PULLDOWN);
  pinMode(PIN_SENSOR_POWER, OUTPUT);
  digitalWrite(PIN_SENSOR_POWER, LOW);
  delay(500);

  // Initialize ADC for battery
  analogRead(PIN_BATTERY); // Dummy read
  analogSetWidth(12);
  analogSetPinAttenuation(PIN_BATTERY, ADC_2_5db);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_2_5, ADC_WIDTH_BIT_12, 1100, &adc_chars_battery);
  getBatteryVoltage();

  // Load persistent settings
  prefs.begin("huemixlink", false);
  HOME_ID = prefs.getUInt("hid", 0);
  memset(&gateways, 0, sizeof(gateways));
  prefs.getBytes("gw", &gateways, sizeof(gateways));
  rtc_reportIntervalSeconds = prefs.getUInt("rpt_int", 600);

  // ======================================================================
  // UNIVERSAL FACTORY RESET CHECK
  // On ANY boot, if RESET button is held for 5 seconds → factory reset.
  // This runs before WiFi/ESP-NOW so it works even with broken firmware.
  // Also handles the hold check for tap-based wakeups (first tap, second tap).
  // ======================================================================
  if (isResetPressed()) {
    Serial.println("[RESET] Button is held at boot. Timing hold duration...");
    unsigned long holdStart = millis();

    while (isResetPressed()) {
      if (millis() - holdStart > 5000) {
        factoryReset();
        return;  // Never reached (ESP restarts)
      }
      delay(10);
    }
    Serial.printf("[RESET] Button released after %lu ms (not a factory reset)\n", millis() - holdStart);
  }

  // Initialize I2C and temperature sensor
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  bool sensorReady = sht31.begin(SHT31_ADDR);
  Serial.printf("[TEMP-BATTERY] SHT31D %s\n", sensorReady ? "ready" : "not found");

  if (!sensorReady) {
    blinkLed(3, 500, 500);
    goToSleep(600);  // Sleep and retry in 10 minutes
  }

  // Initialize WiFi and ESP-NOW
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[TEMP-BATTERY] ESP-NOW init failed");
    delay(1000);
    ESP.restart();
  }

  esp_now_register_recv_cb(OnDataRecv);
  memset(&peerInfo, 0, sizeof(peerInfo));

  // Read current temp and humidity
  readSensor();

  // Determine wakeup reason
  esp_sleep_wakeup_cause_t wakeupReason = esp_sleep_get_wakeup_cause();

  Serial.printf("[TEMP-BATTERY] HOME_ID=0x%08X, Gateways=%d, WakeupReason=%d, WaitingDoubleTap=%d\n",
                HOME_ID, gateways.count, wakeupReason, rtc_waitingForDoubleTap);

  // ======================================================================
  // CASE 1: EXT1 wakeup (RESET button was pressed during deep sleep)
  // At this point, the universal hold check above has already run:
  //   - If held 5s → already factory-reset (we won't reach here)
  //   - If released before 5s → we're here, it was a tap
  // ======================================================================
  if (wakeupReason == ESP_SLEEP_WAKEUP_EXT1) {

    // --- Second tap: double-tap detected → enter OTA mode ---
    if (rtc_waitingForDoubleTap) {
      rtc_waitingForDoubleTap = false;
      Serial.println("[RESET] Double-tap detected! Entering OTA mode...");

      if (HOME_ID == 0) {
        Serial.println("[OTA] Cannot enter OTA mode while unpaired");
        blinkLed(5, 100, 100);
        goToSleepReset();  // Sleep until next button press
        return;  // Never reached
      }

      enterOtaMode();
      return;  // Let loop() handle OTA
    }

    // --- First tap: wait for possible second tap ---
    Serial.println("[RESET] First tap detected. Entering brief sleep for double-tap detection...");
    rtc_waitingForDoubleTap = true;

    // Wait for all reset buttons to be fully released before sleeping
    while (isResetPressed()) {
      delay(10);
    }
    delay(50);  // Debounce

    goToDoubleTapWaitSleep();
    return;  // Never reached (enters deep sleep)
  }

  // ======================================================================
  // CASE 2: Timer wakeup while waiting for double-tap (no second tap came)
  // → Treat as single press: send HELLO + time sync + temperature reading
  // ======================================================================
  if (wakeupReason == ESP_SLEEP_WAKEUP_TIMER && rtc_waitingForDoubleTap) {
    rtc_waitingForDoubleTap = false;
    Serial.println("[RESET] No second tap detected. Handling as single press (re-sync)...");

    if (HOME_ID == 0) {
      // Unpaired: single press triggers pairing attempt
      Serial.println("[TEMP-BATTERY] Single press while unpaired. Attempting pairing...");
      pairingConfirmed = false;
      unsigned long pairStart = millis();

      while (millis() - pairStart < 10000 && !pairingConfirmed) {
        sendHello(true);
        blinkLed(1, 100, 0);
        unsigned long waitLoop = millis();
        while (millis() - waitLoop < 1500 && !pairingConfirmed) {
          delay(10);
        }
      }

      if (pairingConfirmed) {
        blinkLed(5, 50, 50);
      } else {
        Serial.println("[TEMP-BATTERY] Pairing failed from button press");
        blinkLed(2, 500, 500);
        goToSleepReset();
        return;  // Never reached
      }
    }

    // Paired: send HELLO, time sync, and temperature reading
    if (HOME_ID != 0) {
      sendHello(false);
      if (requestTimeSync()) {
        unsigned long timeStart = millis();
        while (millis() - timeStart < 5000 && !timeResponseReceived) {
          delay(10);
        }
      }
    }
  }

  // ======================================================================
  // CASE 3: Unpaired device (cold boot or standard timer wakeup)
  // ======================================================================
  else if (HOME_ID == 0) {
    Serial.println("cold boot");
    blinkLed(5, 100, 100);
    goToSleepReset();  // Sleep until next button press for pairing
    return;  // Never reached
  }

  // ======================================================================
  // CASE 4: Standard timer wakeup (paired, normal 10-minute cycle)
  // ======================================================================
  else if (wakeupReason == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("[TEMP-BATTERY] Standard timer wakeup.");

    bool needSync = !rtc_timeSynced;
    if (!needSync && rtc_lastSyncEpoch != 0) {
      time_t now = time(NULL);
      uint32_t syncInterval = (rtc_reportIntervalSeconds > 3600) ? rtc_reportIntervalSeconds : 3600;
      if ((uint32_t)(now - rtc_lastSyncEpoch) >= syncInterval) {
        Serial.printf("[TEMP-BATTERY] Time sync needed (%u seconds since last sync).\n", (uint32_t)(now - rtc_lastSyncEpoch));
        needSync = true;
      }
    }

    if (needSync) {
      Serial.println("[TEMP-BATTERY] Requesting time sync...");
      if (requestTimeSync()) {
        unsigned long timeStart = millis();
        while (millis() - timeStart < 5000 && !timeResponseReceived) {
          delay(10);
        }
      }
    } else {
      // Re-apply timezone from RTC memory
      setenv("TZ", rtc_tzStr, 1);
      tzset();
    }

    if (rtc_timeSynced) {
      uint32_t interval = rtc_reportIntervalSeconds;
      if (interval < 60) interval = 600;
      time_t now = time(NULL);
      uint32_t secsSinceBoundary = now % interval;
      uint32_t secsToBoundary = interval - secsSinceBoundary;
      uint32_t graceSecs = interval * 5 / 100;
      if (secsToBoundary < graceSecs) {
        Serial.printf("[TEMP-BATTERY] Woke %us before boundary (<5%%), deferring report\n", secsToBoundary);
      } else {
        sendTemperatureReading();
      }
    } else {
      sendTemperatureReading();
    }
  }

  // ======================================================================
  // CASE 5: Cold boot (paired) or software reset (e.g., after OTA)
  // ======================================================================
  else {
    Serial.println("[TEMP-BATTERY] Cold boot (paired).");
    rtc_waitingForDoubleTap = false;  // Safety: clear stale RTC flag
    blinkLed(5, 100, 100);
    // Check if we just rebooted from OTA
    esp_reset_reason_t reset_reason = esp_reset_reason();
    if (reset_reason == ESP_RST_SW && HOME_ID != 0 && gateways.count > 0) {
      Serial.println("[TEMP-BATTERY] Software reset detected - sending HELLO with new version");
      delay(100);
      sendHello(false);
      delay(200);
    }

    if (requestTimeSync()) {
      unsigned long timeStart = millis();
      while (millis() - timeStart < 5000 && !timeResponseReceived) {
        delay(10);
      }
    }

    sendTemperatureReading();
  }

  // ======================================================================
  // SLEEP: If in OTA mode, stay awake. Otherwise, calculate next boundary.
  // ======================================================================
  if (ota_mode) {
    Serial.println("[TEMP-BATTERY] OTA mode active, staying awake...");
    return;  // Let loop() handle OTA
  }

  // Keep radio on briefly so server can deliver any pending config
  waitForConfigWindow();

  uint32_t sleepSeconds = calculateSleepSeconds();
  goToSleep(sleepSeconds);
}

void loop() {
  if (!ota_mode) {
    uint32_t sleepSeconds = calculateSleepSeconds();
    goToSleep(sleepSeconds);
    return;
  }

  if (otaState == OTA_WAITING_NOTIFY) {
    if (millis() - ota_wake_time > 30000) {
      Serial.println("[OTA] No NOTIFY received within 30s, returning to normal");
      abortOta("NOTIFY timeout");
      uint32_t sleepSeconds = calculateSleepSeconds();
      goToSleep(sleepSeconds);
    }
  } else if (otaState == OTA_RECEIVING) {
    if (millis() - last_ota_activity > 30000) {
      Serial.println("[OTA] No chunk received for 30s, aborting");
      uint32_t sleepSeconds = calculateSleepSeconds();
      goToSleep(sleepSeconds);
    }
    if (millis() - ota_wake_time > 600000) {
      Serial.println("[OTA] Overall timeout (10 min), aborting");
      abortOta("Overall timeout");
      uint32_t sleepSeconds = calculateSleepSeconds();
      goToSleep(sleepSeconds);
    }
  }

  if (isResetPressed()) {
    unsigned long holdStart = millis();
    while (isResetPressed()) {
      if (millis() - holdStart > 5000) {
        stopLedBreathing();
        factoryReset();
        return;
      }
      delay(10);
    }
  }

  delay(10);
}
