"""
train_grid_model.py -- IA n°1 : entraîner le modèle "état du réseau" et
l'exporter pour l'ESP32 (poids en int8).

Utilisation normale (avec VOS mesures) :
    python train_grid_model.py data/normal.csv data/baisse.csv data/instable.csv data/coupure.csv

Test sans matériel (données synthétiques, UNIQUEMENT pour vérifier que
le programme marche -- ne pas présenter ce modèle au jury) :
    python train_grid_model.py --synthetic

Résultat : ../firmware/edge_energy_manager/src/grid_model.h
"""
import argparse
import sys
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.metrics import classification_report, confusion_matrix
from sklearn.model_selection import train_test_split
from sklearn.neural_network import MLPClassifier

LABELS = ["normal", "instable", "baisse", "coupure"]   # même ordre que dans le firmware
FEATURES = [f"f{i}" for i in range(8)]
HIDDEN = 16
OUT_H = Path(__file__).resolve().parent.parent / "firmware" / "edge_energy_manager" / "src" / "grid_model.h"

FEAT_LOW_V, FEAT_ZERO_V = 10.4, 1.0


# ----------------------------------------------------------------------------
# Mêmes calculs que grid_features.h (utilisé seulement pour les données synthétiques)
# ----------------------------------------------------------------------------
def compute_features(v, dt):
    v = np.asarray(v, dtype=np.float64)
    n = len(v)
    mean = v.mean()
    t = np.arange(n) * dt
    t = t - t.mean()
    d = v - mean
    den = (t * t).sum()
    return [
        mean, v.min(), v.max(), np.sqrt((d * d).mean()),
        (t * d).sum() / den if den > 0 else 0.0,
        (v < FEAT_LOW_V).mean(), (v < FEAT_ZERO_V).mean(),
        np.abs(np.diff(v)).mean() if n > 1 else 0.0,
    ]


def synthetic_dataset(n_per_class=400, seed=1):
    """Fenêtres d'1 s à 100 Hz qui imitent ce que voit l'INA219 n°1."""
    rng = np.random.default_rng(seed)
    rows = []
    for _ in range(n_per_class):
        t = np.arange(100) * 0.01
        noise = rng.normal(0, 0.02, 100)
        # normal : stable entre 11,8 et 12,6 V
        base = rng.uniform(11.8, 12.6)
        rows.append(compute_features(base + noise + 0.05 * np.sin(2 * np.pi * rng.uniform(0.2, 1) * t), 0.01) + ["normal"])
        # instable : oscillations rapides ou creux brefs
        base = rng.uniform(11.2, 12.4)
        osc = rng.uniform(0.4, 1.2) * np.sin(2 * np.pi * rng.uniform(1, 6) * t)
        dips = np.where(rng.random(100) < 0.05, -rng.uniform(1, 3), 0)
        rows.append(compute_features(base + osc + dips + noise, 0.01) + ["instable"])
        # baisse : tension basse ou qui descend lentement
        start = rng.uniform(10.0, 11.6)
        slope = rng.uniform(-0.8, 0.0)
        rows.append(compute_features(start + slope * t + noise, 0.01) + ["baisse"])
        # coupure : plus de tension (avec parfois la fin de la chute dans la fenêtre)
        v = np.abs(rng.normal(0.05, 0.03, 100))
        if rng.random() < 0.3:
            k = rng.integers(1, 30)
            v[:k] = rng.uniform(9, 12)
        rows.append(compute_features(v, 0.01) + ["coupure"])
    return pd.DataFrame(rows, columns=FEATURES + ["label"])


# ----------------------------------------------------------------------------
# Quantification int8 symétrique, une échelle par couche
# ----------------------------------------------------------------------------
def quantize(w):
    scale = np.abs(w).max() / 127.0
    q = np.clip(np.round(w / scale), -127, 127).astype(np.int8)
    return q, float(scale)


def int8_predict(X, mean, std, W1q, s1, b1, W2q, s2, b2):
    """Reproduit exactement tinyml.h, pour mesurer la perte due à l'int8."""
    x = (X - mean) / std
    h = np.maximum(x @ W1q.T.astype(np.float64) * s1 + b1, 0)
    o = h @ W2q.T.astype(np.float64) * s2 + b2
    return o.argmax(axis=1)


def c_array_2d(name, q):
    rows = ",\n  ".join("{" + ", ".join(str(int(v)) for v in r) + "}" for r in q)
    return f"static const int8_t {name}[{q.shape[0]}][{q.shape[1]}] = {{\n  {rows}\n}};\n"


