// SPDX-License-Identifier: 0BSD
// Copyright (C) 2026 quasd

#pragma once

#include <cmath>
#include <deque>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/select/select.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

// Use at your own risk: unofficial and reverse-engineered, tested on two units only, no warranty.
// Written by Claude (Anthropic's AI assistant). See the repository README.md.
//
// Driver for the bus between an ILTO / Swegon CASA Premium ventilation unit and its control
// panel (the 4P4C modular cable; RS-485, 38400 8N1). Message types 0x21, 0x71 and 0x73 come from
// openHAB 1's swegonventilation binding (swegongw.c and SwegonVentilationDataParser.java), but
// the ILTO 440's header is two bytes longer than openHAB's, as seen on the bus:
//
//   CC | 64 | a b c d e | len | type | ?? ?? | payload (len - 3 bytes) | crc_hi | crc_lo
//
//   CC 64 4A 00 FD 00 FD 03 71 00 FF 06 F0                  request for type 0x71
//   CC 64 8A 00 00 00 00 10 71 00 FF <13 bytes> FF 34       the answer
//
// The CRC is CRC16-CCITT (poly 0x1021, init 0xFFFF) over 0x64 .. the payload's last byte. Each
// frame is preceded by its length counted from CC (0D before a request, 1A/22 before the
// answers); the receiver skips it while hunting for the next CC. The 0x71 payload matches
// openHAB's layout (checked against the panel's sensor 1-8, CO2, RH, rpm and efficiency
// screens); 0x21 and 0x73 are openHAB's apart from the bytes noted below.
//
// The panel (4A) polls the unit (8A) about once a second and sends a command only when a setting
// is confirmed; the unit acknowledges it with a 3-byte frame echoing the command's first bytes:
//
//   20 00 0C 01 00 01 <speed x 20 %> 01 01 00     set fan speed (unit acks 20 00 0C)
//   20 00 10 <as the speed command>              switch the unit off (unit acks 20 00 10)
//   20 00 20 <as the speed command>              switch it back on (unit acks 20 00 20); it comes
//                                                back at speed 3 whatever it ran at before
//   30 00 10 00 00 00 00 <02 start | 01 stop>     fireplace mode (unit acks 30 00 10)
//   54 00 0E <weekday> <h> <min> <s>              set the clock (unit acks 54 00 0E)
//   54 00 01 <weekday> <h> <min> <s>              set the weekday, Monday = 0 (acks 54 00 01)
//
// In the 0x20 and 0x54 commands the third byte selects which of the following fields the unit
// takes (0x0C: speed, 0x0E: time, 0x01: weekday, ...); the panel fills the others in anyway.
// With the panel's 3-step screen (Away / Home / Boost) it sends 20 00 05 <00 | 01 | 02> 00 00 3C
// 00 00 00 instead (acks 20 00 05); the unit maps each step to one of the 5 speeds. Not used here.
//
// 0x73 payload bytes seen on an ILTO 440 econo (p = after "73 00 FF"):
//   p[1] & 0x01  fireplace mode on
//   p[2]         0x04 while the fans run, 0x00 when stopped
//   p[4]         0x01 normally, 0x21 with the 3-step screen, 0x00 while switched off, during
//                fireplace mode, and for ~62 s after switching on (the start-up minute). Speed
//                and fireplace commands sent ~1 s after "on" were acknowledged but dropped; a
//                speed sent 41 s in was stored (p[17]) and the fans followed when it ended
//   p[8]         0x00 / 0x40 / 0xC0, changes around off/on (meaning unknown)
//   CASA p[0]    0x80 electric after-heating in use (cleared when it is switched off on the panel, and while
//                the unit is off), 0x01 with it: the heater is heating (the panel's LED blinks orange)
//   p[9], p[14], p[15], p[18]  follow the panel's screen type and the current 3-step mode
//   p[12], p[13] supply / extract fan power, %
//   p[17]        selected speed x 20 %; while switched off it keeps the old speed, but the first
//                status after switching on already shows speed 3 (0x3C)
//   p[19]        1 while the unit is switched off
//   p[20]        0x07 running, 0x03 then 0x04 while switching off
//   p[0] (0x40) and p[16] (0x10) never changed. The other bits below are openHAB's, unchecked.
// Not decoded: types 0x32 and 0x36, which the panel sends when service-menu settings are saved
// (one 0x36 carried 0x3C = the filter guard's 60 min delay). 0x71 p[10] (openHAB: supply fan
// rpm / 10) read 0 on the econo apart from a few frames of 1-22.
//
// With a flow_control_pin (the transceiver's DE and RE) set, this sends the same commands as the
// panel, in a pause between polls, and resends until the unit acknowledges. Every frame whose
// content is new for its header and type is logged at DEBUG.
//
// A second bus layout ("CASA", short header) was captured on another ILTO / CASA Premium unit.
// Same CC 64 start, same CRC and the same 0x71 / 0x73 payloads, but a 3-byte header
// <flags> <destination> <source>, no length byte before CC, and the unit is the bus master:
//
//   CC | 64 | flags dst src | len | type | ?? A0 | payload (len - 3 bytes) | crc_hi | crc_lo
//
//   CC 64 85 FD 0A 13 71 00 A0 <16 bytes> crc crc   unit (0A) to all (FD): temperatures
//   CC 64 85 FD 0A 18 73 00 A0 <21 bytes> crc crc   unit to all: status
//   CC 64 85 FD 0A <len> <type> <00|01> A0 ...       one of ~35 other types, a different one each round
//   CC 64 45 FD 0A 05 AA 00 00 02 0A crc crc         unit to all (meaning unknown)
//   CC 64 45 01 0A 03 A0 00 00 crc crc               unit polls the panel (01)
//   CC 64 85 0A 01 04 A0 00 00 00 crc crc            panel answers with nothing to report
//
// The round repeats about every 0.6 s. The panel, with its 3-step screen, sends its command in
// place of the empty answer to the poll, and the unit acks it straight away:
//
//   CC 64 45 0A 01 0B 20 00 81 <step> 00 00 28 00 00 00 <min> crc crc   panel to unit
//   CC 64 85 01 0A 06 20 00 81 01 <step> <fan %> crc crc                   unit acks
//
//   step 00 Away (fans 20 %), 01 Home (40 %), 02 Boost (60 %); min 1E / 3C / 78 = 30 / 60 / 120 min,
//   00 = no time limit. Stepping through the panel's list sends every step it passes.
//
// The status (0x73) then carries the choice in p[14] (00 / 01 / 02), the boost minutes in p[5],
// and p[19] = 0x20 during a timed boost. The component sends the same command, as the panel (01),
// in the pause after the panel's empty answer. Off and on are the same 11 bytes with 10 / 20 in place
// of 81 (acked 20 00 10 01 / 20 00 20 01). With the panel's 5-step screen (basic display 2) the
// speed command is 20 00 08 01 00 01 <speed x 20 %> 00 00 00 00 (acked 20 00 08 01 01 <%>), and 0x73
// p[15] = 01; the screen itself is switched with 20 00 04 01 00 <00 3-step | 01 5-step> 28 00 00 00 00
// (acked 20 00 04 01). That command is known but deliberately not sent: the screen is the panel's
// choice, shown read-only by the basic_display text sensor.
// The clock commands are the ILTO's (54 00 0E / 54 00 01 <weekday, Monday = 0> <h> <min> <s>, acked
// 54 00 0E 01 / 54 00 01 01), and type 0x55 carries the unit's clock: <weekday> <h> <min> <s>. So is
// the fireplace command: 30 00 10 00 00 00 00 <02 start | 01 stop>, acked 30 00 10 01. During fireplace
// mode 0x73 p[1] = 01 (00 otherwise), p[4] = 00 and the fans run supply 60 % / extract 20 % (from Home on
// the tested unit); the panel shows "Takkatoiminto 10 min" and the unit ends it by itself. Fireplace is
// not sent while the unit is off (never captured). Both layouts are tried on
// every frame and the CRC picks the one that fits; the first good frame fixes the layout for sending.

