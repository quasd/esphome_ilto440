# ILTO / Swegon CASA Premium panel bus

What is known about the bus between an ILTO (Swegon CASA) ventilation unit and its Premium
control panel, reverse-engineered on an ILTO 440 econo with a Premium panel from 2008 by sniffing
the bus while changing settings on the panel. Other units and panel versions may differ: a second
unit used a different frame header with the unit as bus master (see
[Second bus layout](#second-bus-layout-casa-short-header)). The component recognises
both layouts by their CRC.

Starting points were openHAB 1's `swegonventilation` binding (`swegongw.c`,
`SwegonVentilationDataParser.java`), which only reads the bus, and the unit's installation
manuals. Where this document says "openHAB", the meaning comes from there and has not been
confirmed on an ILTO.

## Physical layer

- Modular 4P4C (RJ10) cable between the panel and the unit; the panel has a second socket for
  chaining. Outer pins: about 5 V and GND from the unit. Middle pair: RS-485.
- Idle, the pair is driven about 1.7 V apart (3.2 V and 1.5 V to GND on the tested cable).
- **38400 baud, 8 data bits, no parity, 1 stop bit.** One bit is 26 µs.
- Half duplex. The panel is the only master.

## Traffic pattern

- The panel (address **4A**) polls the unit (address **8A**) for two messages, temperatures (type
  0x71) and status (0x73), about once a second, alternating; a request/answer pair every ~2 s.
- Nothing else is sent until a setting is confirmed on the panel. The panel then sends one
  command frame and the unit acknowledges it.
- There is about a second of silence between polls, which is where a third device can send.

## Frame format

```
<n> CC 64 a b c d e LEN type data... crc_hi crc_lo
```

| Field | Meaning |
|---|---|
| `<n>` | Length of the frame that follows, counted from `CC` to the end of the CRC. Sent before every frame |
| `CC` | Start of frame |
| `64` | Always 0x64 (first byte of the CRC'd part) |
| `a b c d e` | Header. From the panel: `4A 00 FD 00 FD`; from the unit: `8A 00 00 00 00` |
| `LEN` | Number of bytes from `type` to the end of the data |
| `type` | Message type |
| `crc` | CRC16-CCITT: polynomial 0x1021, initial value 0xFFFF, no reflection, over `64` .. the last data byte, sent high byte first |

The header is two bytes longer than openHAB's documented `CC 64 D S CMD LEN`.

Polls:

```
0D  CC 64 4A 00 FD 00 FD 03 71 00 FF 06 F0                       panel asks for 0x71
1A  CC 64 8A 00 00 00 00 10 71 00 FF <13 data bytes> crc crc      unit answers
0D  CC 64 4A 00 FD 00 FD 03 73 00 FF 68 90                       panel asks for 0x73
22  CC 64 8A 00 00 00 00 18 73 00 FF <21 data bytes> crc crc      unit answers
```

In the answers, the payload starts after `type 00 FF`; `p[0]` below is the first byte after
`FF`.

### Building a frame (Python)

```python
def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc

def command(data):  # data = type onwards
    body = bytes([0x64, 0x4A, 0x00, 0xFD, 0x00, 0xFD, len(data)]) + bytes(data)
    c = crc16(body)
    frame = b"\xCC" + body + bytes([c >> 8, c & 0xFF])
    return bytes([len(frame)]) + frame

command([0x20, 0x00, 0x0C, 0x01, 0x00, 0x01, 0x3C, 0x01, 0x01, 0x00])
# 14 CC 64 4A 00 FD 00 FD 0A 20 00 0C 01 00 01 3C 01 01 00 33 85  (speed 3, as the panel sends it)
```

## Commands (panel → unit)

The unit acknowledges each command with `CC 64 8A 00 00 00 00 03 <type> 00 <select> crc`, where
`<select>` echoes the command's third byte. In type 0x20 and 0x54 commands that byte selects
which of the following fields the unit takes; the panel fills the others with current values.

| Data (from `type`) | Meaning |
|---|---|
| `20 00 0C 01 00 01 <s> 01 01 00` | Fan speed. `<s>` = speed × 20 %: 14, 28, 3C, 50, 64 for speeds 1-5 |
| `20 00 10 <same fields>` | Switch the unit off (fans stop) |
| `20 00 20 <same fields>` | Switch it on again; it resumes the speed it had (the speed byte is ignored) |
| `20 00 05 <m> 00 00 3C 00 00 00` | Sent instead of the speed command when the panel's basic display is the 3-step Away / Home / Boost layout: `<m>` 00 Away, 01 Home, 02 Boost. The unit maps each step to one of the 5 speeds (on the tested unit: 20 %, 60 %, 100 %) |
| `30 00 10 00 00 00 00 02` / `... 01` | Fireplace mode on / off (supply 100 %, extract 20 % for ~10 min) |
| `54 00 0E <wd> <h> <min> <s>` | Set the clock. `<wd>` weekday with **Monday = 0**, then hour, minute, second |
| `54 00 01 <wd> <h> <min> <s>` | Set the weekday (the panel sends this after the time command) |
| `32 01 02 0A 02 00 00 00 01 01 00` | Seen when a service-menu setting was saved. Not decoded |
| `36 01 01 02 3C 02 02 0F 32 00 01`, `36 01 02 ...` | Seen when service-menu functions were changed; 0x3C = 60 matched the filter guard's 60 min delay. Not decoded |

The fields after `20 00 0C` changed between captures (`01 00 01 <s> 01 01 00` and
`01 00 01 <s> 00 00 00`); the component therefore resends the panel's last-seen speed command
with only the speed byte changed.

## Temperatures and efficiency (0x71, 13 bytes)

Checked against the panel's service screens on the tested unit.

| Byte | Meaning |
|---|---|
| p[0]..p[7] | Panel's temperature sensors 1-8, signed °C. openHAB names p0-p3 outdoor, supply, extract, supply after reheater and p7 exhaust, but on the tested unit sensor 1 read 26 °C in November, so the mapping depends on the unit. Unconnected sensors read 111 |
| p[8] | CO2 (openHAB; 0 on the tested unit, scale unchecked) |
| p[9] | Relative humidity (openHAB; 0 on the tested unit) |
| p[10], p[11] | Supply / extract fan rpm ÷ 10 (openHAB; the econo shows 0 rpm, p[10] occasionally read 1-22) |
| p[12] | Heat recovery efficiency, % |

## Status (0x73, 21 bytes)

| Byte | Meaning |
|---|---|
| p[0] | 0x40 constant on the tested unit. openHAB: 0x01 heating, 0x02 cooling, 0x04 bypass, 0x08 freeze protection, 0x10 preheating, 0x20 chilling, 0x40 preheater overheat, 0x80 reheating |
| p[0] (CASA) | C1 running with the electric after-heater heating (the panel's LED blinks orange), 40 with after-heating switched off on the panel, C0 right after switching it back on, 41 while the unit is off. So 0x80 = after-heating in use, 0x80 + 0x01 = heating (confirmed: the heater stopping by itself on its outdoor temperature limit cleared 0x01, LED green) |
| p[1] | 0x01 fireplace mode (confirmed). openHAB: 0x02 underpressure compensation, 0x04 external boost, 0x08 humidity boost, 0x10 CO2 boost, 0x20 defrosting |
| p[2] | 0x04 while the fans run, 0x00 when stopped |
| p[3] | openHAB: 0x04 external boost function, 0x08 external fireplace, 0x10 filter guard, 0x20 IR freeze protection, 0x80 emergency stop |
| p[4] | 0x01 normally, 0x21 with the 3-step display, 0x00 for about a minute after fireplace mode or off/on. During that start-up phase the unit stores a new speed at once (p[17]) but the fans keep the start speed until it ends (seen: ~40 s) |
| p[7] | openHAB: alarms - 0x08 supply fan failure, 0x10 extract fan failure, ... |
| p[8] | 0x00 / 0x40 / 0xC0, changes around off/on. Not the after-heater: it stayed 40 when after-heating was switched off and on. 0x80 only while switched off. openHAB: 0x02 filter guard alarm, 0x04 service reminder |
| p[9], p[14], p[15], p[18] | Follow the display layout and the current Away / Home / Boost step |
| p[12], p[13] | Supply / extract fan power, % |
| p[16] | 0x10 constant |
| p[17] | Selected speed × 20 % |
| p[19] | 1 while the unit is switched off |
| p[20] | 0x07 running; 0x03 then 0x04 while switching off |

openHAB also documents a type 0x21 (operating mode, unit state, fan speed); the tested unit never
sent it.

## Second bus layout: CASA short header

Captured on a second unit, an ILTO 440 Premium, read through an ESP32 and a MAX3485 module.
The physical layer is the same (38400 8N1, same idle voltages on the middle pair), and so are the
`CC 64` start, the CRC and the 0x71 / 0x73 payloads. What differs is the header, and who runs
the bus: **here the unit is the master**.

```
CC 64 <flags> <dst> <src> LEN type <00|01> A0 data... crc_hi crc_lo
```

| Field | Meaning |
|---|---|
| `flags` | 85 on data frames and answers, 45 on polls (guessed from the traffic; not decoded) |
| `dst`, `src` | Addresses. Unit **0A**, panel **01**, FD = everyone |
| `LEN` | Bytes from `type` to the end of the data, as in the ILTO layout |
| `<00\|01> A0` | Where the ILTO layout has `00 FF`. The first byte flips between 00 and 01 for the same type; meaning unknown |
| `crc` | The same CRC16-CCITT, over `64` .. the last data byte |

There is no length byte before `CC`. 226 of 227 captured frames passed the CRC; the one that
failed was hit by a frame sent in the ILTO layout at the same moment.

One round, repeated about every 0.6 s:

```
CC 64 85 FD 0A 18 73 00 A0 <21 data bytes> crc crc     unit to all: status (0x73)
CC 64 85 FD 0A 13 71 00 A0 <16 data bytes> crc crc     unit to all: temperatures (0x71)
CC 64 85 FD 0A LEN <t> <00|01> A0 ... crc crc          unit to all: one other type per round
CC 64 45 FD 0A 05 AA 00 00 02 0A crc crc               unit to all: unknown (looking for device 02?)
CC 64 45 01 0A 03 A0 00 00 crc crc                     unit polls the panel
CC 64 85 0A 01 04 A0 00 00 00 crc crc                  panel answers: nothing to report
```

The "other type" rotates through about 35 values (0x21-0x3D, 0x45-0x55, 0x61-0x6F, 0x79, 0x81-0x89,
0x97), so the unit appears to broadcast its whole configuration over ~20 s. None of them is decoded.

Payloads, checked on that unit against its panel's sensor screens:

- **0x71**: 16 data bytes. p[0]..p[7] are sensors 1-8 as in the table above (9, 13, 19, 17, 14,
  17, 110, 20 °C in the capture; an unconnected sensor read 110, not 111). p[12] was 40 with
  sensors 1-3 at 9 / 13 / 19 °C, exactly (13 - 9) / (19 - 9), so on this unit sensor 1 is outdoor,
  2 supply after the heat exchanger and 3 extract, as openHAB names them. p[13]..p[15] were 0.
- **0x73**: 21 data bytes, `C1 00 14 00 21 1E 0A 00 00 00 4A 00 3C 3C 02 00 00 00 00 20 07` during
  a 30 min boost. p[12] / p[13] fan power % and p[20] 07 (running) fit the ILTO meanings. p[4] =
  0x21 is the 3-step display's value on the ILTO, and p[16] / p[17] were 0 rather than 0x10 / speed;
  whether p[17] carries the speed with the 5-step display was not checked. New here:

  | Byte | Meaning |
  |---|---|
  | p[5] | The boost's length: 1E / 3C / 78 during a 30 / 60 / 120 min boost, 0 otherwise. It stays at the length for the whole boost (checked over 30 min); the time left is not reported |
  | p[10] | Wanders between 0x40 and 0x59 whatever the mode (unknown) |
  | p[14] | The 3-step choice: 00 Away, 01 Home, 02 Boost |
  | p[19] | 0x20 during a timed boost, 00 otherwise (also with a boost without a time limit). On the ILTO p[19] = 1 meant switched off, so only bit 0 is read as "off" |

### Commands on the CASA layout

Captured with the panel's 3-step screen (Away / Home / Boost 30 min / 60 min / 120 min / Boost),
going through each choice from Home and back. The panel does not wait for a pause: it sends its
command **in place of its empty answer** to the unit's poll, and the unit acks within ~20 ms:

```
CC 64 45 01 0A 03 A0 00 00 32 EE                                    unit polls the panel
CC 64 45 0A 01 0B 20 00 81 <step> 00 00 28 00 00 00 <min> crc crc   panel: 3-step command
CC 64 85 01 0A 06 20 00 81 01 <step> <fan %> crc crc                unit acks to the panel
```

| Choice | `<step>` | `<min>` | Ack `<fan %>` (this unit) |
|---|---|---|---|
| Away | 00 | 00 | 14 (20 %) |
| Home | 01 | 00 | 28 (40 %) |
| Boost 30 / 60 / 120 min | 02 | 1E / 3C / 78 | 3C (60 %) |
| Boost (no time limit) | 02 | 00 | 3C |

Switching the unit off and on again from the panel's main menu sent the same 11 bytes with a
different third byte - as on the ILTO layout, where off / on is the speed command with 10 / 20:

```
CC 64 45 0A 01 0B 20 00 10 01 00 00 28 00 00 00 00 CD 90    panel: off
CC 64 85 01 0A 04 20 00 10 01 5A FE                          unit acks
CC 64 45 0A 01 0B 20 00 20 01 00 00 28 00 00 00 00 39 92    panel: on
CC 64 85 01 0A 04 20 00 20 01 5F 6B                          unit acks
```

After off, 0x73 showed p[19] = 01, p[20] 03 then 04, fans 0 %, p[0] C1 -> 41, p[2] 14 -> 10, p[4]
21 -> 00, p[8] 00 -> C0. After on: p[19] 00, p[20] 05 then 07 within a second, fans back to 40 %
at once, and about a minute later (when the panel stopped showing "starting") p[8] C0 -> 40, p[0]
41 -> C1, p[4] 00 -> 01. p[4] stayed 01 rather than going back to 21, so on this unit p[4] does not
tell the 3-step screen apart.

The panel sends one command for every choice it passes on its list: Boost 120 min from Home sent
30, 60 and 120 min in turn, each acked. The `28` byte was the same in every frame (meaning
unknown; the component keeps the panel's last-seen frame and changes only `<step>` and `<min>`).

The component sends the same frame, with the panel's address, **in the pause right after the
panel's empty answer** (`CC 64 85 0A 01 04 A0 00 00 00`): the next frame starts ~100 ms later, and
the 18 bytes take under 5 ms. It sends 5-70 ms after that answer, at most once per round, and tries
three rounds before giving up. Confirmed on the unit: Away and Home sent this way were acked
within ~10 ms on the first try, the fans changed (20 % / 40 %) and the status followed.

### Electric after-heating on the CASA layout

Switching after-heating off and back on in the panel's main menu, with the heater heating (LED
blinking orange):

```
CC 64 45 0A 01 0E 4C 00 02 00 AA 01 00 02 01 FF FF EC 00 2D ..   panel: after-heating off
CC 64 85 01 0A 04 4C 00 02 01 ..                                  unit acks
CC 64 45 0A 01 0E 4C 00 07 00 AA 03 00 02 01 FF FF EC 00 2D ..   panel: after-heating on
CC 64 85 01 0A 04 4C 00 07 01 ..                                  unit acks
```

The unit's broadcast type 0x4D carries the same settings (`00 AA <03 on | 01 off> 00 02 01 FF FF EC
00 2D`), and 0x73 p[0] went C1 -> 40 (off) -> C0 (on again) -> C1 within two minutes, with the LED
blinking orange again. 0x4D p[2] & 0x02 is the `after_heating_enabled` sensor, and 0x73 p[0] 0x80 +
0x01 the `after_heating` one. The 0x4C command is a main-menu setting and is left on the panel on
purpose. Confirmed on the unit: raising the after-heater's outdoor temperature limit (main menu) with
the setting on stopped the heater - the LED turned green, `after_heating` went off and
`after_heating_enabled` stayed on.

### Fireplace mode on the CASA layout

Captured from Home on the 3-step screen: the panel's fireplace start and its "Lopeta" (stop) send the
ILTO's own fireplace command, with the CASA header:

```
CC 64 45 0A 01 08 30 00 10 00 00 00 00 02 B1 65   panel: fireplace on
CC 64 85 01 0A 04 30 00 10 01 41 59               unit acks
CC 64 45 0A 01 08 30 00 10 00 00 00 00 01 81 06   panel: fireplace off
CC 64 85 01 0A 04 30 00 10 01 41 59               unit acks
```

0x73 during fireplace mode: p[1] = 01 (00 in every other capture), p[4] = 00 (21 before and after),
p[12] / p[13] = 3C / 14 (supply 60 %, extract 20 %); p[14] stays at the 3-step choice (Home). After
the stop p[1] goes back to 00 at once and the fans to 40 / 40 within a second. The panel shows
"Takkatoiminto 10 min" with Lopeta / Valikko, and "Takkatoiminto päättyy" briefly after the stop;
the length (10 min) is a panel setting, and p[6] = 0A in every capture may be it (unconfirmed).
The component sends the same frame; it refuses to start fireplace mode while the unit is off (not
captured). Whether a 3-step or 5-step command during fireplace mode ends it is not known.

### 5-step screen on the CASA layout

Switching the panel to basic display 2 (5 speeds) and back, and stepping through speeds 1-5:

```
20 00 04 01 00 <00 | 01> 28 00 00 00 00     screen: 00 3-step, 01 5-step (acked 20 00 04 01)
20 00 08 01 00 01 <speed x 20 %> 00 00 00 00 speed 1-5: 14 28 3C 50 64 (acked 20 00 08 01 01 <%>)
```

So in the panel's 11-byte 0x20 command data[2] selects the field (81 3-step choice, 08 speed,
04 screen, 10 off, 20 on), data[5] is the screen (00 / 01) and data[6] the speed x 20 % (28 in
the 3-step frames). With the 5-step screen, 0x73 p[9] and p[15] are 01 (00 with the 3-step
screen), p[12] / p[13] follow the speed (20-100 %) and p[17] stays 0, so the component reads the
speed as p[12] / 20. p[14] stayed 01 and is not a 3-step choice then. Switching back to the 3-step
screen set p[4] to 21 again (it had been 01 since the off / on). The component sends the speed
command only while p[15] shows the 5-step screen, and the 3-step command only while it does not,
and shows the screen in the `basic_display` text sensor.

**The screen command (`20 00 04 ...`) is recorded here but deliberately not implemented**: which
screen the panel shows is the owner's choice on the panel. To add it later, send
`20 00 04 01 00 <00 | 01> 28 00 00 00 00` the same way as the other CASA commands and wait for the
ack `20 00 04 01` and p[15] to change.

### Clock on the CASA layout

The unit broadcasts its clock as one of the rotating types, about every 25 s:

```
CC 64 85 FD 0A 07 55 00 A0 <weekday> <h> <min> <s> crc crc     weekday Monday = 0
```

Setting the clock on the panel (Main menu -> Clock) sent the ILTO layout's clock commands
unchanged, each acked:

```
CC 64 45 0A 01 07 54 00 0E <weekday> <h> <min> <s> crc crc   set the time    (acked 54 00 0E 01)
CC 64 45 0A 01 07 54 00 01 <weekday> <h> <min> <s> crc crc   set the weekday (acked 54 00 01 01)
```

The panel's time command carried the unit's old weekday (05) and the weekday command that followed
the new one (04, Friday); setting Saturday then Friday sent 05 then 04, so Monday = 0, as on the
ILTO. Before the clock was set the unit reported Saturday 08:33 on a Friday at 19:10. After the time
command the unit also broadcast `45 FD 0A 05 AA 00 00 01 0A` once and the panel answered
`85 0A 01 04 AA 00 00 01` (meaning unknown).

## Settings not decoded, on purpose

Only day-to-day controls are decoded and exposed: fan speed, on/off, fireplace mode and the
clock. The panel's other settings were never captured on the bus, and this project deliberately
leaves them out. Most of them describe the unit's installation - which accessories, actuators
and heaters are fitted - and set wrong for the hardware they can stop the unit from working
properly (for example enabling a heater, a damper motor or a sensor that is not there). They
belong on the panel, set once by the installer or owner, not on a dashboard where they can be
changed by accident.

Not decoded, from the panel's menus (names translated from Finnish):

| Menu | Settings |
|---|---|
| Main menu | Weekly timer, temperature (supply air setpoint), after-heating (electric models), summer cooling (start temperature, fan speed while cooling), basic display layout, fireplace function, shutdown |
| Installation and service: functions | Filter guard (on/off, alarm delay), fireplace function (on/off, duration, whether it overrides another function), boost (on/off, duration), underpressure compensation, service reminder (interval), summer cooling (on/off, temperature difference, outdoor limit) |
| Installation and service: devices | Duct devices 1 and 2, recirculation air, heat recovery actuator (bypass damper, run time), valve motor, heating (setpoint, overheat limit, supply or room control) |
| Installation and service: other | Fan speed for each mode (which of the 5 speeds Away / Home / Boost use), frost protection method, CO2 / humidity / presence inputs, factory reset |

Saving some of these sent the type 0x32 and 0x36 frames in the command table, but one example
of each is not enough to tell which byte is which setting, and sending a wrong guess could
change several settings at once. The unit's state while these features run (bypass, defrosting,
alarms) is still read from the 0x73 status message where its bits are known.

## Learning more

The component logs every frame whose content is new for its header and type at DEBUG level
(`new type 36: CC 64 ...` / `changed type 73: ...`). To decode something new: start a log
session, change exactly one thing on the panel, wait 10 s, change it back, and compare the
frames. A capture of each service-menu setting would decode types 0x32 and 0x36.
