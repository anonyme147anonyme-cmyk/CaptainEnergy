"""
train_safety_model.py -- IA n°3 : temps de sécurité appris sur un jeu de données

But : dès la première seconde d'une coupure, prédire combien d'heures la
marchandise reste sous T_max.

Le boîtier n'a pas de sonde dans la marchandise : T_produit est une donnée
SIMULÉE (« fausse donnée »). Pour entraîner l'IA, ce script fabrique donc un
jeu de données qui couvre tous les cas possibles :
  - le frigo : petite boîte à Peltier ou vrai frigo, isolation, masse de
    marchandise (de 50 g à 40 kg), échange air / marchandise ;
  - l'état au début de la coupure : T_produit, T_air (sonde d'air du frigo),
    T_amb (sonde du magasin, 18 à 42 °C), T_max du profil (4, 8, 10, 16 °C...) ;
  - pendant la coupure : magasin qui se réchauffe ou se refroidit, ouvertures
    de porte (0 à 20 par heure, 5 à 60 s).
Pour chaque cas, la coupure est SIMULÉE (modèle physique air + marchandise)
jusqu'à ce que la marchandise atteigne T_max : c'est l'étiquette, le vrai
temps de sécurité.

Ensuite :
  1. entraînement d'un petit réseau (6 entrées -> 16 -> 1) ;
  2. comparaison avec la formule de Newton utilisée jusqu'ici ;
  3. calcul d'une marge de sécurité (pour ne presque jamais surestimer) ;
  4. export int8 pour l'ESP32 (safety_model.h) + le jeu de données en CSV.

Entrées du modèle (toutes disponibles sur l'ESP32 au début de la coupure) :
  T_produit (simulé), T_air (DS18B20 air du frigo), T_amb (DS18B20 magasin),
  T_max (profil), k (appris par l'ESP32), ouvertures de porte par heure,
  et le résultat de la formule de Newton (le réseau corrige la physique).

Utilisation :
    python train_safety_model.py              (environ 1 à 2 minutes)
    python train_safety_model.py --plot       (+ graphiques pour le rapport)
"""
import argparse
from pathlib import Path

import numpy as np
from sklearn.neural_network import MLPRegressor

HERE = Path(__file__).resolve().parent
OUT_H = HERE.parent / "firmware" / "edge_energy_manager" / "src" / "safety_model.h"
FEATURES = ["t_prod", "t_air", "t_amb", "t_max", "k", "portes_h", "log_newton"]
CAP_H = 24.0                     # au-delà de 24 h, on considère « largement sûr »


# ----------------------------------------------------------------------------
# 1. Tirage au hasard de tous les cas
# ----------------------------------------------------------------------------
def sample_cases(n, rng):
    c = {}
    small = rng.random(n) < 0.5                     # petite boîte (maquette) ou vrai frigo
    c["small"] = small
    c["Ca"] = np.where(small, rng.uniform(60, 400, n), rng.uniform(800, 5000, n))       # J/K air + parois
    mass = np.where(small, np.exp(rng.uniform(np.log(0.05), np.log(2.0), n)),
                           np.exp(rng.uniform(np.log(1.0), np.log(40.0), n)))           # kg (eau)
    c["Cp"] = mass * 4186.0                                                             # J/K marchandise
    c["UA"] = np.where(small, rng.uniform(0.1, 0.8, n), rng.uniform(0.6, 2.5, n))      # W/K murs
    c["H"] = np.where(small, rng.uniform(0.2, 3.0, n), rng.uniform(3.0, 30.0, n))      # W/K air <-> marchandise
    c["UAdoor"] = np.where(small, rng.uniform(0.5, 6.0, n), rng.uniform(5.0, 40.0, n)) # W/K porte ouverte
    # T_max : profils (volaille 4, laitier 4, médicament 8, légumes 10, maquette 16) + valeurs continues
    prof = rng.choice([4.0, 8.0, 10.0, 16.0], n)
    c["tmax"] = np.where(rng.random(n) < 0.7, prof, rng.uniform(2, 18, n))
    # T_produit au début : sous T_max (parfois déjà au-dessus -> 0 h)
    below = rng.uniform(0.1, 10.0, n)
    c["tprod"] = np.where(rng.random(n) < 0.05, c["tmax"] + rng.uniform(0, 1, n), c["tmax"] - below)
    c["tprod"] = np.maximum(c["tprod"], -2.0)
    # magasin : 18-42 °C, puis il évolue pendant la coupure (-1 à +3 °C par heure)
    c["tamb"] = rng.uniform(18, 42, n)
    c["ambTrend"] = rng.uniform(-1.0, 3.0, n) / 3600.0
    # air du frigo au début : autour du produit (le thermostat tournait)
    c["tair"] = np.minimum(c["tprod"] + rng.uniform(-3.0, 2.0, n), c["tamb"] - 1)
    # ouvertures de porte pendant la coupure
    c["doorRate"] = rng.choice([0, 1, 2, 4, 6, 10, 15, 20], n).astype(float)        # par heure
    # k que l'ESP32 aurait appris : vitesse de réchauffement globale, avec une erreur d'apprentissage
    c["k"] = c["UA"] / (c["Ca"] + c["Cp"]) * 3600 * np.exp(rng.normal(0, 0.2, n))   # 1/h
    return c