namespace esphome::ilto_premium {

class IltoPremium;

// The unit's power and fireplace mode as switches. They show the state the unit reports and send
// a command only when switched. They are not Components and have no setup, so nothing is sent at
// boot (the schema also only allows restore_mode DISABLED): a restored "off" switched the unit off
// on every ESP restart when these were template switches.
class IltoSwitch : public switch_::Switch, public Parented<IltoPremium> {
 public:
  enum Kind : uint8_t { POWER, FIREPLACE };
  void set_kind(Kind kind) { this->kind_ = kind; }

 protected:
  void write_state(bool state) override;
  Kind kind_{POWER};
};

// One control for the unit: options Off, 1-5 (fan speed) and Fireplace, in that order. Shows the
// state the unit reports; like the switches it has no setup, so nothing is sent at boot.
class IltoModeSelect : public select::Select, public Parented<IltoPremium> {
 protected:
  void control(size_t index) override;
};

// The panel's 3-step control on the CASA layout: Away, Home, Boost 30 / 60 / 120 min, Boost (no time
// limit), in that order. Built like IltoModeSelect: no setup, nothing sent at boot.
class IltoStepSelect : public select::Select, public Parented<IltoPremium> {
 protected:
  void control(size_t index) override;
};

class IltoPremium : public Component, public uart::UARTDevice {
 public:
  SUB_SENSOR(temperature_1)
  SUB_SENSOR(temperature_2)
  SUB_SENSOR(temperature_3)
  SUB_SENSOR(temperature_4)
  SUB_SENSOR(temperature_5)
  SUB_SENSOR(temperature_6)
  SUB_SENSOR(temperature_7)
  SUB_SENSOR(temperature_8)
  SUB_SENSOR(supply_fan_rpm)
  SUB_SENSOR(extract_fan_rpm)
  SUB_SENSOR(efficiency)
  SUB_SENSOR(co2)
  SUB_SENSOR(humidity)
  SUB_SENSOR(fan_speed)
  SUB_SENSOR(supply_fan_power)
  SUB_SENSOR(extract_fan_power)
  SUB_SENSOR(operating_mode)
  SUB_SENSOR(unit_state)
  SUB_BINARY_SENSOR(fireplace)
  void set_ventilation_switch(IltoSwitch *s) { this->ventilation_switch_ = s; }
  void set_fireplace_mode_switch(IltoSwitch *s) { this->fireplace_mode_switch_ = s; }
  void set_mode_select(IltoModeSelect *s) { this->mode_select_ = s; }
  void set_step_select(IltoStepSelect *s) { this->step_select_ = s; }
  SUB_SENSOR(boost_time)
  // CASA: the unit's own clock, from type 0x55, e.g. "Fri 19:23".
  void set_unit_clock_text_sensor(text_sensor::TextSensor *s) { this->unit_clock_text_sensor_ = s; }
  // CASA: the panel's basic display, "Basic display 1" (3-step) or "Basic display 2" (5-step).
  void set_basic_display_text_sensor(text_sensor::TextSensor *s) { this->basic_display_text_sensor_ = s; }

