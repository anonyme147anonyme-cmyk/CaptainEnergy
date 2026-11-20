// =====================================================================
// main.cpp -- firmware principal du boîtier (projet PlatformIO)
// ---------------------------------------------------------------------
// Chaîne complète : capteurs -> IA (sur l'ESP32) -> décision -> actions
//   IA n°1  état du réseau (réseau de neurones int8)    grid_features.h, tinyml.h
//   IA n°2  niveau de charge et autonomie                battery.h
//   IA n°3  modèle thermique appris (k)                  thermal.h
//   Décision score de risque + moteur de décision        decision.h
// Les appareils sont simulés par des LED.
//
// Bibliothèques : installées automatiquement par PlatformIO (voir platformio.ini) :
//   Adafruit INA219, DHT sensor library (Adafruit) [ou OneWire + DallasTemperature],
//   Adafruit SSD1306,
//   Adafruit GFX Library, PubSubClient, ArduinoJson (version 7)
// =====================================================================
#include <Arduino.h>   // obligatoire dans un fichier .cpp (l'Arduino IDE l'ajoutait tout seul)
#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <Preferences.h>
#include <Adafruit_INA219.h>
#include "config.h"
#if TEMP_SENSOR_DHT11
#include <DHT.h>
#else
#include <OneWire.h>
#include <DallasTemperature.h>
#endif
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "config.h"
#define MEASURE_CURRENT (HAS_CURRENT_SENSOR || (HAS_BATTERY_INA && CURRENT_FROM_INA2))
#include "grid_features.h"
#include "tinyml.h"
#include "thermal.h"
#include "battery.h"
#include "decision.h"
#include "door_detect.h"

// ---------------------------------------------------------------------
// Objets matériels
// ---------------------------------------------------------------------
Adafruit_INA219 inaGrid(0x40);
Adafruit_INA219 inaBat(0x41);
#if TEMP_SENSOR_DHT11
DHT dhtIn(PIN_DHT_IN, DHT11);
#if PIN_DHT_OUT >= 0
DHT dhtOut(PIN_DHT_OUT, DHT11);
#endif
#else
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature sensors(&oneWire);
#endif
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

// ---------------------------------------------------------------------
// État du système
// ---------------------------------------------------------------------
enum State { ST_NORMAL, ST_BATTERY, ST_WAIT, ST_RETURN };
const char* STATE_NAMES[] = {"NORMAL", "BATTERIE", "ATTENTE", "RETOUR"};
State state = ST_NORMAL;

ThermalModel thermal;
Battery battery;
LoadPowers powers;
DecisionInput din;
Decision dec;
Profile profile = DEFAULT_PROFILE;

// IA n°1
const int N_SAMPLES = 100;
const float DT = 0.01f;
float vWindow[N_SAMPLES];
int vIdx = 0;
unsigned long nextSampleUs = 0;
float gridV = 12.0f;               // dernière tension mesurée
GridPrediction lastPred;
float pUnstableAvg = 0;            // moyenne glissante de P(instable)+P(baisse)
int lowStreak = 0;                 // nombre de fenêtres "baisse" d'affilée
unsigned long inferUs = 0;         // durée de la dernière inférence (µs), pour le rapport

// Températures
float tAir = NAN, tProd = NAN, tAmb = NAN;
#if !TEMP_SENSOR_DHT11
DeviceAddress addrAir, addrProd, addrAmb;   // adresses des sondes (addrProd seulement si HAS_PRODUCT_PROBE)
#endif

// Courant du Peltier (ACS712)
float currentZeroV = 2.5f;         // tension de sortie à 0 A, mesurée au démarrage
float peltierA = 0;                // courant mesuré (A), lissé
float peltierW = 0;                // puissance mesurée (W)
float packV = 0;                   // tension du pack n°2 (INA219 n°2), 0 si non mesurée
unsigned long lastTempReqMs = 0;

// Porte
bool doorOpen = false;
unsigned long lastDoorOpenMs = 0;
unsigned long doorEvents[20];
#if !HAS_DOOR_SWITCH
DoorDetector doorDet;            // porte déduite des températures (pas de contact)
#endif
int doorEventIdx = 0;

// Moteur du frigo (LED)
bool compOn = false;
int  compDuty = 0;                 // PWM actuel du Peltier (0-255)
unsigned long pauseUntilMs = 0;    // commande pause= : Peltier forcé éteint jusqu'à cet instant
bool peltierFault = false;         // défaut détecté (surintensité) : Peltier bloqué jusqu'à la commande reset
unsigned long compChangedMs = 0;
unsigned long lastCompOnMs = 0;

// Transitions réseau
unsigned long stateSinceMs = 0;
unsigned long outageStartMs = 0;
uint32_t outageStartS = 0;
unsigned long cutTimes[5];
int cutIdx = 0;
int reconnectStep = 0;
float learnedOutageH = 2.0f;

// Annonce STEG (reçue par MQTT ou par commande série)
uint32_t annStartS = 0, annEndS = 0;
float tmaxForecast = NAN;

Alert lastAlert = ALERT_NONE;
String lastAlertText = "";

// ---------------------------------------------------------------------
// Outils
// ---------------------------------------------------------------------
bool clockValid() { return time(nullptr) > 1700000000; }
uint32_t nowS() { return clockValid() ? (uint32_t)time(nullptr) : millis() / 1000; }

