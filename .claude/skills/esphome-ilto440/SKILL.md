---
name: esphome-ilto440
description: Working on esphome_ilto440, the ESPHome component and package that read and control ILTO / Swegon CASA ventilation units through the Premium panel's RS-485 bus - the ilto_premium C++ component, packages/ilto_premium.yaml, the examples, the protocol notes and the wiring. Use it whenever a task changes, reviews, tests or documents anything in this repository, decodes a new bus message, or helps someone install or debug the bridge.
---

# esphome_ilto440

An ESP8266 with a MAX485 module sits on the RS-485 bus between an ILTO / Swegon CASA ventilation
unit and its Premium panel, reads everything the panel polls, and sends the panel's own commands.
It was reverse-engineered on two units (ILTO 440 econo with a 2008 Premium panel, ILTO layout; ILTO 440
Premium, CASA layout, ESP32 + MAX3485); the protocol
was written down from captures, not from documentation.

## Layout

| Path | What |
|---|---|
| `components/ilto_premium/ilto_premium.h` | The whole component (header-only): framing, CRC, decoding, the send queue. Its header comment is the protocol reference kept next to the code |
| `components/ilto_premium/__init__.py` | Schema: every sensor optional; `flow_control_pin`, `tx_uart_id` |
| `packages/ilto_premium.yaml` | What users include: uart, every entity, the switches, clock sync, pins as substitutions, `ilto_components` (component source) |
| `examples/*.yaml` | Minimal (the tested unit) and custom pins (DI and DE/RE on other GPIOs) |
| `docs/protocol.md`, `docs/wiring.svg` | Protocol and wiring for humans |

Keep `docs/protocol.md`, the header comment and the README's entity table in step when the
protocol knowledge or the entities change.

## Protocol essentials

- 38400 8N1. Panel = 4A, unit = 8A. The panel polls types 0x71 (temperatures) and 0x73 (status)
  about once a second; otherwise the bus is silent until a setting is confirmed on the panel.
- Wire format: `<n> CC 64 a b c d e LEN type data... crc_hi crc_lo`. `<n>` = bytes from CC to the
  end of the CRC, sent before each frame. Header `4A 00 FD 00 FD` from the panel,
  `8A 00 00 00 00` from the unit. LEN = bytes from type to the end of the data.
- CRC16-CCITT, poly 0x1021, init 0xFFFF, over `64` .. last data byte, big-endian. Check any
  hand-built frame against a captured one: speed 3 is
  `14 CC 64 4A 00 FD 00 FD 0A 20 00 0C 01 00 01 3C 01 01 00 33 85`.
- Commands: type 0x20 (speed `0C`, off `10`, on `20`, 3-step mode `05`), 0x30 fireplace, 0x54
  clock (Monday = 0). The third data byte selects which fields the unit takes. The unit acks with
  a LEN-3 frame echoing type and that byte. Details and the unknowns: `docs/protocol.md`.
- Undecoded: types 0x32 / 0x36 (service-menu settings) and several 0x73 bytes. The openHAB-derived
  status bits are unconfirmed on an ILTO - say so, don't present them as verified.
- **A second layout, "CASA"**, was captured on an ILTO 440 Premium: `CC 64 <flags> <dst> <src> LEN
  type <00|01> A0 data crc`, no `<n>` prefix, same CRC and 0x71 / 0x73 payloads (0x71 has 16 data
  bytes there). The unit (0A) is the bus master: it broadcasts (FD) 0x73, 0x71 and one rotating type
  per ~0.6 s round and polls the panel (01), which answers `A0 00 00 00`. `feed_()` tries both
  layouts on every frame and the CRC decides; the first good frame sets `layout_`.
