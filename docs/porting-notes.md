# Porting PotPi from Raspberry Pi 3B to ESP8266 (NodeMCU Amica / ESP-12E)

Notes for migrating a working Pi GPIO irrigation project to ESP8266. Focus: what bites you, what to do instead, and which libraries are still maintained in 2026.

The Pi is a Linux box that doesn't care about pin states at boot, has a clean 3.3 V analog rail off an LDO, has an RTC of sorts (via NTP daemon at leisure), and gives you ~1 GB of RAM. The ESP8266 has none of those luxuries. Most of the work below is reasserting things the Pi gave you for free.

## 1. GPIO bootstrap pins

Three pins decide boot mode at reset and the firmware can't override them until *after* the bootloader runs. The required latched state is **GPIO15 = LOW, GPIO0 = HIGH, GPIO2 = HIGH** for normal flash boot. GPIO0 LOW puts the chip into UART bootloader (download mode); GPIO15 HIGH selects SDIO boot and the chip just hangs. NodeMCU boards bake the correct pull resistors onto the module, but anything you hang off these pins must respect those defaults or boot fails. The pins are also bouncy for the first ~150 ms while the ROM bootloader prints its garbage at 74880 baud on TX (GPIO1) and pulses GPIO2 / GPIO16 HIGH.

**Your pin plan is risky.** Sensor-power gate on GPIO0 and pump relay on GPIO2, both active-LOW idle-HIGH, technically satisfy the boot requirement (both idle HIGH = OK). But the failure modes are nasty:

- **GPIO0**: if the gate transistor's pulldown or the sensor's parasitic load drags GPIO0 below ~1 V at the moment of reset, you enter flash mode and the sketch never starts. A 10 kΩ pull-up to 3V3 directly on the GPIO0 net (in addition to the on-module one) is cheap insurance. Drive the transistor base through a series resistor so the GPIO never sees a low-impedance pull.
- **GPIO2**: same story, plus GPIO2 *briefly outputs HIGH* during boot (the boot-mode read is internal). Your active-LOW relay sees a HIGH idle, so this is benign for the pump — the relay stays off. Good. *However*, if you ever reassign and use GPIO2 as an active-HIGH output to a load, you get a phantom pulse.
- **The real concern is the order of events.** Between power-on and `setup()`, GPIO0/2/16 may pulse HIGH for ~100 ms before your code calls `pinMode()` and `digitalWrite(HIGH)`. For active-LOW loads (your case) that's fine — HIGH = inactive. For active-HIGH it would fire.

**Recommendation:** swap the pump relay to GPIO5 (D1) and the sensor-power gate to GPIO4 (D2). Both are I2C pins by convention but unused here, have no boot-mode role, and stay quiet at boot. Keep GPIO0 free as the FLASH button. If you must use GPIO0/2, add a 10 kΩ pull-up at the GPIO pin (not just at the module) and verify with a scope that the relay input stays > 2.0 V the entire boot window.