String hhmmIn(float hours) {       // heure qu'il sera dans "hours" heures
  if (clockValid()) {
    time_t t = time(nullptr) + (time_t)(hours * 3600);
    struct tm tmv; localtime_r(&t, &tmv);
    char b[8]; snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return String(b);
  }
  int m = (int)(hours * 60);
  return "dans " + String(m / 60) + "h" + (m % 60 < 10 ? "0" : "") + String(m % 60);
}

String fmtH(float h) {             // 1.58 -> "1h35"
  if (h >= 98) return "--";
  int m = (int)(h * 60 + 0.5f);
  return String(m / 60) + "h" + (m % 60 < 10 ? "0" : "") + String(m % 60);
}

void setLed(int pin, bool on) { if (pin >= 0) digitalWrite(pin, on ? HIGH : LOW); }   // -1 = LED absente
bool ledOn(int pin) { return pin >= 0 && digitalRead(pin) == HIGH; }

void setRgb(bool r, bool g, bool b) {
  digitalWrite(PIN_RGB_R, r); digitalWrite(PIN_RGB_G, g); digitalWrite(PIN_RGB_B, b);
}

// ---------- Buzzer NON BLOQUANT : chaque situation a son propre son ----------
// (un delay() bloquerait la mesure du réseau à 100 Hz de l'IA n°1)
//   début de coupure        : 1 bip long
//   retour de la STEG       : 2 bips courts
//   pré-refroidissement     : 1 bip court
//   batterie insuffisante   : 3 bips, répétés toutes les 5 min
//   marchandise trop chaude : 5 bips rapides, répétés chaque minute
//   commande "silence"      : arrête les répétitions jusqu'à la prochaine alerte
struct BuzzerSeq { int left = 0; int onMs = 0, offMs = 0; bool on = false; unsigned long t = 0; } bz;
bool buzzerMuted = false;

void buzz(int count, int onMs, int offMs) {           // lance une suite de bips
  bz.left = count; bz.onMs = onMs; bz.offMs = offMs; bz.on = false; bz.t = millis() - offMs;
}

void updateBuzzer() {                                  // appelée à chaque tour de loop()
  if (PIN_BUZZER < 0) return;
  unsigned long now = millis();
  if (bz.on) {
    if (now - bz.t >= (unsigned long)bz.onMs) { digitalWrite(PIN_BUZZER, LOW); bz.on = false; bz.t = now; bz.left--; }
  } else if (bz.left > 0 && now - bz.t >= (unsigned long)bz.offMs) {
    digitalWrite(PIN_BUZZER, HIGH); bz.on = true; bz.t = now;
  }
}

void setSourceBattery(bool onBattery) {
#if HAS_SRC_RELAY
  bool level = RELAY_ACTIVE_LOW ? !onBattery : onBattery;
  digitalWrite(PIN_SRC_RELAY, level ? HIGH : LOW);
#else
  (void)onBattery;
#endif
}

// Commande du Peltier : PWM sur le MOSFET (démarrage doux), ou simple marche/arrêt par relais
void fridgePwm(int duty) {
#if PELTIER_USE_RELAY
  bool on = duty > 0;
  digitalWrite(PIN_LED_FRIDGE, (RELAY_ACTIVE_LOW ? !on : on) ? HIGH : LOW);
#elif ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_LED_FRIDGE, duty);
#else
  ledcWrite(0, duty);
#endif
}

void setCompressor(bool on) {
  if (on == compOn) return;
  unsigned long now = millis();
  if (on && (now - compChangedMs) < COMP_MIN_OFF_S * 1000UL && compChangedMs != 0) return; // délai du compresseur
  compOn = on;
  compChangedMs = now;
  if (on) lastCompOnMs = now;
  else { compDuty = 0; fridgePwm(0); }   // arrêt immédiat
}

// Démarrage doux sans bloquer le programme : +5 toutes les 40 ms -> 0 à 255 en ~2 s
void updateSoftStart() {
  static unsigned long last = 0;
  if (!compOn || compDuty >= PELTIER_MAX_DUTY || millis() - last < 40) return;
  last = millis();
#if PELTIER_USE_RELAY
  compDuty = PELTIER_MAX_DUTY;     // un relais ne fait pas de démarrage doux : marche directe
#else
  compDuty += 5;
#endif
  if (compDuty > PELTIER_MAX_DUTY) compDuty = PELTIER_MAX_DUTY;
  fridgePwm(compDuty);
}

void saveLearned() {
  prefs.putFloat("Sxy", thermal.Sxy);
  prefs.putFloat("Sxx", thermal.Sxx);
  prefs.putFloat("k", thermal.k);
  prefs.putFloat("cool", thermal.coolRate);
  prefs.putFloat("outH", learnedOutageH);
  prefs.putInt("prof", (int)profile);
}

void publishAlert(const String& text) {
  Serial.println("ALERTE : " + text);
  if (mqtt.connected()) mqtt.publish("eem/alert", text.c_str());
}

// ---------------------------------------------------------------------
// Commandes (port série ou MQTT eem/cmd) -- utiles pour la démo
//   profil=volaille|laitier|medicament|legumes
//   soc=30              batterie utilisable à 30 %
//   annonce=30,2,39     coupure dans 30 min, pendant 2 h, 39 °C prévus
//   annule              efface l'annonce
//   pause=45            Peltier éteint 45 min (pour laisser la boîte se réchauffer et apprendre k)
//   reset               efface un défaut du Peltier (après avoir vérifié le montage)
//   resetk              oublie le k appris
//   silence             arrête la répétition des bips
//   info                affiche l'état
// ---------------------------------------------------------------------
float doorOpensPerHour();   // définie plus bas