  static constexpr size_t MODE_OFF = 0;  // 1-5 are the fan speeds
  static constexpr size_t MODE_FIREPLACE = 6;

  // IltoStepSelect's options: the panel's step byte and boost minutes for each.
  static constexpr size_t STEP_COUNT = 6;
  static constexpr uint8_t STEP_CODE[STEP_COUNT] = {0x00, 0x01, 0x02, 0x02, 0x02, 0x02};
  static constexpr uint8_t STEP_MINUTES[STEP_COUNT] = {0, 0, 30, 60, 120, 0};

  // Away / Home / Boost on the CASA layout, sent as the panel's last-seen 20 00 81 command with only
  // the step and minutes changed. Nothing is sent when the unit already reports that choice.
  void set_step(size_t index);

  // Brings the unit to Off, speed 1-5 or Fireplace with the fewest commands: switches it on first
  // if needed, and ends fireplace mode before a speed is set (whether a speed command alone ends
  // it was never tested). Steps the unit is already in are skipped.
  void set_mode(size_t mode);
  SUB_BINARY_SENSOR(running)
  SUB_BINARY_SENSOR(starting_up)
  SUB_BINARY_SENSOR(bypass)
  SUB_BINARY_SENSOR(boost)
  SUB_BINARY_SENSOR(defrosting)
  SUB_BINARY_SENSOR(freeze_protection)
  SUB_BINARY_SENSOR(filter_alarm)
  SUB_BINARY_SENSOR(service_reminder)
  SUB_BINARY_SENSOR(fan_failure)
  SUB_BINARY_SENSOR(emergency_stop)
  SUB_BINARY_SENSOR(after_heating)
  SUB_BINARY_SENSOR(after_heating_enabled)

