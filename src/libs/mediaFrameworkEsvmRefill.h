#pragma once

#include <cstdint>
#include "libs/mediaFrameworkEsvmDecoder.h"

namespace Libs::MediaFramework {

struct EsvmRefillCapacity {
 uint64_t generation = 0;
 size_t available_packets = 0, available_bytes = 0;
 bool can_refill = false;
};
// These are the SAME limits passed to the owned production decoder, and its
// actual Status snapshot. Audio's additional 1 MiB encoded queue bound is enforced
// in Consume. Video input credit is separate from decoded-output capacity:
// paused output must not prevent bounded prefetch from reaching native readiness.
inline EsvmRefillCapacity ReadEsvmRefillCapacity(const EsvmDecoderStatus& status, const EsvmDecoderLimits& limits) {
 EsvmRefillCapacity out {status.generation};
 size_t byte_limit = limits.max_queued_packet_bytes, packet_limit = limits.max_packet_bytes;
 if (EsvmIsAudioCodec(status.codec)) {
  if (byte_limit > 1024 * 1024) { byte_limit = 1024 * 1024; }
  if (packet_limit > 1024 * 1024) { packet_limit = 1024 * 1024; }
 } else if (!EsvmIsVideoCodec(status.codec)) { return out; }
 if (status.queued_packets > limits.max_queued_packets || status.queued_packet_bytes > byte_limit) { return out; }
 out.available_packets = limits.max_queued_packets - status.queued_packets;
 out.available_bytes = byte_limit - status.queued_packet_bytes;
 out.can_refill = status.state == EsvmDecoderState::Ready && status.failure == EsvmDecoderResult::Ok && !status.eos_accepted &&
                  !status.configuration_pending &&
                  (EsvmIsVideoCodec(status.codec) ||
                   (!status.held_codec_frame && status.queued_frames < limits.max_queued_frames)) &&
                  out.available_packets != 0 && packet_limit != 0 && out.available_bytes >= packet_limit;
 return out;
}

// Source-owned notification state, NOT a capacity estimator or a readiness
// predicate. Observations come from real packet status + its enforced limits.
// Serialized by the source owner gate; it never invokes guest code or allocates.
struct EsvmRefillToken {
 uint64_t generation = 0, revision = 0;
 explicit operator bool() const { return generation != 0 && revision != 0; }
};
class EsvmRefillCredit final {
public:
 void Observe(uint64_t generation, bool can_refill) {
  if (m_retired || !generation || generation < m_generation) { return; }
  if (generation != m_generation) {
   m_generation = generation;
   if (!Advance()) { return; }
   m_armed = true;
  }
  if (m_pending && !can_refill && !Advance()) { return; }
  m_pending = m_armed && can_refill;
 }
 // Native clears its outstanding-notification gate on accepted input work's
 // completion. Rejected input/polls must NOT call this. Capacity is reevaluated
 // separately after the packet operation; accepting bytes does not imply free
 // space, codec readiness, decoded output, EOS, or presentation success.
 void Accepted(uint64_t generation) {
  if (m_retired || !generation || generation < m_generation) { return; }
  m_generation = generation;
  if (!Advance()) { return; }
  m_armed = true;
  m_pending = false;
 }
 [[nodiscard]] EsvmRefillToken Pending() const {
  return m_pending && !m_retired ? EsvmRefillToken {m_generation, m_revision} : EsvmRefillToken {};
 }
 // Claim BEFORE entering guest code: a recursive entry cannot deliver the same
 // credit again. A previously captured ticket fails after rearm/new generation.
 bool Claim(EsvmRefillToken token) {
  if (m_retired || !m_pending || !token || token.generation != m_generation || token.revision != m_revision) { return false; }
  m_pending = false; m_armed = false;
  return true;
 }
 void Retire() { m_retired = true; m_pending = false; m_armed = false; }

private:
 bool Advance() {
  if (m_revision == UINT64_MAX) { Retire(); return false; } // Never wrap/revive an old ticket.
  ++m_revision;
  m_pending = false;
  return true;
 }
 uint64_t m_generation = 0, m_revision = 0;
 bool m_armed = true, m_pending = false, m_retired = false;
};

} // namespace Libs::MediaFramework
