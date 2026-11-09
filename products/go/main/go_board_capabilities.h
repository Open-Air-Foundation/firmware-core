#pragma once

#include <cstdint>
#include <initializer_list>

enum class BoardVariant : uint8_t { V1 = 1, V2 = 2 };

enum class BoardFeature : uint32_t {
  TouchLed = 1U << 0,
  BatteryCalibration = 1U << 1,
};

/// Features supported by the board design, independent of runtime hardware health.
class BoardCapabilities {
public:
  constexpr BoardCapabilities() = default;

  constexpr BoardCapabilities(std::initializer_list<BoardFeature> features) {
    for (const auto feature : features) {
      _features |= static_cast<uint32_t>(feature);
    }
  }

  constexpr bool supports(BoardFeature feature) const {
    return (_features & static_cast<uint32_t>(feature)) != 0;
  }

private:
  uint32_t _features = 0;
};

/// Board identity is normalized by the BSP before selecting capabilities.
constexpr BoardCapabilities board_capabilities(BoardVariant variant) {
  switch (variant) {
  case BoardVariant::V1:
    return {BoardFeature::TouchLed, BoardFeature::BatteryCalibration};
  case BoardVariant::V2:
    return {};
  }
  return {};
}