void handleCommand(String c) {
  c.trim();
  if (c.startsWith("profil=")) {
    String p = c.substring(7);
    for (int i = 0; i < PROF_COUNT; i++)
      if (p == PROFILES[i].name) { profile = (Profile)i; prefs.putInt("prof", i); }
    Serial.println(String("Profil : ") + PROFILES[profile].name);
  } else if (c.startsWith("soc=")) {
    float pct = c.substring(4).toFloat();
    battery.soc = battery.socMin + (1 - battery.socMin) * pct / 100.0f;
    Serial.println("Batterie utilisable : " + String(pct, 0) + " %");
  } else if (c.startsWith("annonce=")) {
    float inMin = 30, durH = 2, tmax = NAN;
    sscanf(c.substring(8).c_str(), "%f,%f,%f", &inMin, &durH, &tmax);
    annStartS = nowS() + (uint32_t)(inMin * 60);
    annEndS = annStartS + (uint32_t)(durH * 3600);
    if (!isnan(tmax)) tmaxForecast = tmax;
    Serial.println("Annonce enregistrée");
  } else if (c == "annule") {
    annStartS = annEndS = 0;
    Serial.println("Annonce effacée");
  } else if (c.startsWith("pause=")) {
    float minutes = c.substring(6).toFloat();
    pauseUntilMs = millis() + (unsigned long)(minutes * 60000UL);
    Serial.println("Peltier eteint pendant " + String(minutes, 0) + " min (apprentissage de k)");
  } else if (c == "silence") {
    buzzerMuted = true;
    Serial.println("Buzzer : repetitions arretees jusqu'a la prochaine alerte");
  } else if (c == "reset") {
    peltierFault = false;
    Serial.println("Defaut Peltier efface (verifier le montage avant !)");
  } else if (c == "resetk") {
    thermal = ThermalModel();
    saveLearned();
    Serial.println("k remis à zéro");
  } else if (c == "info") {
    Serial.printf("Etat %s | V=%.2f | IA1=%s %.0f%% (%lu us) | k=%.3f/h (%d maj) | Tprod=%.2f Tamb=%.2f | risque=%.2f\n",
                  STATE_NAMES[state], gridV, GM_LABELS[lastPred.cls], lastPred.prob[lastPred.cls] * 100,
                  inferUs, thermal.k, thermal.nUpdates, tProd, tAmb, dec.risk);
    Serial.printf("Memoire libre : %u octets | modele IA1 : %d poids int8\n", ESP.getFreeHeap(),
                  GM_IN * GM_HID + GM_HID * GM_OUT);
    Serial.printf("Peltier : %.2f A, %.1f W | pack n°2 : %.2f V | conso moyenne apprise du frigo : %.1f W\n",
                  peltierA, peltierW, packV, powers.fridge);
    Serial.printf("Tair=%.2f | temps de securite = %.2f h | porte %s, %.0f ouverture(s)/h\n",
                  tAir, dec.tSafeH, doorOpen ? "OUVERTE" : "fermee", doorOpensPerHour());
#if !HAS_DOOR_SWITCH
    Serial.printf("Detection porte : air +%.2f C en 10 s (seuil %.2f)\n", doorDet.lastRise, DOOR_RISE_C);
#endif
  }
}

void onMqtt(char* topic, byte* payload, unsigned int len) {
  String t(topic);
  if (t == "eem/announce") {
    JsonDocument doc;
    if (deserializeJson(doc, payload, len)) return;
    annStartS = doc["debut"] | 0;
    annEndS = doc["fin"] | 0;
    if (doc["tmax_prevue"].is<float>()) tmaxForecast = doc["tmax_prevue"].as<float>();
  } else if (t == "eem/cmd") {
    String c;
    for (unsigned int i = 0; i < len; i++) c += (char)payload[i];
    handleCommand(c);
  }
}

// ---------------------------------------------------------------------
// IA n°1 : une mesure toutes les 10 ms, une prédiction par seconde
// ---------------------------------------------------------------------
void sampleGrid() {
  if ((long)(micros() - nextSampleUs) < 0) return;
  nextSampleUs += (unsigned long)(DT * 1e6);
  if ((long)(micros() - nextSampleUs) > 50000) nextSampleUs = micros();  // rattrapage après un retard

  gridV = inaGrid.getBusVoltage_V();
  vWindow[vIdx++] = gridV;
  if (vIdx < N_SAMPLES) return;
  vIdx = 0;

  float f[N_FEATURES];
  unsigned long t0 = micros();
  computeFeatures(vWindow, N_SAMPLES, DT, f);
  lastPred = runGridModel(f);
  inferUs = micros() - t0;                    // caractéristiques + réseau de neurones
  pUnstableAvg = 0.9f * pUnstableAvg + 0.1f * (lastPred.prob[1] + lastPred.prob[2]);
  lowStreak = (lastPred.cls == 2 && lastPred.prob[2] > 0.85f) ? lowStreak + 1 : 0;
}

