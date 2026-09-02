#include <Arduino.h>
#include <cstdint>
#include "M5TimerCAM.h"
#include "MyTimerCam.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_bt.h"          // btStop()
//upload git02_09

// ================= CONFIGURATION =================

// MQTT
const char* MQTT_SERVER = "192.168.2.4";
const uint16_t MQTT_PORT = 1883;
const char* TOPIC_IMAGE = "nichoir/image";
const char* TOPIC_BAT   = "nichoir/batterie";

// Point d'Acces (config WiFi initiale)
const char* AP_SSID     = "NichoirConfigTHERER";
const char* AP_PASSWORD = "12345678";

#define PIR_WAKEUP_PIN GPIO_NUM_4
#define HEARTBEAT_SEC (24UL * 60UL * 60UL) 
#define BM8563_COOLDOWN_SEC (6UL * 60UL)     // 6 minutes
#define POWER_HOLD_PIN GPIO_NUM_33

// ================= OBJETS GLOBAUX =================

WiFiClient espClient;
PubSubClient mqttClient(espClient);
MyTimerCam Camera;
Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

String wifiSsid;
String wifiPass;
bool isStationMode = false;
bool shouldRestart = false;
unsigned long restartStart = 0;

// ================= HTML CONFIG WIFI =================

String getPageHTML() {
  String html = R"rawliteral(<!DOCTYPE html>
  <html><head><meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Nichoir Config</title>
  <style>
    body { font-family: sans-serif; text-align: center; margin: 20px; }
    select, input { padding: 10px; margin: 10px 0; width: 100%; box-sizing: border-box; }
    input[type=submit] { background-color: #4CAF50; color: white; border: none; font-size: 16px; }
  </style></head><body>
  <h2>Configuration Nichoir</h2>
  <form action="/save" method="POST">
    <label>Choisir le reseau :</label>
    <select name="ssid">)rawliteral";

  int n = WiFi.scanNetworks();
  if (n == 0) {
    html += "<option value=''>Aucun reseau trouve</option>";
  } else {
    for (int i = 0; i < n; ++i) {
      html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + WiFi.RSSI(i) + "dBm)</option>";
    }
  }

  html += R"rawliteral(
    </select>
    <br>
    <label>Ou entrer manuellement :</label>
    <input type="text" name="custom_ssid" placeholder="Nom du WiFi (si cache)">
    <br>
    <label>Mot de passe :</label>
    <input type="password" name="password" placeholder="Mot de passe">
    <br>
    <input type="submit" value="Enregistrer">
  </form>
  </body></html>)rawliteral";

  return html;
}

void startConfigAP() {
  Serial.println(">>> MODE AP CONFIGURATION <<<");
  TimerCAM.Power.setLed(255);
  WiFi.disconnect();
  delay(100);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("IP AP: "); Serial.println(WiFi.softAPIP());

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());

  server.on("/", HTTP_GET, []() {
    shouldRestart = false;
    server.send(200, "text/html", getPageHTML());
  });

  server.on("/save", HTTP_POST, []() {
    String newSsid = server.arg("ssid");
    if (server.arg("custom_ssid") != "") newSsid = server.arg("custom_ssid");
    String newPass = server.arg("password");

    if (newSsid != "") {
      preferences.begin("wifi", false);
      preferences.putString("ssid", newSsid);
      preferences.putString("password", newPass);
      preferences.end();

      String resp = "<html><body style='font-family:sans-serif;text-align:center;margin-top:50px;'>";
      resp += "<h1>Sauvegarde OK</h1><p>Redemarrage dans 10s...</p>";
      resp += "<a href='/' style='color:red;'>ANNULER</a></body></html>";
      server.send(200, "text/html", resp);

      shouldRestart = true;
      restartStart = millis();
    } else {
      server.send(400, "text/plain", "Erreur: SSID manquant");
    }
  });

  server.onNotFound([]() {
    server.send(200, "text/html", getPageHTML());
  });

  server.begin();
}

// ================= WIFI (station) =================

bool connectWiFiSTA() {
  if (wifiSsid == "") return false;

  Serial.printf("[WIFI] Connexion a '%s'...\n", wifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 10000) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] Connecte ! IP: "); Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("[WIFI] Echec connexion (timeout).");
  return false;
}

// ================= MQTT =================

bool connectMQTT() {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  if (!mqttClient.connected()) {
    Serial.print("[MQTT] Connexion...");
    String clientId = "TimerCAM-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("OK");
    } else {
      Serial.print("Echec rc="); Serial.println(mqttClient.state());
    }
  }
  return mqttClient.connected();
}

void sendBattery() {
  int voltage_mV = TimerCAM.Power.getBatteryVoltage();
  int level_pct  = TimerCAM.Power.getBatteryLevel();

  String msg = String(voltage_mV / 1000.0, 2); // en Volts, comme avant
  Serial.printf("[BAT] %d mV (%d %%)\n", voltage_mV, level_pct);
  mqttClient.publish(TOPIC_BAT, msg.c_str());
}

