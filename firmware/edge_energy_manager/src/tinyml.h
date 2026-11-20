// =====================================================================
// tinyml.h -- IA n°1 : exécution du petit réseau de neurones sur l'ESP32
// ---------------------------------------------------------------------
// Réseau : 8 entrées -> 16 neurones (ReLU) -> 4 sorties (softmax)
// Les poids sont stockés en int8 (1 octet au lieu de 4) avec un facteur
// d'échelle par couche : poids_réel = poids_int8 * échelle.
// Les valeurs du modèle viennent de grid_model.h, généré par
// tools/train_grid_model.py. Pas de bibliothèque externe : tout est ici.
// =====================================================================
#pragma once
#include <math.h>
#include <stdint.h>
#include "grid_model.h"

struct GridPrediction {
  int   cls;            // indice de la classe gagnante (0..GM_OUT-1)
  float prob[GM_OUT];   // probabilité de chaque classe (somme = 1)
};

inline GridPrediction runGridModel(const float* features) {
  float x[GM_IN], h[GM_HID], o[GM_OUT];

  // 1) Normalisation : on remet chaque caractéristique à la même échelle
  //    qu'à l'entraînement (moyenne 0, écart-type 1).
  for (int i = 0; i < GM_IN; i++) x[i] = (features[i] - GM_MEAN[i]) / GM_STD[i];

  // 2) Couche cachée : h = ReLU(W1 * x + b1), avec W1 en int8
  for (int j = 0; j < GM_HID; j++) {
    float acc = 0;
    for (int i = 0; i < GM_IN; i++) acc += GM_W1[j][i] * x[i];
    acc = acc * GM_S1 + GM_B1[j];          // on ré-applique l'échelle une seule fois
    h[j] = acc > 0 ? acc : 0;               // ReLU
  }

  // 3) Couche de sortie : o = W2 * h + b2
  for (int k = 0; k < GM_OUT; k++) {
    float acc = 0;
    for (int j = 0; j < GM_HID; j++) acc += GM_W2[k][j] * h[j];
    o[k] = acc * GM_S2 + GM_B2[k];
  }

  // 4) Softmax : transforme les scores en probabilités
  float mx = o[0];
  for (int k = 1; k < GM_OUT; k++) if (o[k] > mx) mx = o[k];
  float s = 0;
  GridPrediction p;
  for (int k = 0; k < GM_OUT; k++) { p.prob[k] = expf(o[k] - mx); s += p.prob[k]; }
  p.cls = 0;
  for (int k = 0; k < GM_OUT; k++) {
    p.prob[k] /= s;
    if (p.prob[k] > p.prob[p.cls]) p.cls = k;
  }
  return p;
}
