# PotPi

Plant irrigation on a NodeMCU/ESP8266. Two capacitive soil-moisture sensors
read via an MCP3008 ADC, one mini pump switched by an active-LOW relay, all
powered from USB. Every hour the firmware POSTs a `repository_dispatch` to
GitHub; a workflow appends the reading to `data/YYYY-MM-DD.csv` and the
dashboard at the repo root visualises it on a free GitHub Pages site.

**Live dashboard:** https://philwicke.github.io/potpi/

## Layout

```
potpi/
├── index.html                 dashboard, served by GitHub Pages
├── data/                      populated by ingest-reading workflow
│   ├── YYYY-MM-DD.csv         one row per measurement
│   └── index.json             list of CSV files for the dashboard
├── firmware/                  PlatformIO project (Arduino C++)
│   ├── platformio.ini
│   ├── src/main.cpp
│   └── include/secrets.h.template
├── .github/workflows/
│   └── ingest-reading.yml     repository_dispatch -> CSV row + commit
├── docs/
│   ├── overview.html          interactive hardware migration reference
│   └── porting-notes.md       Pi -> ESP8266 porting research
└── README.md
```

## Hardware

NodeMCU Amica (ESP8266EX, 4 MB flash), MCP3008 ADC, 2 capacitive soil
sensors, 4-channel 5 V relay module (one channel used), mini water pump,
9 V battery (via relay NO/COM contacts).

| Function | NodeMCU | GPIO | Notes |
|---|---|---|---|
| MCP3008 SCLK | D5 | 14 | HSPI |
| MCP3008 MISO | D6 | 12 | HSPI |
| MCP3008 MOSI | D7 | 13 | HSPI |
| MCP3008 CS | D0 | 16 | software CS |
| Sensor power gate | D2 | 4 | active-LOW |
| Pump 2 relay | D1 | 5 | active-LOW, 10 kΩ pull-up to 3V3 recommended |

See `docs/overview.html` for full pinouts (Pi + ESP) and labelled SVG
diagrams.

## How it works

1. Hourly the firmware powers the sensors on, pulse-samples both MCP3008
   channels (median × median to reject noise), then powers them off.
2. Each sensor's voltage is mapped to a 0–100 % wetness using per-sensor
   calibration constants in `firmware/src/main.cpp`.
3. If the aggregate is below `THRESHOLD_PERCENT` and the per-pot cooldown
   has elapsed, the pump runs for `WATERING_SECONDS` (hard-capped).
4. The firmware brings WiFi up, POSTs the reading to GitHub, tears WiFi
   back down to keep the heap free.
5. `ingest-reading.yml` receives the dispatch, appends a CSV row, updates
   `data/index.json`, commits.
6. GitHub Pages serves `index.html`, which fetches `data/index.json`, then
   each CSV, then plots with Chart.js.

## Setup checklist

- [ ] In GitHub: settings → Pages → "Build and deployment" → Source =
      `Deploy from a branch`, Branch = `main`, Folder = `/ (root)`.
- [ ] In GitHub: settings → Actions → Workflow permissions = "Read and
      write permissions".
- [ ] Generate a fine-grained PAT scoped to `PhilWicke/potpi` only, with
      `Contents: read & write` and `Metadata: read`. Paste it into
      `firmware/include/secrets.h` (gitignored).
- [ ] Set WiFi password in the same `secrets.h`.
- [ ] `cd firmware && pio run -t upload` to flash. Then
      `pio device monitor` to watch the boot log.

## Safety

- All active-LOW pins idle HIGH before being driven as outputs (no relay
  twitch on boot).
- Pump pulse is hard-capped by `MAX_PUMP_SECONDS` regardless of caller.
- Voltage-range plausibility check rejects disconnected sensors.
- File-locked CSV write (handled by GitHub via the workflow concurrency
  group) means no double-write on rapid dispatches.
- WiFi up only around the POST, then down — full heap available the rest
  of the time.

## License

MIT
