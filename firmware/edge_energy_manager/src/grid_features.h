// =====================================================================
// grid_features.h -- IA n°1 : calcul des caractéristiques d'une fenêtre de tension
// ---------------------------------------------------------------------
// Ce fichier est en C++ "pur" (aucune fonction Arduino) : il compile sur
// l'ESP32 ET sur un PC, ce qui permet de le tester.
// IMPORTANT : la même copie doit exister dans firmware/collect_grid/
// (le firmware de collecte et le firmware principal doivent calculer
// EXACTEMENT les mêmes caractéristiques, sinon le modèle se trompe).
// =====================================================================
#pragma once
#include <math.h>

// Nombre de caractéristiques extraites de chaque fenêtre (entrées du modèle)
#define N_FEATURES 8

// Seuils utilisés par deux caractéristiques (prototype 12 V)
#define FEAT_LOW_V   10.4f   // en dessous : "tension basse"
#define FEAT_ZERO_V   1.0f   // en dessous : "pas de tension" (coupure)

// Remplit f[0..7] à partir de n mesures de tension v[], prises toutes les dt secondes.
//  f[0] moyenne          f[1] minimum           f[2] maximum
//  f[3] écart-type       f[4] pente (V/s)       f[5] part du temps < 10,4 V
//  f[6] part du temps < 1 V                     f[7] variation moyenne entre 2 mesures
inline void computeFeatures(const float* v, int n, float dt, float* f) {
  float sum = 0, mn = v[0], mx = v[0];
  int low = 0, zero = 0;
  float absDiff = 0;
  for (int i = 0; i < n; i++) {
    sum += v[i];
    if (v[i] < mn) mn = v[i];
    if (v[i] > mx) mx = v[i];
    if (v[i] < FEAT_LOW_V) low++;
    if (v[i] < FEAT_ZERO_V) zero++;
    if (i > 0) absDiff += fabsf(v[i] - v[i - 1]);
  }
  float mean = sum / n;

  // écart-type et pente par moindres carrés (t = i * dt)
  float var = 0, tMean = (n - 1) * dt / 2.0f, num = 0, den = 0;
  for (int i = 0; i < n; i++) {
    float d = v[i] - mean;
    var += d * d;
    float t = i * dt - tMean;
    num += t * d;
    den += t * t;
  }
  f[0] = mean;
  f[1] = mn;
  f[2] = mx;
  f[3] = sqrtf(var / n);
  f[4] = (den > 0) ? num / den : 0;
  f[5] = (float)low / n;
  f[6] = (float)zero / n;
  f[7] = (n > 1) ? absDiff / (n - 1) : 0;
}
