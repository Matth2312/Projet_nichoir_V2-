#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <DNSServer.h>
#include "MyTimerCam.h"
#include <driver/adc.h>
#include "esp_sleep.h"
#include "esp_bt.h"     // btStop()
#include "driver/gpio.h"


// ================= CONFIGURATION =================

// MQTT
const char* MQTT_SERVER   = "192.168.2.4";
const uint16_t MQTT_PORT  = 1883;
const char* TOPIC_IMAGE   = "nichoir/image";
const char* TOPIC_BAT     = "nichoir/batterie";

// Point d'Accès
const char* AP_SSID       = "NichoirConfigTHERER";
const char* AP_PASSWORD   = "12345678";

// --- SLEEP + PIR ---
#define PIR_PIN 4                  // <-- GPIO RTC recommandé pour EXT0
#define COOLDOWN_TIME_SEC 60      // 5 minutes (à adapter)

// Pins
#define BAT_ADC_PIN   38
#define BAT_HOLD_PIN  33
#define LED_PIN 2

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
unsigned long lastCapture = 0; // (plus vraiment utilisé en station sleep)
bool shouldRestart = false;
unsigned long restartStart = 0;

// ================= HTML DYNAMIQUE =================
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

// ================= FONCTIONS BATTERIE =================

float readBatteryVoltage() {
  digitalWrite(BAT_HOLD_PIN, HIGH);
  delay(10);

  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_2, ADC_ATTEN_DB_11);

  long sum = 0;
  for(int i=0; i<20; i++) {
    sum += adc1_get_raw(ADC1_CHANNEL_2);
    delay(5);
  }
  float raw = sum / 20.0;
  Serial.printf("[DEBUG] Raw Low-Level: %.2f\n", raw);

  if (raw < 1) return 0.0;

  // Ton calcul (je ne change pas)
  float voltage = ((raw / 4095.0) * 3.3 * 2.0) - 1;
  return voltage;
}

void sendBattery() {
  float voltage = readBatteryVoltage();
  String msg = String(voltage, 2);
  Serial.print("Tension Batterie: "); Serial.print(msg); Serial.println(" V");
  mqttClient.publish(TOPIC_BAT, msg.c_str());
}

// ================= FONCTIONS CAMERA / MQTT =================

void sendImage(camera_fb_t* fb) {
  if (!fb || fb->len == 0) return;

  if (mqttClient.getBufferSize() < fb->len + 500) {
    mqttClient.setBufferSize(fb->len + 500);
  }

  Serial.printf("Envoi Image MQTT (%u bytes)... ", (unsigned)fb->len);
  bool ok = mqttClient.publish(TOPIC_IMAGE, (const uint8_t*)fb->buf, fb->len, false);
  Serial.println(ok ? "OK" : "ECHEC");
}

void reconnectMQTT() {
  if (!mqttClient.connected()) {
    Serial.print("Connexion MQTT...");
    String clientId = "TimerCAM-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("OK");
    } else {
      Serial.print("Echec rc="); Serial.println(mqttClient.state());
    }
  }
}

bool connectMQTT() {
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  reconnectMQTT();
  return mqttClient.connected();
}

// ================= WIFI & AP LOGIC =================

bool connectToSavedWiFi() {
  if (wifiSsid == "") return false;

  Serial.println("Tentative de connexion STA...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 10000) {
    delay(500); Serial.print(".");
  }
  Serial.println();

  return (WiFi.status() == WL_CONNECTED);
}

