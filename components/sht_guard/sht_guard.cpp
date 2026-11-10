#include "sht_guard.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cmath>

namespace esphome {
namespace sht_guard {

static const char *const TAG = "sht_guard";
// SHT4x "heater on, high power (200 mW), 1 s, then measure" (datasheet 4.5).
// The measurement it returns is discarded: the die is hot by then.
static const uint8_t HEATER_HIGH_1S = 0x39;

void ShtGuard::setup() {
  // The SHT4x driver publishes temperature, then humidity, from the same
  // read. Humidity arriving is therefore "a complete sample is in".
  this->raw_rh_->add_on_state_callback([this](float) { this->on_sample_(); });
  this->set_status_("no reading yet");
}

void ShtGuard::on_sample_() {
  const uint32_t now = millis();
  const float t = this->raw_t_->state;
  const float h = this->raw_rh_->state;

  // Heater blanking: the die is still hot from the pulse. Ignore the
  // sample, and hold the last good value as fresh -- it is at most
  // heater_blank old, and a known transient must not read as a fault.
  if (this->blank_until_ms_ != 0 && (int32_t) (this->blank_until_ms_ - now) > 0) {
    if (this->last_ok_ms_ != 0)
      this->last_ok_ms_ = now;
    return;
  }
  this->blank_until_ms_ = 0;

  if (std::isnan(t) || std::isnan(h)) {
    this->reject_("read error");
    return;
  }

  // Rate, against the previous raw sample. Always advance prev first, so a
  // sustained step is accepted on the next agreeing sample.
  bool jumped = false;
  if (this->have_prev_) {
    const float minutes = (float) (now - this->prev_ms_) / 60000.0f;
    // Floor the interval at 10 s so two samples close together cannot turn
    // ordinary noise into an enormous rate.
    const float rate = std::fabs(t - this->prev_t_) / std::fmax(minutes, 10.0f / 60.0f);
    jumped = rate > this->max_rate_;
  }
  this->have_prev_ = true;
  this->prev_t_ = t;
  this->prev_ms_ = now;

  // Frozen: bit-identical temperature AND humidity for frozen_time.
  if (t != this->last_raw_t_ || h != this->last_raw_rh_ || this->last_change_ms_ == 0) {
    this->last_raw_t_ = t;
    this->last_raw_rh_ = h;
    this->last_change_ms_ = now;
  }

  if (t < 32.0f || t > 122.0f) {
    this->reject_("temperature out of range");
    return;
  }
  if (h < 0.0f || h > 100.5f) {
    this->reject_("humidity out of range");
    return;
  }
  if (jumped) {
    this->reject_("jump");
    return;
  }
  if (now - this->last_change_ms_ >= this->frozen_ms_) {
    // Not cleared by time: the outputs stay unknown until a value moves.
    this->reject_("frozen");
    this->last_ok_ms_ = 0;
    return;
  }

  // Condensation heater. Fire straight after a completed read: the SHT4x
  // driver has just finished with the bus, and the next measurement is a
  // full update_interval away.
  if (h >= this->cond_rh_) {
    if (this->cond_since_ms_ == 0)
      this->cond_since_ms_ = now;
    const bool long_enough = now - this->cond_since_ms_ >= this->cond_ms_;
    const bool gap_ok = !this->heater_fired_ || now - this->last_heater_ms_ >= this->heater_gap_ms_;
    if (long_enough && gap_ok) {
      if (this->write(&HEATER_HIGH_1S, 1) == i2c::ERROR_OK) {
        this->heater_fired_ = true;
        this->last_heater_ms_ = now;
        this->heater_pulses_++;
        this->blank_until_ms_ = now + this->blank_ms_;
        // Measure the next stretch at saturation from scratch.
        this->cond_since_ms_ = now;
        ESP_LOGI(TAG, "RH %.1f%% for %u min: condensation heater pulse #%u, ignoring readings for %u s", h,
                 (unsigned) (this->cond_ms_ / 60000), (unsigned) this->heater_pulses_,
                 (unsigned) (this->blank_ms_ / 1000));
      } else {
        ESP_LOGW(TAG, "Condensation heater command failed");
      }
    }
  } else {
    this->cond_since_ms_ = 0;
  }

  // Accepted.
  this->last_ok_ms_ = now;
  this->faulted_ = false;
  this->last_reject_ = nullptr;
  this->out_t_->publish_state(t + this->off_t_);
  float rh = h + this->off_rh_;
  rh = rh < 0.0f ? 0.0f : (rh > 100.0f ? 100.0f : rh);
  this->out_rh_->publish_state(rh);
  this->set_status_(h >= this->cond_rh_ ? "ok (saturated)" : "ok");
}

void ShtGuard::reject_(const char *why) {
  if (this->last_reject_ != why)
    ESP_LOGW(TAG, "Sample rejected: %s (raw %.2f degF, %.1f %%RH)", why, this->raw_t_->state, this->raw_rh_->state);
  this->last_reject_ = why;
}

void ShtGuard::update() {
  const uint32_t now = millis();
  const bool stale = this->last_ok_ms_ == 0 || now - this->last_ok_ms_ > this->stale_ms_;
  if (!stale)
    return;
  if (!this->faulted_) {
    ESP_LOGW(TAG, "No good reading for %u s: outputs unknown (%s)", (unsigned) (this->stale_ms_ / 1000),
             this->last_reject_ != nullptr ? this->last_reject_ : "no samples");
    this->faulted_ = true;
    this->out_t_->publish_state(NAN);
    this->out_rh_->publish_state(NAN);
  }
  std::string s = "FAULT: ";
  s += this->last_reject_ != nullptr ? this->last_reject_ : "no reading";
  this->set_status_(s);
}

void ShtGuard::set_status_(const std::string &s) {
  if (s == this->last_status_)
    return;
  this->last_status_ = s;
  this->status_->publish_state(s);
}

void ShtGuard::dump_config() {
  ESP_LOGCONFIG(TAG, "SHT guard:");
  LOG_I2C_DEVICE(this);
  ESP_LOGCONFIG(TAG,
                "  Stale after %u s, max rate %.1f degF/min, frozen after %u min\n"
                "  Condensation heater at >= %.0f %%RH for %u min, min gap %u min, blank %u s",
                (unsigned) (this->stale_ms_ / 1000), this->max_rate_, (unsigned) (this->frozen_ms_ / 60000),
                this->cond_rh_, (unsigned) (this->cond_ms_ / 60000), (unsigned) (this->heater_gap_ms_ / 60000),
                (unsigned) (this->blank_ms_ / 1000));
}

}  // namespace sht_guard
}  // namespace esphome
