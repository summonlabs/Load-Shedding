#include "load_shedding/units.hpp"

#include <cstdlib>
#include <limits>

namespace load_shedding {
namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::uint64_t kUint64Max = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t kMagnitudeLimit = 0x8000000000000000ULL;

std::uint64_t magnitude(std::int64_t value) noexcept {
  if (value >= 0) {
    return static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(-(value + 1)) + 1ULL;
}

std::uint64_t greatest_common_divisor(std::uint64_t left, std::uint64_t right) noexcept {
  while (right != 0) {
    const std::uint64_t remainder = left % right;
    left = right;
    right = remainder;
  }
  return left;
}

}  // namespace

Result<std::int64_t> checked_add(std::int64_t left, std::int64_t right) {
  if (right > 0 && left > kInt64Max - right) {
    return Status::error(StatusCode::Overflow, "signed 64-bit addition overflow");
  }
  if (right < 0 && left < kInt64Min - right) {
    return Status::error(StatusCode::Overflow, "signed 64-bit addition underflow");
  }
  return left + right;
}

Result<std::int64_t> checked_sub(std::int64_t left, std::int64_t right) {
  if (right > 0 && left < kInt64Min + right) {
    return Status::error(StatusCode::Overflow, "signed 64-bit subtraction underflow");
  }
  if (right < 0 && left > kInt64Max + right) {
    return Status::error(StatusCode::Overflow, "signed 64-bit subtraction overflow");
  }
  return left - right;
}

Result<std::int64_t> checked_mul(std::int64_t left, std::int64_t right) {
  if (left == 0 || right == 0) {
    return std::int64_t{0};
  }
  const bool negative = (left < 0) != (right < 0);
  const std::uint64_t left_magnitude = magnitude(left);
  const std::uint64_t right_magnitude = magnitude(right);
  if (right_magnitude > kUint64Max / left_magnitude) {
    return Status::error(StatusCode::Overflow, "signed 64-bit multiplication overflow");
  }
  const std::uint64_t product = left_magnitude * right_magnitude;
  if (negative) {
    if (product > kMagnitudeLimit) {
      return Status::error(StatusCode::Overflow, "signed 64-bit multiplication underflow");
    }
    if (product == kMagnitudeLimit) {
      return kInt64Min;
    }
    return -static_cast<std::int64_t>(product);
  }
  if (product > static_cast<std::uint64_t>(kInt64Max)) {
    return Status::error(StatusCode::Overflow, "signed 64-bit multiplication overflow");
  }
  return static_cast<std::int64_t>(product);
}

Result<std::int64_t> checked_mul_div(std::int64_t left, std::int64_t numerator,
                                     std::int64_t denominator, bool require_exact) {
  if (denominator == 0) {
    return Status::error(StatusCode::InvalidArgument, "division by zero");
  }
  if (numerator == 0 || left == 0) {
    return std::int64_t{0};
  }
  std::uint64_t numerator_magnitude = magnitude(numerator);
  std::uint64_t denominator_magnitude = magnitude(denominator);
  const std::uint64_t divisor = greatest_common_divisor(numerator_magnitude, denominator_magnitude);
  if (divisor != 0) {
    numerator_magnitude /= divisor;
    denominator_magnitude /= divisor;
  }
  const std::uint64_t left_magnitude = magnitude(left);
  if (numerator_magnitude != 0 && left_magnitude > kUint64Max / numerator_magnitude) {
    return Status::error(StatusCode::Overflow, "checked multiply-divide overflow");
  }
  const std::uint64_t product = left_magnitude * numerator_magnitude;
  const bool negative = ((left < 0) != (numerator < 0)) != (denominator < 0);
  const std::uint64_t quotient = product / denominator_magnitude;
  if (require_exact && product % denominator_magnitude != 0) {
    return Status::error(StatusCode::OutOfRange, "checked multiply-divide is not exact");
  }
  if (negative) {
    if (quotient > kMagnitudeLimit) {
      return Status::error(StatusCode::Overflow, "checked multiply-divide underflow");
    }
    if (quotient == kMagnitudeLimit) {
      return kInt64Min;
    }
    return -static_cast<std::int64_t>(quotient);
  }
  if (quotient > static_cast<std::uint64_t>(kInt64Max)) {
    return Status::error(StatusCode::Overflow, "checked multiply-divide overflow");
  }
  return static_cast<std::int64_t>(quotient);
}

Result<Power> checked_add(Power left, Power right) {
  auto sum = checked_add(left.watts(), right.watts());
  if (!sum.ok()) {
    return sum.status();
  }
  return Power::from_watts(sum.value());
}

Result<Power> checked_sub(Power left, Power right) {
  auto difference = checked_sub(left.watts(), right.watts());
  if (!difference.ok()) {
    return difference.status();
  }
  return Power::from_watts(difference.value());
}

Result<Power> checked_mul(Power value, std::int64_t factor) {
  auto product = checked_mul(value.watts(), factor);
  if (!product.ok()) {
    return product.status();
  }
  return Power::from_watts(product.value());
}

Result<Power> scale_ppm(Power value, std::uint32_t ppm) {
  if (ppm > limits::kMaxPartsPerMillion) {
    return Status::error(StatusCode::OutOfRange,
                         "parts-per-million value " + std::to_string(ppm) + " exceeds " +
                             std::to_string(limits::kMaxPartsPerMillion));
  }
  auto scaled = checked_mul_div(value.watts(), static_cast<std::int64_t>(ppm),
                                static_cast<std::int64_t>(limits::kMaxPartsPerMillion), false);
  if (!scaled.ok()) {
    return scaled.status();
  }
  return Power::from_watts(scaled.value());
}

Result<Power> positive_difference(Power left, Power right) {
  auto difference = checked_sub(left, right);
  if (!difference.ok()) {
    return difference.status();
  }
  if (difference.value().is_negative()) {
    return Power::zero();
  }
  return difference.value();
}

std::string Power::to_string() const { return std::to_string(watts_) + "W"; }

Result<Power> parse_power(std::string_view text) {
  if (text.empty()) {
    return Status::error(StatusCode::InvalidArgument, "power quantity is empty");
  }
  std::int64_t value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return Status::error(StatusCode::InvalidArgument,
                           "power quantity '" + std::string(text) + "' is not a decimal integer");
    }
    auto scaled = checked_mul(value, 10);
    if (!scaled.ok()) {
      return scaled.status();
    }
    auto shifted = checked_add(scaled.value(), static_cast<std::int64_t>(digit - '0'));
    if (!shifted.ok()) {
      return shifted.status();
    }
    value = shifted.value();
    if (value > limits::kMaxPowerWatts) {
      return Status::error(StatusCode::OutOfRange,
                           "power quantity '" + std::string(text) + "' exceeds the maximum of " +
                               std::to_string(limits::kMaxPowerWatts) + "W");
    }
  }
  return Power::from_watts(value);
}

}  // namespace load_shedding
