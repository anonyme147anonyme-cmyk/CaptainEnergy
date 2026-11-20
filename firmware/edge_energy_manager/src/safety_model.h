// Fichier généré par tools/train_safety_model.py -- ne pas modifier à la main
// IA n°3 : temps de sécurité appris sur des coupures simulées (tous les cas)
// Entrées : t_prod, t_air, t_amb, t_max, k, portes_h, log_newton ; sortie : log(t_securite_h + 0,05)
// Test : erreur relative médiane 23 % (Newton : 33 %)
#pragma once
#include <stdint.h>
#define SM_IN 7
#define SM_HID 16
#define SM_CAP_H 24.0f
#define SM_MARGIN 0.5103f   // facteur de prudence (95 % des cas non surestimés)
static const float SM_MEAN[7] = {5.063134f, 4.555789f, 29.983889f, 9.656113f, 0.844646f, 7.255875f, -0.626975f};

static const float SM_STD[7] = {5.043096f, 5.239854f, 6.977021f, 4.420511f, 1.203013f, 6.710013f, 1.413624f};

static const int8_t SM_W1[16][7] = {
  {-5, -37, -11, 4, -10, 9, 50},
  {16, 48, 6, -16, 8, -25, -48},
  {9, 13, -24, 23, 45, -36, 68},
  {12, 18, 14, -5, -8, 28, -127},
  {-31, 14, -5, 18, 28, 5, -45},
  {-22, 17, -17, -28, -10, -7, -53},
  {-3, 9, 52, -12, 48, -26, 7},
  {4, 13, 15, 2, 28, -3, 96},
  {16, -88, 17, 46, 48, -21, -52},
  {-40, 32, -3, 19, 21, 47, -2},
  {44, -15, 1, 11, 0, -8, 17},
  {0, 30, -11, -41, -22, 10, -18},
  {23, -10, 5, -15, -6, -57, 25},
  {34, -24, 24, -42, -36, 16, 34},
  {3, -5, -1, 1, -3, -4, -101},
  {-44, 13, -12, 5, 1, -26, 93}
};

static const float SM_S1 = 0.00882802f;
static const float SM_B1[16] = {0.138403f, -0.203258f, 0.157480f, 0.345625f, 0.754500f, 0.147903f, 0.301315f, -0.219299f, -0.008424f, -0.278801f, 0.286456f, 0.835150f, -0.330915f, 0.546901f, -1.323519f, -0.270869f};

static const int8_t SM_W2[1][16] = {
  {7, -11, 8, -22, -28, -10, 3, 14, -7, 12, 1, -11, 26, -8, -127, 28}
};

static const float SM_S2 = 0.02120503f;
static const float SM_B2[1] = {0.128240f};