- **CASA commands**: the panel sends `45 0A 01 ... 20 00 81 <step> 00 00 28 00 00 00 <min>` in place
  of its empty answer to the poll; the unit acks `85 01 0A ... 20 00 81 01 <step> <fan %>`. Step 00
  Away, 01 Home, 02 Boost; min 1E/3C/78 or 00 (no limit). Status 0x73: p[14] step, p[5] the boost's length (does not count down; `boost_time` is the ESP's own countdown from the boost's start, unknown after a restart mid-boost),
  p[19] 0x20 during a timed boost (so "off" is `p[19] & 0x01` only). The component sends as the
  panel (01) 5-70 ms after the panel's empty answer (`casa_slot_*`), once per round, 3 tries.
  Off / on: the same 11 bytes with select 10 / 20 (acks `20 00 10 01` / `20 00 20 01`); `set_power()`
  builds them from `step_command_` and refuses while the layout is unknown. 0x73 after off: p[19] 01,
  p[20] 03/04; after on p[20] 05 -> 07, p[4] comes back as 01 (not 21), so on CASA the fan speed is
  always unknown.
  5-step screen: speed `20 00 08 01 00 01 <speed x 20 %> 00 00 00 00` (`casa_speed_command_`), screen
  switch `20 00 04 01 00 <00|01> 28 ...`; 0x73 p[15] = 01 with the 5-step screen (`casa_five_step_`),
  speed = p[12] / 20. Speed commands only with the 5-step screen, 3-step commands only without.
  `basic_display` text sensor shows the screen. The screen switch command is known but **left out
  on purpose** (owner's decision): the screen stays the panel's choice. Don't add it as a control.
  Fireplace: the ILTO's `30 00 10 00 00 00 00 <02|01>` unchanged, acked `30 00 10 01`; 0x73 p[1] = 01,
  p[4] = 00, fans 60 / 20 % meanwhile. Not sent while the unit is off (never captured).
  Clock: the ILTO's `54 00 0E` / `54 00 01` commands unchanged (Monday = 0), acked `54 00 0E 01` /
  `54 00 01 01`; type 0x55 broadcasts `<weekday> <h> <min> <s>` (the `unit_clock` text sensor).
  `supported_()` says which commands have a known form per layout; anything else is dropped with a
  warning, queued ones too. An ILTO-layout frame sent onto the CASA bus only collided with the
  unit's traffic - never send a frame whose form was not captured on that layout. To add a command:
  capture it with the filtered `uart_debug` lambda (panel frames only), step by step, from a known
  state and back.
- **Service and installation settings are left out on purpose** (owner's decision): weekly timer,
  filter guard, summer cooling, heating, actuators, fan speed per mode and the rest of the
  installer menus. A wrong value can stop a unit from working, so they stay on the panel. Don't
  add them as entities; the list and the reason are in `docs/protocol.md` ("Settings not decoded,
  on purpose").

## Rules (each learned from a real failure)

- **Nothing may send a command at boot.** The Ventilation select (`IltoModeSelect`: Off, 1-5,
  Fireplace; the package uses only this) and the optional `ventilation` / `fireplace_mode`
  switches (`IltoSwitch`) are the component's own entities: no setup, so nothing runs at boot,
  and the switch schema rejects any restore_mode but DISABLED. As template switches with ESPHome's default restore they ran
  `turn_off_action` at boot and switched the unit off on every ESP restart. Any new entity that
  sends a command must be built the same way.
- **`delta` filters on whole-number values use 0.5.** `delta: 1` drops changes of exactly 1, so
  the fan speed sensor stuck.
- **The component publishes every reading (~2 s); rate-limiting belongs in the package's
  filters**, so `throttle_average` sees every sample.
- `set_mode()` sends only the needed steps: "on" before a speed or fireplace if the unit is off,
  "fireplace off" before a speed (whether a speed command alone ends fireplace mode was never
  tested). `running_` / `fireplace_` take the target when a command is queued and a status does
  not overwrite them while such a command waits, so quick successive choices build on each other.
- `set_fan_speed()` sends nothing when the last 0x73 status already shows that speed and no speed
  command is waiting. Any type 0x20 frame (panel command or ack) and the 3-step layout make the
  speed unknown until the next status, so a change made in between is never skipped.
- `set_fan_speed()` resends the panel's last-seen `20 00 0C` frame with only the speed byte
  changed - the other fields differ between captures and their meaning is unknown. Don't
  replace them with constants.
- Package list items are not merged by id: to change an item from the package use
  `- id: !extend <id>`; to drop a key use `!remove`.
- Pins: RX must be GPIO13 (swapped hardware UART0). TX is a software UART because GPIO15 must be
  low at boot and the module pulls DI high; GPIO2 lights the LED. The logger goes to UART1.
  ESPHome's ESP8266 `uart.load_settings()` restarts Serial without `swap()` - call
  `Serial.swap()` after it if the baud is ever changed at runtime.
- The ESP8266 allows 4 API connections. Log sessions left running (e.g. a killed client whose
  `esphome logs` process lives on in a container) lock Home Assistant out.

## Debugging the bridge

The component logs `bytes N, frames ok M, bad crc K, last bytes: ...` every 10 s.

- `bytes 0`: no signal. First check the MAX485 has 5 V (VIN on a USB-powered NodeMCU does not),
  GND is shared, and A/B are on the middle pair (~1.7 V apart at idle). An unpowered module only
  passes ~6 µs spikes from the fan relays, which are easy to mistake for fast data.
- `bytes` growing, `frames ok 0`: polarity - flip `ilto_bus_inverted`. With an automatic-direction
  module (no DE/RE pins; TX low drives the bus) `ilto_bus_inverted` must stay "false" or the module
  holds the bus; swap A/B instead. If the last bytes already look like data (temperatures), the
  polarity is right and the frame layout is new: get whole frames with the `uart: - id: !extend
  ilto_bus_rx` debug snippet in the README.
- "No acknowledgement ... giving up": DE/RE or DI not wired, or the bus was busy three times.

## Decoding a new message

1. Run a DEBUG log session (one only) and save the whole log.
2. Have the person change exactly one thing on the panel, wait ~10 s, change it back.
3. The component logs `new type XX: CC ...` / `changed type XX: ...` for every frame whose content
   is new for its header and type; compare the two frames and the following 0x73 status.
4. The person cannot type while at the panel: start the capture before they walk over and give
   them generous time.
5. Add the command to the header comment, `docs/protocol.md`, a component method, and only then
   an entity.

## Testing a change

- Compile every example against the working tree before committing. The examples load the
  package from GitHub, so in a temporary copy replace their `github://.../packages/ilto_premium.yaml@...`
  package reference with `!include esphome_ilto440/packages/ilto_premium.yaml`, set the substitution
  `ilto_components: esphome_ilto440/components`, add dummy `wifi_ssid`, `wifi_password`,
  `api_encryption_key` and `ota_password` secrets, and run `esphome compile` on each.
- Framing and decoding can also be checked without ESPHome: compile `ilto_premium.h` on the host
  against small stand-in headers for the `esphome/...` includes (`#define protected public` before
  including it), push captured bytes into the UART stub and call `loop()`. Run a capture of each
  layout through it and compare the published values with an unmodified build.
- On hardware: after flashing, confirm the unit is still running (Ventilation on, fan power
  unchanged), then test a command with a short round trip (speed +1 and back) and look for
  "Unit acknowledged" in the log. Leave the unit as it was.

## Writing style for this repo

User-facing text is English (the panel and manuals are Finnish; translate: Kotona = Home,
Poissa = Away, Tehostus = Boost, Takka = Fireplace, Anturi = sensor). Keep the "use at your own
risk / written by Claude" disclaimer in the README, the package and the component header.
The project is 0BSD (`LICENSE`); new source files start with the same SPDX and copyright lines
as the existing ones.
