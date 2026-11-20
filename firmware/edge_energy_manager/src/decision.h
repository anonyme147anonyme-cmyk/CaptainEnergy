// =====================================================================
// decision.h -- Score de risque + moteur de décision
// ---------------------------------------------------------------------
// Rassemble tout ce que les IA ont estimé et décide :
//   - faut-il pré-refroidir ?
//   - le moteur du frigo a-t-il le droit de tourner ? entre quelles températures ?
//   - quels appareils garder allumés sur batterie ?
//   - faut-il envoyer une alerte ?
// C++ pur : testable sur PC.
// =====================================================================
#pragma once
#include <math.h>
#include "thermal.h"
#include "battery.h"
#include "safety_ai.h"

// ---------------- profils de marchandise ----------------
enum Profile { PROF_VOLAILLE = 0, PROF_LAITIER, PROF_MEDICAMENT, PROF_LEGUMES, PROF_MAQUETTE, PROF_COUNT };
struct ProfileLimits { const char* name; float tMin; float tMax; };
// Valeurs indicatives (à vérifier avec la réglementation / la notice des médicaments)
static const ProfileLimits PROFILES[PROF_COUNT] = {
  {"volaille",   0.0f,  4.0f},
  {"laitier",    1.0f,  4.0f},
  {"medicament", 2.0f,  8.0f},
  {"legumes",    4.0f, 10.0f},
  // Profil de la maquette : une petite boîte avec un Peltier descend rarement
  // sous 8-10 °C. On décale donc les seuils pour la démo (à dire au jury).
  {"maquette",  10.0f, 16.0f},
};

// ---------------- puissances des appareils simulés (W) ----------------
// Chaque LED représente un appareil réel ; on lui associe la puissance de la maquette 12 V.
struct LoadPowers { float fridge = 12, clim = 6, lights = 5, router = 6; };

// ---------------- entrées ----------------
struct DecisionInput {
  bool  onBattery = false;
  float hour = -1;            // heure locale (0-24), -1 si inconnue (pas d'Internet)
  int   month = -1;           // 1-12, -1 si inconnu
  float announceInMin = -1;   // minutes avant une coupure annoncée, -1 si aucune
  float announcedDurH = -1;   // durée annoncée (h), -1 si inconnue
  float tmaxForecast = NAN;   // température max prévue (°C), NAN si inconnue
  float pUnstable = 0;        // proba "instable" + "baisse" donnée par l'IA n°1
  float tProd = 4, tAir = 4, tAmb = 30;
  float doorOpensPerHour = 0;
  float outageElapsedH = 0;   // depuis combien de temps on est sur batterie
  float learnedOutageH = 2;   // durée moyenne des coupures mesurées (départ : 2 h)
  Profile profile = PROF_VOLAILLE;
  bool  safetyAI = false;     // vrai : temps de sécurité par le réseau appris (IA n°3)
};

// ---------------- sorties ----------------
enum Alert { ALERT_NONE = 0, ALERT_ON_BATTERY, ALERT_BATTERY_SHORT, ALERT_PRECOOL, ALERT_TOO_WARM };
struct Decision {
  float risk = 0;             // score de risque de coupure (0-1)
  bool  precool = false;      // pré-refroidissement actif
  bool  fridgeAllowed = true; // le moteur du frigo a le droit de tourner
  float bandLow = 2, bandHigh = 3;   // le moteur démarre au-dessus de bandHigh, s'arrête sous bandLow
  bool  clim = true, lights = true, router = true;
  bool  climCycle = false;    // clim par cycles 10 min / 20 min
  float tSafeH = 99;          // temps de sécurité de la marchandise (h)
  float runtimeH = 99;        // autonomie avec les appareils gardés (h)
  float expectedRemainingH = 0;
  Alert alert = ALERT_NONE;
  float deadlineH = -1;       // dans combien d'heures la marchandise devient dangereuse
};

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// Score de risque R = 0,5 A + 0,2 H + 0,1 P + 0,2 I
//  A : coupure annoncée dans l'heure (MQTT ou commande annonce=)
//  H : chaleur -- prévision météo si elle existe, sinon température MESURÉE du magasin
//  P : plage 17 h - 22 h en été (seulement si l'heure est connue : Wi-Fi/NTP)
//  I : probabilité "instable" ou "baisse" donnée par l'IA n°1
// Sans annonce STEG (hors ligne, ou aucune coupure annoncée), A n'existe pas :
// on divise par 0,5 (le poids des signaux restants) pour que R puisse encore
// atteindre 0,5. Poids effectifs : H 0,4 - P 0,2 - I 0,4.
inline float riskScore(const DecisionInput& in) {
  bool hasAnnounce = in.announceInMin >= 0;
  bool hasForecast = !isnan(in.tmaxForecast);
  float A = (hasAnnounce && in.announceInMin <= 60) ? 1.0f : 0.0f;
  float heat = hasForecast ? in.tmaxForecast : in.tAmb;
  float H = clamp01((heat - 32.0f) / 8.0f);
  bool summer = (in.month < 0) || (in.month >= 6 && in.month <= 9);
  float P = (in.hour >= 17 && in.hour < 22 && summer) ? 1.0f : 0.0f;
  float I = clamp01(in.pUnstable);
  float local = 0.2f * H + 0.1f * P + 0.2f * I;   // ce que le boîtier sait tout seul
  if (!hasAnnounce) return local / 0.5f;   // pas d'annonce : on rééchelonne
  return 0.5f * A + local;
}

