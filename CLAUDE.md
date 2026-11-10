# lizard-monitor

ESPHome monitor and controller for a **crested gecko** enclosure. Forked in spirit
from `~/tank-monitor` (github.com/JoshuaSeidel/tank-monitor), the aquarium controller
that runs the user's 75 gal, nano and 15 gal tanks. This repo starts empty. The
code and the lessons below come from that project. Read the referenced files there
before rewriting something it already solved.

## What it does

| | |
|---|---|
| Animal | Crested gecko (*Correlophus ciliatus*). Arboreal; **heat kills faster than cold** |
| Board | Seeed **XIAO ESP32-S3** (same family as the 15 gal: `~/tank-monitor/boards/xiao-esp32s3-15g.yaml`) |
| Outputs | **UVB linear lamp**, **13 W heat lamp**, **LED** day light. All on relay outlets |
| Schedules | UVB + day light follow the sun (see Photoperiod) |
| Not controlled (yet) | misting, fan. Humidity is **monitored and alarmed**, not driven |

### Open questions: ask the user before building the part they affect

1. **Relay hardware.** The tank builds use CZH-Labs IoT relay outlets (D-1584TL 2-ch,
   D-1585TL 4-ch). A 4-ch gives the three outlets plus a spare for a future mister/fan.
   What do they have?
2. **Sensors.** Proposed: 2 × SHT45 (temp + RH), one **top** (under the lamp, warmest
   air) and one **bottom** (coolest, where humidity pools). Both are I2C address 0x44,
   so either use two I2C buses (the S3 has two controllers) or buy one SHT40-**BD1B**
   (0x45). Get the PTFE-membrane-capped version: this enclosure is misted daily.
3. **What the 13 W heat lamp is.** If it emits light (incandescent/halogen
   basking bulb), switching it with time-proportioning flickers it on and off over the
   day. Options are a long window with long minimum runs, or an AC phase-cut dimmer
   (ESPHome `ac_dimmer` + zero-cross module). If it is a ceramic emitter, a relay is
   fine. Confirm.
4. **Photoperiod rule.** Civil dawn→dusk at the house (lat 39.8, lon -76.7, the same
   coords the tanks use) runs ~10 h in winter and ~15.5 h in summer. Seasonal swing is
   natural for cresties, but confirm. Optionally clamp with min/max day-length settings.
5. **Home Assistant or standalone?** Default assumption: at home, so MQTT → HA (like
   the 75 gal) **and** fully functional without HA. Confirm.
6. **Enclosure size + UVB product/distance.** These affect heat-lamp authority and UVI.
   The device can't measure UVB (see Biology). The user sets lamp distance with a
   meter or the manufacturer's UVI chart.

## Biology: defaults, all user-adjustable on the device

These are **first-boot defaults** for `restore_value` number entities. They are never
constants in code (see Lessons 2).

| Setting | Default | Notes |
|---|---|---|
| Day target (top sensor) | 76 °F | crested comfort band 72–78 °F |
| Heat-lamp hard cutoff (top) | 80 °F | lamp forced off at or above this, whatever mode it's in |
| Critical hot alarm | 82 °F | sustained mid-80s is lethal. This is the alarm that matters most |
| Night | no heat | 65–72 °F is normal and healthy at night. Heat lamp is day-only |
| Cold warning / critical | 62 / 58 °F | lows in the low 60s are tolerated, not ideal |
| Humidity | not controlled | daily cycle: ~70–80 % after misting, drying to ~50–60 % |
| Wet-too-long alarm | > 85 % RH for 12 h | stagnant wet → mould, skin/respiratory problems |
| Dry-too-long alarm | < 45 % RH for 24 h | missed misting → shedding problems |

- **Failure direction is heat OFF.** On the aquarium, cold was as dangerous as hot.
  Here it isn't. Any sensor fault, clock fault, or stale reading turns the heat lamp
  off and raises an alarm. A cold gecko for a few hours is fine; a cooked one is not.
- **UVB is not measured.** No hobby I2C sensor reads UVB meaningfully (LTR390 and
  similar read UVA/UVI-ish proxies). Don't build a "UVB sensor" or a "UVB health"
  number that pretends otherwise. Track lamp **hours** instead and prompt replacement
  on the manufacturer's schedule (T5 typically 12 months). That number is real.
- **Thermal gradient.** The top/bottom difference matters more than either reading
  alone. A gradient lets the gecko choose. Show it.
- Audit every biology number with the user. Don't ship these as settled facts.

## Photoperiod

Use ESPHome's built-in **`sun`** component (lat/lon as substitutions). Drive outlets from
**sun elevation compared to user-settable elevation thresholds**:

- LED on while elevation > **−6°** (civil dawn → dusk; the tanks use the same
  convention: "civil, not sunrise/sunset").
- UVB on while elevation > a higher threshold (e.g. +10°). This gives a shorter
  window centred on solar noon.
- Heat lamp allowed only while the LED window is on.
- Each outlet has a **mode select: Auto / On / Off** (restore_value). Manual On for the
  heat lamp still obeys the hard cutoff.
