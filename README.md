# Captain Energy

**A small box that keeps a shop's fridge, and the food in it, safe during power cuts, with three tiny AI models running on an ESP32.**

Built for the **TSYP14 Technical Challenge, "Edge AI & IoT for Resilient Smart Infrastructure"**. The code and documents also use the working name *Edge Energy Manager*.

---

## The problem

In summer, small Tunisian shops with a fridge (grocers, butchers, dairy sellers, pharmacies,) face three problems with the grid:

- **Voltage drops and an unstable grid:** the voltage sags or wobbles without cutting completely.
- **Fast reconnects:** the power comes back, cuts again, and comes back within minutes. The fridge compressor restarts under pressure, draws a large inrush current, overheats and eventually burns.
- **Long cuts:** the fridge stops and the food warms up without anyone knowing. It ends up thrown away, or sold when it is no longer safe (meat, milk, insulin and vaccines at 2 to 8 °C).

Existing solutions each cover only part of this:

- **A voltage protector** cuts bad voltage and waits before reconnecting, but it has no battery.
- **A UPS** powers *everything* until it is empty.
- **Neither one knows how long the food stays safe.**

## What Captain Energy does

| When | What the box does |
|---|---|
| **Before the cut** | Recognises a degrading grid (AI 1). Combines it with heat, peak hours and STEG announcements into a risk score. Pre-cools the fridge to store cold in advance. |
| **During the cut** | Switches to the battery in about 1 s, with one long beep. Computes the **battery runtime** (AI 2) and the **food safety time** (AI 3). Keeps the fridge first and sheds the other loads. If the battery won't last, it says when to sell or move the stock: *"vendez avant HH:MM"*. |
| **When power returns** | Waits for a stable grid (2 min, 5 min if unstable) and a 3-minute compressor delay. Then brings the loads back one by one. |

**Alerts that work without reading:**

- **Colour LED:** green = normal, orange = on battery, red = danger.
- **A different beep for each situation:**

  | Beeps | Meaning |
  |---|---|
  | 1 long beep | Power cut |
  | 2 short beeps | STEG power is back |
  | 3 beeps every 5 min | The battery won't last long enough |
  | 5 fast beeps every minute | The food is too warm |

- **OLED screen.**
- **Phone notification** (MQTT → ntfy).

It needs **no cloud**. Wi-Fi only adds outage announcements, weather forecasts and phone alerts.

## Where the AI is

All three models run **on the ESP32**. The PC is only used once, to train AI 1 and AI 3.

| | What it does | Model | Size |
|---|---|---|---|
| **AI 1: grid state** | Samples the voltage 100 times per second and extracts 8 features per second. Classifies the grid as *normal, unstable, dropping or cut*. | int8 MLP, 8 → 16 → 4 | 192 bytes of weights |
| **AI 2: battery** | Reads the start level from the rest voltage, then counts the charge going out (INA219 current). Keeps a 10 % reserve and learns the fridge's average consumption. Runtime = usable energy ÷ measured power. | Coulomb counting + 3S Li-ion table + online learning | — |
| **AI 3: food safety time** | Learns the box's own warming speed *k* on the spot (Newton's law, least squares with forgetting). A network then gives the safety time **from the first second of the cut**. | int8 MLP, 7 → 16 → 1, trained on 40,000 simulated outages | 129 bytes of weights |

**AI 3 results** on 8,000 simulated test outages it never saw:

| Method | Median error | Too optimistic (>10 %) |
|---|---|---|
| Newton formula alone | 33 % | 25.6 % of cases |
| Network | 23 % | 38.9 % |
| Network × 0.51 safety margin (**used**) | 50 % | **3.3 %** |

**Inferred without extra sensors:**

- **Product temperature:** the prototype has no probe in the food. It is *simulated*: it follows the fridge air with a 15-minute delay.
- **Door openings:** there is no door switch. An opening is detected when the air rises at least 0.3 °C in 10 s and passes the product temperature. In simulation, every opening of 10–45 s was detected, with 0 false alarms in 6 h.

## The prototype

| Part | Role |
|---|---|
| ESP32 DevKit V1 (powered by USB) | Runs all the measurements, AI and decisions. Being on USB, it stays on when the "STEG" is cut. |
| Pack n°1: 3 × 18650 in series + 10 kΩ potentiometer | Simulated STEG. Turning the knob gives drops or instability; unplugging it is a cut. |
| INA219 n°1 (0x40) | STEG voltage at 100 Hz (AI 1) |
| Pack n°2: 3 × 18650 (11.1 V, 1.5 Ah) + 5 A fuse | Backup battery |
| INA219 n°2 (0x41, A0 bridge soldered) | Battery voltage and current (AI 2, Peltier protections) |
| Peltier TEC1-12704 + heatsinks + fan, in a polystyrene box | Real cooling, about 3 A |
| 5 V relay module (low-level trigger), NO contact, GPIO18 | Switches the Peltier on and off |
| 2 × DS18B20 + 4.7 kΩ resistor, GPIO4 | Fridge air and shop air |
| 2 LEDs (GPIO25, 33) | Stand for the air conditioner and the lights |
| RGB LED (16/17/23), active buzzer (19), OLED SSD1306 (I2C) | Alerts |

