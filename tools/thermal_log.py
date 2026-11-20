"""
thermal_log.py -- IA n°3 : enregistrer puis tracer le réchauffement du "frigo".

1) Enregistrer (le firmware principal envoie une ligne "T,..." par minute) :
    python thermal_log.py record --port COM5 --out data/rechauffement.csv
   Laissez tourner 1 à 3 heures (Ctrl+C pour arrêter).

2) Tracer et calculer k hors ligne (graphique pour le rapport) :
    python thermal_log.py plot data/rechauffement.csv --tmax 4

Le graphique compare la température mesurée du produit à la courbe prévue par
la loi de Newton avec le k trouvé : si les deux se superposent, le modèle est bon.
"""
import argparse
import csv
import math
from pathlib import Path

COLS = ["minute", "t_air", "t_prod", "t_amb", "porte", "moteur", "k_esp32"]


def record(args):
    import serial  # pip install pyserial
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with serial.Serial(args.port, 115200, timeout=5) as ser, out.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(COLS)
        print("Enregistrement... Ctrl+C pour arrêter")
        try:
            while True:
                line = ser.readline().decode(errors="ignore").strip()
                if line.startswith("T,"):
                    parts = line.split(",")[1:]
                    if len(parts) == len(COLS):
                        w.writerow(parts)
                        f.flush()
                        print(" ".join(f"{c}={p}" for c, p in zip(COLS, parts)))
        except KeyboardInterrupt:
            print("Arrêté. Fichier :", out)


def plot(args):
    import matplotlib.pyplot as plt  # pip install matplotlib
    rows = list(csv.DictReader(open(args.csv)))
    # on garde les minutes où le moteur est éteint et la porte fermée
    rows = [r for r in rows if r["moteur"] == "0" and r["porte"] == "0"]
    t = [float(r["minute"]) / 60 for r in rows]          # heures
    T = [float(r["t_prod"]) for r in rows]
    A = [float(r["t_amb"]) for r in rows]
    if len(t) < 10:
        raise SystemExit("Pas assez de données (moteur éteint + porte fermée).")

    # k hors ligne : moindres carrés sur dT/dt = k (T_amb - T), pas de 5 min
    sxy = sxx = 0.0
    for i in range(5, len(t)):
        dt = t[i] - t[i - 5]
        if dt <= 0:
            continue
        y = (T[i] - T[i - 5]) / dt
        x = (A[i] + A[i - 5]) / 2 - (T[i] + T[i - 5]) / 2
        sxy += x * y
        sxx += x * x
    k = sxy / sxx
    amb = sum(A) / len(A)
    pred = [amb - (amb - T[0]) * math.exp(-k * (ti - t[0])) for ti in t]

    print(f"k trouvé hors ligne : {k:.4f} /h   (k appris par l'ESP32 à la fin : {rows[-1]['k_esp32']})")
    if args.tmax is not None and T[0] < args.tmax < amb:
        ts = -math.log((amb - args.tmax) / (amb - T[0])) / k
        print(f"Temps de sécurité prévu depuis {T[0]:.1f} °C jusqu'à {args.tmax} °C : {ts:.2f} h")

    plt.figure(figsize=(7, 4))
    plt.plot(t, T, "o", ms=3, label="Produit mesuré")
    plt.plot(t, pred, "-", label=f"Loi de Newton, k = {k:.3f} /h")
    plt.plot(t, A, ":", label="Air du magasin")
    if args.tmax is not None:
        plt.axhline(args.tmax, color="red", lw=1, ls="--", label=f"T_max = {args.tmax} °C")
    plt.xlabel("Temps (h)")
    plt.ylabel("Température (°C)")
    plt.legend()
    plt.grid(alpha=0.3)
    plt.tight_layout()
    png = Path(args.csv).with_suffix(".png")
    plt.savefig(png, dpi=150)
    print("Graphique :", png)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record")
    r.add_argument("--port", required=True)
    r.add_argument("--out", required=True)
    p = sub.add_parser("plot")
    p.add_argument("csv")
    p.add_argument("--tmax", type=float, default=4.0)
    args = ap.parse_args()
    record(args) if args.cmd == "record" else plot(args)


if __name__ == "__main__":
    main()
