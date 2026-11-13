// SPDX-License-Identifier: 0BSD
// Copyright (C) 2026 quasd

#pragma once

#include <cmath>
#include <deque>
#include <map>
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

// Use at your own risk: unofficial and reverse-engineered, tested on one unit only, no warranty.
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
//   20 00 20 <as the speed command>              switch it back on (unit acks 20 00 20)
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
//   p[4]         0x01 normally, 0x21 with the 3-step screen, 0x00 for about a minute after
//                fireplace mode or an off/on
//   p[8]         0x00 / 0x40 / 0xC0, changes around off/on (meaning unknown)
//   p[9], p[14], p[15], p[18]  follow the panel's screen type and the current 3-step mode
//   p[12], p[13] supply / extract fan power, %
//   p[17]        selected speed x 20 %
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
// of 81 (acked 20 00 10 01 / 20 00 20 01). The speed 1-5, fireplace and clock commands are not known
// on this layout and are dropped with a warning. The clock commands are the ILTO's (54 00 0E / 54 00
// 01 <weekday, Monday = 0> <h> <min> <s>, acked 54 00 0E 01 / 54 00 01 01), and type 0x55 carries the
// unit's clock: <weekday> <h> <min> <s>. Both layouts are tried on
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

static const char *const TAG = "ilto_premium";

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

  static constexpr size_t MODE_OFF = 0;  // 1-5 are the fan speeds
  static constexpr size_t MODE_FIREPLACE = 6;

  // IltoStepSelect's options: the panel's step byte and boost minutes for each.
  static constexpr size_t STEP_COUNT = 6;
  static constexpr uint8_t STEP_CODE[STEP_COUNT] = {0x00, 0x01, 0x02, 0x02, 0x02, 0x02};
  static constexpr uint8_t STEP_MINUTES[STEP_COUNT] = {0, 0, 30, 60, 120, 0};

  // Away / Home / Boost on the CASA layout, sent as the panel's last-seen 20 00 81 command with only
  // the step and minutes changed. Nothing is sent when the unit already reports that choice.
  void set_step(size_t index) {
    if (index >= STEP_COUNT)
      return;
    if (index == this->published_step_ && !this->command_waiting_(0x20, 0x81)) {
      ESP_LOGD(TAG, "Already in that mode, nothing sent");
      return;
    }
    std::vector<uint8_t> data = this->step_command_;
    data[3] = STEP_CODE[index];
    data[10] = STEP_MINUTES[index];
    this->queue_(std::move(data));
  }

  // Brings the unit to Off, speed 1-5 or Fireplace with the fewest commands: switches it on first
  // if needed, and ends fireplace mode before a speed is set (whether a speed command alone ends
  // it was never tested). Steps the unit is already in are skipped.
  void set_mode(size_t mode) {
    // running_ / fireplace_ take the target as soon as a command is queued, so a second choice made
    // before the unit's next status builds on the first.
    if (mode == MODE_OFF) {
      if (this->running_ != 0)
        this->set_power(false);
      this->running_ = 0;
      return;
    }
    if (this->running_ != 1)
      this->set_power(true);
    this->running_ = 1;
    if (mode == MODE_FIREPLACE) {
      if (this->fireplace_ != 1)
        this->set_fireplace(true);
      this->fireplace_ = 1;
      return;
    }
    if (this->fireplace_ != 0)
      this->set_fireplace(false);
    this->fireplace_ = 0;
    this->set_fan_speed(mode);
  }
  SUB_BINARY_SENSOR(running)
  SUB_BINARY_SENSOR(bypass)
  SUB_BINARY_SENSOR(boost)
  SUB_BINARY_SENSOR(defrosting)
  SUB_BINARY_SENSOR(freeze_protection)
  SUB_BINARY_SENSOR(filter_alarm)
  SUB_BINARY_SENSOR(service_reminder)
  SUB_BINARY_SENSOR(fan_failure)
  SUB_BINARY_SENSOR(emergency_stop)

  void set_flow_control_pin(GPIOPin *pin) { this->flow_control_pin_ = pin; }
  // A separate UART to send on, e.g. a software UART when the receiving UART's TX pin cannot be
  // used (GPIO15 on an ESP8266 must be low at boot).
  void set_tx_uart(uart::UARTComponent *tx_uart) { this->tx_uart_ = tx_uart; }

  void setup() override {
    if (this->flow_control_pin_ != nullptr) {
      this->flow_control_pin_->setup();
      this->flow_control_pin_->digital_write(false);
    }
  }

  // Speed 1-5, sent as the panel's last speed command with only the speed byte changed, so the
  // fields not decoded yet keep the panel's values. Nothing is sent when the unit already reports
  // that speed and no other speed command is waiting, so automations can set it freely.
  void set_fan_speed(int speed) {
    speed = clamp(speed, 1, 5);
    if (speed == this->reported_speed_ && !this->speed_command_waiting_()) {
      ESP_LOGD(TAG, "Fan speed is already %d, nothing sent", speed);
      return;
    }
    std::vector<uint8_t> data = this->speed_command_;
    data[6] = speed * 20;
    this->queue_(std::move(data));
  }

  // The panel's off/on commands are its speed command with 0x10 / 0x20 in place of 0x0C.
  // On CASA it is the panel's 3-step command with 0x10 / 0x20 in place of 0x81. The form depends on
  // the layout, so nothing can be queued before the first frame has shown which one it is.
  void set_power(bool on) {
    if (this->layout_ == LAYOUT_UNKNOWN) {
      ESP_LOGW(TAG, "Bus layout not seen yet; power command not sent");
      return;
    }
    if (this->running_ == (on ? 1 : 0) && !this->command_waiting_(0x20, 0x10) && !this->command_waiting_(0x20, 0x20)) {
      ESP_LOGD(TAG, "The unit is already %s, nothing sent", on ? "on" : "off");
      return;
    }
    std::vector<uint8_t> data = this->layout_ == LAYOUT_CASA ? this->step_command_ : this->speed_command_;
    data[2] = on ? 0x20 : 0x10;
    this->queue_(std::move(data));
  }

  // Sets the unit's clock the way the panel does: the time, then the weekday (Monday = 0).
  void set_clock(uint8_t weekday, uint8_t hour, uint8_t minute, uint8_t second) {
    this->queue_({0x54, 0x00, 0x0E, weekday, hour, minute, second});
    this->queue_({0x54, 0x00, 0x01, weekday, hour, minute, second});
  }

  void set_fireplace(bool on) { this->queue_({0x30, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, uint8_t(on ? 0x02 : 0x01)}); }

  void loop() override {
    uint8_t b;
    const uint32_t now = millis();
    while (this->available() && this->read_byte(&b)) {
      this->last_rx_ = now;
      this->bytes_++;
      this->recent_[this->recent_pos_++ % sizeof(this->recent_)] = b;
      this->feed_(b);
    }
    // Commands queued before the first good frame (e.g. the clock sync at boot) wait until the
    // layout is known; those the layout has no known form for are then dropped.
    if (this->layout_ != LAYOUT_UNKNOWN) {
      for (auto it = this->commands_.begin(); it != this->commands_.end();) {
        if (supported_(this->layout_, *it)) {
          ++it;
        } else {
          ESP_LOGW(TAG, "%s is not known on the %s bus layout; not sent", format_hex_pretty(*it).c_str(),
                   layout_name_(this->layout_));
          it = this->commands_.erase(it);
        }
      }
    }
    if (this->layout_ != LAYOUT_UNKNOWN && this->pending_.empty() && !this->commands_.empty()) {
      this->pending_ = std::move(this->commands_.front());
      this->commands_.pop_front();
      this->attempts_ = 0;
    }
    if (!this->pending_.empty()) {
      if (this->awaiting_ack_ && now - this->sent_at_ >= ACK_TIMEOUT_MS) {
        this->awaiting_ack_ = false;
        if (this->attempts_ >= MAX_ATTEMPTS) {
          ESP_LOGW(TAG, "No acknowledgement for %s after %u tries, giving up",
                   format_hex_pretty(this->pending_).c_str(), (unsigned) this->attempts_);
          this->pending_.clear();
        }
      }
      if (!this->pending_.empty() && !this->awaiting_ack_ && this->may_send_(now))
        this->send_pending_();
    }
    // Wiring check: is anything arriving, and does it frame up?
    if (now - this->last_report_ >= 10000) {
      this->last_report_ = now;
      uint8_t tail[sizeof(this->recent_)];
      const size_t n = std::min<size_t>(this->recent_pos_, sizeof(this->recent_));
      for (size_t i = 0; i < n; i++)
        tail[i] = this->recent_[(this->recent_pos_ - n + i) % sizeof(this->recent_)];
      ESP_LOGD(TAG, "bytes %u, frames ok %u, bad crc %u, last bytes: %s", (unsigned) this->bytes_,
               (unsigned) this->frames_ok_, (unsigned) this->frames_bad_, format_hex_pretty(tail, n).c_str());
    }
  }

  void dump_config() override {
    ESP_LOGCONFIG(TAG, "ILTO Premium bus %s", this->flow_control_pin_ != nullptr ? "listener and sender" : "listener (receive only)");
    ESP_LOGCONFIG(TAG, "  Bus layout: %s", layout_name_(this->layout_));
    LOG_PIN("  Flow control pin: ", this->flow_control_pin_);
  }

  // The two frame layouts seen on the bus (see the comment at the top).
  enum Layout : uint8_t { LAYOUT_UNKNOWN, LAYOUT_ILTO, LAYOUT_CASA };

  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  static constexpr size_t HEADER_LEN = 7;       // ILTO layout: 64 a b c d e len
  static constexpr size_t CASA_HEADER_LEN = 5;  // CASA layout: 64 flags dst src len
  static constexpr uint8_t MAX_LEN = 30;        // swegongw's limit for the len byte
  static constexpr size_t MAX_SEEN = 48;   // distinct header + type remembered for logging
  static constexpr uint32_t QUIET_MS = 30;         // bus idle this long before sending
  // CASA: send between 5 and 70 ms after the panel's empty answer (the next frame starts ~100 ms in).
  static constexpr uint32_t CASA_SLOT_QUIET_MS = 5;
  static constexpr uint32_t CASA_SLOT_END_MS = 70;
  static constexpr uint32_t ACK_TIMEOUT_MS = 500;  // resend after this without an ack
  static constexpr uint8_t MAX_ATTEMPTS = 3;

  uint8_t buf_[HEADER_LEN + MAX_LEN + 2];
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
  std::map<uint64_t, std::vector<uint8_t>> seen_;

  GPIOPin *flow_control_pin_{nullptr};
  uart::UARTComponent *tx_uart_{nullptr};
  // The panel's speed command as seen on the bus (here at speed 1); updated from the panel.
  std::vector<uint8_t> speed_command_{0x20, 0x00, 0x0C, 0x01, 0x00, 0x01, 0x14, 0x01, 0x01, 0x00};
  // CASA: the panel's 3-step command as captured (here Home); updated from the panel.
  std::vector<uint8_t> step_command_{0x20, 0x00, 0x81, 0x01, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00};
  bool casa_slot_open_{false};
  uint32_t casa_slot_at_{0};
  IltoStepSelect *step_select_{nullptr};
  text_sensor::TextSensor *unit_clock_text_sensor_{nullptr};
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

  // Is a command of this type (and select byte, unless -1) queued or waiting for its ack?
  bool command_waiting_(uint8_t type, int select = -1) const {
    auto matches = [&](const std::vector<uint8_t> &c) {
      return c.size() > 2 && c[0] == type && (select < 0 || c[2] == select);
    };
    if (matches(this->pending_))
      return true;
    for (const auto &c : this->commands_) {
      if (matches(c))
        return true;
    }
    return false;
  }
  bool speed_command_waiting_() const { return this->command_waiting_(0x20, 0x0C); }

  void queue_(std::vector<uint8_t> data) {
    if (this->flow_control_pin_ == nullptr) {
      ESP_LOGW(TAG, "Cannot send without flow_control_pin");
      return;
    }
    if (this->layout_ != LAYOUT_UNKNOWN && !supported_(this->layout_, data)) {
      ESP_LOGW(TAG, "%s is not known on the %s bus layout; not sent", format_hex_pretty(data).c_str(),
               layout_name_(this->layout_));
      return;
    }
    // A newer command of the same kind replaces one still waiting (e.g. several speed steps).
    for (auto &queued : this->commands_) {
      if (queued[0] == data[0] && queued[2] == data[2]) {
        queued = std::move(data);
        return;
      }
    }
    this->commands_.push_back(std::move(data));
  }

  // Which commands have a known form on each layout: on CASA only the 3-step command (20 00 81),
  // which in turn is not known on ILTO.
  static bool supported_(Layout layout, const std::vector<uint8_t> &data) {
    // CASA knows the panel's 11-byte 0x20 command with select 81 (3-step), 10 (off) and 20 (on), and
    // the clock commands, which have the same form on both layouts.
    const bool casa_form = data.size() == 11 && data[0] == 0x20 &&
                           (data[2] == 0x81 || data[2] == 0x10 || data[2] == 0x20);
    const bool clock = data.size() == 7 && data[0] == 0x54 && (data[2] == 0x0E || data[2] == 0x01);
    if (layout == LAYOUT_CASA)
      return casa_form || clock;
    return !(data.size() > 2 && data[0] == 0x20 && data[2] == 0x81) && !casa_form;
  }

  // ILTO: the panel polls and the bus is idle for about a second between polls, so any pause will
  // do. CASA: the unit polls the panel and the panel puts its command in its answer; the only quiet
  // spot known is the ~100 ms after the panel's empty answer, before the unit's next broadcast, so
  // send there - after the bus has been idle a moment, and well before the window closes.
  bool may_send_(uint32_t now) const {
    if (this->layout_ == LAYOUT_ILTO)
      return now - this->last_rx_ >= QUIET_MS;
    if (this->layout_ == LAYOUT_CASA)
      return this->casa_slot_open_ && now - this->last_rx_ >= CASA_SLOT_QUIET_MS &&
             now - this->casa_slot_at_ < CASA_SLOT_END_MS;
    return false;
  }

  void send_pending_() {
    std::vector<uint8_t> out;
    if (this->layout_ == LAYOUT_CASA) {
      // As the panel sends it: request flags 45, to the unit (0A), from the panel (01). No length
      // byte before CC on this layout.
      std::vector<uint8_t> body{0x64, 0x45, 0x0A, 0x01, uint8_t(this->pending_.size())};
      body.insert(body.end(), this->pending_.begin(), this->pending_.end());
      const uint16_t crc = crc16_ccitt_(body.data(), body.size());
      out.push_back(0xCC);
      out.insert(out.end(), body.begin(), body.end());
      out.push_back(crc >> 8);
      out.push_back(crc & 0xFF);
      this->casa_slot_open_ = false;  // one try per window
      this->transmit_(out);
      return;
    }
    std::vector<uint8_t> body{0x64, 0x4A, 0x00, 0xFD, 0x00, 0xFD, uint8_t(this->pending_.size())};
    body.insert(body.end(), this->pending_.begin(), this->pending_.end());
    const uint16_t crc = crc16_ccitt_(body.data(), body.size());
    out.push_back(uint8_t(1 + body.size() + 2));
    out.push_back(0xCC);
    out.insert(out.end(), body.begin(), body.end());
    out.push_back(crc >> 8);
    out.push_back(crc & 0xFF);
    this->transmit_(out);
  }

  void transmit_(const std::vector<uint8_t> &out) {
    uart::UARTComponent *tx = this->tx_uart_ != nullptr ? this->tx_uart_ : this->parent_;
    this->flow_control_pin_->digital_write(true);
    tx->write_array(out);
    tx->flush();
    // flush() returns once the FIFO is empty; let the last byte's bits leave before releasing.
    delayMicroseconds(300);
    this->flow_control_pin_->digital_write(false);

    this->attempts_++;
    this->awaiting_ack_ = true;
    this->sent_at_ = millis();
    ESP_LOGD(TAG, "Sent (try %u): %s", (unsigned) this->attempts_, format_hex_pretty(out).c_str());
  }

  static size_t header_len_(Layout layout) { return layout == LAYOUT_CASA ? CASA_HEADER_LEN : HEADER_LEN; }
  static const char *layout_name_(Layout layout) {
    switch (layout) {
      case LAYOUT_ILTO:
        return "ILTO (long header)";
      case LAYOUT_CASA:
        return "CASA (short header)";
      default:
        return "not seen yet";
    }
  }

  // Restart on this byte if it could itself be a start marker.
  void restart_(uint8_t b) {
    this->in_frame_ = b == 0xCC;
    this->pos_ = 0;
    this->crc_failed_ = false;
  }

  // Collects the bytes after CC. The length byte sits at a different place in each layout, so both
  // are tried; a frame is taken when its length is complete and the CRC matches. Neither layout's
  // length byte is plausible in the other's frames (an ILTO header has 00 where CASA has its length,
  // a CASA frame has the type, always above MAX_LEN so far, where ILTO has its length), and the CRC
  // rules out the rest.
  void feed_(uint8_t b) {
    if (!this->in_frame_) {
      if (b == 0xCC)
        this->restart_(b);
      return;
    }
    this->buf_[this->pos_++] = b;
    if (this->buf_[0] != 0x64) {
      this->restart_(b);
      return;
    }
    bool possible = false;
    for (Layout layout : {LAYOUT_CASA, LAYOUT_ILTO}) {
      const size_t header = header_len_(layout);
      if (this->pos_ < header) {
        possible = true;
        continue;
      }
      const uint8_t len = this->buf_[header - 1];
      if (len < 3 || len > MAX_LEN)
        continue;
      const size_t total = header + len + 2;
      if (this->pos_ < total) {
        possible = true;
      } else if (this->pos_ == total) {
        const uint16_t crc = (uint16_t(this->buf_[total - 2]) << 8) | this->buf_[total - 1];
        if (crc16_ccitt_(this->buf_, total - 2) == crc) {
          this->handle_frame_(layout, total);
          this->in_frame_ = false;
          return;
        }
        this->crc_failed_ = true;
      }
    }
    if (!possible) {
      if (this->crc_failed_) {
        this->frames_bad_++;
        ESP_LOGV(TAG, "CRC mismatch: CC %s", format_hex_pretty(this->buf_, this->pos_ - 1).c_str());
      }
      this->restart_(b);
    }
  }

  static uint16_t crc16_ccitt_(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
      crc ^= uint16_t(data[i]) << 8;
      for (int bit = 0; bit < 8; bit++)
        crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
  }

  // A frame with a good CRC, len bytes from 64 to the end of the CRC.
  void handle_frame_(Layout layout, size_t len) {
    this->frames_ok_++;
    if (layout != this->layout_) {
      if (this->layout_ == LAYOUT_UNKNOWN) {
        ESP_LOGI(TAG, "Bus layout: %s", layout_name_(layout));
      } else {
        ESP_LOGW(TAG, "Bus layout changed from %s to %s", layout_name_(this->layout_), layout_name_(layout));
      }
      this->layout_ = layout;
    }

    // From the type on, both layouts carry: type, two bytes (00 FF / 00-01 A0), payload.
    const size_t header = header_len_(layout);
    const uint8_t type = this->buf_[header];
    const uint8_t *p = this->buf_ + header + 3;
    const size_t n = this->buf_[header - 1] - 3;

    ESP_LOGV(TAG, "frame: CC %s", format_hex_pretty(this->buf_, len).c_str());
    // The header bytes plus the type tell the frames apart (request vs answer, which value).
    uint64_t key = type;
    for (size_t i = 1; i < header - 1; i++)
      key = (key << 8) | this->buf_[i];
    key = (key << 8) | layout;
    std::vector<uint8_t> body(this->buf_, this->buf_ + len - 2);
    auto it = this->seen_.find(key);
    if (it == this->seen_.end() || it->second != body) {
      ESP_LOGD(TAG, "%s type %02X: CC %s (ok %u, bad crc %u)", it == this->seen_.end() ? "new" : "changed",
               type, format_hex_pretty(this->buf_, len).c_str(), (unsigned) this->frames_ok_,
               (unsigned) this->frames_bad_);
      if (it != this->seen_.end())
        it->second = std::move(body);
      else if (this->seen_.size() < MAX_SEEN)
        this->seen_.emplace(key, std::move(body));
    }

    // A speed / power / mode command (the panel's or ours) changes the speed before the next status
    // shows it, so treat the speed as unknown until then.
    if (type == 0x20)
      this->reported_speed_ = 0;
    // Acks and the panel's commands are only known for the ILTO layout.
    if (layout == LAYOUT_ILTO) {
      if (this->awaiting_ack_ && this->buf_[1] == 0x8A && n == 0 && type == this->pending_[0] &&
          this->buf_[9] == this->pending_[2]) {
        ESP_LOGI(TAG, "Unit acknowledged %s", format_hex_pretty(this->pending_).c_str());
        this->pending_.clear();
        this->awaiting_ack_ = false;
      }
      if (this->buf_[1] == 0x4A && type == 0x20 && this->buf_[9] == 0x0C && n + 3 == this->speed_command_.size())
        this->speed_command_.assign(this->buf_ + 7, this->buf_ + 7 + n + 3);
    }
    if (layout == LAYOUT_CASA) {
      const uint8_t dst = this->buf_[2], src = this->buf_[3], select = this->buf_[7];
      // The panel's empty answer to the unit's poll opens the window to send in.
      if (src == 0x01 && dst == 0x0A && type == 0xA0) {
        this->casa_slot_open_ = true;
        this->casa_slot_at_ = millis();
      }
      // The panel's 3-step command: 20 00 81 <step> ... <minutes>. (Our own frames are not heard:
      // the transceiver's receiver is off while it sends.)
      if (src == 0x01 && dst == 0x0A && type == 0x20 && select == 0x81 && n + 3 == this->step_command_.size()) {
        this->step_command_.assign(this->buf_ + 5, this->buf_ + 5 + n + 3);
        this->boost_minutes_ = this->step_command_[3] == 0x02 ? this->step_command_[10] : 0;
      }
      // The unit's ack to the panel: 20 00 81 01 <step> <fan %>, or 20 00 10 01 / 20 00 20 01 for
      // off / on.
      const bool ack_matches = select == 0x81 ? n >= 2 && p[1] == this->pending_[3] : n >= 1;
      if (this->awaiting_ack_ && src == 0x0A && dst == 0x01 && type == this->pending_[0] && select == this->pending_[2] &&
          ack_matches) {
        ESP_LOGI(TAG, "Unit acknowledged %s", format_hex_pretty(this->pending_).c_str());
        this->boost_minutes_ = this->pending_[3] == 0x02 ? this->pending_[10] : 0;
        this->pending_.clear();
        this->awaiting_ack_ = false;
      }
    }

    switch (type) {
      case 0x55:
        // CASA: the unit's clock, <weekday, Monday = 0> <h> <min> <s>, broadcast every ~25 s.
        if (layout == LAYOUT_CASA && n >= 4 && this->unit_clock_text_sensor_ != nullptr && p[0] < 7 && p[1] < 24 &&
            p[2] < 60) {
          static const char *const DAYS[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
          char text[16];
          snprintf(text, sizeof(text), "%s %02u:%02u", DAYS[p[0]], p[1], p[2]);
          if (this->unit_clock_text_sensor_->state != text)
            this->unit_clock_text_sensor_->publish_state(text);
        }
        break;
      case 0x21:
        if (n >= 4) {
          publish_(this->operating_mode_sensor_, p[0]);
          publish_(this->unit_state_sensor_, p[1]);
        }
        break;
      case 0x71:
        if (n >= 13) {
          sensor::Sensor *temperatures[] = {this->temperature_1_sensor_, this->temperature_2_sensor_,
                                            this->temperature_3_sensor_, this->temperature_4_sensor_,
                                            this->temperature_5_sensor_, this->temperature_6_sensor_,
                                            this->temperature_7_sensor_, this->temperature_8_sensor_};
          for (size_t i = 0; i < 8; i++)
            publish_(temperatures[i], int8_t(p[i]));
          // openHAB's layout; both read 0 on a unit without the CO2 / humidity option.
          publish_(this->co2_sensor_, p[8]);
          publish_(this->humidity_sensor_, p[9]);
          publish_(this->supply_fan_rpm_sensor_, p[10] * 10);
          publish_(this->extract_fan_rpm_sensor_, p[11] * 10);
          publish_(this->efficiency_sensor_, int8_t(p[12]));
        }
        break;
      case 0x73:
        // Checked on an ILTO 440 econo: p[12]/p[13] are the supply/extract fan power in %
        // (0 when the unit is switched off, supply 100 / extract 20 in fireplace mode) and
        // p[17] the selected speed x 20 %, p[19] 1 while the unit is switched off. The other bits
        // are openHAB's.
        if (n >= 20) {
          // p[4] & 0x20: the panel shows the 3-step Away / Home / Boost screen, and p[17] is then not
          // the speed the fans run at, so the speed is reported as unknown.
          const bool three_step = p[4] & 0x20;
          // Bit 0 of p[19] is "switched off" (the ILTO's p[19] was 1); the CASA unit sets 0x20 there
          // during a timed boost.
          const bool off = p[19] & 0x01;
          publish_(this->supply_fan_power_sensor_, p[12]);
          publish_(this->extract_fan_power_sensor_, p[13]);
          // The selected speed while running, 0 while switched off.
          // The CASA unit leaves p[17] at 0, and p[4] lost its 0x20 after an off/on, so its speed is
          // never known from here.
          const bool speed_unknown = three_step || layout == LAYOUT_CASA;
          publish_(this->fan_speed_sensor_, off ? 0.0f : speed_unknown ? NAN : float(p[17] / 20));
          publish_(this->running_binary_sensor_, !off);
          if (this->ventilation_switch_ != nullptr)
            this->ventilation_switch_->publish_state(!off);
          this->reported_speed_ = (p[4] & 0x20) ? 0 : p[17] / 20;
          // Keep the expected state while a power / fireplace command is still waiting to be sent.
          if (!this->command_waiting_(0x20, 0x10) && !this->command_waiting_(0x20, 0x20))
            this->running_ = !off;
          if (!this->command_waiting_(0x30))
            this->fireplace_ = p[1] & 0x01;
          // CASA: p[14] is the 3-step choice (00 Away, 01 Home, 02 Boost), p[5] the boost's chosen
          // length in minutes (it does not count down) and p[19] & 0x20 a boost with a time limit.
          if (layout == LAYOUT_CASA && !off) {
            const bool timed = p[19] & 0x20;
            // The unit does not report the time left, so count it from when this boost was first seen
            // (a new length restarts it, as the panel's does). A boost already running when the ESP
            // started has an unknown start, so its time left stays unknown.
            const uint8_t boost_length = p[14] == 0x02 && timed ? p[5] : 0;
            const uint32_t now = millis();
            if (boost_length != this->boost_length_) {
              this->boost_started_at_ = now;
              this->boost_start_known_ = this->boost_length_seen_;
              this->boost_length_ = boost_length;
            }
            this->boost_length_seen_ = true;
            float left = 0.0f;
            if (boost_length != 0) {
              const uint32_t elapsed_min = (now - this->boost_started_at_) / 60000;
              left = !this->boost_start_known_ ? NAN : elapsed_min >= boost_length ? 0.0f : float(boost_length - elapsed_min);
            }
            publish_(this->boost_time_sensor_, left);
            size_t step = SIZE_MAX;
            if (p[14] == 0x00 || p[14] == 0x01) {
              step = p[14];
            } else if (p[14] == 0x02 && !timed) {
              step = 5;
            } else if (p[14] == 0x02) {
              // The duration chosen last, if a 3-step command has been seen; otherwise the shortest
              // option that covers the minutes the unit reports.
              const uint8_t minutes = this->boost_minutes_ != 0 ? this->boost_minutes_ : p[5];
              step = minutes <= 30 ? 2 : minutes <= 60 ? 3 : 4;
            }
            if (this->step_select_ != nullptr && step != SIZE_MAX && step != this->published_step_ &&
                !this->command_waiting_(0x20, 0x81)) {
              this->published_step_ = step;
              this->step_select_->publish_state(step);
            }
          }
          const size_t mode = off              ? MODE_OFF
                              : (p[1] & 0x01) ? MODE_FIREPLACE
                                              : size_t(clamp(p[17] / 20, 1, 5));
          // With the 3-step screen a speed mode is not known; show nothing rather than a wrong speed.
          const bool mode_known = !speed_unknown || mode == MODE_OFF || mode == MODE_FIREPLACE;
          if (this->mode_select_ != nullptr && mode_known && mode != this->published_mode_) {
            this->published_mode_ = mode;
            this->mode_select_->publish_state(mode);
          }
        }
        if (n >= 9) {
          publish_(this->bypass_binary_sensor_, p[0] & 0x04);
          publish_(this->freeze_protection_binary_sensor_, p[0] & 0x08);
          publish_(this->fireplace_binary_sensor_, p[1] & 0x01);
          if (this->fireplace_mode_switch_ != nullptr)
            this->fireplace_mode_switch_->publish_state(p[1] & 0x01);
          publish_(this->boost_binary_sensor_, p[1] & 0x04);
          publish_(this->defrosting_binary_sensor_, p[1] & 0x20);
          publish_(this->emergency_stop_binary_sensor_, p[3] & 0x80);
          publish_(this->fan_failure_binary_sensor_, p[7] & 0x18);
          publish_(this->filter_alarm_binary_sensor_, p[8] & 0x02);
          publish_(this->service_reminder_binary_sensor_, p[8] & 0x04);
        }
        break;
    }
  }

  // The unit repeats its status frames every ~2 s. Every reading is published so filters such as
  // throttle_average see all of them; rate-limit each sensor with a filter in the YAML.
  static void publish_(sensor::Sensor *s, float value) {
    if (s != nullptr)
      s->publish_state(value);
  }
  static void publish_(binary_sensor::BinarySensor *s, bool value) {
    if (s != nullptr)
      s->publish_state(value);
  }
};

// The select and switches keep their old state until the unit's next status shows the change
// (about 2 s).
inline void IltoModeSelect::control(size_t index) { this->parent_->set_mode(index); }
inline void IltoStepSelect::control(size_t index) { this->parent_->set_step(index); }

inline void IltoSwitch::write_state(bool state) {
  if (this->kind_ == POWER)
    this->parent_->set_power(state);
  else
    this->parent_->set_fireplace(state);
}

}  // namespace esphome::ilto_premium
