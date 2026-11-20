// Tests sur PC de la logique IA (sans ESP32).
// Compiler : g++ -std=c++17 -I../firmware/edge_energy_manager/src test_logic.cpp -o test_logic
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include "grid_features.h"
#include "tinyml.h"
#include "thermal.h"
#include "battery.h"
#include "decision.h"
#include "door_detect.h"

int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { printf("ECHEC: %s\n", msg); fails++; } else printf("ok   : %s\n", msg); } while (0)

int main() {
  // 1) Le modèle int8 en C++ donne la même classe que Python
  std::ifstream f("../tools/grid_model_testvectors.csv");
  std::string line;
  std::getline(f, line);  // en-tête
  int n = 0, same = 0;
  while (std::getline(f, line)) {
    std::stringstream ss(line);
    float x[8]; std::string cell;
    for (int i = 0; i < 8; i++) { std::getline(ss, cell, ','); x[i] = std::stof(cell); }
    std::getline(ss, cell, ','); int expected = std::stoi(cell);
    GridPrediction p = runGridModel(x);
    n++; if (p.cls == expected) same++;
  }
  printf("Modele : %d/%d identiques a Python\n", same, n);
  CHECK(n > 0 && same == n, "inference C++ = inference Python (int8)");

  // 2) Caractéristiques : fenêtre constante à 12 V
  float v[100]; for (int i = 0; i < 100; i++) v[i] = 12.0f;
  float feat[8]; computeFeatures(v, 100, 0.01f, feat);
  CHECK(fabsf(feat[0] - 12) < 1e-4 && feat[3] < 1e-4 && feat[5] == 0 && feat[6] == 0, "features fenetre stable");
  // rampe 12 -> 11 V en 1 s : pente ~ -1 V/s
  for (int i = 0; i < 100; i++) v[i] = 12.0f - i * 0.01f;
  computeFeatures(v, 100, 0.01f, feat);
  CHECK(fabsf(feat[4] + 1.0f) < 0.01f, "pente de la rampe = -1 V/s");
  GridPrediction pz; for (int i = 0; i < 100; i++) v[i] = 0.02f;
  computeFeatures(v, 100, 0.01f, feat); pz = runGridModel(feat);
  CHECK(pz.cls == 3, "tension nulle -> coupure");
  for (int i = 0; i < 100; i++) v[i] = 12.2f;
  computeFeatures(v, 100, 0.01f, feat); pz = runGridModel(feat);
  CHECK(pz.cls == 0, "12,2 V stable -> normal");

  // 3) Modèle thermique : on simule une boîte avec k = 0,3 /h dans un magasin à 30 °C
  ThermalModel th;
  float T = 3.0f, Tamb = 30.0f, kTrue = 0.3f;
  for (int m = 0; m < 240; m++) {          // 4 heures, une mesure par minute
    th.minuteTick(T, Tamb, true, false);
    T += kTrue * (Tamb - T) / 60.0f;       // Newton, pas d'1 min
  }
  printf("k appris = %.3f /h (vrai : %.3f), mises a jour : %d\n", th.k, kTrue, th.nUpdates);
  CHECK(fabsf(th.k - kTrue) / kTrue < 0.05f, "k appris a 5 % pres");

  // 3b) Même chose avec un DHT11 : mesures arrondies au degré, fenêtre de 30 min,
  //     produit estimé par filtre de l'air (comme dans le firmware)
  {
    ThermalModel dh; dh.setWindowMinutes(30);
    float Ta = 10.0f, Tamb2 = 25.0f, kBox = 0.5f, prodEst = NAN;
    for (int m = 0; m < 180; m++) {                  // 3 heures
      float air = roundf(Ta), amb = roundf(Tamb2);   // DHT11 : au degré près
      for (int s2 = 0; s2 < 30; s2++) {              // filtre toutes les 2 s
        if (std::isnan(prodEst)) prodEst = air;
        else prodEst += (air - prodEst) * (2.0f / 60.0f) / 15.0f;
      }
      dh.minuteTick(prodEst, amb, true, false);
      Ta += kBox * (Tamb2 - Ta) / 60.0f;
    }
    printf("DHT11 : k appris = %.3f /h (vrai : %.3f), mises a jour : %d\n", dh.k, kBox, dh.nUpdates);
    CHECK(fabsf(dh.k - kBox) / kBox < 0.25f, "DHT11 : k appris a 25 % pres malgre l'arrondi au degre");
  }

  // Formule du temps de sécurité : exemple du document (k=0,02, 35 °C, Tmax 4 °C)
  ThermalModel ex; ex.k = 0.02f;
  float t3 = ex.safeTime(3, 35, 4), t1 = ex.safeTime(1, 35, 4);
  printf("t_sur(3 C) = %.2f h, t_sur(1 C) = %.2f h\n", t3, t1);
  CHECK(fabsf(t3 - 1.59f) < 0.02f && fabsf(t1 - 4.62f) < 0.02f, "exemple du document retrouve");

  // 4) Batterie
  Battery b;
  CHECK(fabsf(Battery::socFromRestVoltage(12.1f) - 0.5f) < 1e-3, "12,1 V -> 50 %");
  CHECK(fabsf(b.runtimeHours(29) - 42.0f / 29) < 1e-3, "autonomie tout allume = 1 h 27");
  CHECK(fabsf(b.runtimeHours(12) - 3.5f) < 1e-3, "autonomie frigo seul = 3 h 30");
  float s117 = Battery::socFromLiIon3S(11.7f);
  printf("pack 3S a 11,7 V -> %.0f %%\n", s117 * 100);
  CHECK(s117 > 0.55f && s117 < 0.70f, "pack 3S Li-ion : 11,7 V -> environ 60-65 %");
  CHECK(Battery::socFromLiIon3S(12.6f) == 1.0f && Battery::socFromLiIon3S(9.5f) == 0.0f, "pack 3S : 12,6 V plein, 9,5 V vide");

  // 5) Décisions
  LoadPowers pw;
  DecisionInput in; in.profile = PROF_VOLAILLE; in.tAmb = 35;
  ThermalModel tk; tk.k = 0.02f;

  // a) coupure annoncée dans 30 min + 39 °C -> pré-refroidissement, jamais sous 0 °C
  in.onBattery = false; in.announceInMin = 30; in.tmaxForecast = 39; in.tProd = 3;
  Decision d = decide(in, tk, b, pw);
  printf("risque = %.2f, precool = %d, bande = [%.1f ; %.1f]\n", d.risk, d.precool, d.bandLow, d.bandHigh);
  CHECK(d.precool && d.bandLow >= 0.0f, "pre-refroidissement sans descendre sous T_min");

  // a2) hors ligne (ni annonce ni prévision) : magasin à 38 °C + réseau instable -> pré-refroidit
  {
    DecisionInput off; off.profile = PROF_VOLAILLE; off.tAmb = 38; off.tProd = 3;
    off.pUnstable = 0.7f;   // l'IA n°1 voit un réseau instable
    Decision d2 = decide(off, tk, b, pw);
    printf("hors ligne : risque = %.2f, precool = %d\n", d2.risk, d2.precool);
    CHECK(d2.risk >= 0.5f && d2.precool, "hors ligne, chaleur + instabilite -> pre-refroidissement");
    off.pUnstable = 0.0f; off.tAmb = 25;
    d2 = decide(off, tk, b, pw);
    CHECK(d2.risk < 0.5f && !d2.precool, "hors ligne, reseau calme et magasin frais -> rien");
    // en ligne avec prévision mais sans annonce : même comportement que hors ligne
    DecisionInput on = off; on.tmaxForecast = 39; on.pUnstable = 0.7f;
    d2 = decide(on, tk, b, pw);
    printf("en ligne sans annonce (39 C prevus, instable) : risque = %.2f\n", d2.risk);
    CHECK(d2.risk >= 0.5f && d2.precool, "en ligne sans annonce, chaleur + instabilite -> pre-refroidissement");
  }

  // b) sur batterie, produit pré-refroidi à 1 °C, coupure prévue 2 h -> moteur coupé
  in.onBattery = true; in.announceInMin = -1; in.announcedDurH = 2; in.outageElapsedH = 0; in.tProd = 1;
  d = decide(in, tk, b, pw);
  printf("cas froid stocke : tSafe=%.2f h, moteur autorise=%d, lumieres=%d\n", d.tSafeH, d.fridgeAllowed, d.lights);
  CHECK(!d.fridgeAllowed && d.lights, "froid stocke suffit -> moteur eteint, commerce continue");

  // c) même coupure, produit à 3 °C -> moteur autorisé, bande près de T_max
  in.tProd = 3;
  d = decide(in, tk, b, pw);
  printf("cas sans pre-refroidissement : tSafe=%.2f h, moteur=%d, bande=[%.1f ; %.1f]\n", d.tSafeH, d.fridgeAllowed, d.bandLow, d.bandHigh);
  CHECK(d.fridgeAllowed && d.bandHigh <= 4.0f, "moteur autorise, temperature tenue sous T_max");

  // d) coupure annoncée de 5 h, batterie à moitié -> alerte précoce
  Battery half; half.soc = 0.65f;
  in.announcedDurH = 5;
  d = decide(in, tk, half, pw);
  printf("coupure 5 h, batterie 30 %% utile : alerte=%d, limite dans %.2f h\n", d.alert, d.deadlineH);
  CHECK(d.alert == ALERT_BATTERY_SHORT && d.deadlineH > 0 && d.deadlineH < 5, "alerte precoce avec heure limite");

  // e) batterie < 20 % -> frigo seul
  Battery low; low.soc = 0.55f;
  in.announcedDurH = 2;
  d = decide(in, tk, low, pw);
  CHECK(!d.lights && !d.router && !d.clim, "batterie < 20 % -> frigo seul");

  // f) IA n°3 apprise : le C++ redonne les mêmes temps de sécurité que Python
  {
    std::ifstream fs("../tools/safety_model_testvectors.csv");
    std::string l; std::getline(fs, l);
    int nv = 0, okv = 0; float worst = 0;
    while (std::getline(fs, l)) {
      std::stringstream ss(l); std::string c; float v[8];
      for (int i = 0; i < 8; i++) { std::getline(ss, c, ','); v[i] = std::stof(c); }
      float t = safeTimeAI(v[0], v[1], v[2], v[3], v[4], v[5]);
      float err = fabsf(t - v[7]) / (v[7] + 0.05f);
      if (err > worst) worst = err;
      if (err < 0.02f) okv++;
      nv++;
    }
    printf("IA n°3 : %d/%d vecteurs identiques a Python (ecart max %.2f %%)\n", okv, nv, worst * 100);
    CHECK(nv > 0 && okv == nv, "IA n°3 C++ = Python (ecart < 2 %)");
  }

  // g) decide() avec l'IA n°3 : le temps de sécurité suit la marge de prudence
  {
    DecisionInput ai; ai.onBattery = true; ai.profile = PROF_VOLAILLE;
    ai.tProd = 2; ai.tAir = 3; ai.tAmb = 32; ai.doorOpensPerHour = 4; ai.announcedDurH = 2;
    ai.safetyAI = true;
    Decision da = decide(ai, tk, b, pw);
    float raw = safeTimeAI(2, 3, 32, PROFILES[PROF_VOLAILLE].tMax, tk.k, 4);
    printf("IA n°3 dans decide : brut %.2f h -> retenu %.2f h (x %.2f)\n", raw, da.tSafeH, SM_MARGIN);
    CHECK(fabsf(da.tSafeH - raw * SM_MARGIN) < 1e-4f && da.tSafeH > 0, "decide utilise l'IA n°3 avec la marge");
    ai.tProd = 5;   // déjà au-dessus de 4 °C
    da = decide(ai, tk, b, pw);
    CHECK(da.tSafeH < 0.05f, "produit deja trop chaud -> temps de securite ~0");
    ai.tProd = 2; ai.tAmb = 3;   // magasin plus froid que la limite
    da = decide(ai, tk, b, pw);
    CHECK(da.tSafeH >= 99, "magasin plus froid que T_max -> jamais dangereux");
  }

  // h) Porte détectée SANS contact : simulation d'une boîte (air + produit + sonde lente)
  {
    // physique simplifiée, pas de 1 s : air du frigo, sonde DS18B20 (retard 15 s), produit simulé
    struct Run { int detected = 0, real = 0, falseAlarms = 0, ignored = 0; };
    auto simulate = [](float tamb, int doorS, bool withMotor) {
      Run r; DoorDetector det;
      float air = 12, probe = 12, prod = 12, fake = 12;   // °C
      bool comp = false; int doorLeft = 0;
      int lastOpenStart = -1000; bool thisFound = true;
      unsigned seed = 7;
      auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 8) % 10000 / 10000.0f; };
      for (int t = 0; t < 6 * 3600; t++) {                  // 6 heures, pas de 1 s
        if (withMotor) { if (prod > 13) comp = true; if (prod < 11.5f) comp = false; }
        if (doorS > 0 && doorLeft == 0 && t - lastOpenStart > 300 && rnd() < 1.0f / 1200) {
          if (!thisFound) { if (det.sinceCompChangeS < DOOR_IGNORE_S + doorS) r.ignored++; }
          doorLeft = doorS; r.real++; lastOpenStart = t; thisFound = false;      // ~3 par heure
        }
        bool door = doorLeft > 0;
        float kAir = door ? 1.0f / 20 : 1.5f / 3600 * 4;    // échange d'air porte ouverte / fermée
        air += kAir * (tamb - air) + (comp ? -0.004f : 0) + (prod - air) * 0.002f;
        prod += (air - prod) / (40 * 60.0f);                // la marchandise suit lentement
        probe += (air - probe) / 15.0f;                     // sonde étanche : retard 15 s
        if (doorLeft > 0) doorLeft--;
        if (t % 2 == 0) {
          // le firmware ne connaît pas prod : il utilise la T_produit simulée (filtre)
          bool ev = det.update(probe, fake, comp);
          if (!det.open) fake += (probe - fake) * (2.0f / 60.0f) / 15.0f;
          if (ev) {
            if (t - lastOpenStart <= doorS + 40) { if (!thisFound) { r.detected++; thisFound = true; } }
            else r.falseAlarms++;
          }
        }
      }
      return r;
    };
    for (int dS : {10, 20, 45}) {
      Run a = simulate(30, dS, true);
      printf("porte ouverte %2d s, magasin 30 C : %d/%d ouvertures detectees, %d fausse(s) alerte(s)\n",
             dS, a.detected, a.real, a.falseAlarms);
      if (dS >= 20) CHECK(a.detected >= 0.9f * a.real && a.falseAlarms <= 1, "porte detectee par les temperatures");
    }
    Run q = simulate(30, 0, true);
    printf("aucune ouverture, frigo qui tourne 6 h : %d fausse(s) alerte(s)\n", q.falseAlarms);
    CHECK(q.falseAlarms == 0, "pas de fausse porte quand le froid demarre/s'arrete");
  }

  printf("\n%s (%d echec(s))\n", fails ? "DES TESTS ECHOUENT" : "TOUS LES TESTS PASSENT", fails);
  return fails ? 1 : 0;
}
