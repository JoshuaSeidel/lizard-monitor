#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <string>
#include <vector>

namespace esphome {
namespace enclosure_history {

/// History for an enclosure with no Home Assistant. See __init__.py.
///
/// Each series is sampled every update_interval and averaged into the
/// current wall-clock slot of two rings: 5 min x 288 (24 h) and 1 h x 720
/// (30 days). Values are int16 of value * scale; INT16_MIN is "no data".
/// The slot is the epoch divided by the step, so a ring index is a time and
/// a gap (power off, no clock) stays a visible gap rather than closing up.
class EnclosureHistory : public PollingComponent, public AsyncWebHandler {
 public:
  void set_base(web_server_base::WebServerBase *base) { this->base_ = base; }
  void set_time(time::RealTimeClock *clock) { this->time_ = clock; }
  void add_series(const char *key, sensor::Sensor *sens, float scale, const char *label, const char *chart,
                  const char *unit);

  void setup() override;
  void update() override;
  void on_shutdown() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

  static const int16_t MISSING = INT16_MIN;

 protected:
  struct Series {
    const char *key;
    sensor::Sensor *sensor;
    float scale;
    const char *label;
    const char *chart;
    const char *unit;
  };

  // A ring of means. data is [series][slot % slots]. last_slot is the slot
  // being accumulated now; its mean is only written to data when the slot
  // closes (or on save, as a partial).
  struct Ring {
    uint32_t step;
    uint32_t slots;
    uint32_t last_slot{0};
    std::vector<int16_t> data;
    std::vector<double> sum;
    std::vector<uint32_t> count;
    ESPPreferenceObject pref;
  };

  void init_ring_(Ring &r, uint32_t step, uint32_t slots, uint32_t hash);
  void advance_(Ring &r, uint32_t slot);
  void commit_(Ring &r, bool reset);
  void save_ring_(Ring &r);
  bool load_ring_(Ring &r);
  int16_t pack_(float v, float scale) const;
  void history_json_(AsyncWebServerRequest *request, Ring &r);
  void charts_page_(AsyncWebServerRequest *request);
  uint32_t now_() const;

  web_server_base::WebServerBase *base_{nullptr};
  time::RealTimeClock *time_{nullptr};
  std::vector<Series> series_;
  Ring fast_;
  Ring slow_;
  uint32_t last_save_{0};
};

}  // namespace enclosure_history
}  // namespace esphome