// ---------------------------------------------------------------------
// Machine à états : NORMAL -> BATTERIE -> ATTENTE -> RETOUR -> NORMAL
// ---------------------------------------------------------------------
void goState(State s) {
  state = s;
  stateSinceMs = millis();
  Serial.println(String("-> ") + STATE_NAMES[s]);
}

int cutsInLast2Min() {
  int n = 0;
  for (int i = 0; i < 5; i++) if (cutTimes[i] && millis() - cutTimes[i] < 120000UL) n++;
  return n;
}

void updateStateMachine() {
  bool outOfRange = gridV < V_LOW_CUT || gridV > V_HIGH_CUT;
  bool inReturnBand = gridV >= V_RET_LOW && gridV <= V_RET_HIGH;
  bool aiSaysCut = lastPred.cls == 3 && lastPred.prob[3] > 0.8f;
  bool aiSaysDrop = lowStreak >= 3;     // 3 s de "baisse" sûre : on anticipe

  switch (state) {
    case ST_NORMAL:
      if (outOfRange || aiSaysCut || aiSaysDrop) {
        cutTimes[cutIdx++ % 5] = millis();
        outageStartMs = millis();
        outageStartS = nowS();
        setSourceBattery(true);
        goState(ST_BATTERY);
        buzz(1, 600, 0);                  // début de coupure : 1 bip long
      }
      break;
    case ST_BATTERY:
      if (inReturnBand && !aiSaysCut) goState(ST_WAIT);
      break;
    case ST_WAIT: {
      if (!inReturnBand || aiSaysCut) {           // recoupure : chrono remis à zéro
        cutTimes[cutIdx++ % 5] = millis();
        goState(ST_BATTERY);
        break;
      }
      unsigned long need = (cutsInLast2Min() >= 3 ? STABLE_UNSTABLE_S : STABLE_S) * 1000UL;
      if (millis() - stateSinceMs >= need) {
        setSourceBattery(false);
        float h = (millis() - outageStartMs) / 3600000.0f;
        learnedOutageH = 0.7f * learnedOutageH + 0.3f * h;   // apprend la durée des coupures
        saveLearned();
        reconnectStep = 0;
        goState(ST_RETURN);
      }
      break;
    }
    case ST_RETURN:
      if (outOfRange || aiSaysCut) { goState(ST_BATTERY); setSourceBattery(true); break; }
      // un appareil toutes les RECONNECT_GAP_S secondes : frigo, lumières, routeur, clim
      reconnectStep = (millis() - stateSinceMs) / (RECONNECT_GAP_S * 1000UL);
      if (reconnectStep >= 4) { goState(ST_NORMAL); buzz(2, 100, 150); }   // STEG revenue : 2 bips courts
      break;
  }
}

// ---------------------------------------------------------------------
// Températures, porte, apprentissage de k (une fois par minute)
// ---------------------------------------------------------------------
void updateTemperatures() {
  if (millis() - lastTempReqMs < 2000) return;   // DHT11 : pas plus d'une lecture par seconde
  lastTempReqMs = millis();
#if TEMP_SENSOR_DHT11
  float a = dhtIn.readTemperature();             // NAN si la lecture échoue
  if (!isnan(a)) tAir = a;
#if PIN_DHT_OUT >= 0
  float m = dhtOut.readTemperature();
  if (!isnan(m)) tAmb = m;
#else
  tAmb = T_AMB_FIXED;
#endif
  // Pas de sonde dans le produit : on l'estime. Le produit suit l'air avec retard
  // (filtre du 1er ordre, constante de temps PROD_TAU_MIN). Porte ouverte :
  // on ne suit pas les pics d'air chaud, ils ne réchauffent presque pas le produit.
  if (!isnan(tAir)) {
    if (isnan(tProd)) tProd = tAir;
    else if (!doorOpen) tProd += (tAir - tProd) * (2.0f / 60.0f) / PROD_TAU_MIN;
  }
#else
  float a = sensors.getTempC(addrAir);       // lecture par adresse : rapide
  float m = sensors.getTempC(addrAmb);
  if (a > -50) tAir = a;           // -127 = sonde absente : on garde l'ancienne valeur
  if (m > -50) tAmb = m;
  if (isnan(tAmb)) tAmb = T_AMB_FIXED;     // sonde magasin absente : valeur par défaut
#if HAS_PRODUCT_PROBE
  float p = sensors.getTempC(addrProd);      // vraie sonde dans le produit (futur)
  if (p > -50) tProd = p;
#else
  // Pas de sonde produit : T_produit SIMULÉE (fausse donnée), elle suit l'air avec retard
  if (!isnan(tAir)) {
    if (isnan(tProd)) tProd = tAir;
    else if (!doorOpen) tProd += (tAir - tProd) * (2.0f / 60.0f) / PROD_TAU_MIN;
  }
#endif
  sensors.requestTemperatures();   // lance la mesure suivante (lue dans 2 s)
#endif
}

// ---------------------------------------------------------------------
// Courant du Peltier : ACS712 ou INA219 n°2 -- moyenne de 20 lectures toutes les 50 ms
// ---------------------------------------------------------------------
float readCurrentSensorV() {
  // analogReadMilliVolts utilise la calibration de l'ESP32 (plus juste que analogRead)
  return analogReadMilliVolts(PIN_CURRENT) / 1000.0f / ACS_DIVIDER;   // tension en sortie de l'ACS712
}

#define MEASURE_PACK_V HAS_BATTERY_INA