def c_array_1d(name, a):
    return f"static const float {name}[{len(a)}] = {{" + ", ".join(f"{v:.6f}f" for v in a) + "};\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="*", help="fichiers CSV produits par log_serial.py")
    ap.add_argument("--synthetic", action="store_true", help="données synthétiques (test seulement)")
    args = ap.parse_args()

    if args.synthetic:
        df = synthetic_dataset()
        source = "DONNEES SYNTHETIQUES (test du programme, a remplacer par vos mesures)"
    else:
        if not args.csv:
            sys.exit("Donnez les fichiers CSV, ou --synthetic pour un test.")
        df = pd.concat([pd.read_csv(p) for p in args.csv], ignore_index=True)
        source = "mesures: " + ", ".join(Path(p).name for p in args.csv)

    df = df[df["label"].isin(LABELS)]
    print("Fenêtres par classe :")
    print(df["label"].value_counts().to_string(), "\n")

    X = df[FEATURES].to_numpy(dtype=np.float64)
    y = df["label"].map({l: i for i, l in enumerate(LABELS)}).to_numpy()

    # 1) Séparer : 75 % pour apprendre, 25 % pour tester sur des fenêtres jamais vues
    X_tr, X_te, y_tr, y_te = train_test_split(X, y, test_size=0.25, stratify=y, random_state=0)

    # 2) Normalisation (calculée sur l'entraînement seulement)
    mean = X_tr.mean(axis=0)
    std = X_tr.std(axis=0)
    std[std < 1e-6] = 1.0

    # 3) Le réseau : 8 -> 16 (ReLU) -> 4
    clf = MLPClassifier(hidden_layer_sizes=(HIDDEN,), activation="relu",
                        max_iter=2000, random_state=0)
    clf.fit((X_tr - mean) / std, y_tr)

    # 4) Précision en float
    pred_f = clf.predict((X_te - mean) / std)
    acc_f = (pred_f == y_te).mean()

    # 5) Quantification int8 et précision après quantification
    W1, W2 = clf.coefs_[0].T, clf.coefs_[1].T          # formes (16,8) et (4,16)
    b1, b2 = clf.intercepts_
    W1q, s1 = quantize(W1)
    W2q, s2 = quantize(W2)
    pred_q = int8_predict(X_te, mean, std, W1q, s1, b1, W2q, s2, b2)
    acc_q = (pred_q == y_te).mean()

    print(f"Précision sur le jeu de test (float) : {acc_f*100:.1f} %")
    print(f"Précision sur le jeu de test (int8)  : {acc_q*100:.1f} %\n")
    print(classification_report(y_te, pred_q, target_names=LABELS, digits=3, zero_division=0))
    print("Matrice de confusion (lignes = vraie classe, colonnes = prédite) :")
    print(pd.DataFrame(confusion_matrix(y_te, pred_q, labels=range(4)), index=LABELS, columns=LABELS), "\n")

    n_params = W1q.size + W2q.size
    print(f"Taille du modèle : {n_params} poids int8 = {n_params} octets "
          f"(+ {len(b1)+len(b2)+2*8+2} nombres float)")

    # 6) Écrire grid_model.h
    labels_c = ", ".join(f'"{l}"' for l in LABELS)
    h = [
        "// Fichier généré par tools/train_grid_model.py -- ne pas modifier à la main",
        f"// Source : {source}",
        f"// Précision test : float {acc_f*100:.1f} %, int8 {acc_q*100:.1f} %",
        "#pragma once",
        "#include <stdint.h>",
        f"#define GM_IN {W1.shape[1]}",
        f"#define GM_HID {W1.shape[0]}",
        f"#define GM_OUT {W2.shape[0]}",
        f"static const char* const GM_LABELS[GM_OUT] __attribute__((unused)) = {{{labels_c}}};",
        c_array_1d("GM_MEAN", mean),
        c_array_1d("GM_STD", std),
        c_array_2d("GM_W1", W1q),
        f"static const float GM_S1 = {s1:.8f}f;",
        c_array_1d("GM_B1", b1),
        c_array_2d("GM_W2", W2q),
        f"static const float GM_S2 = {s2:.8f}f;",
        c_array_1d("GM_B2", b2),
    ]
    OUT_H.write_text("\n".join(h) + "\n", encoding="utf-8")
    print(f"Modèle écrit dans {OUT_H}")

    # 7) Quelques vecteurs de test pour vérifier l'ESP32 (même résultat attendu)
    test_path = Path(__file__).resolve().parent / "grid_model_testvectors.csv"
    tv = pd.DataFrame(X_te[:20], columns=FEATURES)
    tv["expected"] = pred_q[:20]
    tv.to_csv(test_path, index=False)
    print(f"Vecteurs de vérification écrits dans {test_path}")


if __name__ == "__main__":
    main()
