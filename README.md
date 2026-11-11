# lizard-monitor

ESPHome controller and monitor for a **crested gecko** (*Correlophus ciliatus*)
enclosure: 18×18×36 in glass with a mesh top.

- **Controller.** A Seeed XIAO ESP32-S3 switches three lamps through a CZH-Labs
  4-channel relay outlet module: a **13 W light-emitting heat bulb**, a **UVB
  linear** lamp and an **LED** day light. It reads two **SHT45**
  temperature/humidity sensors, one at the top and one at the bottom.
- **Panel.** A LAFVIN ESP32-C6 with a 1.47" LCD sits beside the enclosure and
  shows the controller over BLE.
- **Standalone.** There's no Home Assistant and no MQTT. Everything (live values,
  every setting, alarms, 24 h and 30-day charts) is on the controller's own page
  at **http://lizard-monitor.local/**, with charts at **/charts**.

Forked in spirit from [tank-monitor](https://github.com/JoshuaSeidel/tank-monitor).
`CLAUDE.md` holds the design brief and the lessons carried over from that project.

## Rules the firmware is built around

1. **Heat kills faster than cold.** Any sensor fault, missing clock or stale
   reading turns the heat bulb **off** and raises an alarm. A cool gecko for a
   few hours is fine; an overheated one is not.
2. **The device decides everything.** Control, unit conversion, calibration
   offsets and alarms all run on the ESP32. Wi-Fi, the router or the internet
   can go down and the enclosure keeps being regulated exactly as before.
3. **Nothing hard-coded.** Every threshold and timing is a `restore_value`
   entity on the device page. YAML substitutions only seed them on first boot.
4. **Survives reboot.** Settings, the alarm latch, UVB lamp hours, calibration
   dates and chart history are kept in flash. A gap in the data is shown as a
   gap, never filled in.
5. **Fahrenheit everywhere.** That covers the page, the panel and the logs.

## What it does

### Photoperiod (`packages/photoperiod.yaml`)

| Lamp | On when (mode Auto) |
|---|---|
| **Day light (LED)** | Civil dawn to civil dusk (sun above −6°) at the configured location, **clamped** to 10–14 h and kept centred on solar noon. That works out to about 10 h in December and is trimmed to 14 h in June |
| **UVB** | Sun above +10°, and only inside the day-light window. About 6.5 h in December and 13 h in June |
| **Heat bulb** | Only inside the day-light window, under the heat loop below. **Never at night** |

"Day Light Window" and "UVB Window" show today's actual times.

**No clock means no light.** The XIAO has no RTC. If SNTP hasn't answered,
every lamp in Auto stays off and "clock not set" is raised.

### Lamp modes (`packages/lamps.yaml`)

Each lamp has a **Mode** of Auto / On / Off.

- **On** expires after *Manual On Expires After* (4 h by default) and returns to
  Auto, so a lamp switched on by hand doesn't run through the night.
- **Off** doesn't expire.
- Heat-bulb **On** still obeys the hard cutoff and sensor faults.

### Heat bulb (`packages/heat.yaml`)

A gated PI loop on the **top** sensor holds the *Day Target* (76 °F).

- It drives `components/time_proportioning`, whose relay on-time equals the
  commanded level. (`slow_pwm` over-delivered 1.7× on the 75 gal.)
- The bulb emits light, so every switch is visible. The cycle is therefore long:
  30 min, with at least 10 min on and 10 min off.
- At or above *Heat Cutoff* (80 °F) the bulb is forced off in every mode, until
  the top falls 1 °F below it.

**"Heat Lamp Status"** says why the bulb is doing what it does: heating N %,
at target, night, at cutoff, top sensor fault, clock not set, manual off, or
manual on.

### Sensors (`packages/sensors.yaml`, `components/sht_guard`)

Every raw reading is checked before anything acts on it. A sample is rejected
for:

- a read error,
- an out-of-range value,
- a jump faster than 5 °F/min,
- temperature and humidity both bit-identical for 30 min (a hung sensor).

A sensor with no good sample for 2 min goes to **unknown** and raises a fault.

If RH is pinned at 98 % or more for 10 min (water on the die), the SHT45's own
heater fires once. Readings are ignored for 2 min afterwards while the die cools.

**Mount the top sensor out of the bulb's beam.** In the beam it reads radiant
heat as well as air. It measures upper-zone **air**, not the basking surface;
check the surface with an IR thermometer.

### Alarms (`packages/alarms.yaml`)

The device decides every alarm. "Alarm Reason" says what's wrong, and
**"Unacknowledged Alarms"** keeps anything that fired, restored across
reboots, until **Acknowledge Alarms** is pressed. A 2 pm overheat is still news
at 9 pm.

| Alarm | Severity | Default condition |
|---|---|---|
| Too hot | **critical** | Either sensor ≥ 82 °F for 5 min. Also says if the bulb has been off ≥ 20 min, meaning the room is too hot or the relay is stuck |
| Too cold | **critical** | Either sensor ≤ 58 °F for 15 min |
| No temperature reading | **critical** | Both sensors faulted |
| Cool | warning | ≤ 62 °F for 30 min |
| Warm at cutoff | warning | Top ≥ Heat Cutoff for 30 min with the bulb off |
| Wet too long | warning | Bottom RH > 85 % for 12 h (mould risk) |
| Dry too long | warning | Bottom RH < 45 % for 24 h (missed misting) |
| Top / bottom sensor fault | warning | A top fault also means the heat bulb is off |
| Heat bulb not heating | warning | 3 relay closings in a row where the top rose < 0.3 °F within 10 min |
| Clock not set | warning | No SNTP answer. Every lamp in Auto is off |

Humidity alarms use the **bottom** sensor only. RH depends on temperature, so
the warmer top always reads drier.

### Maintenance (`packages/maintenance.yaml`)

- **UVB Lamp Hours** counts real relay-on time. *UVB Replace After* defaults to
  4,400 h (the usual 12-month T5 schedule at ~12 h a day). Press
  **UVB Lamp Replaced** when you fit a new tube.
- Nothing here measures UVB, on purpose: hobby UV sensors read UVA or a UVI
  proxy, not what an ageing T5 loses.
- **Sensor recalibration** is due every 90 days. Check against the 75 % RH
  salt test or a trusted hygrometer and thermometer, set the offsets under
  Sensor Calibration, then press **Sensors Calibrated**.

### History (`components/enclosure_history`)

The controller keeps 24 h at 5-min resolution and 30 days at 1-hour resolution
of top/bottom temperature, top/bottom RH, heat output and lamp on-time. It's
stored in flash and drawn at **http://lizard-monitor.local/charts**.

### Panel (`packages/panel.yaml`)

The panel is read-only, and colours come from the controller's own thresholds.

- **Banner:** red for a critical alarm, amber for a warning, "ALL OK" in green,
  or an amber "earlier: …" for an alarm that hasn't been acknowledged.
- **Two halves:** TOP and BOTTOM temperature with RH. Red means ≥ critical,
  amber ≥ cutoff, green is comfortable, light blue ≤ cool warning, blue ≤ cold
  critical.
- **Row:** the gradient (top − bottom) and what the heat bulb is doing.
- **Footer:** which lamps are on, maintenance due, and how old the data is.
- **No frame for 45 s:** every number becomes "--" under a red **NO DATA**
  banner.
- **Dark at night:** outside the day window the backlight goes to *Night
  Brightness* (off by default), unless there's an active alarm or the link is
  down. Press **BOOT** to wake it.

## Parts

| Part | Qty | Notes | Where |
|---|---|---|---|
| Seeed XIAO ESP32-S3 | 1 | Controller. **In stock**, $7.49 (checked 2026-11-11) | [Seeed](https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html) |
| CZH-Labs 4-channel IoT relay outlet module | 1 | Skip if you already have the D-1585TL. 10 A per outlet, 15 A total, optocoupled 3–120 V DC inputs (a GPIO drives them directly). Sold now as **D-1703TL**. **In stock**, $93.60 (checked 2026-11-10). **Set every channel's DIP switch to "Normally OFF"** | [CZH-Labs](https://czh-labs.com/products/ac-power-4-channel-iot-relay-outlet-module-din-rail-or-screw-mounting-support-normally-off-normally-on-always-off-always-on) · [Amazon search](https://www.amazon.com/s?k=CZH-Labs+4+channel+IoT+relay+outlet+module) |
| Adafruit SHT45 **with PTFE membrane** (product 6174) | 2 | IP67 membrane, needed because the enclosure is misted daily. Both at 0x44, one per I2C bus. $13.50. **Out of stock at Adafruit on 2026-11-11**: use "notify me" there, or a distributor. The non-PTFE version (5665) is in stock but has no membrane, so mist reaches the die | [Adafruit](https://www.adafruit.com/product/6174) · [Amazon search](https://www.amazon.com/s?k=Adafruit+SHT45+PTFE) |
| SparkFun Qwiic Adapter | 2 | JST sockets plus 0.1" holes. Solder one per bus to the XIAO pads with short wires, inside the dry controller box, so nothing is soldered in the enclosure. $1.60, **in stock** | [SparkFun](https://www.sparkfun.com/products/14495) |
| SparkFun Flexible Qwiic Cable, 500 mm | 2 | Adapter → sensor. Silicone, flexible. $2.75, **in stock**. Measure first: if the controller box is far from the enclosure, the bottom sensor may need longer (I2C at 100 kHz is fine to ~1 m with the breakouts' pull-ups) | [SparkFun](https://www.sparkfun.com/products/14429) |
| Hookup wire, 22–26 AWG, a few colours | — | XIAO → Qwiic adapters, and XIAO → relay IN+ ×3 / IN− | — |
| USB-C 5 V supply + cable | 2 | One for each board | — |
| LAFVIN ESP32-C6 1.47" LCD board | — | Panel. Already owned | — |

## Wiring (controller)

**Pad labels aren't GPIO numbers.** Wire by the silkscreen column. The
authoritative table is the header of `boards/xiao-esp32s3-lizard.yaml`.

| Signal | XIAO pad | GPIO | Goes to | Wire |
|---|---|---|---|---|
| I2C bus A SDA | D6 | 43 | **Top** SHT45 SDA | white |
| I2C bus A SCL | D7 | 44 | **Top** SHT45 SCL | brown |
| I2C bus B SDA | D4 | 5 | **Bottom** SHT45 SDA | green |
| I2C bus B SCL | D5 | 6 | **Bottom** SHT45 SCL | yellow |
| Relay CH1 IN+ | D8 | 7 | **Heat bulb** outlet | purple |
| Relay CH2 IN+ | D9 | 8 | **UVB** outlet | blue |
| Relay CH3 IN+ | D10 | 9 | **LED** outlet | orange |
| 3V3 | 3V3 | — | Both SHT45 VIN | red |
| GND | GND | — | Both SHT45 GND, relay IN− ×3 | black |

- **D2 (GPIO3) stays empty.** It's a strapping pin.
- D0, D1 and D3 are spare. Relay CH4 is the spare outlet for a future mister or
  fan; it's not wired and not configured.
- **Both SHT45s are 0x44**, which is why there are two buses. Don't modify an
  address pin.
- **Relay DIP switches: every channel to "Normally OFF".** "Normally ON" inverts
  a channel, and a dead controller would then leave the heat bulb on.
- **Not sure which wire is which outlet?** Set `relay_test: "true"` in the
  wrapper, **unplug the lamps**, and flash. Each relay then clicks its own
  number of times: 1 heat, 2 UVB, 3 LED. Set it back to "false" and reflash.

## Building and flashing

ESPHome 2026.8.x. Secrets:

```sh
cp secrets.yaml.example secrets.yaml   # fill in; gitignored
```

**Check before pushing.** The `local-*.yaml` wrappers build this checkout,
components included:

```sh
esphome config  local-lizard.yaml       && esphome compile local-lizard.yaml
esphome config  local-lizard-panel.yaml && esphome compile local-lizard-panel.yaml
```

**Deploying.** The ESPHome dashboard builds `lizard-monitor-remote.yaml` and
`lizard-panel-remote.yaml`. These pull every package and component from GitHub
**main** at compile start, with `refresh: 0s`. A push made during a build isn't
in that image. Before believing a change is live, compare **Firmware Build** on
the device page.

### First bring-up

1. Set the relay DIP switches to Normally OFF. Leave the lamps **unplugged**.
2. Flash the controller (`lizard-monitor-remote.yaml`, first time over USB).
   Open http://lizard-monitor.local/ and check:
   - **Top Sensor** and **Bottom Sensor** read `ok`.
   - **Clock** shows the time.
   - **Day Light Window** shows today's times.
3. Optional: run the relay test above to confirm the outlets.
4. Copy **Controller BLE MAC** (Panel group) into `lizard-panel-remote.yaml` as
   `controller_ble_mac` (uncomment it). The panel build **fails until you do**,
   on purpose: a wrong MAC compiles and then silently never connects.
5. Flash the panel. "Controller Link" should go on within about 30 s.
6. Plug in the lamps. Set **UVB Lamp Replaced** for a new tube, and check the
   SHT45s against a reference, then press **Sensors Calibrated**.

## Defaults to audit

These are first-boot defaults, all adjustable on the device page. **They still
need your audit; they aren't settled facts.**

| Setting | Default | Where |
|---|---|---|
| Day Target (top) | 76 °F | Heat Lamp Control |
| Heat Cutoff (top) | 80 °F, re-allowed below 79 °F | Heat Lamp Control |
| Critical Hot | 82 °F for 5 min | Alarm Thresholds |
| Cold Warning / Cold Critical | 62 °F for 30 min / 58 °F for 15 min | Alarm Thresholds |
| Wet / Dry | > 85 % for 12 h / < 45 % for 24 h (bottom sensor) | Alarm Thresholds |
| Night | No heat, no light | Photoperiod |
| Day length clamp | 10–14 h | Photoperiod |
| UVB | Sun above +10° | Photoperiod |
| Heat cycle | 30 min, 10 min minimum on and off | Heat Lamp Control |
| PI gains | 25 %/°F, integral time 60 min | Heat Lamp Control |
| Not-heating check | 0.3 °F within 10 min, 3 misses | Alarm Thresholds |
| UVB replace | 4,400 h | Maintenance |
| Sensor recalibration | 90 days | Maintenance |
| Manual On expiry | 4 h | Lamps |

The sensor-guard limits (2 min stale, 5 °F/min, 30 min frozen, condensation
heater at 98 % RH) describe the SHT45, not the animal. They're component options
in `packages/sensors.yaml` rather than page entities.

## When something fails

| Fails | What happens |
|---|---|
| Wi-Fi / router / internet | Nothing changes. Control is local and the panel uses BLE |
| Internet down **at boot** | No clock: every lamp in Auto stays off and "clock not set" is raised. A DS3231 RTC is the fix if this happens |
| One sensor | Its readings go unknown and an alarm is raised. If it's the top sensor, the heat bulb goes off |
| Both sensors | Critical "no temperature reading", heat bulb off |
| Heat bulb burns out | "Heat bulb not heating" after 3 failed checks |
| Relay welded closed | Can't be sensed electrically. "Too hot … bulb off 20+ min: room too hot or relay stuck" covers it |
| Controller dies | Panel shows **NO DATA** within 45 s. With the DIP switches at Normally OFF, all outlets drop |
| Panel dies | Controller unaffected |
| Unexplained reboot | Read **Reset Reason** and **Last Crash** on the device page |

## Layout

| Path | Contents |
|---|---|
| `boards/xiao-esp32s3-lizard.yaml` | Controller pins (the only file naming GPIOs) and the relay test |
| `boards/lafvin-esp32c6-panel.yaml` | Panel display, backlight and BOOT button |
| `packages/base.yaml` | Identity, Wi-Fi, SNTP, OTA, web page groups, diagnostics (both boards) |
| `packages/sensors.yaml` | SHT45s, guards, offsets, gradient |
| `packages/photoperiod.yaml` | Sun, day and UVB windows, clock |
| `packages/lamps.yaml` | Modes, manual expiry, UVB/LED relays |
| `packages/heat.yaml` | Heat loop and status |
| `packages/alarms.yaml` | Alarm thresholds, decisions, latch |
| `packages/maintenance.yaml` | UVB hours, recalibration reminder |
| `packages/history.yaml` | 24 h / 30 d charts |
| `packages/ble_link.yaml` | Controller → panel frame (v1, documented there) |
| `packages/panel.yaml` | Panel BLE client, display, backlight |
| `components/` | `time_proportioning`, `crash_report`, `nvs_wipe` (from tank-monitor); `enclosure_history` (adapted from `tank_history`); `sht_guard` (new) |