# ----------------------------------------------------------------------------
# 2. Simulation de la coupure jusqu'à T_max (moteur éteint)
# ----------------------------------------------------------------------------
def simulate_safe_time(c, rng, dt=10.0, sub=10):
    n = len(c["tprod"])
    Ta, Tp = c["tair"].copy(), c["tprod"].copy()
    done = Tp >= c["tmax"]
    tsafe = np.where(done, 0.0, CAP_H)
    doorLeft = np.zeros(n)
    h = dt / sub
    steps = int(CAP_H * 3600 / dt)
    for k in range(steps):
        t = k * dt
        Tamb = np.minimum(c["tamb"] + c["ambTrend"] * t, 45.0)
        newOpen = (doorLeft <= 0) & (rng.random(n) < c["doorRate"] / 3600 * dt)
        doorLeft = np.where(newOpen, rng.uniform(5, 60, n), doorLeft - dt)
        door = doorLeft > 0
        for _ in range(sub):
            dTa = (c["UA"] * (Tamb - Ta) + c["UAdoor"] * door * (Tamb - Ta) + c["H"] * (Tp - Ta)) / c["Ca"]
            dTp = c["H"] * (Ta - Tp) / c["Cp"]
            Ta = Ta + h * dTa
            Tp = Tp + h * dTp
        reached = (~done) & (Tp >= c["tmax"])
        tsafe = np.where(reached, (t + dt) / 3600.0, tsafe)
        done |= reached
        if done.all():
            break
    return tsafe


def newton_raw(c):
    """Formule de Newton seule : t = -(1/k) ln((Tamb - Tmax)/(Tamb - Tprod)) (même calcul que thermal.h)."""
    with np.errstate(divide="ignore", invalid="ignore"):
        t = -np.log((c["tamb"] - c["tmax"]) / (c["tamb"] - c["tprod"])) / c["k"]
    t = np.where(c["tprod"] >= c["tmax"], 0, t)
    t = np.where(c["tamb"] <= c["tmax"], 99, t)
    return np.clip(t, 0, 99)


def newton_formula(c):
    """Méthode utilisée jusqu'ici : Newton, réduit si la porte s'ouvre souvent."""
    return np.clip(newton_raw(c) / (1 + 0.05 * c["doorRate"]), 0, CAP_H)


def quantize(w):
    s = np.abs(w).max() / 127.0
    return np.clip(np.round(w / s), -127, 127).astype(np.int8), float(s)


def predict_int8(X, m):
    x = (X - m["mean"]) / m["std"]
    hdn = np.maximum(x @ m["W1q"].T.astype(np.float64) * m["s1"] + m["b1"], 0)
    y = (hdn @ m["W2q"].T.astype(np.float64) * m["s2"] + m["b2"])[:, 0]
    return np.clip(np.exp(y) - 0.05, 0, CAP_H)        # le réseau prédit log(t + 0,05)


def c1(name, a):
    return f"static const float {name}[{len(a)}] = {{" + ", ".join(f"{v:.6f}f" for v in a) + "};\n"


