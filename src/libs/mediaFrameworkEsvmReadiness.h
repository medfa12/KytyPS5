#pragma once

#include <cstdint>
#include <limits>

namespace Libs::MediaFramework {

// regular13.60 engine 0x19760..0x19b90. These are native observations,
// not interchangeable host queue lengths. In particular, input_count is the
// ingress queue (including its active element), and free_bytes is AVAILABLE
// allocator space, not encoded bytes in use. See the native-readiness audit.
struct EsvmReadinessObservation {
 uint64_t decoded = 0;
 uint64_t input_count = 0;
 uint64_t buffered = 0;
 uint64_t decoding = 0;
 uint64_t free_bytes = 0;
 uint32_t allocation_state = 0;
 bool input_ended = false;
};

class EsvmReadiness final {
public:
 // The source service currently accepts only rate 1, whose native threshold
 // is 12 irrespective of the rate-control boolean (0x19ae0).
 static constexpr uint64_t NormalRateThreshold = 12;
 static constexpr uint64_t FreeByteThreshold = 0x900000;

 static constexpr bool Sufficient(const EsvmReadinessObservation& value,
                                  uint64_t threshold = NormalRateThreshold) {
  const auto upstream = Add(value.buffered, value.decoding);
  return value.decoded != 0 &&
         (Add(upstream, value.decoded) >= threshold ||
          (upstream >= value.input_count &&
           ((value.allocation_state == 1 && value.free_bytes < FreeByteThreshold) || value.input_ended)));
 }

 void Pause() { flags_ |= 1; has_timestamp_ = false; }
 void Play() { flags_ = 0; Invalidate(); }
 void SyncVideo() { flags_ |= 2; has_timestamp_ = false; }
 void Timestamp(int64_t value) {
  if (flags_ && has_timestamp_ && timestamp_ != value) { flags_ = 0; Invalidate(); }
  timestamp_ = value; has_timestamp_ = true;
 }
 // Host lifecycle discontinuities revoke deferred delivery even when the
 // guest has not yet reentered the service. Preserve only the pause arm.
 void Discontinuity() { flags_ &= 1; has_timestamp_ = false; Invalidate(); }
 void Retire() { flags_ = 0; Invalidate(); }

 void Observe(const EsvmReadinessObservation& value) {
  if (flags_ == 3 && Sufficient(value)) {
   flags_ = 1; // Native consumes the sync arm before invoking the callback.
   Invalidate();
   pending_ = serial_;
  }
 }
 [[nodiscard]] uint64_t Pending() const { return pending_; }
 // Read-only diagnostic state; never arms, observes or claims readiness.
 [[nodiscard]] uint32_t Flags() const { return flags_; }
 [[nodiscard]] bool HasTimestamp() const { return has_timestamp_; }
 [[nodiscard]] uint64_t Revision() const { return serial_; }
 bool Claim(uint64_t ticket) {
  if (!ticket || pending_ != ticket || serial_ != ticket) { return false; }
  pending_ = 0;
  return true;
 }

private:
 static constexpr uint64_t Add(uint64_t left, uint64_t right) {
  return right > std::numeric_limits<uint64_t>::max() - left
         ? std::numeric_limits<uint64_t>::max() : left + right;
 }
 void Invalidate() { pending_ = 0; if (++serial_ == 0) { ++serial_; } }
 uint64_t serial_ = 1, pending_ = 0;
 uint32_t flags_ = 0;
 int64_t timestamp_ = 0;
 bool has_timestamp_ = false;
};
} // namespace Libs::MediaFramework