Sources:
- [ESP8266 Pinout Reference (Random Nerd Tutorials)](https://randomnerdtutorials.com/esp8266-pinout-reference-gpios/)
- [ESP8266 Arduino core – boards & pin reference](https://arduino.esp8266.com/Arduino/versions/2.0.0/doc/boards.html)
- [Espressif forum: GPIO2/GPIO15 boot behavior](https://bbs.espressif.com/viewtopic.php?t=774)

## 2. GPIO16 (D0) quirks

GPIO16 lives in the RTC domain, not in the main GPIO mux. Consequences: no level-triggered interrupts, no `INPUT_PULLUP` (only `INPUT_PULLDOWN_16`), PWM via `analogWrite` is software-emulated and unreliable, and writes are 5–10x slower than other pins because the API has to go through RTC registers. It's reserved as the deep-sleep wake line.

For your use case — **software-driven CS for the MCP3008** — GPIO16 is *fine but suboptimal*. SPI CS doesn't need interrupts or PWM, and the MCP3008 doesn't care if CS toggles in 200 ns vs 2 µs. The slower write just nibbles your sampling rate by a few percent. The only real concern is that GPIO16 outputs HIGH at boot (which is the *idle* state for active-LOW CS, so the MCP3008 stays deselected — good).

**Recommendation:** keep CS on GPIO16 if you want; works fine. If you'd rather use hardware CS, the HSPI hardware CS is GPIO15 — but GPIO15 is a strapping pin that must be LOW at boot, which conflicts with idle-HIGH CS. So you'd need a small inverter or a level-shifter trick, and it's not worth it. **Use GPIO16 as a software CS, driven before/after each `SPI.transfer()` block.** Add a 10 kΩ pull-up to 3V3 on the CS line so the MCP3008 is deselected even during the boot pulse uncertainty window.

Sources:
- [esp8266/Arduino issue #7765 – GPIO16 as PWM is a bad idea](https://github.com/esp8266/Arduino/issues/7765)
- [Random Nerd Tutorials – ESP8266 GPIO reference](https://randomnerdtutorials.com/esp8266-pinout-reference-gpios/)

## 3. HSPI on ESP8266 and the MCP3008

The ESP8266 has two SPI controllers: SPI (used internally for flash, hands off) and HSPI on D5/D6/D7. The Arduino-core `SPI` library targets HSPI and is hardware-accelerated — much faster and more reliable than any bit-banged option. CircuitPython's `busio.SPI` bit-bang approach is unnecessary here; you have a real SPI peripheral, use it.

MCP3008 maxes at **1.35 MHz @ 2.7 V VDD, 3.6 MHz @ 5 V VDD**. At 3.3 V, derate to ~2 MHz to be safe. The ESP8266's HSPI runs up to 80 MHz, so you're entirely limited by the ADC. One conversion takes ~17 SPI clocks plus overhead, so at 1 MHz you get ~30 kSPS — plenty for soil moisture (a 10 Hz read rate is overkill). Stay at `SPISettings(1000000, MSBFIRST, SPI_MODE0)` and you'll have no signal-integrity headaches.

**Library:** **`Adafruit_MCP3008`** is still the cleanest API and is actively maintained as of 2026 (`Adafruit_MCP3008.h`, `mcp.begin(cs_pin)`, `mcp.readADC(channel)`). It uses HSPI under the hood on ESP8266. **`bakercp/MCP3XXX`** is a fine alternative with a slightly nicer object-per-channel API. Avoid any library that claims to bit-bang — wasted CPU.

Sources:
- [Adafruit_MCP3008 library on GitHub](https://github.com/adafruit/Adafruit_MCP3008)
- [bakercp/MCP3XXX library](https://github.com/bakercp/MCP3XXX)
- [ESP8266 Arduino SPI library source](https://github.com/esp8266/Arduino/blob/master/libraries/SPI/SPI.cpp)

## 4. Power and current

The ESP8266 sits at ~70 mA when WiFi is associated but quiet, and spikes to **250–400 mA for sub-millisecond bursts during TX**. Brown-outs manifest as random `rst cause:4` (wdt reset) or `rst cause:2` (exception) at boot, the WiFi failing to connect, or the chip rebooting every time it tries to TLS-handshake. The ESP8266 has no software brownout detector you can poll — there's no equivalent of the Pi's `throttled=0x50000`. You diagnose by seeing crashes correlated with WiFi activity. (The ESP32 has a programmable brownout register; the ESP8266 does not.)

**The NodeMCU Amica's onboard CP2102 (or CH340) regulator can deliver ~500 mA from USB**, which is technically enough but lives close to the edge. A USB host that throttles below 1 A at 5 V will brown you out under WiFi load. The pump driving 5 V at typical 200–500 mA is the other big concern — **do not power the pump from the NodeMCU's VIN/Vbus rail**. Give it its own 5 V supply (or a separate USB brick) with a common ground.

**Decoupling:** the ESP-12E module has only a small ceramic on-board. Add **a 10 µF tantalum or electrolytic + 100 nF ceramic in parallel** between 3V3 and GND, as close to the module as your PCB/proto layout allows. If you see WiFi flakiness, drop in **a 470 µF bulk cap on the 5 V rail near the USB connector**.

**MCP3008 VREF on the same noisy 3V3** is fine for soil moisture (you don't need 10 bits of accuracy from a sensor that's already ±5%). If you want cleaner readings, fit an **LM4040-2.5 shunt reference + 100 nF cap** on VREF and feed the sensor from 3V3 still — your ratio just becomes (Vsensor / 2.5 V) over the 0–1023 code range. Simpler alternative: tie VREF to VDD via a 100 Ω + 1 µF RC, both on the analog side of a star ground. Either is a real improvement.

Sources:
- [Espressif – ESP8266 voltage spikes & power supply guidance](https://bbs.espressif.com/viewtopic.php?t=2494)
- [Let's Control It – Power wiki](https://www.letscontrolit.com/wiki/index.php/Power)

## 5. HTTPS / TLS memory

This is the largest source of pain in the port. After WiFi associates, you have **~25–35 kB free heap**. BearSSL needs ~6 kB for its secondary stack and **another 16 kB + 16 kB by default** for the per-connection receive/transmit buffers. That alone doesn't fit. You must use **MFLN (Maximum Fragment Length Negotiation)** to renegotiate down to 512–1024 byte buffers. `api.github.com` supports MFLN at 1024 bytes (last verified mid-2025), which makes the TLS connection comfortably fit.

Standard pattern:

```cpp
BearSSL::WiFiClientSecure client;
if (!client.probeMaxFragmentLength("api.github.com", 443, 1024)) {
  // server does not support MFLN at this size; either bump or fail
}
client.setBufferSizes(1024, 1024);  // RX, TX
client.setTrustAnchors(&githubCAs); // see below
// ... HTTP client begin / GET / POST
```

**Identity verification: pick one, in this order of preference.**

1. **`setTrustAnchors()` with a small custom CA bundle.** GitHub uses certs chained to a couple of well-known CAs (currently `DigiCert Global Root G2` and `Sectigo`/`USERTrust`). Bake those two roots into PROGMEM via `BearSSL::X509List` and you have proper PKI validation that survives leaf-cert rotation. Use `certs-from-mozilla.py` from the ESP8266 Arduino core to extract a *minimal* `certs.ar` rather than the full Mozilla bundle (the full bundle won't fit). Keep CPU at 160 MHz (`system_update_cpu_freq(160)`) so the handshake doesn't take 6+ seconds.
2. **`setFingerprint()` with the SHA-1 fingerprint of the GitHub cert.** Tiny memory footprint. Will break the day GitHub rotates the leaf cert (every ~90 days for Let's Encrypt-class, longer for DigiCert). Not recommended for a device you don't want to babysit.
3. **`setInsecure()`** — works, zero security. Anyone on your network can MITM your push token. Don't.

The CPU-clock bump from 80 to 160 MHz roughly halves handshake time and costs ~10 mA extra. Do it.

Sources:
- [BearSSL WiFi Classes – ESP8266 Arduino Core docs](https://arduino-esp8266.readthedocs.io/en/latest/esp8266wifi/bearssl-client-secure-class.html)
- [Chris Dzombak – HTTPS Requests with a Root Cert Store on ESP8266](https://www.dzombak.com/blog/2021/10/https-requests-with-a-root-certificate-store-on-esp8266-arduino/)
- [Random Nerd Tutorials – ESP8266 HTTPS Requests](https://randomnerdtutorials.com/esp8266-nodemcu-https-requests/)

## 6. GitHub Contents API from ESP8266

The Contents API works but is fiddly: every update requires you to know the file's current SHA, base64-encode the new content, and PUT with the JSON body `{"message":"...","content":"<base64>","sha":"<old sha>"}`. That means GET first, then PUT — two TLS handshakes (each ~30 KB heap blip) per write. With a fine-grained PAT, you get **5,000 requests/hour**, easily covering 10–30 CSV updates/day.

**Three realistic approaches, ranked:**

1. **Repository Dispatch (recommended).** From the ESP, POST a single payload to `/repos/:owner/:repo/dispatches` with your sensor reading inside `client_payload`. A GitHub Actions workflow listens, appends to your CSV, and commits. The ESP makes one cheap HTTPS POST per reading; all the file-SHA gymnastics live in YAML on GitHub's infrastructure. The Action can also regenerate any derived JSON for Chart.js. Downsides: ~10–60 s latency before the chart updates; Actions has its own free-tier quota (2,000 min/month for private repos, unlimited for public) — at one short run per reading, you're fine. **This is the right answer for your project.**
2. **Issues API as a log.** POST sensor data to a single issue as comments. Vastly simpler than Contents (no SHA dance), but turns into a junk-issue stream and you still need a way to render it. Use only if you don't care about a clean repo.
3. **Direct Contents API.** Works, but every update is a GET+PUT and you have to handle 409 conflicts. Only do this if you genuinely need the ESP to be the source of truth without any GitHub-side compute.

**Token handling.** A fine-grained PAT scoped to one repository with only `contents:write` (or, for the Dispatch approach, `actions:write`) limits blast radius. Store it in flash via `LittleFS` in a config file outside source control, not hard-coded in the sketch. Better: keep the token in EEPROM and wipe-and-rewrite during a one-time provisioning step over a captive-portal WiFi config (e.g. `WiFiManager`). Treat the token as recoverable, not secret-grade — it will end up on the device, and the device is physical.

Sources:
- [GitHub REST API rate limits](https://docs.github.com/en/rest/using-the-rest-api/rate-limits-for-the-rest-api)
- [GitHub Repository Dispatch event](https://docs.github.com/en/rest/repos/repos#create-a-repository-dispatch-event)
- [galihru/githubiot – Arduino lib for the Contents API pattern](https://github.com/galihru/githubiot)

## 7. LittleFS for persistence

Default 4 MB NodeMCU board: use the **"4MB (FS:1MB OTA:~1019kB)"** partition scheme in the Arduino IDE / PlatformIO. That gives you 1 MB of LittleFS, which is huge for your workload (~30 lines/day at 80 bytes/line = 2.4 KB/day = 1 year of data uses 0.9 MB, but you'll be rotating). If you don't need OTA, "4MB (FS:2MB)" is also fine.

**LittleFS, not SPIFFS** — SPIFFS is deprecated and known-broken for power-loss durability. LittleFS does dynamic wear leveling and is power-loss safe (incomplete writes roll back). Library is **`LittleFS.h`** in the ESP8266 core; no external dep.

**Wear amplification is the real concern.** Flash erase blocks are 4 KB on this part. Each append to a small CSV line rewrites the entire 4 KB block. At 30 lines/day, that's ~30 erases/day on the same block, or ~10,000 erases/year. Cheap flash is rated for 100,000 erase cycles → you'd theoretically last 10 years even without wear leveling, and LittleFS spreads writes around so realistically it's effectively indefinite. **Still, batch writes**: buffer readings in RAM and `flush()` once every N readings or on time boundary (e.g. every hour). Open with `"a"` (append) mode.

**File-size and per-file overhead.** LittleFS has more overhead per file than SPIFFS, so prefer one rotating CSV over many tiny files. Use a daily/weekly rotation pattern: `data-YYYY-WW.csv`, keep last 8, delete older. Check `LittleFS.usedBytes()` once a day and prune if over 70% full.

Sources:
- [Filesystem – ESP8266 Arduino Core docs](https://arduino-esp8266.readthedocs.io/en/latest/filesystem.html)
- [littlefs-project/littlefs](https://github.com/littlefs-project/littlefs)
- [Random Nerd Tutorials – ESP8266 LittleFS write](https://randomnerdtutorials.com/esp8266-nodemcu-write-data-littlefs-arduino/)

## 8. NTP time and timezone

Use the core's built-in `configTime()` with a POSIX TZ string. This handles DST correctly. No third-party library needed.

```cpp
configTime("CET-1CEST,M3.5.0/02,M10.5.0/03",
           "pool.ntp.org", "time.nist.gov");
```

(That string is Berlin. Find yours in [nayarsystems/posix_tz_db zones.csv](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv).)

**Common bugs:**

- **Stale clock.** `time()` returns 1970-01-01 until the first SNTP packet arrives — typically 1–5 s after WiFi associates. Don't write timestamps until `time(nullptr) > 8 * 3600` (anything past 1970-01-01 08:00) or you'll log Unix-epoch garbage. Loop `delay(200); now = time(nullptr);` until valid.
- **No RTC.** Every reboot loses time. If you deep-sleep, you must re-NTP on wake (~1–5 s). The RTC memory persists across deep sleep but the system clock does not survive a hard reset.
- **Drift.** ESP8266 has a software clock backed by the 80 MHz crystal — drifts ~10 s/day. Re-sync every few hours (`configTime` is idempotent — call it again).
- **DNS.** If your home WiFi has flaky DNS, NTP fails silently. Use an IP literal as a fallback or hard-code the gateway: `configTime(tz, "192.168.1.1");`.

Sources:
- [esp8266/Arduino NTP-TZ-DST example](https://github.com/esp8266/Arduino/blob/master/libraries/esp8266/examples/NTP-TZ-DST/NTP-TZ-DST.ino)
- [Werner Rothschopf – NTP & DST without 3rd-party libs](https://werner.rothschopf.net/202011_arduino_esp8266_ntp_en.htm)
- [POSIX timezone database](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv)

## 9. Sleep modes

For a **USB-powered** irrigation controller: **don't bother with deep sleep.** The math: deep sleep saves ~70 mA continuous → ~6 Wh/day, which is meaningless if you're plugged into mains. Deep sleep also costs ~130 ms boot + 1–5 s NTP resync + 2–4 s WiFi association + 1–2 s for one TLS handshake = ~10 s of overhead per wake. Loses RAM, breaks any state machine, requires the GPIO16→RST jumper which interferes with USB reprogramming.

**Use Forced Modem Sleep or Automatic Modem Sleep.** Both keep the system clock and RAM. Modem sleep drops the WiFi radio between DTIM beacons; current goes from ~70 mA → ~15 mA. Set it up once and forget:

```cpp
WiFi.setSleepMode(WIFI_MODEM_SLEEP, 3);  // sleep through 3 DTIMs
```

If you want lower idle current (e.g. battery), **Timed Light Sleep** drops to ~0.4 mA and recovers in 5 ms — but again, you're on USB. Not worth the complexity.

If you ever migrate to battery: connect GPIO16 to RST via a **Schottky diode** (1N5819, cathode to GPIO16, anode to RST) rather than a direct wire — this keeps the USB-serial DTR/RTS reset working for flashing. Direct wire blocks the auto-reset circuit.

Sources:
- [ESP8266 Arduino core – LowPowerDemo README](https://github.com/esp8266/Arduino/blob/master/libraries/esp8266/examples/LowPowerDemo/README.md)
- [Espressif – ESP8266 Low Power Solutions PDF](https://www.espressif.com/sites/default/files/9b-esp8266-low_power_solutions_en_0.pdf)
- [ThingPulse – Max deep sleep deep-dive](https://thingpulse.com/max-deep-sleep-for-esp8266/)

## 10. MCP3008 reference and sensor-power switching

**VREF.** As above: tie VREF to VDD through an RC (100 Ω + 1 µF) and put a 100 nF cap directly between AGND and VREF. Don't pull VREF straight from 3V3 with no filtering. If you want measurable repeatability, fit an LM4040DBZ-2.5 (TO-92 shunt reference) with a 1 kΩ feed from 3V3, decoupled with 100 nF. Cost: 60 cents and one trace.

**Sensor-power switching topology.** You have two choices:

- **Low-side N-MOSFET / NPN switch** (sensor GND through the FET to ground). Simplest, gate is easy to drive from 3.3 V logic, no level-shifting. *But* the sensor's ground floats when off, which can confuse other sensors sharing the same supply.
- **High-side P-MOSFET / PNP switch** (VDD through the FET to sensor's V+). Sensor's GND stays solid. Needs a level-shifter (NPN switch) on the gate because a P-MOSFET source is at 3V3 and you need Vgs ≈ -3 V to turn it fully on. This is what most "sensor power" textbook circuits do.

Your Pi project used a transistor on GPIO18 — if that was the existing low-side NPN switching the sensor's V+, that's a *common-emitter on the high side*, which kind of works for small loads but droops Vsensor by Vbe. Capacitive soil sensors typically draw < 5 mA, so this is fine, but you lose ~0.7 V of headroom for VREF.

**Recommended:** keep the Pi circuit if it works, just change the gate-driving pin to GPIO4 (D2). Active-LOW (the way the Pi did it) is preserved. Verify Vsensor > 2.5 V when the gate is on — if not, swap the NPN for a small P-MOSFET (AO3401 or similar, SOT-23) and add an NPN inverter (BC817 + 10 kΩ pullup, 10 kΩ + 10 kΩ divider) for proper high-side switching.

**Settling time.** Capacitive soil moisture sensors need 100–500 ms after power-up before the reading stabilizes. Toggle the gate, `delay(200)`, then read four times and average.

Sources:
- [Microchip MCP3004/MCP3008 datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/MSLD/ProductDocuments/DataSheets/MCP3004-MCP3008-Data-Sheet-DS20001295.pdf)
- [Rheingold Heavy – MCP3008 Tutorial: Functionality Overview](https://rheingoldheavy.com/mcp3008-tutorial-01-functionality-overview/)

## 11. Active-LOW relay at startup

The 5 V opto-isolated relay module reads logic-HIGH as "off". Between mains-on and your `setup()` calling `pinMode(D4, OUTPUT); digitalWrite(D4, HIGH);`, the GPIO is in an indeterminate state for ~30–150 ms — the on-die pull-up keeps it weakly HIGH but a low-impedance load (the opto's input through a current-limiting resistor) can drag it below the threshold. **In practice this manifests as a 50–100 ms relay click and pump twitch at every reset.**

**Fixes, in order of effectiveness:**

1. **External 10 kΩ pull-up from the GPIO to 3V3** at the relay module side. Forces idle HIGH even if the ESP is tri-stated. This is the single most reliable mitigation. Always do this for any relay/MOSFET/load that should be off at startup.
2. **Don't use GPIO0/2/15/16** for active-LOW loads if you can avoid it (see §1). GPIO4 (D2) and GPIO5 (D1) have no boot duties and stay quiet.
3. **Use the relay module's own opto LED current as a logic-level indicator** — measure with a scope what voltage actually appears on the IN pin during the boot pulse, and add a series resistor if needed to bias the opto away from its trigger.
4. **If you really care**, put a small RC delay (100 kΩ + 1 µF) between GPIO and relay IN, so the relay never sees pulses shorter than ~100 ms.

Don't rely on the on-die pull-up alone — it's ~50 kΩ and the opto can sink enough to pull it down. Always add the external pull-up.

Sources:
- [Home Assistant community: ESP8266 active-LOW relay boot pulse, solved](https://community.home-assistant.io/t/esp8266-relay-active-low-how-to-prevent-triggering-on-boot-or-reset-solved/88279)
- [Let's Control It forum: relay during boot, NodeMCU](https://www.letscontrolit.com/forum/viewtopic.php?t=6932)

## 12. Toolchain

**Use PlatformIO.** Arduino IDE 2.x with arduino-cli works, but for a project that pulls in BearSSL, LittleFS, ArduinoJson, and an MCP3008 driver, you'll want PlatformIO's `lib_deps`, `monitor_filters`, partition-table selection, and OTA upload baked into `platformio.ini`. Reproducible builds from clean checkout. Works the same in CI if you ever want to build via GitHub Actions.

Minimum `platformio.ini`:

```ini
[env:nodemcuv2]
platform = espressif8266 @ ^4.2
board = nodemcuv2
framework = arduino
monitor_speed = 115200
upload_speed = 460800
board_build.filesystem = littlefs
board_build.ldscript = eagle.flash.4m1m.ld  ; 1 MB FS, ~1 MB OTA
build_flags =
  -D BEARSSL_SSL_BASIC
  -D PIO_FRAMEWORK_ARDUINO_ESPRESSIF_SDK22x_191024
lib_deps =
  bblanchon/ArduinoJson @ ^7.0
  adafruit/Adafruit MCP3008 @ ^1.4
  tzapu/WiFiManager @ ^2.0
```

**Maintained libraries (verified active as of 2026):**

- `ESP8266WiFi` (core) — WiFi + WiFiClientSecure
- `ESP8266HTTPClient` (core) — HTTP/HTTPS convenience layer
- `LittleFS` (core) — filesystem
- `time.h` + `configTime` (core, posix-ish) — NTP/time
- `Adafruit_MCP3008` — ADC driver
- `ArduinoJson` v7 — JSON for the GitHub API payloads
- `WiFiManager` (tzapu) — captive-portal WiFi provisioning, optional but pleasant

Avoid: anything in this space that hasn't been touched since 2021, the old `ESP8266HTTPSUpdate` for non-OTA HTTP, and SPIFFS.

Sources:
- [PlatformIO – Espressif 8266 platform docs](https://docs.platformio.org/en/latest/platforms/espressif8266.html)
- [ESP8266 Arduino core libraries reference](https://arduino-esp8266.readthedocs.io/en/latest/libraries.html)

## 13. Other non-obvious migration gotchas

- **`yield()` / `delay()` is mandatory.** The ESP8266 runs WiFi and TCP/IP as cooperative tasks on the same core as your sketch. Any loop > ~20 ms that doesn't call `yield()` or `delay(0)` will trigger the soft watchdog and reboot you in 3 s. Convert any `while(...)` polling loops to event-driven code, or sprinkle `yield()` inside. The hard watchdog fires at ~8 s.
- **`Serial.print()` in tight loops is a sink** — at 115200 baud, you can print maybe 10 KB/s. A debug `Serial.println(String(value, 6))` inside a 1 kHz loop will absolutely WDT you.
- **No malloc after WiFi is up (rule of thumb).** Allocate big buffers in `setup()` before `WiFi.begin()`. After WiFi associates, the heap is fragmented and `malloc(8 KB)` can fail even when `ESP.getFreeHeap()` shows 20 KB. Use `ESP.getMaxFreeBlockSize()` for the *real* answer. Avoid `String` concatenation in long-running loops — each `+` reallocs. Use `char buf[N]` + `snprintf` for any payload assembly.
- **Avoid `String` for the GitHub payload.** Build the JSON with ArduinoJson into a fixed `StaticJsonDocument<512>` or a `JsonDocument` with a preallocated `char[]`. The base64 encoding of CSV chunks should also go into a stack buffer.
- **Watchdog during TLS.** A slow handshake (no MFLN, 80 MHz CPU) can take 6+ seconds. `client.setTimeout(15000)` and bump CPU to 160 MHz: `system_update_cpu_freq(160);` in `setup()` after WiFi connects.
- **No `os.system`, no subprocess, no Python.** Everything you'd do in a one-liner on the Pi (`requests.put(...)`, `datetime.now()`, `open('/var/log/x', 'a')`) is now boilerplate C++. Budget for the line-count growth: a 200-line Python script is typically 500–800 lines of Arduino C++.
- **WiFi reconnection.** WiFi will drop. Set `WiFi.setAutoReconnect(true); WiFi.persistent(true);` and structure your main loop so it tolerates `WiFi.status() != WL_CONNECTED` for many seconds. Never block on WiFi — let the sensor reads continue offline, queue them in LittleFS, and flush on reconnect.
- **Reset cause.** After every boot, log `ESP.getResetReason()` and `ESP.getResetInfo()`. Distinguishes "I pressed reset" from "WDT fired during TLS" from "brownout". Huge debugging timesaver.
- **Reboot, don't recover.** If you're not sure how to handle an exceptional state (failed allocation, repeated WiFi disconnect, weird sensor reading), `ESP.restart()`. The ESP boots in < 200 ms; embrace it.
- **Crash decoder.** Stack-trace text in the serial monitor is hex; PlatformIO has `pio device monitor` with `esp8266_exception_decoder` filter that turns it into source lines. Set this up before you start, not after the first crash.

Sources:
- [esp8266/Arduino FAQ – my ESP crashes](https://arduino-esp8266.readthedocs.io/en/latest/faq/a02-my-esp-crashes.html)
- [bblanchon/cpp4arduino – heap fragmentation notes](https://github.com/bblanchon/cpp4arduino/blob/master/HeapFragmentation/Ports/MemoryInfo.Esp8266.cpp)
- [Medium: Cooperative Multitasking on the ESP8266](https://medium.com/@srmq/cooperative-multitasking-on-the-esp8266-arduino-665a040457c8)

---

## Migration risk register

Ranked by priority for this specific project. "Likelihood" assumes you don't apply mitigation; "Severity" is the impact if it triggers.

| # | Risk | Severity | Likelihood | Mitigation |
|---|------|----------|------------|------------|
| 1 | TLS handshake exhausts heap → random crashes during GitHub push | **High** | High | Use MFLN + `setBufferSizes(1024,1024)`; CPU @ 160 MHz; allocate JSON buffers statically; use `setTrustAnchors` not `setInsecure` |
| 2 | Pump fires at every boot (active-LOW relay floating during ~150 ms boot window) | **High** | Med-High | External 10 kΩ pull-up at the relay IN pin; move pump to GPIO5 (D1) off the strapping pins |
| 3 | GPIO0 sensor-power gate breaks flash boot (chip enters bootloader instead of running sketch) | **High** | Low-Med | Move sensor-power gate to GPIO4 (D2); keep GPIO0 free for FLASH button; or guarantee idle-HIGH with strong pull-up |
| 4 | GitHub Contents API SHA-update churn → write conflicts and 30–60 KB heap pressure per write | Med | Med | Switch to Repository Dispatch + Actions; ESP just POSTs the sample, GitHub does the file writes |
| 5 | WiFi-spike brownouts crash the ESP whenever the pump runs | Med | Med-High | Separate 5 V supply for pump; 10 µF + 100 nF decoupling on 3V3; 470 µF bulk on USB 5 V |
| 6 | NTP not yet synced → CSV timestamps logged as 1970 | Med | High (without code guard) | Wait for `time(nullptr) > 8h` before logging; resync every few hours |
| 7 | Token leaked via flashed firmware or serial dump | Med | Low-Med | Fine-grained PAT scoped to one repo with minimal perms; store outside source control; rotate annually |
| 8 | LittleFS write amplification wears out a block in shared use | Low | Low (this workload) | Batch writes hourly; rotate daily/weekly CSV files; monitor `usedBytes` |
| 9 | MCP3008 readings noisy due to 3V3 rail noise | Low-Med | Med | RC filter on VREF; or LM4040-2.5 external reference; settle 200 ms after sensor power-up |
| 10 | Soft WDT reset from long-running loops (no `yield()`) | Med | Med | Use non-blocking patterns; never sleep more than 20 ms without `delay()` or `yield()` |
| 11 | GPIO16 CS interferes with deep sleep wake (if you ever add deep sleep) | Low | Low | You're USB-powered; skip deep sleep entirely. If added later, use Schottky diode on GPIO16→RST |
| 12 | Memory fragmentation after long uptime (weeks) from String/HTTP/JSON allocs | Low-Med | Med | `ESP.getMaxFreeBlockSize()` monitoring; `ESP.restart()` daily as a safety net; avoid `String` in loops |

---

**Net advice:** Address risks 1, 2, 3 before you flash a single byte of code. Adopt the Repository Dispatch pattern (risk 4) to side-step most of the TLS churn. Everything else is hygiene that you can pick up incrementally during development.