// Tension du pack n°2 par l'INA219 n°2, en volts (0 si non mesurée)
float readPackV() {
#if HAS_BATTERY_INA
  // tension mesurée + petite chute dans la résistance de mesure de l'INA219
  return inaBat.getBusVoltage_V() + inaBat.getShuntVoltage_mV() / 1000.0f;
#else
  return 0;
#endif
}

// Toutes les secondes : tension du pack, alerte "vide", recalage du niveau au repos
void updatePackVoltage() {
#if MEASURE_PACK_V
  static unsigned long last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  packV = readPackV();
  // Batterie vide : tension trop basse -> plus d'énergie utilisable, alerte une fois
  static bool emptyAlerted = false;
  if (packV > 1 && packV < BAT_EMPTY_V) {
    battery.soc = battery.socMin;
    if (!emptyAlerted) publishAlert("Batterie de secours presque vide (" + String(packV, 1) + " V).");
    emptyAlerted = true;
  } else if (packV > BAT_EMPTY_V + 0.5f) {
    emptyAlerted = false;
  }
  // Recalage du niveau de charge par la tension, Peltier éteint depuis 10 min (batterie au repos)
  static unsigned long lastRest = 0;
  if (!compOn && millis() - compChangedMs > 600000UL && millis() - lastRest > 60000UL && packV > 1) {
    lastRest = millis();
    battery.correctWithRestVoltage(packV);
  }
#endif
}

// Une lecture du courant du Peltier, en ampères
float readPeltierAmps() {
#if HAS_BATTERY_INA && CURRENT_FROM_INA2
  return inaBat.getCurrent_mA() / 1000.0f;          // INA219 n°2, en série sur le pack n°2
#elif HAS_CURRENT_SENSOR
  return (readCurrentSensorV() - currentZeroV) / ACS_SENS_V_PER_A;   // ACS712
#else
  return 0;
#endif
}

void updateCurrent() {
#if MEASURE_CURRENT
  static unsigned long last = 0;
  static float acc = 0;
  static int n = 0;
  if (millis() - last < 50) return;
  last = millis();
  acc += readPeltierAmps();
  if (++n < 20) return;
  float amps = acc / n;
  acc = 0; n = 0;
  if (amps < 0.05f) amps = 0;                          // bruit autour de zéro
  // Protection 1 : surintensité -> on coupe tout de suite et on bloque jusqu'à "reset"
  if (amps > PELTIER_MAX_A && !peltierFault) {
    peltierFault = true;
    setCompressor(false);
    publishAlert("DEFAUT : courant Peltier trop fort (" + String(amps, 1) + " A). Peltier coupe.");
  }
  // Protection 2 : Peltier allumé à fond depuis 10 s mais presque aucun courant
  static bool noCurrentAlerted = false;
  if (compOn && compDuty >= PELTIER_MAX_DUTY && millis() - compChangedMs > 10000UL && amps < PELTIER_MIN_A) {
    if (!noCurrentAlerted) publishAlert("DEFAUT : Peltier allume mais aucun courant (fil, fusible ou relais).");
    noCurrentAlerted = true;
  } else if (amps >= PELTIER_MIN_A) {
    noCurrentAlerted = false;
  }
  peltierA = 0.7f * peltierA + 0.3f * amps;
  peltierW = peltierA * (packV > 1 ? packV : SUPPLY_V);   // P = I x U (U mesurée si possible)
  // IA n°2 : consommation MOYENNE apprise du frigo (cycles compris), sur ~15 min
  powers.fridge = 0.999f * powers.fridge + 0.001f * peltierW;
#endif
}

void updateDoor() {
#if HAS_DOOR_SWITCH
  bool open = digitalRead(PIN_DOOR) == HIGH;   // aimant éloigné = contact ouvert = HIGH
  if (open && !doorOpen) {
    doorEvents[doorEventIdx++ % 20] = millis();
    Serial.println("Porte ouverte");
  }
#else
  static unsigned long lastMs = 0;
  if (millis() - lastMs < 2000 || isnan(tAir) || isnan(tProd)) return;  // une fois par lecture
  lastMs = millis();
  bool wasOpen = doorDet.open;
  if (doorDet.update(tAir, tProd, compOn)) {
    doorEvents[doorEventIdx++ % 20] = millis();
    Serial.printf("Porte ouverte (detectee : air +%.2f C en 10 s, air - produit = %.2f C)\n",
                  doorDet.lastRise, tAir - tProd);
  }
  if (wasOpen && !doorDet.open) Serial.println("Porte refermee (l'air ne monte plus)");
  bool open = doorDet.open;
#endif
  if (open) lastDoorOpenMs = millis();
  doorOpen = open;
}

float doorOpensPerHour() {
  int n = 0;
  for (int i = 0; i < 20; i++) if (doorEvents[i] && millis() - doorEvents[i] < 3600000UL) n++;
  return n;
}

