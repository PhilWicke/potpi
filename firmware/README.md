# PotPi firmware (ESP8266 / NodeMCU Amica)

PlatformIO project. Reads two capacitive soil-moisture sensors via an MCP3008
over HSPI, drives one active-LOW relay for the pump, and POSTs each reading as
a `repository_dispatch` event to GitHub. A workflow in `.github/workflows/`
turns each dispatch into a row in `data/YYYY-MM-DD.csv` and the dashboard at
the repo root visualises it.

## Pin map

| Function | NodeMCU | GPIO | Notes |
|---|---|---|---|
| MCP3008 SCLK | D5 | 14 | HSPI |
| MCP3008 MISO | D6 | 12 | HSPI |
| MCP3008 MOSI | D7 | 13 | HSPI |
| MCP3008 CS | D0 | 16 | software CS |
| Sensor power gate | D2 | 4 | active-LOW |
| Pump relay IN | D1 | 5 | active-LOW; add external 10 kΩ pull-up to 3V3 |

Do **not** put outputs on D3 (GPIO 0), D4 (GPIO 2) or D8 (GPIO 15) — strapping
pins. If their loads pull low at boot the chip will not run our firmware.

## Build / flash

```bash
brew install platformio              # or: uv tool install platformio
cd firmware
cp include/secrets.h.template include/secrets.h
$EDITOR include/secrets.h            # fill WiFi password + GitHub PAT
pio run                              # compile
pio run -t upload                    # flash
pio device monitor                   # 115200 baud serial log
```

## Calibration

Defaults inherited from the Pi project. To recalibrate, write down the
sensor voltage in dry air vs. fully submerged in a glass of water, then
edit `V_DRY_*` / `V_WET_*` in `src/main.cpp`.

A future iteration will move calibration into LittleFS or `secrets.h` so
it can be tuned without recompiling.

## Behaviour

- Every `MEASUREMENT_INTERVAL_MS` (default: 1 hour) the firmware takes a
  pulse-sampled reading.
- If aggregated moisture < `THRESHOLD_PERCENT` AND it's inside the watering
  window AND last watering was >= `MIN_HOURS_BETWEEN_WATERING` ago AND the
  reading is valid, the pump pulses for `WATERING_SECONDS` (capped by
  `MAX_PUMP_SECONDS`).
- Every reading is POSTed to GitHub regardless of whether watering happened,
  so the dashboard has a continuous trace.

## Safety details baked in

- All active-LOW pins are `digitalWrite(HIGH)` before `pinMode(OUTPUT)` so
  the relay/transistor never sees a glitch low pulse on boot.
- The pump call has a hard cap (`MAX_PUMP_SECONDS`) even if a future code
  path passes a runaway value.
- `valid` flag rejects out-of-range ADC reads (suggests disconnected sensor).
- WiFi is up only around the POST and torn down again, so most of the time
  the full heap (~50 kB) is available for sampling.

## Why not deep sleep?

The board is USB-powered, so the savings are marginal compared to the wiring
complexity (GPIO 16 → RST loopback wire). If we move to battery later, deep
sleep is the right call — see `docs/porting-notes.md`.