def c2(name, q):
    rows = ",\n  ".join("{" + ", ".join(str(int(v)) for v in r) + "}" for r in q)
    return f"static const int8_t {name}[{q.shape[0]}][{q.shape[1]}] = {{\n  {rows}\n}};\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=40000, help="nombre de coupures simulées")
    ap.add_argument("--plot", action="store_true")
    args = ap.parse_args()
    rng = np.random.default_rng(3)

    print(f"Simulation de {args.n} coupures (tous les cas)...")
    c = sample_cases(args.n, rng)
    y = simulate_safe_time(c, rng)
    c["logNewton"] = np.log(newton_raw(c) + 0.05)
    X = np.stack([c[f] for f in ("tprod", "tair", "tamb", "tmax", "k", "doorRate", "logNewton")], axis=1)
    print(f"  temps de sécurité : médiane {np.median(y):.2f} h, de {y.min():.2f} à {y.max():.1f} h "
          f"({(y >= CAP_H).mean()*100:.0f} % au-delà de 24 h)")

    # jeu de données complet en CSV (pour le rapport et pour le réutiliser)
    csv = HERE / "safety_dataset.csv"
    with csv.open("w") as f:
        f.write(",".join(FEATURES) + ",petite_boite,t_securite_h\n")
        for i in range(len(y)):
            f.write(",".join(f"{v:.4f}" for v in X[i]) + f",{int(c['small'][i])},{y[i]:.4f}\n")
    print(f"  jeu de données : {csv} ({len(y)} lignes)")

    # séparation : 80 % entraînement, 20 % test (cas jamais vus)
    idx = rng.permutation(len(y))
    tr, te = idx[: int(0.8 * len(y))], idx[int(0.8 * len(y)):]
    mean, std = X[tr].mean(0), X[tr].std(0)
    std[std < 1e-6] = 1
    target = np.log(y + 0.05)
    reg = MLPRegressor(hidden_layer_sizes=(16,), activation="relu", max_iter=800, early_stopping=True,
                       n_iter_no_change=30, alpha=1e-4, random_state=0)
    reg.fit((X[tr] - mean) / std, target[tr])
    W1q, s1 = quantize(reg.coefs_[0].T)
    W2q, s2 = quantize(reg.coefs_[1].T)
    m = dict(mean=mean, std=std, W1q=W1q, s1=s1, b1=reg.intercepts_[0], W2q=W2q, s2=s2, b2=reg.intercepts_[1])

    yt = y[te]
    p_ai = predict_int8(X[te], m)
    sub_te = {k: v[te] for k, v in c.items()}
    p_nw = newton_formula(sub_te)

    # marge de sécurité : facteur tel que 95 % des prédictions ne dépassent pas la vraie valeur
    ok = yt > 0.05
    ratio = yt[ok] / np.maximum(p_ai[ok], 1e-3)
    margin = float(min(1.0, np.percentile(ratio, 5)))

    def report(name, p):
        e = np.abs(p - yt)
        rel = np.abs(p[ok] - yt[ok]) / yt[ok]
        over = (p[ok] > yt[ok] * 1.10).mean() * 100
        print(f"  {name:32s} erreur moyenne {e.mean():5.2f} h | erreur relative médiane {np.median(rel)*100:4.0f} % "
              f"| surestime de plus de 10 % : {over:4.1f} % des cas")

    print("\nTest sur des coupures jamais vues :")
    report("formule de Newton (avant)", p_nw)
    report("réseau int8 (ESP32)", p_ai)
    report(f"réseau int8 x marge {margin:.2f}", p_ai * margin)
    for nm, msk in (("petite boîte", sub_te["small"]), ("vrai frigo", ~sub_te["small"])):
        e_ai = np.median(np.abs(p_ai[msk & ok] - yt[msk & ok]) / yt[msk & ok]) * 100
        e_nw = np.median(np.abs(p_nw[msk & ok] - yt[msk & ok]) / yt[msk & ok]) * 100
        print(f"    {nm:13s} erreur relative médiane : réseau {e_ai:4.0f} %  | Newton {e_nw:4.0f} %")

    hdr = [
        "// Fichier généré par tools/train_safety_model.py -- ne pas modifier à la main",
        "// IA n°3 : temps de sécurité appris sur des coupures simulées (tous les cas)",
        f"// Entrées : {', '.join(FEATURES)} ; sortie : log(t_securite_h + 0,05)",
        f"// Test : erreur relative médiane {np.median(np.abs(p_ai[ok]-yt[ok])/yt[ok])*100:.0f} % "
        f"(Newton : {np.median(np.abs(p_nw[ok]-yt[ok])/yt[ok])*100:.0f} %)",
        "#pragma once",
        "#include <stdint.h>",
        "#define SM_IN 7",
        f"#define SM_HID {W1q.shape[0]}",
        f"#define SM_CAP_H {CAP_H:.1f}f",
        f"#define SM_MARGIN {margin:.4f}f   // facteur de prudence (95 % des cas non surestimés)",
        c1("SM_MEAN", mean), c1("SM_STD", std),
        c2("SM_W1", W1q), f"static const float SM_S1 = {s1:.8f}f;", c1("SM_B1", m["b1"]),
        c2("SM_W2", W2q), f"static const float SM_S2 = {s2:.8f}f;", c1("SM_B2", m["b2"]),
    ]
    OUT_H.write_text("\n".join(hdr) + "\n", encoding="utf-8")
    print(f"\nModèle écrit dans {OUT_H}")

    tv = HERE / "safety_model_testvectors.csv"
    with tv.open("w") as f:
        f.write(",".join(FEATURES) + ",attendu_h\n")
        for i in range(20):
            f.write(",".join(f"{v:.5f}" for v in X[te][i]) + f",{p_ai[i]:.5f}\n")
    print(f"Vecteurs de vérification : {tv}")

    if args.plot:
        import matplotlib.pyplot as plt
        lim = 12
        fig, ax = plt.subplots(1, 2, figsize=(10, 4.5), sharey=True)
        for a, p, title in ((ax[0], p_nw, "Formule de Newton"), (ax[1], p_ai, "Réseau int8 (ESP32)")):
            a.scatter(yt, p, s=3, alpha=0.3)
            a.plot([0, lim], [0, lim], "r--", lw=1)
            a.set_xlim(0, lim); a.set_ylim(0, lim)
            a.set_xlabel("Vrai temps de sécurité (h)"); a.set_title(title); a.grid(alpha=0.3)
        ax[0].set_ylabel("Temps de sécurité prédit (h)")
        plt.tight_layout()
        png = HERE / "temps_securite_test.png"
        plt.savefig(png, dpi=150)
        print("Graphique :", png)


if __name__ == "__main__":
    main()
