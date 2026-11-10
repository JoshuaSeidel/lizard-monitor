#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include <cmath>
#include <string>

namespace esphome {
namespace sht_guard {

/// Range / rate / frozen / stale guard and condensation-heater control for
/// one SHT4x. See __init__.py.
class ShtGuard : public PollingComponent, public i2c::I2CDevice {
 public:
  void set_raw(sensor::Sensor *t, sensor::Sensor *rh) {
    this->raw_t_ = t;
    this->raw_rh_ = rh;
  }
  void set_outputs(sensor::Sensor *t, sensor::Sensor *rh, text_sensor::TextSensor *status) {
    this->out_t_ = t;
    this->out_rh_ = rh;
    this->status_ = status;
  }
  void set_stale_timeout(uint32_t ms) { this->stale_ms_ = ms; }
  void set_max_rate(float f_per_min) { this->max_rate_ = f_per_min; }
  void set_frozen_time(uint32_t ms) { this->frozen_ms_ = ms; }
  void set_condensation(float rh, uint32_t ms) {
    this->cond_rh_ = rh;
    this->cond_ms_ = ms;
  }
  void set_heater(uint32_t min_gap_ms, uint32_t blank_ms) {
    this->heater_gap_ms_ = min_gap_ms;
    this->blank_ms_ = blank_ms;
  }
  /// Calibration, from the number entities. Applied to the next sample.
  void set_offsets(float t_f, float rh) {
    this->off_t_ = std::isnan(t_f) ? 0.0f : t_f;
    this->off_rh_ = std::isnan(rh) ? 0.0f : rh;
  }

  /// True while the outputs carry a reading the controller may act on.
  bool is_fresh() const { return !this->faulted_ && this->last_ok_ms_ != 0; }
  /// Whether the condensation heater has fired since boot (diagnostics).
  uint32_t heater_pulses() const { return this->heater_pulses_; }

  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  void on_sample_();
  void reject_(const char *why);
  void set_status_(const std::string &s);

  sensor::Sensor *raw_t_{nullptr};
  sensor::Sensor *raw_rh_{nullptr};
  sensor::Sensor *out_t_{nullptr};
  sensor::Sensor *out_rh_{nullptr};
  text_sensor::TextSensor *status_{nullptr};

  uint32_t stale_ms_{120000};
  float max_rate_{5.0f};
  uint32_t frozen_ms_{1800000};
  float cond_rh_{98.0f};
  uint32_t cond_ms_{600000};
  uint32_t heater_gap_ms_{300000};
  uint32_t blank_ms_{120000};
  float off_t_{0.0f};
  float off_rh_{0.0f};

  // Previous RAW sample, accepted or not -- the rate check compares
  // neighbours, so a lone spike is dropped and a real step gets through on
  // the sample after it.
  bool have_prev_{false};
  float prev_t_{NAN};
  uint32_t prev_ms_{0};
  // Frozen detection: when either raw value last changed.
  float last_raw_t_{NAN};
  float last_raw_rh_{NAN};
  uint32_t last_change_ms_{0};
  // Condensation heater.
  uint32_t cond_since_ms_{0};
  uint32_t last_heater_ms_{0};
  uint32_t blank_until_ms_{0};
  bool heater_fired_{false};
  uint32_t heater_pulses_{0};

  uint32_t last_ok_ms_{0};
  bool faulted_{true};
  const char *last_reject_{"no reading yet"};
  std::string last_status_;
};

}  // namespace sht_guard
}  // namespace esphome