void minuteTasks() {
  static unsigned long lastMin = 0;
  static int minuteCount = 0;
  if (millis() - lastMin < 60000UL) return;
  lastMin = millis();
  minuteCount++;
  if (isnan(tProd) || isnan(tAmb)) return;

  unsigned long win = thermal.windowMinutes() * 60000UL;   // durée de la fenêtre d'apprentissage
  bool doorQuiet = millis() - lastDoorOpenMs > win || lastDoorOpenMs == 0;
#if REAL_COOLING
  bool compQuiet = !compOn && (millis() - lastCompOnMs > win || lastCompOnMs == 0);
  bool compRunning = compOn && millis() - compChangedMs > win;
#else
  bool compQuiet = true;           // maquette : la LED ne refroidit pas, la boîte se réchauffe toujours
  bool compRunning = false;
#endif
  thermal.minuteTick(tProd, tAmb, doorQuiet && compQuiet, doorQuiet && compRunning);

  // ligne pour tools/thermal_log.py
  Serial.printf("T,%d,%.3f,%.3f,%.3f,%d,%d,%.4f\n", minuteCount, tAir, tProd, tAmb,
                doorOpen ? 1 : 0, (REAL_COOLING && compOn) ? 1 : 0, thermal.k);   // 1 = froid réellement produit
  if (minuteCount % 10 == 0) saveLearned();
}

// ---------------------------------------------------------------------
// Batterie (simulée avec les puissances des vrais appareils)
// ---------------------------------------------------------------------
void updateBattery() {
  static unsigned long last = millis();
  float dtH = (millis() - last) / 3600000.0f * DEMO_SPEED;
  last = millis();
  bool onBat = state == ST_BATTERY || state == ST_WAIT;
  if (onBat) {
#if MEASURE_CURRENT
    float fridgeNow = peltierW;                            // puissance MESURÉE du Peltier
#else
    // courant non mesuré : Peltier allumé = courant nominal x tension du pack
    float fridgeNow = compOn ? PELTIER_NOMINAL_A * (packV > 1 ? packV : SUPPLY_V) : 0;
#endif
    float p = fridgeNow
            + (ledOn(PIN_LED_CLIM) ? powers.clim : 0)
            + (ledOn(PIN_LED_LIGHTS) ? powers.lights : 0)
            + (ledOn(PIN_LED_ROUTER) ? powers.router : 0);
    battery.drain(p, dtH);
  } else {
    battery.soc += 0.1f * dtH;                             // recharge à 0,1 C
    if (battery.soc > 1) battery.soc = 1;
  }
}

// ---------------------------------------------------------------------
// Décision et actions
// ---------------------------------------------------------------------
void decideAndAct() {
  static unsigned long last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  if (isnan(tProd) || isnan(tAmb) || isnan(tAir)) return;

  bool onBat = state == ST_BATTERY || state == ST_WAIT;
  uint32_t now = nowS();
  din = DecisionInput();
  din.onBattery = onBat;
  if (clockValid()) {
    time_t t = time(nullptr); struct tm tmv; localtime_r(&t, &tmv);
    din.hour = tmv.tm_hour + tmv.tm_min / 60.0f;
    din.month = tmv.tm_mon + 1;
  }
  if (annStartS > 0 && annEndS > now) {
    din.announceInMin = annStartS > now ? (annStartS - now) / 60.0f : 0;
    din.announcedDurH = (annEndS - annStartS) / 3600.0f;
  }
  // coupure en cours sans lien avec l'annonce : on utilise la durée apprise
  if (onBat && !(annStartS > 0 && outageStartS + 1800 >= annStartS && outageStartS <= annEndS))
    din.announcedDurH = -1;
  din.tmaxForecast = tmaxForecast;
  din.pUnstable = pUnstableAvg;
  din.tProd = tProd; din.tAir = tAir; din.tAmb = tAmb;
  din.doorOpensPerHour = doorOpensPerHour();
  din.outageElapsedH = onBat ? (millis() - outageStartMs) / 3600000.0f * DEMO_SPEED : 0;
  din.learnedOutageH = learnedOutageH;
  din.profile = profile;
  din.safetyAI = SAFETY_USE_AI;

  dec = decide(din, thermal, battery, powers);

  // ---- moteur du frigo : thermostat sur l'air, dans la bande choisie ----
  bool wantComp = compOn;
  if (peltierFault) wantComp = false;                       // défaut : Peltier bloqué
  else if (!dec.fridgeAllowed) wantComp = false;
  else if (pauseUntilMs && (long)(pauseUntilMs - millis()) > 0) wantComp = false;   // pause d'apprentissage
  else if (tAir > dec.bandHigh) wantComp = true;
  else if (tAir < dec.bandLow) wantComp = false;
  setCompressor(wantComp);

  // ---- autres appareils ----
  if (state == ST_NORMAL) {
    setLed(PIN_LED_LIGHTS, true); setLed(PIN_LED_ROUTER, true); setLed(PIN_LED_CLIM, true);
  } else if (state == ST_RETURN) {
    setLed(PIN_LED_LIGHTS, reconnectStep >= 1);
    setLed(PIN_LED_ROUTER, reconnectStep >= 2);
    setLed(PIN_LED_CLIM, reconnectStep >= 3);
  } else {
    setLed(PIN_LED_LIGHTS, dec.lights);
    setLed(PIN_LED_ROUTER, dec.router);
    bool clim = dec.clim;
    if (dec.climCycle) clim = (millis() / 60000UL) % 30 < 10;   // 10 min sur 30
    setLed(PIN_LED_CLIM, clim);
  }

  // ---- LED de couleur ----
  if (dec.alert == ALERT_BATTERY_SHORT || dec.alert == ALERT_TOO_WARM) setRgb(1, 0, 0);  // rouge
  else if (onBat) setRgb(1, 1, 0);                                                        // orange
  else setRgb(0, 1, 0);                                                                    // vert

  // ---- alertes (envoyées quand elles changent) ----
  String text = "";
  switch (dec.alert) {
    case ALERT_ON_BATTERY:
      text = "Coupure. Batterie : " + fmtH(dec.runtimeH) + ". Marchandise sure : " + fmtH(dec.tSafeH) +
             (dec.lights ? "." : ". Lumieres coupees.");
      break;
    case ALERT_BATTERY_SHORT:
      text = String("La batterie ne suffira pas. Vendez ou deplacez la marchandise (") +
             PROFILES[profile].name + ") avant " + hhmmIn(dec.deadlineH) + ".";
      break;
    case ALERT_PRECOOL:
      text = "Risque de coupure (" + String(dec.risk, 2) + ") : refroidissement renforce.";
      break;
    case ALERT_TOO_WARM:
      text = "ATTENTION : la marchandise a depasse " + String(PROFILES[profile].tMax, 0) + " C.";
      break;
    default: break;
  }
  // son selon l'alerte : à chaque nouvelle alerte, puis répété pour les dangers
  static unsigned long lastRepeat = 0;
  bool newAlert = dec.alert != lastAlert;
  if (newAlert) buzzerMuted = false;
  if (newAlert && text.length()) publishAlert(text);
  unsigned long repeatMs = dec.alert == ALERT_TOO_WARM ? 60000UL :
                           dec.alert == ALERT_BATTERY_SHORT ? 300000UL : 0;
  if (newAlert || (repeatMs && !buzzerMuted && millis() - lastRepeat >= repeatMs)) {
    lastRepeat = millis();
    if (dec.alert == ALERT_TOO_WARM) buzz(5, 120, 120);            // danger : 5 bips rapides
    else if (dec.alert == ALERT_BATTERY_SHORT) buzz(3, 250, 250);  // batterie : 3 bips
    else if (dec.alert == ALERT_PRECOOL && newAlert) buzz(1, 100, 0);
  }
  lastAlert = dec.alert;
  lastAlertText = text;
}