**Planned, not built yet:**

- a probe inside the food;
- LM393 hardware cut-off;
- MOSFET soft start;
- KiCad PCB and DIN-rail case;
- derja voice messages (DFPlayer Mini);
- RFID profile badges.

## Repository layout

```
firmware/
  edge_energy_manager/   main firmware (PlatformIO, ESP32, Arduino)
    src/config.h         all settings: pins, thresholds, battery, Wi-Fi
    src/main.cpp         measurements, state machine, alerts, MQTT
    src/grid_features.h  AI 1: the 8 features
    src/tinyml.h         AI 1: int8 inference
    src/grid_model.h     AI 1: weights (generated by Python)
    src/battery.h        AI 2: charge level and runtime
    src/thermal.h        AI 3: online learning of k, Newton formula
    src/safety_ai.h      AI 3: int8 network inference
    src/safety_model.h   AI 3: weights (generated by Python)
    src/door_detect.h    door openings from temperatures
    src/decision.h       risk score and decision engine
  collect_grid/          small firmware that records voltage features for AI 1
tools/                   Python: data logging, training, export, MQTT feed
tests/                   PC tests of the firmware logic (g++, mocked Arduino)
guide/                   full French guide (LaTeX + PDF)
docs/                    full design document (LaTeX + PDF)
```

## Quick start

1. Open `edge_energy_manager.code-workspace` in **VS Code** and install the suggested extensions (PlatformIO IDE, Python).
2. Wire the prototype as described in `guide/guide_ia_embarquee.pdf` (section "Le câblage").
3. Adjust `firmware/edge_energy_manager/src/config.h` if needed (pins, Wi-Fi), then click **Build** and **Upload** in PlatformIO.
4. Open the serial monitor (115200 baud) and type `info`.

Serial commands:

| Command | What it does |
|---|---|
| `profil=volaille` | Sets the goods profile (also `laitier`, `medicament`, `legumes`, `maquette`) |
| `annonce=30,2,39` | Announces a cut in 30 min, lasting 2 h, with 39 °C forecast |
| `annule` | Clears the announcement |
| `pause=45` | Turns the Peltier off for 45 min so the box can learn *k* |
| `soc=` | Sets the battery level |
| `silence` | Stops repeated beeps |
| `reset` | Clears a Peltier fault |
| `resetk` | Forgets the learned *k* |
| `info` | Prints the current state |

### Train the models (PC)

```bash
cd tools
python -m venv .venv
.venv\Scripts\activate          # Windows  (Linux/macOS: source .venv/bin/activate)
python -m pip install -r requirements.txt

# AI 1: record the 4 situations with the collect_grid firmware, then train
python log_serial.py --port COM5 --label normal   --seconds 180 --out data/normal.csv
python log_serial.py --port COM5 --label instable --seconds 180 --out data/instable.csv
python log_serial.py --port COM5 --label baisse   --seconds 180 --out data/baisse.csv
python log_serial.py --port COM5 --label coupure  --seconds 120 --out data/coupure.csv
python train_grid_model.py data/normal.csv data/instable.csv data/baisse.csv data/coupure.csv

# AI 3: simulate 40,000 outages, train, export (about 1 minute)
python train_safety_model.py --plot
```

Both scripts write their `.h` file straight into `firmware/edge_energy_manager/src/`. Rebuild and upload the firmware afterwards.

### Run the PC tests

```bash
cd tests
g++ -std=c++17 -I../firmware/edge_energy_manager/src test_logic.cpp -o test_logic
./test_logic
```

These 28 checks cover: the C++ inference matching Python for both networks, *k* learning, battery maths, the decision cases, and door detection.

## Honest limits

- **The `grid_model.h` in this repository is a test model** trained on synthetic data. The real accuracy of AI 1 must come from our own recordings.
- The product temperature is **simulated**. AI 3 was trained on **simulated** outages, not real ones.
- The door detection thresholds must be tuned on the real box.
- The 1.5 Ah pack gives about 26 minutes of Peltier from full charge.
- Over-voltage cannot be shown on the prototype: the pack peaks at 12.6 V.

## Documentation

- `guide/guide_ia_embarquee.pdf`: the AI part, step by step (French). Covers the problem, materials, wiring, each AI line by line, results, the demo and troubleshooting.
- `docs/Captain_Energy_Document_complet.pdf`: the full design document (French). Covers the product vision, circuit, PCB and prototype.

## Anonymity

This repository is anonymous on purpose, as the TSYP14 rules require: no team, member or university names.
