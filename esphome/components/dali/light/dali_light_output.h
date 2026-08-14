#pragma once

#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "esphome/components/light/light_traits.h"
#include "esphome/components/light/color_mode.h"
#include "../dali_component.h"  // provides DaliBusLight + DaliComponent
#include "dali_light_state_mailbox.h"
#include "dali_light_command_mailbox.h"
#include "dali_light_profile.h"

#include <cstdint>

extern "C" {
#include "../../../../components/dali/dali_control.h"
#include "../../../../components/dali/dali_dim_curve.h"
#include "../../../../components/dali/dali_frame.h"
#include "../../../../components/dali/dali_light_write.h"
}

namespace esphome {
namespace dali {

class DaliLightOutput : public light::LightOutput, public DaliBusLight {
 public:
  // Called from __init__.py codegen — registers self with the bus component.
  void set_dali_component(DaliComponent *comp);

  void set_target(uint8_t type, uint8_t address) {
    target_.type    = static_cast<DaliAddressType>(type);
    target_.address = address;
  }

  // Optional short address to use for QUERY_ACTUAL_LEVEL (boot/refresh/poll).
  // A short-address entity defaults to its own target; group/broadcast entities
  // require an explicit or scan-derived representative short address.
  void    set_query_address(uint8_t addr) { query_address_ = addr; }
  uint8_t get_query_address() const override {
    if (query_address_ != 0xFFu) return query_address_;
    return target_.type == DALI_ADDR_SHORT ? target_.address : 0xFFu;
  }

  // Bitmask of DALI groups this entity belongs to (bit N = group N).
  // Used by notify_lights to propagate group-addressed dispatch results to
  // short-address entities.  Defaults to 0 (no group membership tracked).
  void set_member_groups(uint16_t groups) { member_groups_ = groups; }

  light::LightTraits get_traits() override {
    auto t = light::LightTraits();
    t.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return t;
  }

  void setup_state(light::LightState *state) override;
  void write_state(light::LightState *state) override;

  // DaliBusLight interface — Core 1 writes, Core 0 drains.
  void mark_state_from_bus(bool is_on, uint8_t level) override;
  void apply_bus_state() override;
  void flush_pending_write() override;
  void begin_level_profile_update(uint32_t generation) override;
  void set_level_profile(const DaliLevelProfile &profile,
                         uint32_t generation) override;

  void set_min_level_override(uint8_t level) {
    min_level_override_ = level;
    has_min_level_override_ = true;
  }
  void set_max_level_override(uint8_t level) {
    max_level_override_ = level;
    has_max_level_override_ = true;
  }
  void set_dimming_curve_override(uint8_t curve) {
    curve_override_ = static_cast<DaliDimCurve>(curve);
    has_curve_override_ = true;
  }

  // Reports query_address_ raw rather than through get_query_address(): the
  // latter answers "who do we ask", which for a short-address entity is itself
  // and for a group entity may be a scan-derived pick. Neither was in the YAML.
  void describe_config(DaliLightConfig *out) const override {
    if (out == nullptr) return;
    out->name              = state_ != nullptr ? state_->get_name().c_str() : "";
    out->target_type       = static_cast<uint8_t>(target_.type);
    out->target_address    = target_.address;
    out->query_address     = query_address_;
    out->member_groups     = member_groups_;
    out->has_min_level     = has_min_level_override_;
    out->min_level         = min_level_override_;
    out->has_max_level     = has_max_level_override_;
    out->max_level         = max_level_override_;
    out->has_dimming_curve = has_curve_override_;
    out->dimming_curve     = static_cast<uint8_t>(curve_override_);
  }

 protected:
  DaliTarget      target_{};
  DaliComponent  *comp_{nullptr};
  uint8_t         query_address_{0xFFu};
  uint16_t        member_groups_{0};

  // Coherent latest bus state — published on Core 1, drained on Core 0.
  DaliLightStateMailbox bus_state_mailbox_;
  // Scheduler completion for the in-flight command — Core 1 publishes,
  // Core 0 drains before it decides what to send next.
  DaliLightCommandMailbox command_mailbox_;

  // Core 0 only — desired/in-flight/confirmed arbitration. The confirmed state
  // it holds is what suppresses a redundant command, and it is committed only
  // from a scheduler completion, never from a successful enqueue.
  DaliLightWrite    write_{};
  // Logical HA intent is retained until the raw command mapped under the same
  // profile generation is confirmed. This prevents a boot/scan profile change
  // from transmitting a queued raw level calculated from an obsolete profile.
  DaliLevelProfile  profile_{DALI_DIM_CURVE_STANDARD, 1u, 254u};
  uint32_t          profile_generation_{0u};
  bool              profile_ready_{false};
  bool              logical_pending_{false};
  bool              logical_is_on_{false};
  float             logical_brightness_{0.0f};
  uint32_t          logical_revision_{0u};
  uint32_t          mapped_revision_{0u};
  uint32_t          mapped_generation_{0u};
  uint32_t          in_flight_revision_{0u};
  uint32_t          in_flight_generation_{0u};

  bool              has_min_level_override_{false};
  bool              has_max_level_override_{false};
  uint8_t           min_level_override_{1u};
  uint8_t           max_level_override_{254u};
  bool              has_curve_override_{false};
  DaliDimCurve      curve_override_{DALI_DIM_CURVE_STANDARD};

  // Core 0 only — tells apart the two write_state() calls that are not an
  // operator's intent. See the comment on write_state().
  //
  // suppress_initial_write_ covers ESPHome's one restore/default write at
  // setup. The echo_* trio is the exact state apply_bus_state() last pushed
  // into ESPHome, so the write it schedules is recognised by its value rather
  // than by a one-shot flag that the wrong call could consume.
  bool              suppress_initial_write_{true};
  bool              echo_valid_{false};
  bool              echo_is_on_{false};
  uint8_t           echo_level_{0};
  uint32_t          echo_profile_generation_{0u};
  light::LightState *state_{nullptr};

  static void clear_unused_color_fields_(light::LightColorValues &values);
  // ESPHome brightness is a fraction of light output; the bus carries arc power
  // levels. dali_dim_curve owns the conversion — see the note on write_state().
  bool brightness_to_level_(float brightness, uint8_t *level) const;
  bool level_to_brightness_(uint8_t level, float *brightness) const;
  void map_logical_request_();
  // Core 1 — scheduler completion for a level/off command.
  static void on_command_complete_(DaliError result, const DaliFrame *reply, void *ctx);
  // Core 0 — collect completions, then admit at most one command.
  void pump_write_();
};

}  // namespace dali
}  // namespace esphome
