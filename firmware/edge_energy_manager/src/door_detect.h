// Détection de l'ouverture de la porte SANS contact de porte.
// Idée : porte ouverte -> l'air chaud du magasin entre -> la sonde AIR monte vite
// et passe au-dessus du produit (qui, lui, ne bouge presque pas grâce à sa masse).
// Porte fermée, l'air ne peut monter que lentement (loi de Newton).
// Appelée toutes les 2 s (à chaque nouvelle lecture des DS18B20).
#pragma once

#ifndef DOOR_RISE_C
#define DOOR_RISE_C   0.30f   // montée de l'air en 10 s qui signifie "porte ouverte" (°C)
#endif
#ifndef DOOR_GAP_C
#define DOOR_GAP_C    0.30f   // l'air doit dépasser le produit d'au moins ça (°C)
#endif
#ifndef DOOR_STILL_C
#define DOOR_STILL_C  0.05f   // montée en 10 s sous laquelle l'air est "calme" (°C)
#endif
#define DOOR_MAX_OPEN_S  180  // au-delà, on considère la porte refermée (sécurité)
#define DOOR_IGNORE_S     60  // on ignore la 1re minute après un démarrage/arrêt du froid

struct DoorDetector {
  float hist[6] = {0};        // 6 dernières mesures de l'air = 10 s d'historique
  int   n = 0;
  bool  open = false;
  int   openS = 0;            // depuis combien de secondes la porte est "ouverte"
  int   calmCount = 0;        // mesures calmes consécutives
  int   sinceCompChangeS = 999;
  bool  lastComp = false;
  float lastRise = 0;         // montée de l'air sur 10 s (affichée pour le réglage)

  // Renvoie true au moment où une NOUVELLE ouverture est détectée.
  bool update(float tAir, float tProd, bool compOn, float dtS = 2.0f) {
    if (compOn != lastComp) { sinceCompChangeS = 0; lastComp = compOn; }
    else if (sinceCompChangeS < 999) sinceCompChangeS += (int)dtS;

    for (int i = 5; i > 0; i--) hist[i] = hist[i - 1];   // décale l'historique
    hist[0] = tAir;
    if (n < 6) { n++; return false; }                    // pas encore 10 s de mesures

    float minOld = hist[1];
    for (int i = 2; i < 6; i++) if (hist[i] < minOld) minOld = hist[i];
    lastRise = tAir - minOld;                            // montée sur les 10 dernières s

    if (!open) {
      bool fast = lastRise >= DOOR_RISE_C;               // monte trop vite pour une porte fermée
      bool aboveProduct = tAir - tProd >= DOOR_GAP_C;    // l'air chaud dépasse le produit
      if (fast && aboveProduct && sinceCompChangeS >= DOOR_IGNORE_S) {
        open = true; openS = 0; calmCount = 0;
        return true;
      }
    } else {
      openS += (int)dtS;
      calmCount = (lastRise < DOOR_STILL_C) ? calmCount + 1 : 0;
      if (calmCount >= 3 || openS >= DOOR_MAX_OPEN_S) open = false;  // l'air ne monte plus : refermée
    }
    return false;
  }
};