// ---------------------------------------------------------------------
// Écran et MQTT
// ---------------------------------------------------------------------
void updateDisplay() {
#if HAS_OLED
  static unsigned long last = 0;
  if (millis() - last < 1000) return;
  last = millis();
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.printf("%s %.1fV %s\n", STATE_NAMES[state], gridV, GM_LABELS[lastPred.cls]);
  oled.printf("Bat %.0f%% aut %s\n", battery.usablePercent(), fmtH(dec.runtimeH).c_str());
  oled.printf("Prod %.1fC sur %s\n", tProd, fmtH(dec.tSafeH).c_str());
  oled.printf("Mag %.1fC k=%.3f\n", tAmb, thermal.k);
  oled.printf("Risque %.2f %s\n", dec.risk, dec.precool ? "PRE-FROID" : "");
  oled.printf("%s %s\n", PROFILES[profile].name, doorOpen ? "PORTE" : "");
  oled.display();
#endif
}

void updateNetwork() {
  static unsigned long lastTry = 0, lastPub = 0;
  if (strlen(WIFI_SSID) == 0) return;
  if (WiFi.status() != WL_CONNECTED) return;          // WiFi.begin() se reconnecte tout seul
  if (!mqtt.connected() && millis() - lastTry > 10000) {
    lastTry = millis();
    if (mqtt.connect("edge-energy-manager")) {
      mqtt.subscribe("eem/announce");
      mqtt.subscribe("eem/cmd");
      Serial.println("MQTT connecté");
    }
  }
  mqtt.loop();
  if (mqtt.connected() && millis() - lastPub > 10000) {
    lastPub = millis();
    JsonDocument doc;
    doc["etat"] = STATE_NAMES[state];
    doc["v"] = gridV;
    doc["ia1"] = GM_LABELS[lastPred.cls];
    doc["bat"] = battery.usablePercent();
    doc["autonomie_h"] = dec.runtimeH;
    doc["t_prod"] = tProd; doc["t_air"] = tAir; doc["t_mag"] = tAmb;
    doc["t_sur_h"] = dec.tSafeH;
    doc["k"] = thermal.k;
    doc["i_peltier"] = peltierA;
    doc["v_pack"] = packV;
    doc["risque"] = dec.risk;
    doc["profil"] = PROFILES[profile].name;
    char buf[384];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish("eem/state", (const uint8_t*)buf, n, false);
  }
}

// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);                   // lecture des commandes sans bloquer
  int outs[] = {PIN_LED_CLIM, PIN_LED_LIGHTS, PIN_LED_ROUTER, PIN_BUZZER, PIN_RGB_R, PIN_RGB_G, PIN_RGB_B};
  for (int p : outs) { if (p >= 0) { pinMode(p, OUTPUT); digitalWrite(p, LOW); } }
  if (PIN_LED_ROUTER < 0) powers.router = 0;   // pas de routeur dans la maquette
  battery.capacityAh = BAT_CAPACITY_AH;        // pack 3S Li-ion
  battery.nominalV = BAT_NOMINAL_V;
  battery.socMin = BAT_SOC_MIN;
#if HAS_SRC_RELAY
  pinMode(PIN_SRC_RELAY, OUTPUT);
  setSourceBattery(false);                 // à faire tout de suite : sinon le relais claque au reset
#endif
#if HAS_DOOR_SWITCH
  pinMode(PIN_DOOR, INPUT);                // GPIO35 : pas de pull-up interne, résistance externe