void sendImage(camera_fb_t* fb) {
  if (!fb || fb->len == 0) return;

  if (mqttClient.getBufferSize() < fb->len + 500) {
    mqttClient.setBufferSize(fb->len + 500);
  }

  Serial.printf("[MQTT] Envoi Image (%u bytes)... ", (unsigned)fb->len);
  bool ok = mqttClient.publish(TOPIC_IMAGE, (const uint8_t*)fb->buf, fb->len, false);
  Serial.println(ok ? "OK" : "ECHEC");
}

// ================= LES DEUX EVENEMENTS POSSIBLES EN SENTINELLE =================

// Reveil PIR (EXT0) : photo + batterie, puis cooldown BM8563 (6 min)
void handlePIRDetection() {
  Serial.println("\n=== [EVENT] Mouvement detecte sur le PIR ===");
  TimerCAM.Power.setLed(255);

  if (connectWiFiSTA()) {
    Serial.print("[CAM] Initialisation camera...");
    bool camOK = Camera.begin(FRAMESIZE_SVGA, PIXFORMAT_JPEG, 1, 12);
    Serial.println(camOK ? "OK" : "ERREUR");

    if (camOK) {
      camera_fb_t* fb = Camera.capture();
      if (fb) {
        if (connectMQTT()) {
          mqttClient.loop();
          sendBattery();
          sendImage(fb);
          mqttClient.loop();
        } else {
          Serial.println("[MQTT] Connexion impossible, abandon envoi.");
        }
        Camera.freeFrame(fb);
      } else {
        Serial.println("[CAM] Erreur: Framebuffer vide !");
      }
    }
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
  TimerCAM.Power.setLed(0);
}

void handleHeartbeat() {
  Serial.println("\n=== [EVENT] Heartbeat 24h ===");
  TimerCAM.Power.setLed(255);

  if (connectWiFiSTA()) {
    if (connectMQTT()) {
      mqttClient.loop();
      sendBattery();
      mqttClient.loop();
    } else {
      Serial.println("[MQTT] Connexion impossible, abandon envoi.");
    }
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
  TimerCAM.Power.setLed(0);
}

void goToSentinelSleep() {
  TimerCAM.Power.setLed(0);

  gpio_hold_en(POWER_HOLD_PIN);
  gpio_deep_sleep_hold_en();

  rtc_gpio_pulldown_en(PIR_WAKEUP_PIN);
  rtc_gpio_pullup_dis(PIR_WAKEUP_PIN);
  esp_sleep_enable_ext0_wakeup(PIR_WAKEUP_PIN, 1);
  esp_sleep_enable_timer_wakeup((uint64_t)HEARTBEAT_SEC * 1000000ULL);

  Serial.println("[SLEEP] Sentinelle : Deep Sleep natif (PIR + 24h)");
  Serial.flush();
  esp_deep_sleep_start();
  
}

void goToBM8563Cooldown() {
  Serial.println("[SLEEP] Cooldown BM8563 (6 min) apres detection");

  
  preferences.begin("nichoir", false);
  preferences.putBool("bm8563_ret", true);
  preferences.end();

  TimerCAM.Power.setLed(0);
  TimerCAM.Rtc.disableIRQ();
  TimerCAM.Rtc.setAlarmIRQ(BM8563_COOLDOWN_SEC);

  Serial.flush();
  TimerCAM.Power.powerOff();
}

// ================= SETUP =================

void setup() {
  Serial.begin(115200);
  delay(200);

  gpio_hold_dis(POWER_HOLD_PIN);
  gpio_deep_sleep_hold_dis();

  TimerCAM.begin(true);     // I2C pour le BM8563 (RTC)
  TimerCAM.Power.begin();   // maintien alim (GPIO33) + ADC batterie + LED (GPIO2)

  // --- Cas 1 : on revient d'un cooldown BM8563 (apres une detection) ---
  preferences.begin("nichoir", false);
  bool returningFromCooldown = preferences.getBool("bm8563_ret", false);
  if (returningFromCooldown) {
    preferences.putBool("bm8563_ret", false);
    preferences.end();
    Serial.println("[BOOT] Retour du cooldown BM8563 -> reprise en sentinelle");
    goToSentinelSleep();
    return;
  }
  preferences.end();

  // --- Chargement des identifiants WiFi ---
  preferences.begin("wifi", true);
  wifiSsid = preferences.getString("ssid", "");
  wifiPass = preferences.getString("password", "");
  preferences.end();

  if (wifiSsid == "") {
  
    Serial.println("[WIFI] Pas de SSID sauvegarde -> Mode AP");
    isStationMode = false;
    startConfigAP();
    return;
  }
  isStationMode = true;

  // --- Cas 2 : identification de la cause du reveil (mode sentinelle) ---
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    handlePIRDetection();
    goToBM8563Cooldown();
    return; // jamais atteint
  }
  else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
    handleHeartbeat();
    goToSentinelSleep();
    return; // jamais atteint
  }
  else {
    Serial.println("[BOOT] Premier demarrage");
    goToSentinelSleep();
    return;
  }
}

// ================= LOOP =================

void loop() {
  if (!isStationMode) {
    dnsServer.processNextRequest();
    server.handleClient();

    if (shouldRestart && (millis() - restartStart > 10000)) {
      Serial.println("Redemarrage...");
      ESP.restart();
    }
  }
}