  void set_flow_control_pin(GPIOPin *pin) { this->flow_control_pin_ = pin; }
  // A separate UART to send on, e.g. a software UART when the receiving UART's TX pin cannot be
  // used (GPIO15 on an ESP8266 must be low at boot).
  void set_tx_uart(uart::UARTComponent *tx_uart) { this->tx_uart_ = tx_uart; }

  void setup() override;

  // Speed 1-5, sent as the panel's last speed command with only the speed byte changed, so the
  // fields not decoded yet keep the panel's values. Nothing is sent when the unit already reports
  // that speed and no other speed command is waiting, so automations can set it freely.
  void set_fan_speed(int speed);

  // The panel's off/on commands are its speed command with 0x10 / 0x20 in place of 0x0C.
  // On CASA it is the panel's 3-step command with 0x10 / 0x20 in place of 0x81. The form depends on
  // the layout, so nothing can be queued before the first frame has shown which one it is.
  void set_power(bool on);

  // Sets the unit's clock the way the panel does: the time, then the weekday (Monday = 0).
  void set_clock(uint8_t weekday, uint8_t hour, uint8_t minute, uint8_t second);

  // Fireplace mode on / off; the same frame on both layouts. Nothing is sent when the unit already
  // reports that state and no fireplace command is waiting.
  void set_fireplace(bool on);

  void loop() override;

  void dump_config() override;

  // The two frame layouts seen on the bus (see the comment at the top).
  enum Layout : uint8_t { LAYOUT_UNKNOWN, LAYOUT_ILTO, LAYOUT_CASA };

  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  static constexpr size_t HEADER_LEN = 7;       // ILTO layout: 64 a b c d e len
  static constexpr size_t CASA_HEADER_LEN = 5;  // CASA layout: 64 flags dst src len
  static constexpr uint8_t MAX_LEN = 30;        // swegongw's limit for the len byte
  // Most bytes any log line shows: a whole outgoing frame (<len> CC, header, MAX_LEN, CRC).
  static constexpr size_t LOG_HEX_BYTES = 2 + HEADER_LEN + MAX_LEN + 2;
  // Stack buffer for format_hex_pretty_to() ("AA.BB.CC"); a longer input is cut, never overrun.
  using HexBuf = char[format_hex_pretty_size(LOG_HEX_BYTES)];
  static constexpr size_t MAX_SEEN = 48;   // distinct header + type remembered for logging
  static constexpr uint32_t QUIET_MS = 30;         // bus idle this long before sending
  // CASA: send between 5 and 70 ms after the panel's empty answer (the next frame starts ~100 ms in).
  static constexpr uint32_t CASA_SLOT_QUIET_MS = 5;
  static constexpr uint32_t CASA_SLOT_END_MS = 70;
  static constexpr uint32_t ACK_TIMEOUT_MS = 500;  // resend after this without an ack
  static constexpr uint8_t MAX_ATTEMPTS = 3;
  // A held choice is sent anyway when the unit still reports its start-up this long after the
  // choice (the start-up takes ~62 s), so an unknown state with p[4] 0x00 cannot block it for good.
  static constexpr uint32_t DEFERRED_MAX_MS = 180000;

