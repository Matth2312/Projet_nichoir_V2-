#include <Arduino.h>
#include <cstdint>
#include "M5TimerCAM.h"
#include <WiFi.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include "driver/gpio.h"
#include "driver/rtc_io.h"

#define GROVE_POWER_PIN GPIO_NUM_13
#define WAKE_INTERVAL_SEC 60

void setup() {

  TimerCAM.begin(true); 

  // pinMode(GROVE_POWER_PIN, OUTPUT);  //juste pour voir s'il se rallume bien
  // digitalWrite(GROVE_POWER_PIN, HIGH);

  Serial.begin(115200);
  delay(5000);

  int voltage_mV = TimerCAM.Power.getBatteryVoltage(); // en mV
  int level_pct  = TimerCAM.Power.getBatteryLevel();   // en %
  TimerCAM.Power.setLed(255);

  Serial.println("=== Reveil (RTC BM8563) ===");
  Serial.printf("Tension batterie : %d mV\n", voltage_mV);
  Serial.printf("Niveau batterie  : %d %%\n", level_pct);
  Serial.println("Retour en Deep Sleep...\n");

  Serial.flush(); // on s'assure que tout est bien envoyé avant de dormir

  TimerCAM.Power.timerSleep(WAKE_INTERVAL_SEC);
}

void loop() {
}

// //----------------------------------------------------------------------- Partie deepsleep natif ok --------------------------------------------------------
// #define uS_TO_S_FACTOR 1000000ULL  // Microsecondes -> secondes
// #define TIME_TO_SLEEP  120         // Reveil de secours (timer) : 120 s
// #define WAKEUP_PIN     GPIO_NUM_4  // Fil blanc du PIR (voir rapport §2.2)

// RTC_DATA_ATTR int bootCount = 0;   // Persiste entre les cycles de Deep Sleep natif

// void setup() {
//   Serial.begin(115200);
//   delay(200); 
//   bootCount++;
//   Serial.printf("Boot #%d\n", bootCount);

//   
//   esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause(); //cause reveil

//   if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
//     Serial.println("Reveil : timer de secours (heartbeat)");
//     }
//   else if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
//     Serial.println("Reveil : mouvement detecte sur le PIR (GPIO4)");
//      }
//   else {Serial.println("Demarrage initial (pas un reveil de Deep Sleep)")}

//   Serial.println("DELAY");
//   delay(10000); //me laisse le temps de voir le delay

//   rtc_gpio_pulldown_en(WAKEUP_PIN);   // force le pin a 0V au repos
//   rtc_gpio_pullup_dis(WAKEUP_PIN);    // desactive toute resistance de tirage haut

//   esp_sleep_enable_ext0_wakeup(WAKEUP_PIN, 1); // reveil pir
//   esp_sleep_enable_timer_wakeup(TIME_TO_SLEEP * uS_TO_S_FACTOR); //reveil avec timer

//   Serial.println("Au dodo Anick...");
//   Serial.flush(); //on nettoie
//   esp_deep_sleep_start();
// }

// void loop() {
// }