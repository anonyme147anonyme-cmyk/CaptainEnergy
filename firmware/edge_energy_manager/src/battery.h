// =====================================================================
// battery.h -- IA n°2 : niveau de charge (SoC) et autonomie
// ---------------------------------------------------------------------
// 1) Comptage de charge : on enlève l'énergie consommée.
// 2) Recalage par la tension au repos (table de la batterie plomb 12 V).
// 3) Autonomie = énergie utilisable / puissance consommée (lissée).
// C++ pur : testable sur PC.
// =====================================================================
#pragma once

struct Battery {
  float capacityAh = 7.0f;     // batterie 12 V - 7 Ah
  float nominalV   = 12.0f;
  float socMin     = 0.50f;    // on ne descend pas sous 50 % (batterie plomb)
  float soc        = 1.0f;     // niveau de charge, de 0 à 1
  float powerAvgW  = 0.0f;     // puissance consommée, lissée (W)

  // Tension au repos -> niveau de charge (interpolation linéaire)
  static float socFromRestVoltage(float v) {
    const float V[] = {11.8f, 12.1f, 12.2f, 12.5f, 12.7f};
    const float S[] = {0.30f, 0.50f, 0.60f, 0.80f, 1.00f};
    if (v <= V[0]) return S[0] * (v > 10.5f ? (v - 10.5f) / (V[0] - 10.5f) : 0);
    for (int i = 1; i < 5; i++)
      if (v <= V[i]) return S[i - 1] + (S[i] - S[i - 1]) * (v - V[i - 1]) / (V[i] - V[i - 1]);
    return 1.0f;
  }

  // Pack 3S Li-ion (3 cellules en série) : tension au repos -> niveau de charge.
  // Table approximative d'une cellule (3,27 V = vide, 4,20 V = pleine), x 3.
  bool liIon3S = false;
  static float socFromLiIon3S(float v) {
    const float V[] = {9.81f, 10.83f, 11.07f, 11.19f, 11.31f, 11.40f, 11.52f, 11.73f, 11.94f, 12.24f, 12.60f};
    const float S[] = {0.00f, 0.05f,  0.10f,  0.20f,  0.30f,  0.40f,  0.50f,  0.65f,  0.75f,  0.85f,  1.00f};
    if (v <= V[0]) return 0;
    for (int i = 1; i < 11; i++)
      if (v <= V[i]) return S[i - 1] + (S[i] - S[i - 1]) * (v - V[i - 1]) / (V[i] - V[i - 1]);
    return 1.0f;
  }

  // À appeler régulièrement pendant la décharge.
  //  powerW : puissance tirée sur la batterie ; dtHours : temps écoulé (h)
  void drain(float powerW, float dtHours) {
    float usedAh = (powerW / nominalV) * dtHours;
    soc -= usedAh / capacityAh;
    if (soc < 0) soc = 0;
    // lissage (moyenne glissante exponentielle) pour une autonomie stable
    powerAvgW = (powerAvgW <= 0) ? powerW : 0.9f * powerAvgW + 0.1f * powerW;
  }

  // Quand la batterie est au repos depuis un moment : on recale.
  void correctWithRestVoltage(float v) {
    float s = liIon3S ? socFromLiIon3S(v) : socFromRestVoltage(v);
    soc = 0.7f * soc + 0.3f * s;     // on mélange pour éviter les sauts
  }

  // Énergie encore utilisable (Wh)
  float usableWh() const {
    float u = (soc - socMin) * capacityAh * nominalV;
    return u > 0 ? u : 0;
  }

  // Autonomie (heures) pour une puissance donnée
  float runtimeHours(float powerW) const {
    if (powerW < 0.1f) return 99;
    return usableWh() / powerW;
  }

  // Niveau "affiché" à l'utilisateur : 0 % = plus d'énergie utilisable
  float usablePercent() const {
    float p = (soc - socMin) / (1.0f - socMin) * 100.0f;
    return p < 0 ? 0 : (p > 100 ? 100 : p);
  }
};
