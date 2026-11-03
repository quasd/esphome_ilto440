// SPDX-License-Identifier: 0BSD
// Copyright (C) 2026 quasd

#pragma once

#include <deque>
#include <map>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
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

namespace esphome::ilto_premium {

static const char *const TAG = "ilto_premium";

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
  void set_power(bool on) {
    std::vector<uint8_t> data = this->speed_command_;
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
    if (this->pending_.empty() && !this->commands_.empty()) {
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
      if (!this->pending_.empty() && !this->awaiting_ack_ && now - this->last_rx_ >= QUIET_MS)
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
    LOG_PIN("  Flow control pin: ", this->flow_control_pin_);
  }

  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  static constexpr size_t HEADER_LEN = 7;  // 64 a b c d e len
  static constexpr uint8_t MAX_LEN = 30;   // swegongw's limit for the len byte
  static constexpr size_t MAX_SEEN = 48;   // distinct header + type remembered for logging
  static constexpr uint32_t QUIET_MS = 30;         // bus idle this long before sending
  static constexpr uint32_t ACK_TIMEOUT_MS = 500;  // resend after this without an ack
  static constexpr uint8_t MAX_ATTEMPTS = 3;

  uint8_t buf_[HEADER_LEN + MAX_LEN + 2];
  size_t pos_{0};
  bool in_frame_{false};
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
  std::deque<std::vector<uint8_t>> commands_;  // commands (type onwards) not sent yet
  std::vector<uint8_t> pending_;               // the command being sent, until the unit acks it
  bool awaiting_ack_{false};
  uint8_t attempts_{0};
  uint32_t sent_at_{0};
  uint32_t last_rx_{0};
  // The selected speed from the last 0x73 status, 0 while unknown: before the first status, and
  // with the panel's 3-step (Away / Home / Boost) layout, where p[17] is not what the fans run at.
  int reported_speed_{0};

  bool speed_command_waiting_() const {
    auto is_speed = [](const std::vector<uint8_t> &c) { return c.size() > 2 && c[0] == 0x20 && c[2] == 0x0C; };
    if (is_speed(this->pending_))
      return true;
    for (const auto &c : this->commands_) {
      if (is_speed(c))
        return true;
    }
    return false;
  }

  void queue_(std::vector<uint8_t> data) {
    if (this->flow_control_pin_ == nullptr) {
      ESP_LOGW(TAG, "Cannot send without flow_control_pin");
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

  void send_pending_() {
    std::vector<uint8_t> body{0x64, 0x4A, 0x00, 0xFD, 0x00, 0xFD, uint8_t(this->pending_.size())};
    body.insert(body.end(), this->pending_.begin(), this->pending_.end());
    const uint16_t crc = crc16_ccitt_(body.data(), body.size());
    std::vector<uint8_t> out{uint8_t(1 + body.size() + 2), 0xCC};
    out.insert(out.end(), body.begin(), body.end());
    out.push_back(crc >> 8);
    out.push_back(crc & 0xFF);

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

  void feed_(uint8_t b) {
    if (!this->in_frame_) {
      if (b == 0xCC) {
        this->in_frame_ = true;
        this->pos_ = 0;
      }
      return;
    }
    this->buf_[this->pos_++] = b;
    bool bad = this->buf_[0] != 0x64 ||
               (this->pos_ == HEADER_LEN && (this->buf_[6] < 3 || this->buf_[6] > MAX_LEN));
    if (bad) {
      // Restart on this byte if it could itself be a start marker.
      this->in_frame_ = b == 0xCC;
      this->pos_ = 0;
      return;
    }
    if (this->pos_ > HEADER_LEN && this->pos_ == HEADER_LEN + this->buf_[6] + 2) {
      this->handle_frame_();
      this->in_frame_ = false;
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

  void handle_frame_() {
    const size_t len = this->pos_;
    const uint16_t crc = (uint16_t(this->buf_[len - 2]) << 8) | this->buf_[len - 1];
    if (crc16_ccitt_(this->buf_, len - 2) != crc) {
      this->frames_bad_++;
      ESP_LOGV(TAG, "CRC mismatch: CC %s", format_hex_pretty(this->buf_, len).c_str());
      return;
    }
    this->frames_ok_++;

    const uint8_t type = this->buf_[7];
    const uint8_t *p = this->buf_ + 10;
    const size_t n = this->buf_[6] - 3;

    ESP_LOGV(TAG, "frame: CC %s", format_hex_pretty(this->buf_, len).c_str());
    // Header bytes a..e plus the type tell the frames apart (request vs answer, which value).
    uint64_t key = type;
    for (size_t i = 1; i <= 5; i++)
      key = (key << 8) | this->buf_[i];
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

    if (this->awaiting_ack_ && this->buf_[1] == 0x8A && n == 0 && type == this->pending_[0] &&
        this->buf_[9] == this->pending_[2]) {
      ESP_LOGI(TAG, "Unit acknowledged %s", format_hex_pretty(this->pending_).c_str());
      this->pending_.clear();
      this->awaiting_ack_ = false;
    }
    // A speed / power / mode command (the panel's or ours) changes the speed before the next status
    // shows it, so treat the speed as unknown until then.
    if (type == 0x20)
      this->reported_speed_ = 0;
    if (this->buf_[1] == 0x4A && type == 0x20 && this->buf_[9] == 0x0C && n + 3 == this->speed_command_.size())
      this->speed_command_.assign(this->buf_ + 7, this->buf_ + 7 + n + 3);

    switch (type) {
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
          publish_(this->supply_fan_power_sensor_, p[12]);
          publish_(this->extract_fan_power_sensor_, p[13]);
          publish_(this->fan_speed_sensor_, p[17] / 20);
          publish_(this->running_binary_sensor_, p[19] == 0);
          this->reported_speed_ = (p[4] & 0x20) ? 0 : p[17] / 20;
        }
        if (n >= 9) {
          publish_(this->bypass_binary_sensor_, p[0] & 0x04);
          publish_(this->freeze_protection_binary_sensor_, p[0] & 0x08);
          publish_(this->fireplace_binary_sensor_, p[1] & 0x01);
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

}  // namespace esphome::ilto_premium