- **No valid clock → LED, UVB and heat lamp off**, plus a "clock not set" alarm. The
  XIAO has no RTC; time comes from SNTP, not HA. If the internet is down at boot,
  the clock is unknown. A DS3231 is the fix if that turns out to matter.

## Lessons carried over from tank-monitor (read these)

Each one cost real time or put animals at risk. File references are in `~/tank-monitor`.

### Architecture: the user's standing requirements

1. **Control runs on the ESP32. HA is a consumer, never part of the loop.** If HA, the
   broker or Wi-Fi goes down, the enclosure keeps being regulated exactly as before.
   Every correction (calibration offsets, smoothing, unit conversion, averages) is
   computed **on the device** before publishing. The controls to change it must be
   reachable on the device's own web page, not only from HA.
2. **Nothing hard-coded.** Every tunable setting is a `restore_value` `number`/`select`/
   global. YAML substitutions are first-boot defaults only. **One quantity, one source
   of truth**: `PLATFORM-REVIEW.md` documents GH living in 11 places, which produced
   four unit bugs in one day. HA automations and dashboards read the device's
   threshold entities. They don't keep their own copies of "82 °F".
3. **Last-known settings and displayed data survive reboot and outage.** Anything a
   page or chart shows lives in flash (`restore_value`, or `tank_history`-style rings),
   not RAM. Show gaps as gaps, never fabricated data.
4. **The device prompts for recalibration on a schedule.** For SHT45s: check against a
   reference (the 75 % RH salt test, or a trusted hygrometer) every few months. Also a
   UVB lamp-age reminder.
5. **Fahrenheit only on every user surface**: web page, HA entities, logs, notifications
   and chat. Celsius is internal at most. Convert at the boundary. Grep `ESP_LOG.*degC`
   before shipping.

### Firmware reliability

6. **`mqtt: idf_send_async: true`.** Without it a publish can block the main loop for
   10 s during a reconnect. On 2026-09-30 a 4-minute router outage tripped the task
   watchdog and rebooted the 75 gal controller (`packages/base.yaml` comment). Also
   set **`reboot_timeout: 0s`** on both `wifi:` and `mqtt:`. Otherwise ESPHome reboots
   the controller every 15 min while HA/broker/Wi-Fi is down.
7. **Use `time_proportioning`, not `slow_pwm`**, for the heat lamp
   (`components/time_proportioning/`, copy it). `slow_pwm` restarted its window on
   every new demand and delivered 1.7× the commanded heat on the 75 gal. The
   credit-based version delivers exactly the commanded on-time and has min-on/min-off
   floors for relay life.
8. **Sensor-fault signatures pass CRC.** The DS18B20 returned exactly 0 °C, 85 °C, and
   *half* the real value, all with valid CRCs, and each one drove the heater the wrong
   way. Guard every input for range **and rate of change**, and treat the guard tripping
   as "no reading", not a reading. For SHT45: NaN, a frozen value (same reading for
   N minutes), and pinned 100 % RH from condensation (use the SHT4x built-in heater,
   `heater_max_duty`). If a DS18B20 is ever added: **no external pull-up**, because the
   gpio one_wire driver uses the internal one. That's settled; don't propose it.
9. **Know why it rebooted.** Copy `components/crash_report/` and the `debug:` block from
   `packages/base.yaml` (reset reason, heap free, heap max block, loop time, firmware
   build). Read **Last Crash** after any unexplained reboot.
10. **`discovery_unique_id_generator: mac` goes in the board file** (git-managed), never in
    the wrapper. Without it, same-named entities on two boards collide and HA silently
    drops one. That fix landed in a wrapper and was lost **four times** on tank-monitor.
11. **XIAO pad labels are not GPIO numbers.** `D0=1 D1=2 D2=3 D3=4 D4=5 D5=6 D6=43 D7=44
    D8=7 D9=8 D10=9`. **GPIO3 (D2) is a strapping pin, so leave it empty.** GPIO43/44 are
    free for I2C because the logger runs over native USB. Put a silkscreen→GPIO→wire-colour
    table at the top of the board file, as the 15 gal board does.
12. **Never hand-splice sensor address pins.** A hand-soldered ADDR→VCC splice on a
    BH1750 shorted the 3V3 rail twice on the 75 gal. Pick parts at the address you need.

### Repo and build conventions (copy the tank-monitor layout)

13. Layout: `boards/` (everything that names a GPIO), `packages/` (board-independent
    behaviour), `components/` (external C++ components), `*-remote.yaml` wrappers (what
    the ESPHome dashboard builds; pull packages from **GitHub main** with **`refresh: 0s`**)
    and `local-*.yaml` (same files via `!include`, for compile checks before pushing).
    Wrappers are not pulled from git, so **nothing load-critical lives in a wrapper**.
14. **`external_components` must point at `github.com/JoshuaSeidel/lizard-monitor`.**
    Components copied from tank-monitor still have its URL in their packages. Copy the
    components into this repo. Don't make this repo depend on tank-monitor at build
    time.
