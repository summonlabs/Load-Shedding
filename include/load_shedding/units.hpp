#pragma once

// Exact integer power quantities. Load Shedding never uses floating point for an
// authoritative quantity: every addition, subtraction, and scaling is checked and
// either produces an exact result or refuses the operation.

#include <cstdint>
#include <string>
#include <string_view>

#include "load_shedding/limits.hpp"
#include "load_shedding/status.hpp"

namespace load_shedding {

/// Signed 64-bit watt quantity. Capacities, deficits, and headroom are
/// non-negative in authoritative positions; intermediate arithmetic may be
/// negative and is always checked.
class Power {
 public:
  constexpr Power() noexcept = default;
  static constexpr Power from_watts(std::int64_t watts) noexcept { return Power(watts); }
  static constexpr Power zero() noexcept { return Power(0); }

  constexpr std::int64_t watts() const noexcept { return watts_; }
  constexpr bool is_zero() const noexcept { return watts_ == 0; }
  constexpr bool is_negative() const noexcept { return watts_ < 0; }

  /// `"<watts>W"`.
  std::string to_string() const;

  friend constexpr bool operator==(Power a, Power b) noexcept { return a.watts_ == b.watts_; }
  friend constexpr bool operator!=(Power a, Power b) noexcept { return a.watts_ != b.watts_; }
  friend constexpr bool operator<(Power a, Power b) noexcept { return a.watts_ < b.watts_; }
  friend constexpr bool operator<=(Power a, Power b) noexcept { return a.watts_ <= b.watts_; }
  friend constexpr bool operator>(Power a, Power b) noexcept { return a.watts_ > b.watts_; }
  friend constexpr bool operator>=(Power a, Power b) noexcept { return a.watts_ >= b.watts_; }

 private:
  constexpr explicit Power(std::int64_t watts) noexcept : watts_(watts) {}
  std::int64_t watts_ = 0;
};

/// Checked signed 64-bit addition. Refuses overflow instead of wrapping.
Result<std::int64_t> checked_add(std::int64_t left, std::int64_t right);
/// Checked signed 64-bit subtraction. Refuses overflow instead of wrapping.
Result<std::int64_t> checked_sub(std::int64_t left, std::int64_t right);
/// Checked signed 64-bit multiplication. Refuses overflow instead of wrapping.
Result<std::int64_t> checked_mul(std::int64_t left, std::int64_t right);
/// Checked `left * numerator / denominator` with the intermediate product held
/// exactly. Refuses overflow, a zero denominator, or a non-exact division when
/// `require_exact` is set.
Result<std::int64_t> checked_mul_div(std::int64_t left, std::int64_t numerator,
                                     std::int64_t denominator, bool require_exact);

Result<Power> checked_add(Power left, Power right);
Result<Power> checked_sub(Power left, Power right);
Result<Power> checked_mul(Power value, std::int64_t factor);

/// Scales `value` by a parts-per-million ratio, rounding down (truncating toward
/// zero). Values are non-negative in every authoritative use; the helper is exact
/// for the full signed 64-bit input range.
Result<Power> scale_ppm(Power value, std::uint32_t ppm);

/// Saturating non-negative difference: `max(left - right, 0)`.
Result<Power> positive_difference(Power left, Power right);

/// Parses a decimal watt quantity. Rejects signs, leading `+`, empty input,
/// non-digits, and values beyond `limits::kMaxPowerWatts`.
Result<Power> parse_power(std::string_view text);

}  // namespace load_shedding