void startConfigAP() {
  Serial.println(">>> MODE AP CONFIGURATION <<<");
  digitalWrite(LED_PIN, HIGH);
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
      resp += "<h1>Sauvegarde OK</h1>";
      resp += "<p>Redemarrage dans 10s...</p>";
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

// ================= SLEEP LOGIC (PIR -> LIGHT SLEEP -> ACTION -> DEEP SLEEP) =================

void runDetectionCycle() {
  // ------------------------------------------------------
  // ETAPE 1 : ATTENTE PIR (méthode fonctionnelle: digitalRead)
  // ------------------------------------------------------
  Serial.println("\n=== [ETAPE 1] ATTENTE PIR (digitalRead) ===");

  pinMode(PIR_PIN, INPUT);           // <-- comme ton exemple (pas de pulldown)
  digitalWrite(LED_PIN, LOW);
  digitalWrite(BAT_HOLD_PIN, LOW);

  // (optionnel) couper WiFi/BT pendant l'attente pour économiser
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();

  // On attend un HIGH stable
  while (true) {
    int state = digitalRead(PIR_PIN);

    if (state == HIGH) {
      Serial.println("Signal détecté !");
      digitalWrite(LED_PIN, HIGH);
      break;
    } else {
      Serial.println("Pas de signal");
      digitalWrite(LED_PIN, LOW);
    }

    delay(500); // évite le spam + laisse respirer
  }

  // ------------------------------------------------------
  // ETAPE 2 : REVEIL (mouvement) -> WiFi -> Camera -> MQTT
  // ------------------------------------------------------
Serial.println("\n=== [ETAPE 2] EVENT PIR (digitalRead) ===");
Serial.println("[EVENT] Mouvement detecte (PIR)!");


  // Recharge creds
  preferences.begin("wifi", true);
  wifiSsid = preferences.getString("ssid", ""); 
  wifiPass = preferences.getString("password", "");
  preferences.end();

  if (wifiSsid == "") {
    Serial.println("[WIFI] Pas de SSID sauvegarde -> Mode AP");
    startConfigAP();
    isStationMode = false;
    return;
  }

  Serial.printf("[WIFI] Connexion a '%s'...\n", wifiSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

  unsigned long startW = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startW < 10000) {
    delay(100); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] Connecte ! IP: "); Serial.println(WiFi.localIP());
    digitalWrite(LED_PIN, HIGH);

    Serial.print("[CAM] Initialisation camera...");
    bool camOK = Camera.begin(FRAMESIZE_SVGA, PIXFORMAT_JPEG, 1, 12);
    Serial.println(camOK ? "OK" : "ERREUR");

    if (camOK) {
      Serial.println("[CAM] Prise de photo...");
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
    delay(200);
  } else {
    Serial.println("[WIFI] Echec connexion (timeout).");
  }

  // ------------------------------------------------------
  // ETAPE 3 : DEEP SLEEP (Cooldown)
  // ------------------------------------------------------
  Serial.println("\n=== [ETAPE 3] PREPARATION DEEP SLEEP ===");
  Serial.printf("[TIMER] Cooldown %d secondes\n", COOLDOWN_TIME_SEC);

  digitalWrite(BAT_HOLD_PIN, LOW);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup((uint64_t)COOLDOWN_TIME_SEC * 1000000ULL);

  Serial.println("[SLEEP] Deep sleep...");
  Serial.flush();
  esp_deep_sleep_start();
}

// ================= SETUP =================

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // --- Préparer le deep sleep / maintien alim sur batterie (M5TimerCAM) ---
  pinMode(BAT_HOLD_PIN, OUTPUT);
  digitalWrite(BAT_HOLD_PIN, HIGH);                 // garde l'alim active
  gpio_hold_en((gpio_num_t)BAT_HOLD_PIN);
  gpio_deep_sleep_hold_en();

  

  // Lire SSID/PASS
  preferences.begin("wifi", true);
  wifiSsid = preferences.getString("ssid", "");
  wifiPass = preferences.getString("password", "");
  preferences.end();

  // Si on a déjà un WiFi : station mode.
  // Sinon : AP config (comme avant)
  if (connectToSavedWiFi()) {
    isStationMode = true;
    Serial.println("Connecte au WiFi !");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  } else {
    isStationMode = false;
    startConfigAP();
  }
}

// ================= LOOP =================

void loop() {
 digitalWrite(LED_PIN, HIGH);

  if (isStationMode) {
    // Nouveau comportement: on attend PIR en light sleep,
    // puis on fait photo+MQTT et on part en deep sleep.
    runDetectionCycle();

    // Si jamais on revient ici (réveil inattendu), petite pause
    delay(50);
  } else {
    // Mode AP inchangé
    dnsServer.processNextRequest();
    server.handleClient();

    if (shouldRestart && (millis() - restartStart > 10000)) {
      Serial.println("Redemarrage...");
      ESP.restart();
    }
  }
}


 


     // --- DEBUT BLOC EFFACEMENT ---
  // Serial.println("!!! EFFACEMENT DU WIFI !!!");
  // preferences.begin("wifi", false); // On ouvre l'espace mémoire "wifi"
  // preferences.clear();              // On efface tout ce qu'il y a dedans
  // preferences.end();
  // Serial.println("Mémoire effacée. Redémarrage en mode AP au prochain boot.");
  // --- FIN BLOC EFFACEMENT ---