// =====================================================================
// config.h -- TOUS LES RÉGLAGES À ADAPTER sont ici
// =====================================================================
#pragma once

// ---------- Wi-Fi et MQTT (WIFI_SSID "" pour fonctionner sans Internet) ----------
#define WIFI_SSID     "NomDuWifi"
#define WIFI_PASS     "MotDePasse"
#define MQTT_HOST     "192.168.1.10"   // adresse IP du PC qui fait tourner Mosquitto
#define MQTT_PORT     1883

// ---------- Capteurs de température ----------
// 1 = DHT11, 0 = DS18B20 (votre matériel : 2 sondes DS18B20 sur GPIO4 + 4,7 k)
#define TEMP_SENSOR_DHT11 0
#define PIN_DHT_IN     4     // (mode DHT11 seulement)
#define PIN_DHT_OUT   27     // (mode DHT11 seulement) ; -1 si un seul
#define T_AMB_FIXED  25.0f   // T_amb utilisée s'il n'y a pas de capteur magasin
// Pas de sonde dans la marchandise : T_produit est SIMULÉE (fausse donnée)
// en filtrant l'air du frigo (le produit suit l'air avec retard).
#define HAS_PRODUCT_PROBE 0  // 1 = 3e DS18B20 plongée dans le produit (futur)
#define PROD_TAU_MIN  15.0f  // constante de temps du filtre (minutes)
// DS18B20 : ordre des sondes sur le fil (lancer une fois et lire le moniteur série)
#define PIN_ONEWIRE    4
#define IDX_AIR   0          // sonde dans l'air du frigo
#define IDX_AMB   1          // sonde dans le magasin (T_amb)
#define IDX_PROD  2          // seulement si HAS_PRODUCT_PROBE 1
// IA n°3 : 1 = réseau entraîné sur 40 000 coupures simulées (safety_model.h)
//          0 = formule de Newton seule
#define SAFETY_USE_AI 1

// ---------- Capteur de courant (ACS712) sur l'alimentation du Peltier ----------
#define HAS_CURRENT_SENSOR 0     // 1 = ACS712 sur GPIO34 ; 0 = pas d'ACS712 (le courant vient de l'INA219 n°2)
#define PIN_CURRENT     34   // ADC1 (obligatoire avec le Wi-Fi), entrée seule
#define ACS_SENS_V_PER_A 0.185f  // ACS712-05B : 0,185 V/A ; -20A : 0,100 ; -30A : 0,066
#define ACS_DIVIDER      0.667f  // pont diviseur 10 k / 20 k : V_adc = 0,667 x V_sortie
#define SUPPLY_V        11.1f    // tension du Peltier : pack 3S Li-ion (12,6 V plein, 11,1 V nominal, 9 V vide)

// ---------- Froid réel ou non ----------
// 1 = le Peltier refroidit vraiment : k n'est appris que Peltier éteint.
// 0 = le "moteur" est une simple LED : k est appris pendant tout le réchauffement.
#define REAL_COOLING 1
#define PELTIER_MAX_DUTY 255     // à baisser (ex. 180) si le Peltier tire trop de courant
// Protections logicielles du Peltier (en plus du fusible, qui reste obligatoire)
#define PELTIER_MAX_A    5.0f     // TEC1-12704 : 4 A max ; au-dessus de 5 A, Peltier coupé (ACS712). Avec l INA219 n°2 la mesure plafonne vers 3,2 A : le fusible reste indispensable
#define PELTIER_MIN_A    0.3f     // Peltier allumé mais courant sous ce seuil : alerte (fil, relais, fusible)
// 0 = Peltier commandé par le MOSFET (PWM, démarrage doux)
// 1 = Peltier commandé par un module relais (marche / arrêt seulement, pas de PWM)
#define PELTIER_USE_RELAY 1      // votre montage : module relais 10 A sur GPIO18, contact NO