inline Decision decide(const DecisionInput& in, const ThermalModel& th,
                       const Battery& bat, const LoadPowers& pw) {
  Decision d;
  const ProfileLimits& lim = PROFILES[in.profile];
  d.risk = riskScore(in);

  // Une porte souvent ouverte fait entrer de la chaleur : on raccourcit le temps
  // de sécurité par prudence (10 ouvertures/h -> -33 %).
  float doorFactor = 1.0f / (1.0f + 0.05f * in.doorOpensPerHour);
  if (in.safetyAI && in.tAmb <= lim.tMax)
    d.tSafeH = 99;   // le magasin est plus froid que la limite : jamais dangereux
  else if (in.safetyAI)   // réseau appris sur 40 000 coupures simulées, avec marge de prudence
    d.tSafeH = safeTimeAI(in.tProd, in.tAir, in.tAmb, lim.tMax, th.k, in.doorOpensPerHour) * SM_MARGIN;
  else               // repli : formule de Newton seule
    d.tSafeH = th.safeTime(in.tProd, in.tAmb, lim.tMax) * doorFactor;

  float mid = (lim.tMin + lim.tMax) / 2.0f;

  // ======================= SUR LE RÉSEAU STEG =======================
  if (!in.onBattery) {
    d.bandLow = mid - 0.5f;
    d.bandHigh = mid + 0.5f;
    float target = lim.tMin + 1.0f;                       // cible du pré-refroidissement
    float leadH = th.precoolLeadHours(in.tProd, target);  // temps nécessaire pour y arriver
    bool announcedSoon = in.announceInMin >= 0 && in.announceInMin <= leadH * 60.0f;
    bool riskyNoAnnounce = in.announceInMin < 0 && d.risk >= 0.5f;
    if (d.risk >= 0.5f && (announcedSoon || riskyNoAnnounce)) {
      d.precool = true;
      d.bandLow = target - 0.5f;
      if (d.bandLow < lim.tMin) d.bandLow = lim.tMin;     // jamais sous le seuil bas
      d.bandHigh = target + 0.5f;
      d.alert = ALERT_PRECOOL;
    }
    d.runtimeH = bat.runtimeHours(pw.fridge + pw.clim + pw.lights + pw.router);
    return d;
  }

  // ======================= SUR BATTERIE =======================
  d.alert = ALERT_ON_BATTERY;
  float expected = (in.announcedDurH > 0) ? in.announcedDurH : in.learnedOutageH;
  d.expectedRemainingH = expected - in.outageElapsedH;
  if (d.expectedRemainingH < 0.25f) d.expectedRemainingH = 0.25f;  // on reste prudent
  float rem = d.expectedRemainingH;

  float rtFridge = bat.runtimeHours(pw.fridge);

  // Batterie presque vide : seul le frigo reste
  if (bat.usablePercent() < 20.0f) {
    d.clim = d.lights = d.router = false;
    d.bandLow = lim.tMax - 1.5f;
    d.bandHigh = lim.tMax - 0.5f;
    d.runtimeH = rtFridge;
  }
  // Cas 1 : le froid stocké suffit -> moteur éteint, le commerce continue
  else if (d.tSafeH > rem + 0.25f) {
    d.fridgeAllowed = false;
    float base = 0;                       // le frigo ne consomme rien
    d.lights = bat.runtimeHours(base + pw.lights) >= rem;
    base += d.lights ? pw.lights : 0;
    d.router = bat.runtimeHours(base + pw.router) >= rem;
    base += d.router ? pw.router : 0;
    d.clim = false;
    d.runtimeH = bat.runtimeHours(base > 0 ? base : 0.1f);
  }
  // Cas 2 : il faut faire tourner le moteur -> on le laisse monter près de T_max
  else {
    d.bandLow = lim.tMax - 1.5f;
    d.bandHigh = lim.tMax - 0.5f;
    float base = pw.fridge;
    d.lights = bat.runtimeHours(base + pw.lights) >= rem;   // lumières d'abord (P3)
    base += d.lights ? pw.lights : 0;
    d.router = bat.runtimeHours(base + pw.router) >= rem;   // routeur ensuite (P4)
    base += d.router ? pw.router : 0;
    d.clim = false;
    d.runtimeH = bat.runtimeHours(base);
  }

  // Règle clinique : médicaments + plus de 35 °C + batterie > 50 % -> clim en cycles courts
  if (in.profile == PROF_MEDICAMENT && in.tAmb > 35.0f && bat.usablePercent() > 50.0f) {
    d.clim = true;
    d.climCycle = true;
  }

  // Alerte précoce : la batterie ne tiendra pas jusqu'au retour du courant
  float toBand = th.timeToReach(in.tProd, in.tAmb, lim.tMax - 0.5f);
  float lastMargin = th.timeToReach(lim.tMax - 0.5f, in.tAmb, lim.tMax);
  float holdH = toBand + rtFridge + lastMargin;   // durée totale avant T_max
  if (holdH < rem) {
    d.alert = ALERT_BATTERY_SHORT;
    d.deadlineH = holdH;
  }
  if (in.tProd >= lim.tMax) {
    d.alert = ALERT_TOO_WARM;
    d.deadlineH = 0;
  }
  return d;
}