15. **A push isn't live until it's flashed. Verify by firmware build/config hash.** The
    wrapper builds from main *at compile start*. A push during a build isn't in the
    image. Before telling the user a change is live, read the device's `sw_version` /
    Firmware Build in HA and confirm it changed. Physical rewiring happens strictly
    **after** that confirmation (a pin swap once cost 40 minutes of rewiring against old
    firmware).
16. **Validate before pushing:** `esphome config local-<x>.yaml` then
    `esphome compile local-<x>.yaml` (ESPHome 2026.8.x is installed via Homebrew).
    `secrets.yaml` is gitignored. Ship a `secrets.yaml.example`.
17. **Push straight to main. No PRs** unless the user asks. Personal single-maintainer
    repo, and the wrappers only see main. Still check the push is a fast-forward.
18. **Don't keep hand-maintained snapshots of HA dashboards in the repo.** They drift
    (`homeassistant/aquarium-dashboard.json` did). The live dashboard is the source.
    Back it up before editing, don't mirror it.
19. **Device name is load-bearing**: mDNS, AP name, MQTT topics and HA entity ids all
    derive from it. Choose it once. Renaming later means renaming entities.
    **Deleting a device in HA does not free its entity ids.** HA restores them by
    unique_id when a board with the same MAC returns.

### Home Assistant side

20. **Load the HA best-practices skill first** (the homeassistant MCP server's
    `ha_get_skill_guide`) before touching automations or dashboards.
21. Alerts reuse the house pattern: `script.aquarium_alert` + latch in an
    `input_text` (see tank-monitor memory "aquarium alert framework"). A dropped device
    shows **unavailable, not off**. Treat unavailable as its own alarm. The 75 gal's
    heat alarm (heater not responding / cold / hot / probe / offline, critical push)
    is the model for this enclosure's overheat alarm.
22. The device decides the alarm (a binary_sensor/text_sensor with the reason). HA only
    relays it as a push notification. It doesn't re-derive thresholds.

### Working style the user expects

23. **Fix holistically, never patch.** Before changing anything, list every place it
    lives (firmware, packages, HA automations/scripts/dashboards, docs) and change them
    together, or state which you're leaving and why.
24. **Audit designs through several expert lenses before presenting them**: herpetologist /
    experienced crested keeper, exotics vet, metrologist (what does this sensor
    really measure?), embedded firmware, reliability (what happens when X dies?), UX.
    Then check the design against the five standing requirements above. Say what each
    lens changed.
25. **Check device logs before diagnosing a sensor fault.** HA history shows values, not
    causes.
26. **Report measured, not assumed.** Status means the last reading with its time.
    Predictions are labelled as predictions.
27. **Act on user-confirmed physical fixes.** If they say they reseated it, don't stall
    re-verifying.
28. **Parts lists**: Amazon entries are *searches*, not ASIN links (listings churn).
    Manufacturer links are verified and in stock.
29. **No stubs, mocks or placeholder UI** (user's global rule). If a feature can't be
    finished in a turn, say what's missing.
30. **Keep memory lean.** tank-monitor's MEMORY.md grew to 2,300+ lines and is mostly
    truncated on load. One line per entry, under ~200 chars, details in topic files,
    and update existing entries instead of adding near-duplicates.

## What to copy from ~/tank-monitor

| Copy | Why |
|---|---|
| `components/time_proportioning/` | heat-lamp relay drive (Lessons 7) |
| `components/crash_report/` | reboot forensics (Lessons 9) |
| `components/nvs_wipe/` | only if a board is reused from another build: wipes old NVS by generation number |
| `components/tank_history/` | if on-device 24 h / 30 d charts are wanted without HA. It stores int16 rings in flash and serves JSON. It needs a page that draws them |
| `packages/base.yaml` | as the template for identity/network/debug. Strip the aquarium colour bands and TDS/lux substitutions |
| `boards/xiao-esp32s3-15g.yaml` | as the template for the board file's header, pin table and "board contract" comment |

**Don't copy** `tank_controller` (2,800 lines of water-thermal-model learning; an air
enclosure with a 13 W lamp needs a gated PI loop, not that), `tank_webui` (renders a
fish tank and only shows entities named in its JS map, so new entities are invisible
until added there, a trap that already bit once). Start with ESPHome's stock
`web_server` v3 with `sorting_groups`: it renders every entity, so nothing goes
missing, and it works with HA down. Build a custom page only if the user asks.

## Heat-lamp control sketch (to be audited, not gospel)

- Gated PI on the **top** sensor toward the day target → `time_proportioning` output.
  Long period and min-on/off if the lamp emits light (open question 3).
- `allowed = clock_valid && led_window && top_sensor_fresh && top < cutoff && mode != Off`.
  Not allowed → output 0 immediately, and reset the integrator (anti-windup).
- Overheat stall detection: lamp off but top still rising past the critical line → the
  room itself is too hot. Alarm; the device can't fix it (no fan yet).
- Publish the reason the lamp is off ("night", "at cutoff", "sensor fault", "manual
  off", "no clock") as a text sensor. "Why isn't it heating" should be one read.