#endif
#if PELTIER_USE_RELAY
  digitalWrite(PIN_LED_FRIDGE, RELAY_ACTIVE_LOW ? HIGH : LOW);  // relais au repos AVANT d'activer la sortie
  pinMode(PIN_LED_FRIDGE, OUTPUT);
#elif ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_LED_FRIDGE, 1000, 8);
#else
  ledcSetup(0, 1000, 8);
  ledcAttachPin(PIN_LED_FRIDGE, 0);
#endif
  fridgePwm(0);                            // Peltier éteint dès le démarrage

  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(400000);
  if (!inaGrid.begin()) Serial.println("ERREUR : INA219 n°1 introuvable");
  inaGrid.setCalibration_32V_2A();
  battery.liIon3S = BAT_LIION_3S;
#if HAS_BATTERY_INA
  if (inaBat.begin()) {
    inaBat.setCalibration_32V_2A();          // jusqu'à 32 V et environ 3,2 A
    float v = inaBat.getBusVoltage_V();      // Peltier éteint au démarrage : tension au repos
    battery.soc = battery.liIon3S ? Battery::socFromLiIon3S(v) : Battery::socFromRestVoltage(v);
    Serial.printf("Pack n°2 : %.2f V -> niveau de charge %.0f %%\n", v, battery.soc * 100);
  } else {
    Serial.println("ERREUR : INA219 n°2 (0x41) introuvable -- pont A0 soudé ?");
  }
#endif
#if HAS_OLED
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("ERREUR : écran OLED introuvable");
#endif

#if TEMP_SENSOR_DHT11
  dhtIn.begin();
#if PIN_DHT_OUT >= 0
  dhtOut.begin();
#endif
  delay(1500);                             // le DHT11 a besoin d'un moment après l'allumage
  Serial.printf("DHT11 boite : %.1f C", dhtIn.readTemperature());
#if PIN_DHT_OUT >= 0
  Serial.printf(" | DHT11 magasin : %.1f C", dhtOut.readTemperature());
#endif
  Serial.println();
  thermal.setWindowMinutes(30);            // DHT11 arrondi au degré : fenêtre de 30 min
#else
  sensors.begin();
  sensors.setWaitForConversion(false);     // on ne bloque pas le programme pendant la mesure
  sensors.requestTemperatures();
  Serial.printf("%d sonde(s) DS18B20 trouvée(s)\n", sensors.getDeviceCount());
  delay(800);
  for (int i = 0; i < sensors.getDeviceCount(); i++)
    Serial.printf("  sonde %d : %.2f C\n", i, sensors.getTempCByIndex(i));
  if (!sensors.getAddress(addrAir, IDX_AIR) || !sensors.getAddress(addrAmb, IDX_AMB))
    Serial.println("ERREUR : il faut 2 sondes DS18B20 (vérifier le câblage et la 4,7 k)");
#if HAS_PRODUCT_PROBE
  if (!sensors.getAddress(addrProd, IDX_PROD))
    Serial.println("ERREUR : 3e sonde (produit) introuvable");
#endif
  sensors.requestTemperatures();
  thermal.setWindowMinutes(5);
#endif

#if HAS_CURRENT_SENSOR
  // Zéro du capteur de courant : Peltier éteint, on mesure la tension de repos
  float z = 0;
  for (int i = 0; i < 200; i++) { z += readCurrentSensorV(); delay(2); }
  currentZeroV = z / 200;
  Serial.printf("Capteur de courant : zero = %.3f V (doit etre proche de 2,5 V)\n", currentZeroV);
#endif

  prefs.begin("eem", false);
  thermal.Sxy = prefs.getFloat("Sxy", thermal.Sxy);
  thermal.Sxx = prefs.getFloat("Sxx", thermal.Sxx);
  thermal.k = prefs.getFloat("k", thermal.k);
  thermal.coolRate = prefs.getFloat("cool", thermal.coolRate);
  learnedOutageH = prefs.getFloat("outH", 2.0f);
  profile = (Profile)prefs.getInt("prof", (int)DEFAULT_PROFILE);
  Serial.printf("k chargé = %.3f /h, profil %s\n", thermal.k, PROFILES[profile].name);

  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    configTzTime("CET-1", "pool.ntp.org");     // heure de Tunis (UTC+1)
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMqtt);
    mqtt.setBufferSize(512);
  }
  stateSinceMs = millis();
  nextSampleUs = micros();
  Serial.println("Prêt. Commandes : profil=..., soc=..., annonce=min,h,tmax, annule, pause=min, reset, resetk, info");
}

void loop() {
  sampleGrid();            // IA n°1 (toutes les 10 ms)
  updateStateMachine();    // protection et bascule
  updateTemperatures();    // DS18B20 (toutes les 2 s)
  updateDoor();
  minuteTasks();           // IA n°3 : apprentissage de k
  updatePackVoltage();     // tension du pack n°2 (INA219 n°2)
  updateCurrent();         // courant du Peltier
  updateBattery();         // IA n°2
  decideAndAct();          // décision (toutes les secondes)
  updateSoftStart();       // démarrage doux du moteur
  updateBuzzer();          // sons d'alerte (non bloquant)
  updateDisplay();
  updateNetwork();
  if (Serial.available()) handleCommand(Serial.readStringUntil('\n'));
}
