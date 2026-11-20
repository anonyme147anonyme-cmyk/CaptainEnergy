"""
log_serial.py -- enregistre les fenêtres envoyées par le firmware de collecte
(collect_grid.ino) dans un fichier CSV, avec l'étiquette du scénario.

Une séance = un scénario. Exemple :
    python log_serial.py --port COM5 --label normal   --seconds 180 --out data/normal.csv
    python log_serial.py --port COM5 --label baisse   --seconds 180 --out data/baisse.csv
    python log_serial.py --port COM5 --label instable --seconds 180 --out data/instable.csv
    python log_serial.py --port COM5 --label coupure  --seconds 120 --out data/coupure.csv

Sous Linux/Mac, le port ressemble à /dev/ttyUSB0 ou /dev/cu.usbserial-xxxx.
Fermez le moniteur série de l'Arduino IDE avant de lancer ce script.
"""
import argparse
import csv
import time
from pathlib import Path

import serial  # pip install pyserial

LABELS = ["normal", "instable", "baisse", "coupure"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--label", required=True, choices=LABELS)
    ap.add_argument("--seconds", type=int, default=180)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    new_file = not out.exists()

    with serial.Serial(args.port, 115200, timeout=2) as ser, out.open("a", newline="") as f:
        w = csv.writer(f)
        if new_file:
            w.writerow([f"f{i}" for i in range(8)] + ["label"])
        print(f"Enregistrement '{args.label}' pendant {args.seconds} s... (Ctrl+C pour arrêter)")
        t_end = time.time() + args.seconds
        n = 0
        while time.time() < t_end:
            line = ser.readline().decode(errors="ignore").strip()
            # le firmware envoie : F,f0,f1,f2,f3,f4,f5,f6,f7
            if not line.startswith("F,"):
                continue
            parts = line.split(",")[1:]
            if len(parts) != 8:
                continue
            w.writerow(parts + [args.label])
            n += 1
            if n % 10 == 0:
                print(f"  {n} fenêtres  (moyenne {float(parts[0]):.2f} V)")
    print(f"Terminé : {n} fenêtres ajoutées à {out}")


if __name__ == "__main__":
    main()
