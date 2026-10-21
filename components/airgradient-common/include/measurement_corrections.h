/**
 * AirGradient measurement correction types and transforms.
 */

#ifndef MEASUREMENT_CORRECTIONS_H
#define MEASUREMENT_CORRECTIONS_H

#include <cstdint>

#include "measures_types.h"

enum class Pm25CorrectionAlgorithm : uint8_t {
  None,
  Epa2021,
  CustomViaPm25Raw,
  FactoryCalibration,
};

enum class LinearCorrectionAlgorithm : uint8_t {
  None,
  Custom,
};

struct Pm25Correction {
  Pm25CorrectionAlgorithm algorithm = Pm25CorrectionAlgorithm::None;
  float scaling_factor = 1.0f;
  float intercept = 0.0f;
  bool use_epa2021 = false;
};

struct LinearCorrection {
  LinearCorrectionAlgorithm algorithm = LinearCorrectionAlgorithm::None;
  float scaling_factor = 1.0f;
  float intercept = 0.0f;
};

struct MeasurementCorrections {
  Pm25Correction pm25{};
  LinearCorrection temperature{};
  LinearCorrection humidity{};
};

bool is_pm25_correction_valid(const Pm25Correction &correction);
bool is_linear_correction_valid(const LinearCorrection &correction);
bool are_measurement_corrections_valid(const MeasurementCorrections &corrections);
bool measurement_corrections_equal(const MeasurementCorrections &lhs,
                                   const MeasurementCorrections &rhs);

/// Return a derived view of @p raw. Storage and cloud callers must retain @p raw.
MeasuresAGo apply_measurement_corrections(const MeasuresAGo &raw,
                                          const MeasurementCorrections &corrections);

#endif // MEASUREMENT_CORRECTIONS_H
