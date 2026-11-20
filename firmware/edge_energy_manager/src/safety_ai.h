// IA n°3 (version apprise) : temps de sécurité de la marchandise dès la 1re seconde
// de la coupure. Réseau 7 -> 16 -> 1, poids int8, entraîné par
// tools/train_safety_model.py sur 40 000 coupures simulées (tous les cas de
// T_produit, T_air, T_amb, T_max, k, portes).
#pragma once
#include <math.h>
#include "safety_model.h"

// Newton seul (même calcul que dans le script Python) : entrée n°7 du réseau.
inline float newtonRawH(float tProd, float tAmb, float tMax, float k) {
  if (tAmb <= tMax) return 99.0f;                 // le magasin ne peut pas réchauffer au-delà de T_max
  if (tProd >= tMax) return 0.0f;                 // déjà trop chaud
  float t = -logf((tAmb - tMax) / (tAmb - tProd)) / k;
  return t > 99.0f ? 99.0f : (t < 0 ? 0 : t);
}

// Renvoie le temps de sécurité prédit (heures, SANS la marge de prudence).
inline float safeTimeAI(float tProd, float tAir, float tAmb, float tMax,
                        float k, float doorsPerH) {
  float in[SM_IN] = {tProd, tAir, tAmb, tMax, k, doorsPerH,
                     logf(newtonRawH(tProd, tAmb, tMax, k) + 0.05f)};
  float x[SM_IN];
  for (int i = 0; i < SM_IN; i++) x[i] = (in[i] - SM_MEAN[i]) / SM_STD[i];  // normalisation
  float y = SM_B2[0];
  for (int j = 0; j < SM_HID; j++) {               // couche cachée (ReLU)
    float acc = 0;
    for (int i = 0; i < SM_IN; i++) acc += SM_W1[j][i] * x[i];
    float h = acc * SM_S1 + SM_B1[j];
    if (h > 0) y += SM_W2[0][j] * SM_S2 * h;       // couche de sortie
  }
  float t = expf(y) - 0.05f;                       // le réseau sort log(t + 0,05)
  if (t < 0) t = 0;
  if (t > SM_CAP_H) t = SM_CAP_H;
  return t;
}
