// =====================================================================
// thermal.h -- IA n°3 : modèle thermique appris du frigo
// ---------------------------------------------------------------------
// Loi de Newton : dT/dt = k * (T_amb - T)
//   T     = température du produit (ou de l'air du frigo, filtrée)
//   T_amb = température de l'air du magasin, autour du frigo
//   k     = vitesse de réchauffement propre à CE frigo (en 1/heure)
// L'ESP32 apprend k tout seul par moindres carrés avec oubli.
// La vitesse de montée est calculée par régression linéaire sur une
// fenêtre glissante (5 min avec un DS18B20, 30 min avec un DHT11 qui ne
// mesure qu'au degré près).
// C++ pur : testable sur PC.
// =====================================================================
#pragma once
#include <math.h>

#define THERM_MAX_HIST 31    // jusqu'à 30 minutes d'historique (une mesure par minute)
#define THERM_LAMBDA 0.98f   // facteur d'oubli (0,98 : les vieilles mesures comptent moins)
#define THERM_MIN_GAP 2.0f   // on n'apprend que si T_amb - T > 2 °C (sinon trop de bruit)

struct ThermalModel {
  // --- valeurs apprises ---
  float Sxy = 0.05f;         // départ : k = 0,05 /h, avec un poids faible (Sxx = 1)
  float Sxx = 1.0f;
  float k   = 0.05f;
  int   nUpdates = 0;

  // vitesse de refroidissement quand le moteur tourne (°C/h), apprise elle aussi
  float coolRate = 2.0f;

  // --- historique (une mesure par minute, la plus récente en premier) ---
  float hist[THERM_MAX_HIST];
  int   histLen = 6;         // 6 mesures = fenêtre de 5 min (DS18B20) ; 31 = 30 min (DHT11)
  int   nHist = 0;

  void setWindowMinutes(int minutes) {
    histLen = minutes + 1;
    if (histLen > THERM_MAX_HIST) histLen = THERM_MAX_HIST;
    if (histLen < 3) histLen = 3;
    nHist = 0;
  }
  int windowMinutes() const { return histLen - 1; }

  // Pente (°C/h) par moindres carrés sur toute la fenêtre : bien plus robuste
  // qu'une simple différence quand le capteur arrondit au degré.
  float slopePerHour() const {
    float tMean = (histLen - 1) / 2.0f, yMean = 0;
    for (int i = 0; i < histLen; i++) yMean += hist[i];
    yMean /= histLen;
    float num = 0, den = 0;
    for (int i = 0; i < histLen; i++) {
      float t = (histLen - 1 - i) - tMean;   // hist[0] est la mesure la plus récente
      num += t * (hist[i] - yMean);
      den += t * t;
    }
    return (num / den) * 60.0f;              // °C par minute -> °C par heure
  }

  // À appeler UNE FOIS PAR MINUTE.
  //  tProd, tAmb : températures actuelles (°C)
  //  canLearnWarm : vrai si moteur éteint ET porte fermée pendant toute la fenêtre
  //  canLearnCool : vrai si moteur allumé ET porte fermée pendant toute la fenêtre
  void minuteTick(float tProd, float tAmb, bool canLearnWarm, bool canLearnCool) {
    for (int i = histLen - 1; i > 0; i--) hist[i] = hist[i - 1];   // décale l'historique
    hist[0] = tProd;
    if (nHist < histLen) nHist++;
    if (nHist < histLen) return;                       // fenêtre pas encore pleine

    float dTdt = slopePerHour();
    float tMid = 0;
    for (int i = 0; i < histLen; i++) tMid += hist[i];
    tMid /= histLen;
    float x = tAmb - tMid;                             // écart moyen avec le magasin

    if (canLearnWarm && x > THERM_MIN_GAP && dTdt > 0) {
      // moindres carrés avec oubli : k = somme(x*y) / somme(x*x)
      Sxy = THERM_LAMBDA * Sxy + x * dTdt;
      Sxx = THERM_LAMBDA * Sxx + x * x;
      k = Sxy / Sxx;
      if (k < 0.001f) k = 0.001f;                      // garde-fou
      nUpdates++;
    }
    if (canLearnCool && dTdt < 0) {
      coolRate = 0.8f * coolRate + 0.2f * (-dTdt);     // vitesse de descente, lissée
    }
  }

  // Temps (en heures) pour que le produit passe de tNow à tLimit, frigo éteint.
  // Formule : t = -(1/k) * ln((T_amb - T_limit) / (T_amb - T_now))
  float timeToReach(float tNow, float tAmb, float tLimit) const {
    if (tNow >= tLimit) return 0;                      // déjà au-dessus
    if (tAmb <= tLimit) return 99;                     // le magasin est plus froid que la limite : jamais
    return -logf((tAmb - tLimit) / (tAmb - tNow)) / k;
  }

  // Temps de sécurité de la marchandise (heures)
  float safeTime(float tProd, float tAmb, float tMax) const {
    return timeToReach(tProd, tAmb, tMax);
  }

  // Combien d'heures avant la coupure faut-il commencer à pré-refroidir ?
  //  tTarget = T_min du profil + 1 °C ; marge = 15 min
  float precoolLeadHours(float tProd, float tTarget) const {
    if (tProd <= tTarget) return 0;
    return (tProd - tTarget) / coolRate + 0.25f;
  }
};
