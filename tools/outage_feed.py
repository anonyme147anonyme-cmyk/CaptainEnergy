"""
outage_feed.py -- tourne sur le PC (pas sur l'ESP32).

Toutes les 10 minutes :
  1. récupère la température maximale prévue aujourd'hui (API Open-Meteo, gratuite, sans clé) ;
  2. lit les flux RSS de sites d'actualité et cherche une annonce de coupure STEG
     pour votre zone, avec les heures (ex. « entre 17h00 et 22h00 ») ;
  3. envoie le résultat à l'ESP32 par MQTT, sur le sujet eem/announce.
En option, il transfère les alertes de l'ESP32 (eem/alert) vers votre téléphone
avec l'application gratuite ntfy.

Utilisation :
    python outage_feed.py --broker 127.0.0.1 --zone Ariana --lat 36.86 --lon 10.19
    python outage_feed.py --broker 127.0.0.1 --ntfy mon-sujet-secret-123   (+ alertes téléphone)

Pour la démo (fausse annonce : coupure dans 30 min, pendant 2 h, 39 °C prévus) :
    python outage_feed.py --broker 127.0.0.1 --mock 30 --mock-hours 2 --mock-tmax 39

Les sites changent souvent leur structure : vérifiez les adresses RSS dans FEEDS
(ouvrez-les dans un navigateur). Le boîtier fonctionne aussi sans ce script.
"""
import argparse
import email.utils
import json
import re
import time
import unicodedata
import xml.etree.ElementTree as ET
from datetime import datetime, timedelta

import requests                      # pip install requests
import paho.mqtt.client as mqtt      # pip install paho-mqtt

FEEDS = [  # à vérifier : adresses RSS habituelles des sites WordPress
    "https://www.tunisienumerique.com/feed/",
    "https://www.webdo.tn/fr/feed/",
]
TOPIC_ANNOUNCE = "eem/announce"
TOPIC_ALERT = "eem/alert"


def strip_accents(s):
    return "".join(c for c in unicodedata.normalize("NFD", s) if unicodedata.category(c) != "Mn").lower()


# « entre 17h00 et 22h00 », « de 9h à 13h30 », « 21h - minuit »
TIME_RE = re.compile(
    r"(\d{1,2})\s*h\s*(\d{2})?\s*(?:et|a|-|jusqu'a|jusqu a)\s*(?:(\d{1,2})\s*h\s*(\d{2})?|(minuit))")


def find_window(text):
    """Retourne (heure_debut, heure_fin) en heures décimales, ou None."""
    m = TIME_RE.search(strip_accents(text))
    if not m:
        return None
    h1 = int(m.group(1)) + int(m.group(2) or 0) / 60
    if m.group(5):
        h2 = 24.0
    else:
        h2 = int(m.group(3)) + int(m.group(4) or 0) / 60
    if not (0 <= h1 < 24 and 0 < h2 <= 24):
        return None
    return h1, h2


def is_outage_news(text, zone):
    t = strip_accents(text)
    return ("steg" in t and ("coupure" in t or "delestage" in t)
            and strip_accents(zone) in t)


def get_tmax(lat, lon):
    url = ("https://api.open-meteo.com/v1/forecast"
           f"?latitude={lat}&longitude={lon}&daily=temperature_2m_max"
           "&timezone=Africa%2FTunis&forecast_days=1")
    r = requests.get(url, timeout=10)
    r.raise_for_status()
    return float(r.json()["daily"]["temperature_2m_max"][0])


def scan_feeds(zone):
    """Cherche l'annonce la plus récente pour la zone. Retourne un dict ou None."""
    best = None
    for feed in FEEDS:
        try:
            xml = requests.get(feed, timeout=10, headers={"User-Agent": "Mozilla/5.0"}).content
            root = ET.fromstring(xml)
        except Exception as e:
            print(f"  flux illisible {feed} : {e}")
            continue
        for item in root.iter("item"):
            title = item.findtext("title") or ""
            desc = item.findtext("description") or ""
            content = item.findtext("{http://purl.org/rss/1.0/modules/content/}encoded") or ""
            text = re.sub("<[^>]+>", " ", f"{title} {desc} {content}")
            if not is_outage_news(text, zone):
                continue
            win = find_window(text)
            pub = item.findtext("pubDate")
            day = email.utils.parsedate_to_datetime(pub).astimezone() if pub else datetime.now().astimezone()
            if "demain" in strip_accents(text):
                day += timedelta(days=1)
            if not win:
                continue
            start = day.replace(hour=0, minute=0, second=0, microsecond=0) + timedelta(hours=win[0])
            end = day.replace(hour=0, minute=0, second=0, microsecond=0) + timedelta(hours=win[1])
            if end.timestamp() < time.time():
                continue  # annonce déjà passée
            cand = {"debut": int(start.timestamp()), "fin": int(end.timestamp()),
                    "source": item.findtext("link") or feed}
            if best is None or cand["debut"] < best["debut"]:
                best = cand
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--broker", default="127.0.0.1")
    ap.add_argument("--zone", default="Ariana")
    ap.add_argument("--lat", type=float, default=36.86)
    ap.add_argument("--lon", type=float, default=10.19)
    ap.add_argument("--every", type=int, default=600, help="secondes entre deux vérifications")
    ap.add_argument("--ntfy", help="sujet ntfy pour recevoir les alertes sur le téléphone")
    ap.add_argument("--mock", type=float, help="DÉMO : coupure annoncée dans N minutes")
    ap.add_argument("--mock-hours", type=float, default=2.0)
    ap.add_argument("--mock-tmax", type=float, default=39.0)
    args = ap.parse_args()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)

    if args.ntfy:
        def on_message(_c, _u, msg):
            text = msg.payload.decode(errors="ignore")
            print("Alerte ESP32 :", text)
            try:
                requests.post(f"https://ntfy.sh/{args.ntfy}", data=text.encode("utf-8"), timeout=10)
            except Exception as e:
                print("  envoi ntfy impossible :", e)
        client.on_message = on_message

    client.connect(args.broker, 1883)
    if args.ntfy:
        client.subscribe(TOPIC_ALERT)
    client.loop_start()

    if args.mock is not None:
        now = time.time()
        msg = {"zone": args.zone, "debut": int(now + args.mock * 60),
               "fin": int(now + args.mock * 60 + args.mock_hours * 3600),
               "tmax_prevue": args.mock_tmax, "source": "DEMO"}
        client.publish(TOPIC_ANNOUNCE, json.dumps(msg), retain=True).wait_for_publish()
        print("Fausse annonce envoyée :", msg)
        if not args.ntfy:
            client.loop_stop()
            return

    while True:
        msg = {"zone": args.zone, "debut": 0, "fin": 0}
        try:
            msg["tmax_prevue"] = get_tmax(args.lat, args.lon)
        except Exception as e:
            print("  météo indisponible :", e)
        ann = scan_feeds(args.zone)
        if ann:
            msg.update(ann)
        if args.mock is None:
            client.publish(TOPIC_ANNOUNCE, json.dumps(msg), retain=True)
            print(datetime.now().strftime("%H:%M"), "->", msg)
        time.sleep(args.every)


if __name__ == "__main__":
    main()