  // Longest frame after CC: the longer header, MAX_LEN, the CRC.
  static constexpr size_t BUF_LEN = (HEADER_LEN > CASA_HEADER_LEN ? HEADER_LEN : CASA_HEADER_LEN) + MAX_LEN + 2;
  uint8_t buf_[BUF_LEN];
  size_t pos_{0};
  bool in_frame_{false};
  bool crc_failed_{false};  // a candidate length was reached in this frame but its CRC did not match
  Layout layout_{LAYOUT_UNKNOWN};
  uint32_t frames_ok_{0};
  uint32_t frames_bad_{0};
  uint32_t bytes_{0};
  uint32_t last_report_{0};
  uint8_t recent_[24];
  size_t recent_pos_{0};
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  // Each frame kind (header bytes + type) with the CRC of its last content, to log only new and
  // changed frames. The frame's own CRC stands in for the content: no copy, no heap.
  struct Seen {
    uint64_t key;
    uint16_t crc;
  };
  Seen seen_[MAX_SEEN];
  size_t seen_count_{0};
#endif

  GPIOPin *flow_control_pin_{nullptr};
  uart::UARTComponent *tx_uart_{nullptr};
  // The panel's speed command as seen on the bus (here at speed 1); updated from the panel.
  std::vector<uint8_t> speed_command_{0x20, 0x00, 0x0C, 0x01, 0x00, 0x01, 0x14, 0x01, 0x01, 0x00};
  // CASA: the panel's 3-step command as captured (here Home); updated from the panel.
  std::vector<uint8_t> step_command_{0x20, 0x00, 0x81, 0x01, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00};
  // CASA: the panel's 5-step speed command as captured (here speed 2); updated from the panel.
  std::vector<uint8_t> casa_speed_command_{0x20, 0x00, 0x08, 0x01, 0x00, 0x01, 0x28, 0x00, 0x00, 0x00, 0x00};
  // CASA: 1 while the panel shows the 5-step screen, 0 with the 3-step screen, -1 before a status.
  int8_t casa_five_step_{-1};
  bool casa_slot_open_{false};
  uint32_t casa_slot_at_{0};
  IltoStepSelect *step_select_{nullptr};
  text_sensor::TextSensor *unit_clock_text_sensor_{nullptr};
  text_sensor::TextSensor *basic_display_text_sensor_{nullptr};
  size_t published_step_{SIZE_MAX};
  // Boost minutes of the last 3-step command seen on the bus (the panel's or ours), to tell the
  // timed boosts apart in the select; 0 while unknown.
  uint8_t boost_minutes_{0};
  // The running timed boost's length (0: none), when it was first seen, and whether that moment is
  // its real start (false for a boost already running when the first status arrived).
  uint8_t boost_length_{0};
  uint32_t boost_started_at_{0};
  bool boost_start_known_{false};
  bool boost_length_seen_{false};
  std::deque<std::vector<uint8_t>> commands_;  // commands (type onwards) not sent yet
  std::vector<uint8_t> pending_;               // the command being sent, until the unit acks it
  bool awaiting_ack_{false};
  uint8_t attempts_{0};
  uint32_t sent_at_{0};
  uint32_t last_rx_{0};
  IltoSwitch *ventilation_switch_{nullptr};
  IltoModeSelect *mode_select_{nullptr};
  size_t published_mode_{SIZE_MAX};
  // From the last 0x73 status: 1 / 0, or -1 before the first one.
  int8_t running_{-1};
  int8_t fireplace_{-1};
  IltoSwitch *fireplace_mode_switch_{nullptr};
  // The selected speed from the last 0x73 status, 0 while unknown: before the first status, and
  // with the panel's 3-step (Away / Home / Boost) layout, where p[17] is not what the fans run at.
  int reported_speed_{0};
  // The unit is in its start-up minute after switching on (0x73 p[4] 0x00 while running and not in
  // fireplace mode). A speed or Fireplace chosen while off or in that minute waits in
  // deferred_mode_ (MODE_OFF: nothing waiting); deferred_tries_ counts the commands sent for it.
  bool starting_up_{false};
  size_t deferred_mode_{MODE_OFF};
  uint8_t deferred_tries_{0};
  uint32_t deferred_since_{0};  // millis() of the choice, or of the last try sent during a start-up