// ---------- Broches ----------
#define PIN_SDA        21
#define PIN_SCL        22
// Porte : PAS de contact de porte. L'ouverture est déduite des températures
// (l'air du frigo monte vite et dépasse le produit) -> door_detect.h
#define HAS_DOOR_SWITCH 0
#define PIN_DOOR       35    // seulement si HAS_DOOR_SWITCH 1 (reed switch + 10 k vers 3V3)
#define DOOR_RISE_C   0.30f  // montée de l'air en 10 s = porte ouverte (à régler, voir guide)
#define DOOR_GAP_C    0.30f  // l'air doit dépasser T_produit d'au moins ça
#define DOOR_STILL_C  0.05f  // montée en 10 s sous laquelle on considère la porte refermée
#define PIN_LED_FRIDGE 18    // IN1 du module relais 2 canaux (Peltier), ou grille du MOSFET
#define PIN_LED_CLIM   25    // LED "climatiseur"
#define PIN_LED_LIGHTS 33    // LED "lumières"
#define PIN_LED_ROUTER -1    // LED "routeur" : -1 = absente (votre maquette a 2 LED : clim + lumières)
#define PIN_BUZZER     19
#define PIN_RGB_R      16
#define PIN_RGB_G      17
#define PIN_RGB_B      23
#define PIN_SRC_RELAY  26    // relais STEG/batterie (optionnel)
#define HAS_SRC_RELAY   0
#define RELAY_ACTIVE_LOW 1
#define HAS_BATTERY_INA 1    // INA219 n°2 (adresse 0x41 : souder le pont A0) sur le pack n°2
#define CURRENT_FROM_INA2 1  // 1 = le courant du Peltier est mesuré par l'INA219 n°2 (max ~3,2 A)
#define BAT_LIION_3S    1    // pack n°2 = 3 cellules Li-ion en série
#define BAT_EMPTY_V   9.6f   // en dessous (Peltier allumé) : batterie considérée vide, alerte
#define HAS_OLED        1

// ---------- Seuils de tension du "STEG" simulé ----------
// "STEG" = batterie 11,7 V + potentiomètre. Tension normale : 11,0 à 11,7 V.
// Ces seuils sont à environ -11 % (coupure) et -8 % (retour) de la normale.
// La surtension (13,3 V) ne peut pas être atteinte avec cette batterie.
#define V_LOW_CUT   10.4f
#define V_HIGH_CUT  13.3f
#define V_RET_LOW   10.8f
#define V_RET_HIGH  13.0f

// ---------- Délais de protection (secondes) ----------
#define STABLE_S          120
#define STABLE_UNSTABLE_S 300
#define COMP_MIN_OFF_S    180
#define RECONNECT_GAP_S    10

// ---------- Batterie de secours : pack n°2, 3S Li-ion ----------
// Le niveau du pack n°2 est CALCULÉ à partir de sa tension et du courant mesurés
// par l'INA219 n°2.
// Cellules 18650 3,7 V 1500 mAh (étiquette) : 3 en série -> 1,5 Ah.
// Cellules bon marché : la vraie capacité peut être plus faible ; baissez si l autonomie réelle est plus courte.
#define BAT_CAPACITY_AH  1.5f
#define BAT_NOMINAL_V   11.1f
#define BAT_SOC_MIN      0.10f   // Li-ion : on garde 10 % (environ 3,2 V par cellule)

#define PELTIER_NOMINAL_A 3.0f   // courant supposé du Peltier seulement s'il n'est PAS mesuré

// ---------- Simulation de la batterie ----------
// Pas de vraie batterie : le niveau est calculé. Le Peltier est compté avec son
// courant MESURÉ, les LED avec la puissance des vrais appareils (decision.h).
// DEMO_SPEED accélère le temps de la batterie (1 = réel, 30 = 1 h en 2 min).
#define DEMO_SPEED 1.0f

// ---------- Profil de marchandise au démarrage ----------
// Avec un Peltier, la boîte descend rarement sous 8-10 °C : profil "maquette" (10-16 °C).
#define DEFAULT_PROFILE PROF_MAQUETTE
