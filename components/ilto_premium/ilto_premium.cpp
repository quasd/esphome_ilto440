// SPDX-License-Identifier: 0BSD
// Copyright (C) 2026 quasd
//
// The ILTO / Swegon CASA Premium bus driver. The protocol is described in ilto_premium.h.

#include "ilto_premium.h"

namespace esphome::ilto_premium {

static const char *const TAG = "ilto_premium";

void IltoPremium::set_step(size_t index) {
  if (index >= STEP_COUNT)
    return;
  if (this->layout_ == LAYOUT_CASA && this->casa_five_step_ == 1) {
    ESP_LOGW(TAG, "Away / Home / Boost needs the panel's 3-step screen (basic display 1); not sent");
    return;
  }
  if (index == this->published_step_ && !this->command_waiting_(0x20, 0x81)) {
    ESP_LOGD(TAG, "Already in that mode, nothing sent");
    return;
  }
  std::vector<uint8_t> data = this->step_command_;
  data[3] = STEP_CODE[index];
  data[10] = STEP_MINUTES[index];
  this->queue_(std::move(data));
}

void IltoPremium::set_mode(size_t mode) {
  // A new choice replaces one still waiting for the unit's start-up to end.
  this->deferred_mode_ = MODE_OFF;
  // running_ / fireplace_ take the target as soon as a command is queued, so a second choice made
  // before the unit's next status builds on the first.
  if (mode == MODE_OFF) {
    if (this->running_ != 0)
      this->set_power(false);
    this->running_ = 0;
    return;
  }
  // Switching on selects speed 3 and starts a minute in which the unit acknowledges speed and
  // fireplace commands but does not reliably take them (dropped ~1 s after "on", 2026-11-15). So
  // a choice made while the unit is off or in that minute waits until a status shows the start-up
  // has ended (see apply_deferred_()).
  if (this->running_ != 1 || this->starting_up_) {
    if (this->running_ != 1)
      this->set_power(true);
    this->running_ = 1;
    this->starting_up_ = true;
    this->deferred_mode_ = mode;
    this->deferred_tries_ = 0;
    this->deferred_since_ = millis();
    return;
  }
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

void IltoPremium::setup() {
  if (this->flow_control_pin_ != nullptr) {
    this->flow_control_pin_->setup();
    this->flow_control_pin_->digital_write(false);
  }
}

void IltoPremium::set_fan_speed(int speed) {
  speed = clamp(speed, 1, 5);
  if (speed == this->reported_speed_ && !this->speed_command_waiting_()) {
    ESP_LOGD(TAG, "Fan speed is already %d, nothing sent", speed);
    return;
  }
  std::vector<uint8_t> data = this->speed_command_;
  if (this->layout_ == LAYOUT_CASA) {
    // CASA: only with the panel's 5-step screen (Main menu -> Basic display 2), whose command this is.
    if (this->casa_five_step_ != 1) {
      ESP_LOGW(TAG, "Fan speed needs the panel's 5-step screen (basic display 2); not sent");
      return;
    }
    data = this->casa_speed_command_;
  }
  data[6] = speed * 20;
  this->queue_(std::move(data));
}

void IltoPremium::set_power(bool on) {
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

void IltoPremium::set_clock(uint8_t weekday, uint8_t hour, uint8_t minute, uint8_t second) {
  this->queue_({0x54, 0x00, 0x0E, weekday, hour, minute, second});
  this->queue_({0x54, 0x00, 0x01, weekday, hour, minute, second});
}

void IltoPremium::set_fireplace(bool on) {
  if (this->fireplace_ == (on ? 1 : 0) && !this->command_waiting_(0x30)) {
    ESP_LOGD(TAG, "Fireplace mode is already %s, nothing sent", on ? "on" : "off");
    return;
  }
  // CASA: the panel offers fireplace mode only while the unit runs; starting it on a stopped unit was
  // never captured.
  if (on && this->layout_ == LAYOUT_CASA && this->running_ == 0) {
    ESP_LOGW(TAG, "The unit is off; fireplace mode not sent");
    return;
  }
  this->queue_({0x30, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, uint8_t(on ? 0x02 : 0x01)});
  this->fireplace_ = on;
}

void IltoPremium::loop() {
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
        HexBuf hex;
        ESP_LOGW(TAG, "%s is not known on the %s bus layout; not sent", format_hex_pretty_to(hex, *it, '.'),
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
        HexBuf hex;
        ESP_LOGW(TAG, "No acknowledgement for %s after %u tries, giving up",
                 format_hex_pretty_to(hex, this->pending_, '.'), (unsigned) this->attempts_);
        // The unit never took "on": a choice held for after it must not wait for some later start.
        if (this->pending_.size() > 2 && this->pending_[0] == 0x20 && this->pending_[2] == 0x20 &&
            this->deferred_mode_ != MODE_OFF) {
          ESP_LOGW(TAG, "Unit was not switched on; the chosen mode is dropped");
          this->deferred_mode_ = MODE_OFF;
        }
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
    HexBuf hex;
    ESP_LOGD(TAG, "bytes %u, frames ok %u, bad crc %u, last bytes: %s", (unsigned) this->bytes_,
             (unsigned) this->frames_ok_, (unsigned) this->frames_bad_, format_hex_pretty_to(hex, tail, n, '.'));
  }
}

void IltoPremium::dump_config() {
  ESP_LOGCONFIG(TAG, "ILTO Premium bus %s", this->flow_control_pin_ != nullptr ? "listener and sender" : "listener (receive only)");
  ESP_LOGCONFIG(TAG, "  Bus layout: %s", layout_name_(this->layout_));
  LOG_PIN("  Flow control pin: ", this->flow_control_pin_);
}

bool IltoPremium::command_waiting_(uint8_t type, int select) const {
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

void IltoPremium::queue_(std::vector<uint8_t> data) {
  if (this->flow_control_pin_ == nullptr) {
    ESP_LOGW(TAG, "Cannot send without flow_control_pin");
    return;
  }
  if (this->layout_ != LAYOUT_UNKNOWN && !supported_(this->layout_, data)) {
    HexBuf hex;
    ESP_LOGW(TAG, "%s is not known on the %s bus layout; not sent", format_hex_pretty_to(hex, data, '.'),
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

bool IltoPremium::supported_(Layout layout, const std::vector<uint8_t> &data) {
  // CASA knows the panel's 11-byte 0x20 command with select 81 (3-step), 10 (off) and 20 (on), and
  // the clock commands, which have the same form on both layouts.
  const bool casa_form = data.size() == 11 && data[0] == 0x20 &&
                         (data[2] == 0x81 || data[2] == 0x10 || data[2] == 0x20 || data[2] == 0x08);
  const bool clock = data.size() == 7 && data[0] == 0x54 && (data[2] == 0x0E || data[2] == 0x01);
  // Fireplace start / stop: 30 00 10 00 00 00 00 <02 | 01>, also the same on both layouts.
  const bool fireplace = data.size() == 8 && data[0] == 0x30 && data[2] == 0x10;
  if (layout == LAYOUT_CASA)
    return casa_form || clock || fireplace;
  return !(data.size() > 2 && data[0] == 0x20 && data[2] == 0x81) && !casa_form;
}

bool IltoPremium::may_send_(uint32_t now) const {
  if (this->layout_ == LAYOUT_ILTO)
    return now - this->last_rx_ >= QUIET_MS;
  if (this->layout_ == LAYOUT_CASA)
    return this->casa_slot_open_ && now - this->last_rx_ >= CASA_SLOT_QUIET_MS &&
           now - this->casa_slot_at_ < CASA_SLOT_END_MS;
  return false;
}

void IltoPremium::send_pending_() {
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

void IltoPremium::transmit_(const std::vector<uint8_t> &out) {
  uart::UARTComponent *tx = this->tx_uart_ != nullptr ? this->tx_uart_ : this->parent_;
  this->flow_control_pin_->digital_write(true);
  tx->write_array(out);
  const uart::UARTFlushResult flushed = tx->flush();
  // flush() returns once the FIFO is empty; let the last byte's bits leave before releasing.
  delayMicroseconds(300);
  this->flow_control_pin_->digital_write(false);
  if (flushed == uart::UARTFlushResult::UART_FLUSH_RESULT_TIMEOUT ||
      flushed == uart::UARTFlushResult::UART_FLUSH_RESULT_FAILED)
    ESP_LOGW(TAG, "UART flush %s; the frame may have been cut short",
             flushed == uart::UARTFlushResult::UART_FLUSH_RESULT_TIMEOUT ? "timed out" : "failed");

  this->attempts_++;
  this->awaiting_ack_ = true;
  this->sent_at_ = millis();
  HexBuf hex;
  ESP_LOGD(TAG, "Sent (try %u): %s", (unsigned) this->attempts_, format_hex_pretty_to(hex, out, '.'));
}

const char *IltoPremium::layout_name_(Layout layout) {
  switch (layout) {
    case LAYOUT_ILTO:
      return "ILTO (long header)";
    case LAYOUT_CASA:
      return "CASA (short header)";
    default:
      return "not seen yet";
  }
}

void IltoPremium::restart_(uint8_t b) {
  this->in_frame_ = b == 0xCC;
  this->pos_ = 0;
  this->crc_failed_ = false;
}

void IltoPremium::feed_(uint8_t b) {
  if (!this->in_frame_) {
    if (b == 0xCC)
      this->restart_(b);
    return;
  }
  // The length checks below end every frame by BUF_LEN; this only guards against a later change.
  if (this->pos_ >= sizeof(this->buf_)) {
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
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
      HexBuf hex;
      ESP_LOGV(TAG, "CRC mismatch: CC %s", format_hex_pretty_to(hex, this->buf_, this->pos_ - 1, '.'));
#endif
    }
    this->restart_(b);
  }
}

uint16_t IltoPremium::crc16_ccitt_(const uint8_t *data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= uint16_t(data[i]) << 8;
    for (int bit = 0; bit < 8; bit++)
      crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
  }
  return crc;
}

void IltoPremium::handle_frame_(Layout layout, size_t len) {
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

#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_VERBOSE
  HexBuf hex;
  ESP_LOGV(TAG, "frame: CC %s", format_hex_pretty_to(hex, this->buf_, len, '.'));
#endif
#if ESPHOME_LOG_LEVEL >= ESPHOME_LOG_LEVEL_DEBUG
  // The header bytes plus the type tell the frames apart (request vs answer, which value).
  uint64_t key = type;
  for (size_t i = 1; i < header - 1; i++)
    key = (key << 8) | this->buf_[i];
  key = (key << 8) | layout;
  const uint16_t crc = (uint16_t(this->buf_[len - 2]) << 8) | this->buf_[len - 1];
  Seen *seen = nullptr;
  for (size_t i = 0; i < this->seen_count_; i++) {
    if (this->seen_[i].key == key) {
      seen = &this->seen_[i];
      break;
    }
  }
  if (seen == nullptr || seen->crc != crc) {
    HexBuf hex;
    ESP_LOGD(TAG, "%s type %02X: CC %s (ok %u, bad crc %u)", seen == nullptr ? "new" : "changed", type,
             format_hex_pretty_to(hex, this->buf_, len, '.'), (unsigned) this->frames_ok_,
             (unsigned) this->frames_bad_);
    if (seen != nullptr)
      seen->crc = crc;
    else if (this->seen_count_ < MAX_SEEN)
      this->seen_[this->seen_count_++] = {key, crc};
  }
#endif

  // A speed / power / mode command (the panel's or ours) changes the speed before the next status
  // shows it, so treat the speed as unknown until then.
  if (type == 0x20)
    this->reported_speed_ = 0;
  // Acks and the panel's commands are only known for the ILTO layout.
  if (layout == LAYOUT_ILTO) {
    if (this->awaiting_ack_ && this->buf_[1] == 0x8A && n == 0 && type == this->pending_[0] &&
        this->buf_[9] == this->pending_[2]) {
      HexBuf hex;
      ESP_LOGI(TAG, "Unit acknowledged %s", format_hex_pretty_to(hex, this->pending_, '.'));
      this->pending_.clear();
      this->awaiting_ack_ = false;
    }
    if (this->buf_[1] == 0x4A && type == 0x20 && this->buf_[9] == 0x0C && n + 3 == this->speed_command_.size())
      this->speed_command_.assign(this->buf_ + 7, this->buf_ + 7 + n + 3);
    // A speed / power command from the panel replaces a choice still waiting. (Our own frames are
    // never heard: the transceiver's receiver is off while it sends.)
    if (this->buf_[1] == 0x4A && type == 0x20)
      this->deferred_mode_ = MODE_OFF;
  }
  if (layout == LAYOUT_CASA) {
    const uint8_t dst = this->buf_[2], src = this->buf_[3], select = this->buf_[7];
    // The panel's empty answer to the unit's poll opens the window to send in.
    if (src == 0x01 && dst == 0x0A && type == 0xA0) {
      this->casa_slot_open_ = true;
      this->casa_slot_at_ = millis();
    }
    // Any 0x20 command from the panel (3-step, speed, off / on) replaces a choice still waiting.
    if (src == 0x01 && dst == 0x0A && type == 0x20)
      this->deferred_mode_ = MODE_OFF;
    // The panel's 3-step command: 20 00 81 <step> ... <minutes>. (Our own frames are not heard:
    // the transceiver's receiver is off while it sends.)
    if (src == 0x01 && dst == 0x0A && type == 0x20 && select == 0x81 && n + 3 == this->step_command_.size()) {
      this->step_command_.assign(this->buf_ + 5, this->buf_ + 5 + n + 3);
      this->boost_minutes_ = this->step_command_[3] == 0x02 ? this->step_command_[10] : 0;
    }
    // The panel's 5-step speed command: 20 00 08 01 00 01 <speed x 20 %> 00 00 00 00.
    if (src == 0x01 && dst == 0x0A && type == 0x20 && select == 0x08 && n + 3 == this->casa_speed_command_.size()) {
      this->casa_speed_command_.assign(this->buf_ + 5, this->buf_ + 5 + n + 3);
    }
    // The unit's ack to the panel: 20 00 81 01 <step> <fan %>, or 20 00 10 01 / 20 00 20 01 for
    // off / on.
    // pending_ is read only while a sent command waits for its ack (it holds that command then), and
    // its length is checked before each byte beyond the type and select.
    if (this->awaiting_ack_ && this->pending_.size() >= 3 && src == 0x0A && dst == 0x01 &&
        type == this->pending_[0] && select == this->pending_[2] &&
        (select == 0x81 ? n >= 2 && this->pending_.size() >= 4 && p[1] == this->pending_[3] : n >= 1)) {
      HexBuf hex;
      ESP_LOGI(TAG, "Unit acknowledged %s", format_hex_pretty_to(hex, this->pending_, '.'));
      // Only the 3-step command (20 00 81 <step> ... <minutes>) sets the boost length; a clock
      // command's 4th byte is the weekday, so it must not be read as a step.
      if (this->pending_[0] == 0x20 && this->pending_[2] == 0x81 && this->pending_.size() >= 11)
        this->boost_minutes_ = this->pending_[3] == 0x02 ? this->pending_[10] : 0;
      this->pending_.clear();
      this->awaiting_ack_ = false;
    }
  }

  switch (type) {
    case 0x4D:
      // CASA: the unit's after-heating setting, p[2] 03 on / 01 off (the panel changes it with type 0x4C,
      // which is left on the panel on purpose).
      // Two records alternate (the byte after the type, 00 / 01); only record 00 changed with the setting,
      // record 01 stayed 03.
      if (layout == LAYOUT_CASA && n >= 3 && this->buf_[header + 1] == 0x00)
        publish_(this->after_heating_enabled_binary_sensor_, p[2] & 0x02);
      break;
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
      // (0 when the unit is switched off, supply 100 / extract 20 in fireplace mode; 60 / 20 on the
      // CASA unit) and
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
        // On CASA, p[15] (and p[9]) is 01 with the 5-step screen; the speed is then the fan power / 20
        // (p[17] stays 0). With the 3-step screen the speed is not known.
        const bool casa_five = layout == LAYOUT_CASA && p[15] == 0x01;
        if (layout == LAYOUT_CASA) {
          this->casa_five_step_ = casa_five;
          const char *display = casa_five ? "Basic display 2" : "Basic display 1";
          if (this->basic_display_text_sensor_ != nullptr && this->basic_display_text_sensor_->state != display)
            this->basic_display_text_sensor_->publish_state(display);
        }
        const int speed = casa_five ? clamp(p[12] / 20, 1, 5) : p[17] / 20;
        const bool speed_unknown = layout == LAYOUT_CASA ? !casa_five : three_step;
        publish_(this->fan_speed_sensor_, off ? 0.0f : speed_unknown ? NAN : float(speed));
        publish_(this->running_binary_sensor_, !off);
        if (this->ventilation_switch_ != nullptr)
          this->ventilation_switch_->publish_state(!off);
        this->reported_speed_ = speed_unknown ? 0 : speed;
        // Keep the expected state while a power / fireplace command is still waiting to be sent.
        if (!this->command_waiting_(0x20, 0x10) && !this->command_waiting_(0x20, 0x20))
          this->running_ = !off;
        if (!this->command_waiting_(0x30))
          this->fireplace_ = p[1] & 0x01;
        // CASA: p[14] is the 3-step choice (00 Away, 01 Home, 02 Boost), p[5] the boost's chosen
        // length in minutes (it does not count down) and p[19] & 0x20 a boost with a time limit.
        if (layout == LAYOUT_CASA && !off && !casa_five) {
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
                                            : size_t(clamp(speed, 1, 5));
        // With the 3-step screen a speed mode is not known; show nothing rather than a wrong speed.
        const bool mode_known = !speed_unknown || mode == MODE_OFF || mode == MODE_FIREPLACE;
        if (this->mode_select_ != nullptr && mode_known && mode != this->published_mode_) {
          this->published_mode_ = mode;
          this->mode_select_->publish_state(mode);
        }
        // p[4] 0x00 while running and not in fireplace mode. The CASA unit shows the same for about a
        // minute after "on" (while its panel says "starting"); its p[20] is 05 for only ~1 s.
        const bool starting_up = p[19] == 0 && p[4] == 0x00 && !(p[1] & 0x01);
        publish_(this->starting_up_binary_sensor_, starting_up);
        if (!this->command_waiting_(0x20, 0x20))
          this->starting_up_ = starting_up;
        if (this->deferred_mode_ != MODE_OFF)
          this->apply_deferred_(p, layout, layout == LAYOUT_CASA ? off : p[19] != 0, speed_unknown, speed);
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
        // CASA: p[0] C1 while the panel's LED blinked orange (heater on), 40 with after-heating switched
        // off on the panel, C0 right after switching it back on. Only published on that layout.
        if (layout == LAYOUT_CASA)
          publish_(this->after_heating_binary_sensor_, (p[0] & 0x81) == 0x81);
      }
      break;
  }
}

void IltoPremium::apply_deferred_(const uint8_t *p, Layout layout, bool off, bool speed_unknown, int speed) {
  if (this->command_waiting_(0x20, 0x20))
    return;
  const uint32_t now = millis();
  const bool overdue = now - this->deferred_since_ >= DEFERRED_MAX_MS;
  if (off) {
    // Still off with no "on" waiting: a status from just before the ack, or something else
    // switched the unit off again. Don't keep the choice for a later start.
    if (overdue) {
      ESP_LOGW(TAG, "Unit is still off; the chosen mode is dropped");
      this->deferred_mode_ = MODE_OFF;
    }
    return;
  }
  if (this->starting_up_ && !overdue)
    return;
  const bool fireplace = this->deferred_mode_ == MODE_FIREPLACE;
  // 3-step screen: the speeds mean nothing there. (On ILTO a held Fireplace is dropped too, as
  // before; on CASA fireplace mode works with either screen.)
  if (speed_unknown && (layout != LAYOUT_CASA || !fireplace)) {
    this->deferred_mode_ = MODE_OFF;
    return;
  }
  if (fireplace ? (p[1] & 0x01) != 0 : speed == int(this->deferred_mode_)) {
    ESP_LOGI(TAG, "Unit is now in the chosen mode (%s)", fireplace ? "Fireplace" : "speed");
    this->deferred_mode_ = MODE_OFF;
    return;
  }
  if (this->command_waiting_(0x20, 0x0C) || this->command_waiting_(0x20, 0x08) || this->command_waiting_(0x30))
    return;
  if (this->deferred_tries_ >= 2) {
    ESP_LOGW(TAG, "Unit did not take the chosen mode after %u tries; it runs at speed %d",
             (unsigned) this->deferred_tries_, speed);
    this->deferred_mode_ = MODE_OFF;
    return;
  }
  this->deferred_tries_++;
  if (this->starting_up_) {
    ESP_LOGW(TAG, "Unit still reports its start-up after %u s; sending the chosen mode anyway",
             (unsigned) ((now - this->deferred_since_) / 1000));
    this->deferred_since_ = now;
  }
  if (fireplace) {
    ESP_LOGI(TAG, "Starting fireplace mode");
    this->set_fireplace(true);
    this->fireplace_ = 1;
  } else {
    ESP_LOGI(TAG, "Unit runs at speed %d; setting speed %d", speed, int(this->deferred_mode_));
    // ILTO: 20 00 0C ... <speed x 20 %> at [6]; CASA: 20 00 08 01 00 01 <speed x 20 %> 00 00 00 00.
    std::vector<uint8_t> data = layout == LAYOUT_CASA ? this->casa_speed_command_ : this->speed_command_;
    data[6] = uint8_t(this->deferred_mode_ * 20);
    this->queue_(std::move(data));
  }
}

void IltoPremium::publish_(sensor::Sensor *s, float value) {
  if (s != nullptr)
    s->publish_state(value);
}

void IltoPremium::publish_(binary_sensor::BinarySensor *s, bool value) {
  if (s != nullptr)
    s->publish_state(value);
}

// The select and switches keep their old state until the unit's next status shows the change
// (about 2 s).
void IltoModeSelect::control(size_t index) { this->parent_->set_mode(index); }
void IltoStepSelect::control(size_t index) { this->parent_->set_step(index); }

void IltoSwitch::write_state(bool state) {
  if (this->kind_ == POWER)
    this->parent_->set_power(state);
  else
    this->parent_->set_fireplace(state);
}

}  // namespace esphome::ilto_premium