  // Is a command of this type (and select byte, unless -1) queued or waiting for its ack?
  bool command_waiting_(uint8_t type, int select = -1) const;
  bool speed_command_waiting_() const { return this->command_waiting_(0x20, 0x0C) || this->command_waiting_(0x20, 0x08); }

  void queue_(std::vector<uint8_t> data);

  // Which commands have a known form on each layout. CASA: its own 0x20 forms (3-step, off / on,
  // 5-step speed), which in turn are not known on ILTO, and the clock and fireplace commands.
  static bool supported_(Layout layout, const std::vector<uint8_t> &data);

  // ILTO: the panel polls and the bus is idle for about a second between polls, so any pause will
  // do. CASA: the unit polls the panel and the panel puts its command in its answer; the only quiet
  // spot known is the ~100 ms after the panel's empty answer, before the unit's next broadcast, so
  // send there - after the bus has been idle a moment, and well before the window closes.
  bool may_send_(uint32_t now) const;

  void send_pending_();

  void transmit_(const std::vector<uint8_t> &out);

  static size_t header_len_(Layout layout) { return layout == LAYOUT_CASA ? CASA_HEADER_LEN : HEADER_LEN; }
  static const char *layout_name_(Layout layout);

  // Restart on this byte if it could itself be a start marker.
  void restart_(uint8_t b);

  // Collects the bytes after CC. The length byte sits at a different place in each layout, so both
  // are tried; a frame is taken when its length is complete and the CRC matches. Neither layout's
  // length byte is plausible in the other's frames (an ILTO header has 00 where CASA has its length,
  // a CASA frame has the type, always above MAX_LEN so far, where ILTO has its length), and the CRC
  // rules out the rest.
  void feed_(uint8_t b);

  static uint16_t crc16_ccitt_(const uint8_t *data, size_t len);

  // A frame with a good CRC, len bytes from 64 to the end of the CRC.
  void handle_frame_(Layout layout, size_t len);

  // With each 0x73 status while a choice waits (see set_mode()): once the unit has acknowledged
  // "on" and its start-up has ended, start fireplace mode or set the speed, check the next status,
  // and try once more if the unit did not take it. Measured 2026-11-15 on the econo: the first
  // status after "on" shows speed 3 and p[4] 0x00; p[4] turns 0x01 about 62 s later, and a speed
  // sent then is shown within 2 s. If the start-up is still reported DEFERRED_MAX_MS after the
  // choice, the command is sent anyway (and once more that long after).
  // off, speed_unknown and speed as the status handler read them for this layout: on CASA "off" is
  // p[19] bit 0 (0x20 there is a timed boost), the speed is the fan power / 20 with the 5-step
  // screen and unknown with the 3-step one.
  void apply_deferred_(const uint8_t *p, Layout layout, bool off, bool speed_unknown, int speed);

  // The unit repeats its status frames every ~2 s. Every reading is published so filters such as
  // throttle_average see all of them; rate-limit each sensor with a filter in the YAML.
  static void publish_(sensor::Sensor *s, float value);
  static void publish_(binary_sensor::BinarySensor *s, bool value);
};

}  // namespace esphome::ilto_premium
