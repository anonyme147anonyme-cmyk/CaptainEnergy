// =====================================================================
// main.cpp (collect_grid) -- IA n°1, étape 1 : COLLECTE DES DONNÉES
// ---------------------------------------------------------------------
// Mesure la tension "STEG" (pack n°1 + potentiomètre) avec l'INA219 n°1,
// 100 fois par seconde. Chaque seconde, calcule les 8 caractéristiques
// de la fenêtre et les envoie sur le port série sous la forme :
//     F,f0,f1,f2,f3,f4,f5,f6,f7
// Le script tools/log_serial.py enregistre ces lignes dans un CSV.
//
// Bibliothèque : "Adafruit INA219", installée automatiquement par PlatformIO (platformio.ini)
// Câblage : INA219 SDA -> GPIO21, SCL -> GPIO22, VCC -> 3V3, GND -> GND
//           Pack n°1 (+) -> potentiomètre -> VIN+ de l'INA219, (-) -> GND commun
// =====================================================================
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_INA219.h>
#include "grid_features.h"

Adafruit_INA219 inaGrid(0x40);          // INA219 n°1 (adresse par défaut)

const int   N_SAMPLES = 100;            // 100 mesures par fenêtre
const float DT = 0.01f;                 // une mesure toutes les 10 ms = 1 s par fenêtre
float window_[N_SAMPLES];
int   idx = 0;
unsigned long nextSampleUs = 0;

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000);                // I2C rapide : une lecture prend ~0,3 ms
  if (!inaGrid.begin()) {
    Serial.println("# ERREUR : INA219 n°1 introuvable (vérifier le câblage)");
    while (true) delay(1000);
  }
  inaGrid.setCalibration_32V_2A();      // mesure jusqu'à 32 V
  Serial.println("# Collecte prête : une ligne F,... par seconde");
  nextSampleUs = micros();
}

void loop() {
  // On attend le moment exact de la prochaine mesure (toutes les 10 ms)
  if ((long)(micros() - nextSampleUs) < 0) return;
  nextSampleUs += (unsigned long)(DT * 1e6);

  window_[idx++] = inaGrid.getBusVoltage_V();   // tension en volts

  if (idx == N_SAMPLES) {               // fenêtre complète : on calcule et on envoie
    float f[N_FEATURES];
    computeFeatures(window_, N_SAMPLES, DT, f);
    Serial.print("F");
    for (int i = 0; i < N_FEATURES; i++) {
      Serial.print(',');
      Serial.print(f[i], 4);
    }
    Serial.println();
    idx = 0;
  }
}
