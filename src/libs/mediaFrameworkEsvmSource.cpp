#include "libs/mediaFrameworkEsvmSource.h"

#include "common/singleton.h"
#include "common/threads.h"
#include "graphics/presentation/nativeVideo.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "libs/libs.h"
#include "libs/audio_internal.h"
#include "libs/mediaFrameworkEsvm.h"
#include "loader/elf.h"
#include "loader/runtimeLinker.h"
#include "loader/runtimeProcessIdentity.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <system_error>
#include <thread>

namespace Libs::MediaFramework {
LIB_VERSION("MediaFrameworkInterface", 1, "MediaFrameworkInterface", 1, 1);
namespace {
constexpr int Invalid = static_cast<int>(0x81790001u);
constexpr int Full = static_cast<int>(0x81790007u);
constexpr int Failed = static_cast<int>(0x81790fffu);
constexpr int InputNotStarted = static_cast<int>(0x81780002u);
constexpr size_t ContextLimit = 8, DecoderLimit = 8, ChildrenLimit = 2, PendingLimit = 8;
namespace Fs = LibKernel::FileSystem;
namespace Nv = Graphics;
namespace Ai = Audio::AudioInternal;
using Clock = std::chrono::steady_clock;
using PacketResult = EsvmDecoderResult;
#if !defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
enum class EsvmSourceFailure { None, Allocate, ShmOpen, ShmResize, ShmMap, Publish };
#endif
thread_local EsvmSourceFailure failure = EsvmSourceFailure::None;
bool Fail(EsvmSourceFailure point) {
 if (failure != point) { return false; }
 failure = EsvmSourceFailure::None;
 return true;
}
void Allocate() { if (Fail(EsvmSourceFailure::Allocate)) { throw std::bad_alloc(); } }
void Trace(const char* operation, int handle, uint64_t bytes, int result, int detail = 0, int owner_pid = 0) {
 static const bool enabled = [] { const char* v = std::getenv("KYTY_MEDIA_TRACE"); return v && std::strcmp(v, "1") == 0; }();
 if (!enabled) { return; }
 // Polling Render must never spend the packet/lifecycle evidence budget. Keep
 // the first observed AU header independently visible even after config retries.
 enum Group : size_t { Lifecycle, Render, Submit, AccessUnit, Frames, Audio, Refills, Colour, Progress, Count };
 const auto group = std::strncmp(operation, "render-", 7) == 0 ? Render :
                    std::strcmp(operation, "source-progress-delivered") == 0 ? Progress :
                    std::strcmp(operation, "submit-wire-au") == 0 ? AccessUnit :
                    std::strncmp(operation, "submit-", 7) == 0 ? Submit :
                    std::strncmp(operation, "audio-", 6) == 0 ? Audio :
                     std::strncmp(operation, "decoder-refill-", 15) == 0 ? Refills :
                     std::strcmp(operation, "decoded-colour-rejected") == 0 ? Colour :
                    (std::strcmp(operation, "decoded-frames-queued") == 0 ||
                     std::strncmp(operation, "mailbox-allocation-", 19) == 0) ? Frames : Lifecycle;
 static std::array<std::atomic_uint, Count> counts {};
 constexpr std::array<unsigned, Count> limits {128, 16, 256, 64, 32, 64, 64, 16, 64};
 auto& counter = counts[group];
 auto count = counter.load(std::memory_order_relaxed);
 while (count < limits[group]) {
  if (!counter.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) { continue; }
  LOGF("MediaFramework source: operation=%s pid=%d handle=%d bytes=%llu result=%08x detail=%d\n",
       operation, owner_pid > 0 ? owner_pid : Loader::GetCurrentGuestProcessId(), handle, static_cast<unsigned long long>(bytes),
       static_cast<unsigned>(result), detail);
  break;
 }
}
void TraceAacConfiguration(int handle, std::span<const uint8_t> snapshot) {
 static const bool enabled = [] { const char* v = std::getenv("KYTY_MEDIA_TRACE"); return v && std::strcmp(v, "1") == 0; }();
 if (!enabled || snapshot.size() != 32) { return; }
 uint32_t magic = 0, subtype = 0; uint64_t length = 0, config_size = 0;
 std::memcpy(&magic, snapshot.data(), 4); std::memcpy(&subtype, snapshot.data() + 4, 4);
 std::memcpy(&length, snapshot.data() + 8, 8);
 std::memcpy(&config_size, snapshot.data() + 16, 8);
 // Exact whole fixed config record ONLY, after safe admission copy. Never walk
 // AU lists, inspect encoded/protected bytes, or dump surrounding guest memory.
 if (magic != 100 || subtype != 4 || length != 16 || config_size != 16) { return; }
 static std::atomic_uint captures {0};
 auto count = captures.load(std::memory_order_relaxed);
 while (count < 4) {
  if (!captures.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) { continue; }
  constexpr char digits[] = "0123456789abcdef";
  char hex[33] {};
  for (size_t i = 0; i < 16; ++i) {
   const auto value = snapshot[16 + i]; hex[i * 2] = digits[value >> 4]; hex[i * 2 + 1] = digits[value & 15];
  }
  uint32_t rate = 0, container = 0;
  std::memcpy(&rate, snapshot.data() + 24, 4); std::memcpy(&container, snapshot.data() + 28, 4);
  LOGF("MediaFramework source: operation=aac-config-fixed pid=%d handle=%d bytes=16 rate=%u container=%u payload_hex=%s\n",
       Loader::GetCurrentGuestProcessId(), handle, rate, container, hex);
  break;
 }
}
const char* PacketResultName(PacketResult result) {
 switch (result) {
  case PacketResult::Ok: return "ok";
  case PacketResult::Backpressure: return "backpressure";
  case PacketResult::Yield: return "yield";
  case PacketResult::InvalidLimits: return "invalid-limits";
  case PacketResult::UnsupportedSchema: return "unsupported-schema";
  case PacketResult::InvalidWire: return "invalid-wire";
  case PacketResult::UnsupportedCodec: return "unsupported-codec";
  case PacketResult::UnsupportedConfig: return "unsupported-config";
  case PacketResult::UnsupportedAccessUnit: return "unsupported-access-unit";
  case PacketResult::ProtectedContent: return "protected-content";
  case PacketResult::InvalidState: return "invalid-state";
  case PacketResult::SequenceMismatch: return "sequence-mismatch";
  case PacketResult::UnsupportedTimestamp: return "unsupported-timestamp";
  case PacketResult::LimitExceeded: return "limit-exceeded";
  case PacketResult::AllocationFailed: return "allocation-failed";
  case PacketResult::DecodeFailed: return "decode-failed";
  case PacketResult::UnsupportedPixelFormat: return "unsupported-pixel-format";
  case PacketResult::CodecProtocolError: return "codec-protocol-error";
  case PacketResult::UnsupportedSampleFormat: return "unsupported-sample-format";
 }
 return "unknown";
}
void TraceSubmissionFailure(int handle, uint64_t bytes, int result, const char* stage,
                            int detail, const EsvmDecoderStatus& status, std::span<const uint8_t> snapshot) {
 if (!result || result == Full) { return; } // Queue pressure is not a fatal media rejection.
 // Esvm maps backend errors to 0x8a790001. Preserve the actual cause even when
 // verbose media tracing is off, with a separate bounded error-only budget.
 static std::atomic_uint captures {0};
 auto count = captures.load(std::memory_order_relaxed);
 while (count < 32) {
  if (!captures.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) { continue; }
  uint32_t magic = 0, subtype = UINT32_MAX, rate = 0, container = UINT32_MAX;
  uint64_t length = 0, config_size = 0;
  // Only safely copied framing/fixed AAC configuration, never encoded AUs,
  // guest pointers, key material, or an unvalidated variable-length record.
  if (snapshot.size() >= 16) {
   std::memcpy(&magic, snapshot.data(), 4);
   std::memcpy(&subtype, snapshot.data() + 4, 4);
   std::memcpy(&length, snapshot.data() + 8, 8);
  }
  if (magic == 100 && subtype == 4 && length == 16 && snapshot.size() == 32) {
   std::memcpy(&config_size, snapshot.data() + 16, 8);
   if (config_size == 16) {
    std::memcpy(&rate, snapshot.data() + 24, 4);
    std::memcpy(&container, snapshot.data() + 28, 4);
   }
  }
  LOGF("MediaFramework source failure: pid=%d handle=%d bytes=%llu stage=%s result=%08x "
       "packet=%s codec=%d ffmpeg=%d magic=%u subtype=%u payload_bytes=%llu aac_rate=%u aac_container=%u\n",
       Loader::GetCurrentGuestProcessId(), handle, static_cast<unsigned long long>(bytes), stage,
       static_cast<unsigned>(result), PacketResultName(static_cast<PacketResult>(detail)),
       static_cast<int>(status.codec), status.ffmpeg_error, magic, subtype,
       static_cast<unsigned long long>(length), rate, container);
  break;
 }
}
template<class T> bool Field(const void* base, size_t offset, T* out) {
 const auto address = reinterpret_cast<uintptr_t>(base);
 return address != 0 && offset <= UINTPTR_MAX - address &&
        EsvmReadGuest(out, reinterpret_cast<const void*>(address + offset), sizeof(T));
}
template<class T> T Load(const std::array<uint8_t, EsvmNativeSourceBytes>& config, size_t offset) {
 T value {}; std::memcpy(&value, config.data() + offset, sizeof(value)); return value;
}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
std::atomic<EsvmSourceProvenanceProbe> provenance_probe {nullptr};
#endif
std::mutex provenance_mutex;
bool Provenance(uint64_t source, uint64_t event = 0, uint64_t complete = 0) {
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
 if (const auto probe = provenance_probe.load()) { return probe(source, event, complete); }
#endif
 auto* linker = Common::Singleton<Loader::RuntimeLinker>::Instance();
 if (!linker) { return false; }
 auto* program = linker->FindProgramByAddr(source);
 if (!program || !program->elf || source != program->base_vaddr + 0x24ec0) { return false; }
 const auto* current = Loader::RuntimeLinker::GetCurrentProgram();
 if (program->application_owner && (!current || program->application_owner != current->application_owner)) { return false; }
 if ((event && event != program->base_vaddr + 0x253a0) ||
     (complete && complete != program->base_vaddr + 0x25470)) { return false; }
 // Hash the same open backing actually loaded, NOT a filename/version setting,
 // AU size, user option or any unrelated firmware on disk. No 4.03 parser guess.
 // Keep the identity as text and decode exactly 32 bytes once per admission.
 constexpr char identity[] = "31593167e10813bdc195fb099da1da723c36818c063c35de6f6a086962d2e3f2";
 static_assert(sizeof(identity) == 65);
 uint8_t expected[32] {};
 auto hex = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
 for (size_t i = 0; i < 32; ++i) { expected[i] = static_cast<uint8_t>((hex(identity[i * 2]) << 4) | hex(identity[i * 2 + 1])); }
 std::lock_guard backing_lock(provenance_mutex); // Shared module's seekable backing across owner gates.
 return program->elf->MatchesFileSha256(expected, 2 * 1024 * 1024);
}
struct Mapping {
 int descriptor = -1;
 void* address = nullptr;
 bool Reset() {
  if (address && LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(address), EsvmDecoderMappingBytes) == 0) { address = nullptr; }
  if (descriptor >= 0 && Fs::KernelClose(descriptor) == 0) { descriptor = -1; }
  return !address && descriptor < 0;
 }
 bool Create() {
  if (Fail(EsvmSourceFailure::ShmOpen)) { return false; }
  descriptor = Fs::KernelShmOpenAnonymous(2, 0600);
  if (descriptor < 0 || Fail(EsvmSourceFailure::ShmResize) || Fs::KernelFtruncate(descriptor, EsvmDecoderMappingBytes) != 0) { return false; }
  if (Fail(EsvmSourceFailure::ShmMap) || Fs::KernelMmap(nullptr, EsvmDecoderMappingBytes, 3, 1, descriptor, 0, &address) != 0) { return false; }
  if (Fs::KernelClose(descriptor) != 0) { return false; }
  descriptor = -1; // Actual mapping retains the anonymous SHM backing.
  return true;
 }
};
struct Context {
 int handle = 0;
 bool live = true, playing = false, muted = false;
 bool input_started = false;
 double rate = 1, audible_rate = 1, volume = 1;
 int64_t elapsed_us = 0;
 Clock::time_point anchor {};
 std::array<float, 4> rectangle {};
 bool rectangle_set = false;
 EsvmSourceCallbacks24 callbacks {};
 EsvmReadiness readiness;
 int readiness_decoder = 0;
 uint64_t readiness_generation = 0;
 int progress_decoder = 0;
 uint64_t progress_generation = 0, progress_serial = 0;
 size_t readiness_output_count = 0;
 bool readiness_waiting_output = false;
 uintptr_t trace_caller = 0;
 const char* trace_entry = "internal";
 unsigned trace_count = 0;
 std::array<uint64_t, 39> trace_state {};
 bool trace_seen = false;
 std::array<uint8_t, EsvmNativeSourceBytes> config {};
};
struct Decoder {
 int handle = 0;
 bool live = false;
 int service_failure = 0;
 Mapping mapping;
 EsvmDecoderCallbacks24 callbacks {};
 std::shared_ptr<Context> context;
 std::unique_ptr<EsvmPacketDecoder> packets;
 EsvmDecoderLimits limits;
 EsvmRefillCredit refill;
 std::shared_ptr<Ai::NativeOutput> audio;
 EsvmDecodedAudio staged_audio;
 uint64_t audio_generation = 0;
 uint32_t audio_rate = 0, audio_channels = 0;
 Clock::time_point audio_retry_after {};
 uint64_t audio_accepted_frames = 0;
 uint64_t audio_input_ended_generation = 0;
 Ai::NativeQueueResult audio_result = Ai::NativeQueueResult::Unavailable;
 std::shared_ptr<Nv::NativeVideoStream> video;
 Nv::NativeVideoFrame staged;
 uint64_t video_generation = 0;
 bool clock_started = false;
 int64_t origin_pts = 0, origin_played_us = 0;
 uint64_t trace_batch = 0, trace_units = 0, trace_subtype = 0, trace_result = 0;
 std::array<uint64_t, 39> trace_state {};
 bool trace_seen = false;
};
struct Owner {
 int pid;
 uint64_t epoch;
 std::atomic_bool live {true};
 std::shared_ptr<std::recursive_mutex> dispatch;
 size_t callbacks = 0, pending = 0;
 bool refill_dispatching = false;
 std::array<std::shared_ptr<Context>, ContextLimit> contexts {};
 std::array<std::shared_ptr<Decoder>, DecoderLimit> decoders {};
 // Atomic mailbox leases allow nonblocking lifecycle invalidation without
 // reading decoder state under a gate held by a guest callback.
 std::array<std::atomic<std::shared_ptr<Nv::NativeVideoStream>>, DecoderLimit> video_leases {};
 std::array<std::atomic<std::shared_ptr<Ai::NativeOutput>>, DecoderLimit> audio_leases {};
 std::shared_ptr<Owner> retired_next;
 Owner(int p, uint64_t e, std::shared_ptr<std::recursive_mutex> d): pid(p), epoch(e), dispatch(std::move(d)) {}
};
struct Registry {
 std::mutex mutex;
 std::map<int, std::shared_ptr<Owner>> owners;
 std::shared_ptr<Owner> retired;
 // Backend has at most eight live/closing leases. This allocation-free close
 // queue owns detached leases until Close outside registry/lifecycle/owner gates.
 std::array<std::shared_ptr<Ai::NativeOutput>, 8> closing_audio {};
 std::atomic_size_t closing_audio_count {0};
 std::mutex scheduler_mutex;
 std::condition_variable scheduler_wake;
 std::thread scheduler;
 std::atomic_bool scheduler_stopping {false};
};
Registry& Service() { static auto* value = new Registry; return *value; }
void Retire(Registry& registry, const std::shared_ptr<Owner>& owner) {
 owner->live.store(false);
 for (auto& lease: owner->video_leases) { if (auto stream = lease.load()) { stream->Retire(); } }
 for (auto& lease: owner->audio_leases) { if (auto stream = lease.load()) { stream->Retire(); } }
 owner->retired_next = std::move(registry.retired);
 registry.retired = owner;
}
std::shared_ptr<Owner> GetOwner(int& result) {
 const int pid = Loader::GetCurrentGuestProcessId();
 uint64_t epoch = 0; std::shared_ptr<std::recursive_mutex> gate;
 result = GetEsvmClientLease(pid, &epoch, &gate);
 if (result) { return nullptr; }
 Common::HostLockCriticalGuard critical;
 auto& registry = Service(); std::lock_guard lock(registry.mutex);
 auto it = registry.owners.find(pid);
 if (it != registry.owners.end() && it->second->epoch != epoch) {
  Retire(registry, it->second); registry.owners.erase(it); it = registry.owners.end();
 }
 if (it != registry.owners.end()) { return it->second; }
 size_t retained = registry.owners.size();
 for (auto retired = registry.retired; retired; retired = retired->retired_next) { ++retained; }
 if (retained >= 64) { result = Full; return nullptr; }
 Allocate();
 auto owner = std::make_shared<Owner>(pid, epoch, std::move(gate));
 registry.owners.emplace(pid, owner); return owner;
}
bool Valid(const Owner& owner) {
 uint64_t epoch = 0;
 return owner.live.load() && GetEsvmClientEpoch(owner.pid, &epoch) == 0 && epoch == owner.epoch;
}
bool StateTraceEnabled() {
 static const bool enabled = [] { const auto* v = std::getenv("KYTY_MEDIA_TRACE"); return v && std::strcmp(v, "1") == 0; }();
 return enabled;
}
void StateTraceEntry(Context& source, const char* entry, uintptr_t caller) {
 if (!StateTraceEnabled()) { return; }
 source.trace_entry = entry; source.trace_caller = caller;
}
// Owner dispatch gate is held at every call. This only reads existing state:
// never Pump/Observe/Sync/Claim, inspect guest payload, or call guest code here.
void TraceSourceState(Owner& owner, Context& source, const char* reason) {
 if (!StateTraceEnabled()) { return; }
 const auto emit = [&](Decoder* child) {
  const auto status = child && child->packets ? child->packets->Status() : EsvmDecoderStatus {};
  EsvmReadinessObservation predicate;
  predicate.decoded = status.queued_frames; predicate.buffered = status.queued_access_units;
  predicate.decoding = status.held_codec_frame ? 1 : 0; predicate.input_count = predicate.buffered;
  predicate.input_ended = status.eos_accepted;
  const bool video = EsvmIsVideoCodec(status.codec);
  const bool sufficient = video && EsvmReadiness::Sufficient(predicate);
  const auto capacity = child ? ReadEsvmRefillCapacity(status, child->limits) : EsvmRefillCapacity {};
  const auto refill = child ? child->refill.Pending() : EsvmRefillToken {};
  const bool guest = owner.pid == Loader::GetCurrentGuestProcessId();
  const std::array<uint64_t, 39> state {
   source.live, source.playing, source.input_started, source.readiness.Flags(), source.readiness.Pending(),
   source.readiness.Revision(), source.readiness_waiting_output, static_cast<uint64_t>(source.readiness_decoder),
   source.readiness_generation, static_cast<uint64_t>(child ? child->handle : 0), child && child->live,
   static_cast<uint64_t>(status.state), static_cast<uint64_t>(status.codec), status.generation,
   status.queued_frames, status.queued_access_units, status.queued_packets, status.held_codec_frame,
   status.eos_accepted, static_cast<uint64_t>(status.failure), static_cast<uint32_t>(child ? child->service_failure : 0),
   child && child->staged_audio.pcm != nullptr, child && child->staged.rgba != nullptr,
   status.audio_sample_rate, status.audio_channels, source.readiness.HasTimestamp(),
   status.queued_packet_bytes, status.queued_frame_bytes,
   child ? child->trace_batch : 0, child ? child->trace_units : 0, child ? child->trace_subtype : 0,
   child ? child->trace_result : 0, capacity.can_refill, refill.generation, refill.revision,
   owner.live.load(), source.callbacks.event != nullptr, guest, owner.callbacks};
  auto& previous = child ? child->trace_state : source.trace_state;
  auto& seen = child ? child->trace_seen : source.trace_seen;
  if (seen && previous == state) { return; }
  previous = state; seen = true;
  if (source.trace_count > 64) { return; }
  const bool capped = source.trace_count++ == 64;
  static std::atomic_uint total {0};
  auto count = total.load(std::memory_order_relaxed);
  while (count < 1025 && !total.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) {}
  if (count >= 1024) {
   if (count == 1024) LOGF("MediaFramework state: global-trace-cap=1024\n");
   return;
  }
  if (capped) {
   LOGF("MediaFramework state: pid=%d source=%d epoch=%llu trace-cap=64\n", owner.pid, source.handle,
        static_cast<unsigned long long>(owner.epoch));
   return;
  }
  const bool eligible = owner.live.load() && source.live && source.callbacks.event &&
                        source.readiness.Pending() && guest && !owner.callbacks;
  LOGF("MediaFramework state: pid=%d source=%d epoch=%llu child=%d reason=%s entry=%s caller=%llx host-us=%llu "
       "live=%u playing=%u input-started=%u flags=%u timestamp=%u waiting-sync=%u selected=%d selected-gen=%llu "
       "codec=%u state=%u generation=%llu decoded=%llu payload-aus=%llu packets=%llu held=%u eos=%u "
       "sufficient=%u threshold=12 allocator-known=0 pending=%llu revision=%llu callback=%u guest=%u depth=%llu eligible=%u "
       "staged-video=%u staged-audio=%u audio-rate=%u channels=%u failure=%u service=%08x "
       "batch=%llu batch-units=%llu subtype=%llu batch-result=%llu refill-capacity=%u refill-pending=%u\n",
       owner.pid, source.handle, static_cast<unsigned long long>(owner.epoch), child ? child->handle : 0,
       reason, source.trace_entry, static_cast<unsigned long long>(source.trace_caller),
       static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count()),
       unsigned(source.live), unsigned(source.playing), unsigned(source.input_started), source.readiness.Flags(),
       unsigned(source.readiness.HasTimestamp()), unsigned(source.readiness_waiting_output), source.readiness_decoder,
       static_cast<unsigned long long>(source.readiness_generation), unsigned(status.codec), unsigned(status.state),
       static_cast<unsigned long long>(status.generation), static_cast<unsigned long long>(status.queued_frames),
       static_cast<unsigned long long>(status.queued_access_units), static_cast<unsigned long long>(status.queued_packets),
       unsigned(status.held_codec_frame), unsigned(status.eos_accepted), unsigned(sufficient),
       static_cast<unsigned long long>(source.readiness.Pending()), static_cast<unsigned long long>(source.readiness.Revision()),
       unsigned(source.callbacks.event != nullptr), unsigned(guest), static_cast<unsigned long long>(owner.callbacks), unsigned(eligible),
       unsigned(child && child->staged.rgba != nullptr), unsigned(child && child->staged_audio.pcm != nullptr),
       status.audio_sample_rate, status.audio_channels, unsigned(status.failure), unsigned(child ? child->service_failure : 0),
       static_cast<unsigned long long>(child ? child->trace_batch : 0), static_cast<unsigned long long>(child ? child->trace_units : 0),
       static_cast<unsigned long long>(child ? child->trace_subtype : 0), static_cast<unsigned long long>(child ? child->trace_result : 0),
       unsigned(capacity.can_refill), unsigned(static_cast<bool>(refill)));
 };
 bool children = false;
 for (const auto& child: owner.decoders) {
  if (child && child->context.get() == &source) { children = true; emit(child.get()); }
 }
 if (!children) emit(nullptr);
}
void ObserveRefill(Decoder& decoder) {
 if (!decoder.packets) { return; }
 const auto capacity = ReadEsvmRefillCapacity(decoder.packets->Status(), decoder.limits);
 decoder.refill.Observe(capacity.generation, decoder.live && decoder.context->live && decoder.context->input_started &&
                        !decoder.service_failure && capacity.can_refill);
}
std::shared_ptr<Context> ObserveReadiness(Owner& owner, int handle) {
 std::shared_ptr<Context> source;
 for (const auto& candidate: owner.contexts) {
  if (candidate && candidate->live && candidate->handle == handle) { source = candidate; break; }
 }
 if (!source) { return nullptr; }
 TraceSourceState(owner, *source, "observe-enter");
 struct TraceExit { Owner& owner; Context& source; ~TraceExit() { TraceSourceState(owner, source, "observe-exit"); } } trace_exit {owner, *source};
 std::shared_ptr<Decoder> video;
 for (const auto& decoder: owner.decoders) {
  if (!decoder || !decoder->live || decoder->context != source || !decoder->packets) { continue; }
  if (!EsvmIsVideoCodec(decoder->packets->Status().codec)) { continue; }
  if (video || decoder->service_failure || decoder->packets->Status().state == EsvmDecoderState::Failed) {
   source->readiness.Discontinuity(); source->readiness_waiting_output = false; source->readiness_output_count = 0;
   return source; // No guessed selection among multiple video children.
  }
  video = decoder;
 }
 if (!video) {
  if (source->readiness_decoder) {
   source->readiness.Discontinuity(); source->readiness_waiting_output = false;
  }
  source->readiness_decoder = 0; source->readiness_generation = 0;
  source->readiness_output_count = 0;
  return source;
 }
 const auto status = video->packets->Status();
 if (source->readiness_decoder != video->handle || source->readiness_generation != status.generation) {
  // Native first-output state belongs to the source (+0x1d8), not a codec
  // generation: Flush sets 1 at 0x1e088; output 0->positive consumes it at
  // 0x226d7..0x22706. Repeated config before that output must not cancel it.
  // Still revoke old tickets/Sync on replacement; only an unfinished request
  // on the same selected child survives, never an already-completed Sync.
  if (source->readiness_decoder) {
   const bool waiting = source->readiness_decoder == video->handle &&
                        source->readiness_waiting_output && !source->readiness_output_count;
   source->readiness.Discontinuity(); source->readiness_waiting_output = waiting;
  }
  source->readiness_decoder = video->handle; source->readiness_generation = status.generation;
  source->readiness_output_count = 0;
 }
 if (status.queued_frames && !source->readiness_output_count && source->readiness_waiting_output) {
  source->readiness.SyncVideo(); source->readiness_waiting_output = false;
 }
 source->readiness_output_count = status.queued_frames;
 EsvmReadinessObservation observation;
 observation.decoded = status.queued_frames;
 // Consume copies every AU before completing: this is the host's buffered
 // encoded-unit queue, not the decoder's distinct-timestamp in-flight tree.
 // EOS is a control entry, never an encoded unit. For B only the single
 // already-received frame blocked on output capacity is certain. Unobserved
 // FFmpeg reorder state is not inferred from accepted-minus-output counts.
 observation.buffered = status.queued_access_units;
 observation.decoding = status.held_codec_frame ? 1 : 0;
 observation.input_count = observation.buffered;
 // Native wire EOS (AU +0x18) reaches engine flag58 through input admission,
 // not output EOF. Waiting for codec EOF deadlocks short input behind the
 // paused output queue. Consume has already copied all preceding payload AUs;
 // the predicate still requires decoded output and coverage of buffered input.
 observation.input_ended = status.eos_accepted;
 // Native allocator notifications are not host queue backpressure. Until the
 // two native pools have corresponding accounting, leave that fallback off.
 source->readiness.Observe(observation);
 return source;
}
bool PopReadinessFrame(Owner& owner, const std::shared_ptr<Decoder>& decoder, EsvmDecodedFrame* output) {
 // Native 0x1b7b0 queues Timestamp after successful video-output retrieval.
 // Observe the post-Pump count first, so a Flush's Sync precedes this timestamp
 // even when the scheduler retrieves the first output before a guest entry.
 const auto source = ObserveReadiness(owner, decoder->context->handle);
 if (!decoder->packets->PopFrame(output)) { return false; }
 if (source && Valid(owner) && source->readiness_decoder == decoder->handle &&
     source->readiness_generation == output->generation && output->has_pts) {
  source->readiness.Timestamp(output->pts); // Decoder PTS is already microseconds.
 }
 // Record dequeue too: the next Pump may produce a genuine zero-to-positive
 // transition. No event is dispatched by this worker/diagnostic path.
 ObserveReadiness(owner, decoder->context->handle);
 return true;
}
void DispatchRefills(Owner& owner, int context) {
 // Called ONLY from explicit guest interface entry sites, never SchedulerLoop,
 // diagnostic frame-transfer helpers, lifecycle drainers or audio/graphics code.
 if (owner.refill_dispatching || owner.callbacks || owner.pid != Loader::GetCurrentGuestProcessId() || !Valid(owner)) { return; }
 owner.refill_dispatching = true;
 struct Exit { Owner& owner; ~Exit() { owner.refill_dispatching = false; } } exit {owner};
 const auto source = ObserveReadiness(owner, context);
 const auto ready_ticket = source ? source->readiness.Pending() : 0;
 struct Ticket { std::shared_ptr<Decoder> decoder; EsvmRefillToken token; };
 std::array<Ticket, DecoderLimit> tickets {};
 size_t count = 0;
 for (const auto& decoder: owner.decoders) {
  if (!decoder || !decoder->live || decoder->context->handle != context || !decoder->context->live) { continue; }
  ObserveRefill(*decoder);
  if (const auto token = decoder->refill.Pending()) { tickets[count++] = {decoder, token}; }
 }
 // Snapshot + fixed callback bound prevents a recursive Submit/control from
 // delivering another endlessly rearmed credit during this entry's pump.
 for (size_t i = 0; i < count && Valid(owner) && owner.pid == Loader::GetCurrentGuestProcessId(); ++i) {
  auto& decoder = tickets[i].decoder;
  if (!decoder->live || !decoder->context->live || !decoder->packets || !decoder->callbacks.event) { continue; }
  ObserveRefill(*decoder); // Revalidate capacity/generation after earlier callback reentry.
  if (!decoder->refill.Claim(tickets[i].token)) { continue; }
  TraceSourceState(owner, *decoder->context, "refill-claimed");
  const auto event = decoder->callbacks.event;
  ++owner.callbacks;
  struct CallbackExit { Owner& owner; ~CallbackExit() { --owner.callbacks; } } callback_exit {owner};
  Trace("decoder-refill-delivered", decoder->handle, 0, 0, 0, owner.pid);
  event(decoder->handle, 0); // Exact public Q6 ABI, NOT source-ready/can-play/EOS.
 }
 if (source && source->live && Valid(owner) && owner.pid == Loader::GetCurrentGuestProcessId()) {
  ObserveReadiness(owner, context); // Revalidate generation/selection after refill reentry.
  if (source->callbacks.event && source->readiness.Claim(ready_ticket)) {
   TraceSourceState(owner, *source, "ready-claimed");
   const auto callback = source->callbacks;
   ++owner.callbacks;
   struct CallbackExit { Owner& owner; ~CallbackExit() { --owner.callbacks; } } callback_exit {owner};
   Trace("source-ready-delivered", source->handle, 0, 0, 0, owner.pid);
   callback.event(0x2001, nullptr, callback.user);
  }
 }
 if (source && source->live && source->callbacks.event && Valid(owner) &&
     owner.pid == Loader::GetCurrentGuestProcessId()) {
  std::shared_ptr<Decoder> video;
  for (const auto& decoder: owner.decoders) {
   if (!decoder || !decoder->live || decoder->context != source || !decoder->video || decoder->video->IsRetired()) { continue; }
   if (video) { return; }
   video = decoder;
  }
  if (!video) { return; }
  const auto shown = video->video->Presented();
  if (!shown.serial || !shown.has_pts || shown.pts < 0 || shown.generation != video->packets->Status().generation) { return; }
  if (source->progress_decoder == video->handle && source->progress_generation == shown.generation &&
      source->progress_serial == shown.serial) { return; }
  // Native libSceEsvm 13.60 0x28add handles source event 0xf by querying
  // PLJs4B6fCk0 and forwarding public progress event 2 with PTS/sequence.
  // Claim before reentry; only renderer-acknowledged output supplies progress.
  source->progress_decoder = video->handle; source->progress_generation = shown.generation;
  source->progress_serial = shown.serial;
  const auto callback = source->callbacks;
  ++owner.callbacks;
  struct CallbackExit { Owner& owner; ~CallbackExit() { --owner.callbacks; } } callback_exit {owner};
  Trace("source-progress-delivered", source->handle, shown.serial, 0, shown.sequence, owner.pid);
  callback.event(0xf, nullptr, callback.user);
 }
}
template<class T, size_t N> std::shared_ptr<T> Find(const std::array<std::shared_ptr<T>, N>& array, int id) {
 for (const auto& object: array) { if (object && object->live && object->handle == id) { return object; } }
 return nullptr;
}
template<class T, size_t N> auto Free(std::array<std::shared_ptr<T>, N>& array) { return std::find(array.begin(), array.end(), nullptr); }
bool RetireAudio(Owner& owner, const std::shared_ptr<Decoder>& decoder, bool discard = true) {
 if (discard) { decoder->staged_audio = {}; }
 if (!decoder->audio) { return true; }
 decoder->audio->Retire();
 auto& registry = Service();
 {
  Common::HostLockCriticalGuard critical; std::lock_guard lock(registry.mutex);
  auto slot = Free(registry.closing_audio);
  if (slot == registry.closing_audio.end()) { return false; } // Retain retired lease for retry, never destroy under this lock.
  *slot = decoder->audio;
  registry.closing_audio_count.fetch_add(1, std::memory_order_relaxed);
 }
 for (size_t i = 0; i < owner.decoders.size(); ++i) { if (owner.decoders[i] == decoder) { owner.audio_leases[i].store(nullptr); break; } }
 decoder->audio.reset(); decoder->audio_generation = 0; decoder->audio_input_ended_generation = 0;
 decoder->audio_rate = decoder->audio_channels = 0;
 return true;
}
void CloseRetiredAudio() {
 std::array<std::shared_ptr<Ai::NativeOutput>, 8> closing {};
 {
  auto& registry = Service(); Common::HostLockCriticalGuard critical; std::lock_guard lock(registry.mutex);
  closing.swap(registry.closing_audio);
 }
 for (auto& lease: closing) {
  if (lease) { lease->Close(); Service().closing_audio_count.fetch_sub(1, std::memory_order_relaxed); }
 }
}
void RetireVideo(Owner& owner, const std::shared_ptr<Decoder>& decoder) {
 if (decoder->video) { decoder->video->Retire(); decoder->video.reset(); }
 decoder->staged = {}; decoder->clock_started = false; decoder->video_generation = 0;
 for (size_t i = 0; i < owner.decoders.size(); ++i) {
  if (owner.decoders[i] == decoder) { owner.video_leases[i].store(nullptr); break; }
 }
}
int64_t PlayedUs(const Context& source, Clock::time_point now) {
 if (!source.playing) { return source.elapsed_us; }
 const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - source.anchor).count();
 if (elapsed <= 0) { return source.elapsed_us; }
 return elapsed > INT64_MAX - source.elapsed_us ? INT64_MAX : source.elapsed_us + elapsed;
}
bool SdrColour(uint8_t transfer, uint8_t primaries) {
 // Parent's mailbox is RGBA8 SDR, with no PQ/HLG tone mapping or BT.2020/P3
 // wide-gamut conversion. Unknown metadata is NOT permission to label it SDR.
 return (transfer == 1 || transfer == 6 || transfer == 13) &&
        (primaries == 1 || primaries == 5 || primaries == 6);
}
bool SdrInput(std::span<const uint8_t> bytes, int handle) {
 // Only inspect known outer records and the fixed VP9 metadata, not AU payloads
 // or decryption. The packet module remains the sole structural/codec parser.
 // Invalid/truncated/too-many records defer to its precise transactional error.
 for (size_t records = 0; !bytes.empty() && records < 64; ++records) {
  if (bytes.size() < 16) { return true; }
  uint32_t magic = 0, subtype = 0; uint64_t length = 0;
  std::memcpy(&magic, bytes.data(), 4); std::memcpy(&subtype, bytes.data() + 4, 4); std::memcpy(&length, bytes.data() + 8, 8);
  bytes = bytes.subspan(16);
  if (magic != 100 || length > bytes.size()) { return true; }
  if (subtype == 3 && length == 24) {
   uint64_t size = 0; std::memcpy(&size, bytes.data(), 8);
   if (size == 24 && !SdrColour(bytes[17], bytes[18])) {
    Trace("unsupported-colour", handle, length, Failed, (int(bytes[17]) << 8) | bytes[18]); return false;
   }
  }
  bytes = bytes.subspan(static_cast<size_t>(length));
 }
 return true;
}
void SyncVideoGeneration(Owner& owner, const std::shared_ptr<Decoder>& decoder) {
 if (!decoder->video) { return; }
 const auto status = decoder->packets->Status();
 if (status.state == EsvmDecoderState::Failed || !EsvmIsVideoCodec(status.codec)) { RetireVideo(owner, decoder); return; }
 if (status.generation != decoder->video_generation) {
  decoder->staged = {}; decoder->clock_started = false;
  if (!decoder->video->Flush(status.generation)) { RetireVideo(owner, decoder); return; }
  decoder->video_generation = status.generation;
 }
}
bool StartScheduler();
int PacketError(PacketResult result);
int SyncAudioGeneration(Owner& owner, const std::shared_ptr<Decoder>& decoder) {
 const auto status = decoder->packets->Status();
 if (decoder->staged_audio.pcm && decoder->staged_audio.generation != status.generation) { decoder->staged_audio = {}; }
 // Sticky decoder failure invalidates same-generation PCM even before a native
 // lease exists. Never let an output-open retry publish pre-failure staging.
 if (status.state == EsvmDecoderState::Failed) {
  decoder->audio_result = Ai::NativeQueueResult::Failed;
  RetireAudio(owner, decoder); return Failed;
 }
 if (!EsvmIsAudioCodec(status.codec)) {
  return RetireAudio(owner, decoder) ? 0 : Failed;
 }
 if (!decoder->audio) { return 0; }
 if (status.generation != decoder->audio_generation) {
  if (!decoder->audio->Flush(status.generation)) {
   decoder->service_failure = Failed; RetireAudio(owner, decoder);
   Trace("audio-flush-failed", decoder->handle, 0, Failed, 0, owner.pid); return Failed;
  }
  decoder->audio_generation = status.generation;
  decoder->audio_input_ended_generation = 0;
 }
 return 0;
}
int ApplyAudioControls(Owner& owner, const std::shared_ptr<Context>& source) {
 int result = 0;
 for (const auto& decoder: owner.decoders) {
  if (!decoder || !decoder->live || decoder->context != source || !decoder->audio) { continue; }
  if (!decoder->audio->SetGain(source->muted ? 0.0f : static_cast<float>(source->volume)) ||
      !decoder->audio->SetPaused(!source->playing)) {
   decoder->service_failure = Failed; RetireAudio(owner, decoder, false); result = Failed;
   Trace("audio-control-failed", decoder->handle, 0, result, 0, owner.pid);
  }
 }
 return result;
}
int FinishAudioInput(Owner& owner, const std::shared_ptr<Decoder>& decoder) {
 if (!decoder->audio || decoder->staged_audio.pcm) { return 0; }
 const auto status = decoder->packets->Status();
 if (status.state != EsvmDecoderState::Ended || status.queued_frames || status.held_codec_frame ||
     decoder->audio_input_ended_generation == status.generation) { return 0; }
 // This flushes the real resampler TAIL, unlike generation Flush which clears
 // queued future audio. It proves neither hardware consumption nor speaker PTS.
 if (!decoder->audio->EndInput(status.generation)) {
  decoder->audio_result = Ai::NativeQueueResult::Failed;
  Trace("audio-end-input-failed", decoder->handle, 0, Failed, 0, owner.pid); return Failed;
 }
 decoder->audio_input_ended_generation = status.generation;
 decoder->audio_result = Ai::NativeQueueResult::Accepted;
 Trace("audio-input-ended", decoder->handle, 0, 0, 0, owner.pid);
 return 0;
}
int PublishAudio(Owner& owner, const std::shared_ptr<Decoder>& decoder, Clock::time_point now) {
 if (!decoder->live || !decoder->context->live || !owner.live.load()) { return 0; }
 if (const int result = SyncAudioGeneration(owner, decoder)) { return result; }
 if (decoder->service_failure) { return decoder->service_failure; }
 const auto status = decoder->packets->Status();
 if (!EsvmIsAudioCodec(status.codec)) { return 0; }
 if (!decoder->context->playing) { return FinishAudioInput(owner, decoder); }
 if (!decoder->staged_audio.pcm) {
  const auto pumped = decoder->packets->Pump();
   if (PacketError(pumped) != 0) {
    decoder->audio_result = Ai::NativeQueueResult::Failed;
    RetireAudio(owner, decoder); Trace("audio-decode-failed", decoder->handle, 0, Failed, static_cast<int>(pumped), owner.pid); return Failed;
  }
  if (!decoder->packets->PopAudio(&decoder->staged_audio)) { return FinishAudioInput(owner, decoder); }
 }
 const auto& pcm = decoder->staged_audio;
 const bool layout = (pcm.channels == 1 && pcm.channel_mask == 4) || (pcm.channels == 2 && pcm.channel_mask == 3);
 if (!pcm.pcm || !layout || pcm.sample_rate < 7350 || pcm.sample_rate > 96000 ||
     (status.configured_sample_rate && pcm.sample_rate != status.configured_sample_rate) ||
     !pcm.samples || pcm.samples > decoder->limits.max_audio_samples_per_frame ||
     pcm.bytes != uint64_t(pcm.samples) * pcm.channels * sizeof(float) || pcm.generation != status.generation ||
     !std::all_of(pcm.pcm.get(), pcm.pcm.get() + size_t(pcm.samples) * pcm.channels, [](float x) { return std::isfinite(x); })) {
  decoder->service_failure = Failed; RetireAudio(owner, decoder);
  Trace("audio-format-rejected", decoder->handle, 0, Failed, 0, owner.pid); return Failed;
 }
 if (decoder->audio && (decoder->audio_rate != pcm.sample_rate || decoder->audio_channels != pcm.channels)) {
  // No relabel/resample or implicit midstream channel remix. Remove/recreate
  // the decoder for a different output format in this first supported slice.
  decoder->service_failure = Failed; RetireAudio(owner, decoder);
  Trace("audio-format-change-unsupported", decoder->handle, 0, Failed, 0, owner.pid); return Failed;
 }
 if (!decoder->audio) {
  if (now < decoder->audio_retry_after) { return Failed; }
  if (!StartScheduler()) { return Failed; }
  // The shared SDL adapter reserves eight frames for its bounded resampler tail.
  const auto capacity = std::max(decoder->limits.max_audio_samples_per_frame + 8, pcm.sample_rate / 10);
  auto stream = Ai::OpenNativeOutput({owner.pid, decoder->context->handle, decoder->handle, owner.epoch},
                                    pcm.sample_rate, pcm.channels == 1 ? Ai::Format::FloatMono : Ai::Format::FloatStereo,
                                    pcm.generation, capacity);
  if (!stream) {
   decoder->audio_result = Ai::NativeQueueResult::Unavailable;
   decoder->audio_retry_after = now + std::chrono::milliseconds(250);
   Trace("audio-output-unavailable", decoder->handle, pcm.bytes, Failed, 0, owner.pid); return Failed;
  }
  decoder->audio = stream; decoder->audio_generation = pcm.generation;
  decoder->audio_rate = pcm.sample_rate; decoder->audio_channels = pcm.channels;
  for (size_t i = 0; i < owner.decoders.size(); ++i) { if (owner.decoders[i] == decoder) { owner.audio_leases[i].store(stream); break; } }
  // Invalidation may have run before the atomic publication, without this gate.
  if (!Valid(owner) || !stream->SetGain(decoder->context->muted ? 0.0f : static_cast<float>(decoder->context->volume)) ||
      !stream->SetPaused(false)) {
   decoder->service_failure = Failed; RetireAudio(owner, decoder, false); return Failed;
  }
  Trace("audio-lease-opened", decoder->handle, capacity, 0, static_cast<int>(pcm.channels), owner.pid);
 }
 const auto queued = decoder->audio->TryQueue({reinterpret_cast<const uint8_t*>(pcm.pcm.get()), pcm.bytes}, pcm.samples, pcm.generation);
 decoder->audio_result = queued;
 if (queued == Ai::NativeQueueResult::Backpressure) { return 0; } // Keep exact owned PCM, no drop/padding/spin.
 if (queued != Ai::NativeQueueResult::Accepted) {
  decoder->service_failure = Failed; RetireAudio(owner, decoder, false);
  Trace("audio-queue-failed", decoder->handle, 0, Failed, static_cast<int>(queued), owner.pid); return Failed;
 }
 Trace("audio-pcm-accepted", decoder->handle, pcm.samples, 0, static_cast<int>(pcm.channels), owner.pid);
 decoder->audio_accepted_frames = pcm.samples > UINT64_MAX - decoder->audio_accepted_frames ? UINT64_MAX : decoder->audio_accepted_frames + pcm.samples;
 decoder->staged_audio = {}; // SDL really copied these exact frames; not speaker-presented PTS.
 return FinishAudioInput(owner, decoder);
}
int SelectConfiguredVideo(Owner& owner, const std::shared_ptr<Context>& source) {
 if (!source->rectangle_set || !source->input_started) { return 0; }
 std::shared_ptr<Decoder> video;
 for (const auto& decoder: owner.decoders) {
  if (!decoder || !decoder->live || decoder->service_failure || decoder->context != source) { continue; }
  const auto status = decoder->packets->Status();
  if (!EsvmIsVideoCodec(status.codec) || status.state == EsvmDecoderState::Unconfigured || status.state == EsvmDecoderState::Failed) { continue; }
  if (video) {
   // Do not keep publishing a formerly unique child after another video config
   // makes context selection ambiguous. Both live leases must stop, not guess.
   for (const auto& child: owner.decoders) { if (child && child->context == source) { RetireVideo(owner, child); } }
   Trace("render-ambiguous-video", source->handle, 0, Failed); return Failed;
  }
  video = decoder;
 }
 if (!video) { Trace("render-geometry-deferred", source->handle, 0, 0); return 0; }
 const auto& r = source->rectangle;
 const Nv::NativeVideoRect rectangle {r[0], r[1], r[2], r[3]};
 SyncVideoGeneration(owner, video);
 if (!video->video || video->video->IsRetired()) {
  auto stream = std::make_shared<Nv::NativeVideoStream>(owner.pid, source->handle, video->handle, video->packets->Status().generation);
  if (!stream->SetRect(rectangle)) { return Failed; }
  if (!Nv::SelectNativeVideoStream(stream)) { stream->Retire(); Trace("render-stream-conflict", source->handle, 0, Failed); return Failed; }
  video->video = stream; video->video_generation = video->packets->Status().generation;
  for (size_t i = 0; i < owner.decoders.size(); ++i) { if (owner.decoders[i] == video) { owner.video_leases[i].store(stream); break; } }
  if (!Valid(owner) || !StartScheduler()) { RetireVideo(owner, video); return Failed; }
  Trace("video-mailbox-activated", source->handle, video->handle, 0); // Transition evidence, not polling budget.
 } else if (!video->video->SetRect(rectangle)) { return Failed; }
 Trace("render-mailbox-selected", source->handle, video->handle, 0);
 return 0; // Geometry/selection only; no readiness, decoded pixels or present ack.
}
int PublishDue(Owner& owner, const std::shared_ptr<Decoder>& decoder, Clock::time_point now) {
 if (decoder->service_failure) { return decoder->service_failure; }
 if (!decoder->live || !decoder->context->live || !decoder->context->playing || !decoder->video || !owner.live.load()) { return 0; }
 SyncVideoGeneration(owner, decoder);
 if (!decoder->video) { return Failed; }
 if (!decoder->staged.rgba) {
  const auto pumped = decoder->packets->Pump();
  if (PacketResult::Ok != pumped && PacketResult::Yield != pumped && PacketResult::Backpressure != pumped) {
   RetireVideo(owner, decoder); return Failed;
  }
  EsvmDecodedFrame decoded;
  if (!PopReadinessFrame(owner, decoder, &decoded)) { return 0; }
  if (!SdrColour(decoded.transfer_characteristics, decoded.colour_primaries)) {
   decoder->service_failure = Failed;
   // Numeric metadata only: no compressed samples, pixels or credentials.
   Trace("decoded-colour-rejected", decoder->handle, decoded.matrix_coefficients, Failed,
         (int(decoded.transfer_characteristics) << 8) | decoded.colour_primaries, owner.pid);
   RetireVideo(owner, decoder); return Failed;
  }
  decoder->staged = {std::move(decoded.rgba), decoded.bytes, decoded.width, decoded.height, decoded.stride,
                     decoded.generation, decoded.pts, decoded.pts_sequence, decoded.has_pts};
 }
 if (decoder->staged.generation != decoder->video_generation) { decoder->staged = {}; return Failed; }
 if (decoder->staged.has_pts) {
  const auto played = PlayedUs(*decoder->context, now);
  if (!decoder->clock_started) {
   decoder->origin_pts = decoder->staged.pts; decoder->origin_played_us = played; decoder->clock_started = true;
  }
  if (decoder->staged.pts > decoder->origin_pts) {
   const auto delta = static_cast<uint64_t>(decoder->staged.pts) - static_cast<uint64_t>(decoder->origin_pts);
   if (delta > static_cast<uint64_t>(played - decoder->origin_played_us)) { return 0; }
  }
 }
 // Mailbox full keeps this one owned staged frame intact. A host present ack
 // frees mailbox capacity; the scheduler retries without a graphics callback.
 if (decoder->video->Publish(std::move(decoder->staged))) { decoder->staged = {}; }
 return 0;
}
void SchedulerLoop() {
 auto& registry = Service();
 while (!registry.scheduler_stopping.load()) {
  std::array<std::shared_ptr<Owner>, 64> owners {}; size_t count = 0;
  { Common::HostLockCriticalGuard critical; std::lock_guard lock(registry.mutex);
    for (const auto& [pid, owner]: registry.owners) { owners[count++] = owner; } }
  for (const auto& owner: owners) {
   if (!owner || registry.scheduler_stopping.load()) { continue; }
   // Never block shutdown/reentrant guest retirement behind a callback gate.
    EsvmDispatchLock gate(*owner->dispatch, false);
   if (!gate.owns_lock() || !Valid(*owner)) { continue; }
   for (const auto& decoder: owner->decoders) {
    if (!decoder || !decoder->live) { continue; }
    try {
      if (EsvmIsAudioCodec(decoder->packets->Status().codec)) { PublishAudio(*owner, decoder, Clock::now()); }
     else if (decoder->video) { PublishDue(*owner, decoder, Clock::now()); }
     ObserveRefill(*decoder); // Host work ONLY marks pending; never dispatches guest code.
    }
    catch (const std::bad_alloc&) { Trace("mailbox-allocation-failed", decoder->handle, 0, Failed, 0, owner->pid); }
   }
  }
  CloseRetiredAudio(); // No owner/registry/lifecycle gate held, no guest callbacks.
  std::unique_lock lock(registry.scheduler_mutex);
  registry.scheduler_wake.wait_for(lock, std::chrono::milliseconds(5), [&] { return registry.scheduler_stopping.load(); });
 }
 CloseRetiredAudio(); // Final shutdown drain before joining the host worker.
}
bool StartScheduler() {
 auto& registry = Service(); std::lock_guard lock(registry.scheduler_mutex);
 if (registry.scheduler_stopping.load()) { return false; }
 if (!registry.scheduler.joinable()) {
  try { registry.scheduler = std::thread(SchedulerLoop); }
  catch (const std::system_error&) { return false; }
  catch (const std::bad_alloc&) { return false; }
 }
 registry.scheduler_wake.notify_all(); return true;
}
void ReapDead(Owner& owner) {
 for (auto& d: owner.decoders) {
  if (d && !d->live) { RetireVideo(owner, d); if (RetireAudio(owner, d) && d->mapping.Reset()) { d.reset(); } }
 }
 for (auto& c: owner.contexts) {
  if (c && !c->live && std::none_of(owner.decoders.begin(), owner.decoders.end(), [&](const auto& d) { return d && d->context == c; })) { c.reset(); }
 }
}
struct Drain { ~Drain() { DrainRetiredEsvmServices(); } };
struct Pending {
 Owner& owner;
 explicit Pending(Owner& o): owner(o) { ++owner.pending; }
 ~Pending() { --owner.pending; }
};
void Complete(Owner& owner, EsvmDecoderComplete callback, int handle, int result, void* user) {
 if (!Valid(owner)) { return; }
 ++owner.callbacks;
 struct Exit { Owner& o; ~Exit() { --o.callbacks; } } exit {owner};
 callback(handle, result, user);
}
int PacketError(PacketResult result) {
 switch (result) {
  case PacketResult::Ok: case PacketResult::Yield: case PacketResult::Backpressure: return 0;
  case PacketResult::InvalidWire: return Invalid;
  default: return Failed;
 }
}
int FinishInitialConfiguration(Owner& owner, const std::shared_ptr<Context>& source) {
 if (source->input_started) { return 0; } // Native source+0xac makes this a one-shot producer.
 size_t children = 0;
 for (const auto& decoder: owner.decoders) {
  if (!decoder || !decoder->live || decoder->context != source) { continue; }
  ++children;
  const auto status = decoder->packets->Status();
  if (status.state == EsvmDecoderState::Unconfigured) { return 0; }
  // Before native 0x2b8a0's barrier, 0x2ab90 rejects input and no refill is
  // issued. Enforce that invariant rather than flushing already accepted AUs.
  if (decoder->service_failure || status.state != EsvmDecoderState::Ready || status.codec == EsvmDecoderCodec::None ||
      status.has_sequence || status.queued_packets || status.queued_frames || status.held_codec_frame || status.eos_accepted ||
      status.generation == UINT64_MAX || decoder->audio || decoder->video || decoder->staged_audio.pcm || decoder->staged.rgba) {
   return Failed;
  }
 }
 if (!children) { return 0; }
 // Serialize the internally produced Flush before releasing startup credits.
 // All queues/output leases are proven empty; this performs real codec resets
 // without retiring a payload, waiting for a renderer or calling guest code.
 source->readiness.Discontinuity(); source->readiness_waiting_output = false;
 for (const auto& decoder: owner.decoders) {
  if (!decoder || !decoder->live || decoder->context != source) { continue; }
  const int result = PacketError(decoder->packets->FlushCurrent());
  if (result) { Trace("startup-flush-complete", source->handle, children, result, 0, owner.pid); return result; }
 }
 ObserveReadiness(owner, source->handle); // Bind reset generations before requesting first output.
 source->readiness_waiting_output = true; // Sync still requires an actual zero-to-positive video output.
 source->input_started = true;
 Trace("startup-flush-complete", source->handle, children, 0, 0, owner.pid);
 return 0;
}
} // namespace

bool ReadMediaFrameworkUri(const char* source, char (&output)[0x1000], size_t* length) {
 if (!length) { return false; }
 const auto address = reinterpret_cast<uintptr_t>(source);
 for (size_t i = 0; i < sizeof(output); ++i) {
  if (!address || i > UINTPTR_MAX - address || !EsvmReadGuest(&output[i], reinterpret_cast<const void*>(address + i), 1)) { return false; }
  if (!output[i]) { *length = i; return true; }
 }
 return false;
}

bool TryAddEsvmSource(const void* config, const void* callbacks, int32_t* output, int* result) try {
 Drain drain;
 uint8_t first = 0;
 *result = Invalid;
 if (!EsvmReadGuest(&first, config, 1)) { return true; }
 if (first != 0) { return false; }
 if (!EsvmWritableGuest(output, sizeof(*output))) { return true; }
 const int zero = 0; if (!EsvmStoreGuest(output, &zero, sizeof(zero))) { return true; }
 auto owner = GetOwner(*result);
 if (!owner) { return true; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { *result = Failed; return true; }
 EsvmSourceCallbacks24 copied {};
 uint64_t a = 0, b = 0, tail = 0; uint32_t c = 0, d = 0;
 if (!EsvmReadGuest(&copied, callbacks, sizeof(copied)) || !copied.event || copied.mask != 0xb ||
     !Field(config, 0x2948, &a) || a != UINT64_C(0x0000000200000001) ||
     !Field(config, 0x295c, &b) || b != UINT64_C(0x0000000200000003) ||
     !Field(config, 0x2964, &c) || c != 3 || !Field(config, 0x296c, &d) || d != 2 ||
     !Field(config, 0x2978, &tail) || tail != 0) { *result = Invalid; return true; }
 if (!Provenance(reinterpret_cast<uint64_t>(copied.event))) { *result = Failed; Trace("source-schema-rejected", 0, 0, *result); return true; }
 ReapDead(*owner);
 auto slot = Free(owner->contexts);
 if (slot == owner->contexts.end()) { *result = Full; return true; }
 Allocate(); auto source = std::make_shared<Context>();
 // Full-record copy is only reached after ownership, callback block/mask,
 // verified structural probes AND actual firmware provenance have succeeded.
 if (!EsvmReadGuest(source->config.data(), config, source->config.size())) { *result = Invalid; return true; }
 const auto& bytes = source->config;
 const char* options = reinterpret_cast<const char*>(bytes.data() + 0x1000);
 const auto* end = static_cast<const char*>(std::memchr(options, 0, 0x1000));
 if (Load<uint64_t>(bytes, 0x2948) != a || Load<uint64_t>(bytes, 0x295c) != b ||
     Load<uint32_t>(bytes, 0x2964) != c || Load<uint32_t>(bytes, 0x296c) != d || Load<uint64_t>(bytes, 0x2978) != 0 ||
     std::any_of(bytes.begin(), bytes.begin() + 0x1000, [](auto value) { return value != 0; }) ||
     !end || bytes[0x2974] > 1 || bytes[0x2975] > 1) { *result = Invalid; return true; }
 const std::string_view text(options, static_cast<size_t>(end - options));
 if (bytes[0x2976] != 0 || (!text.empty() && text != "pass_through=false" &&
     text != ";music_video_mode=\"cmc_audio_render\"" && text != "pass_through=false;music_video_mode=\"cmc_audio_render\"")) {
  *result = Failed; return true;
 }
 source->callbacks = copied;
 // Native construction initializes readiness to zero and queues Play, not
 // Pause. The host output clock remains stopped until explicit guest Play.
 source->handle = AllocateEsvmServiceHandle();
 if (!source->handle || !Valid(*owner)) { *result = Failed; return true; }
 if (!EsvmStoreGuest(output, &source->handle, sizeof(source->handle))) { *result = Invalid; return true; }
 *slot = source; *result = 0;
 Trace("source-admitted", source->handle, bytes.size(), 0);
 TraceSourceState(*owner, *source, "source-admitted");
 return true;
} catch (const std::bad_alloc&) { *result = Failed; return true; }

#define SOURCE_ABI __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
int SOURCE_ABI EsvmAddDecoder(int context, uint64_t bytes, const EsvmDecoderCallbacks24* callbacks, void** mapping) try {
 Drain drain;
 Trace("decoder-enter", context, bytes, 0);
 EsvmDecoderCallbacks24 copied {};
 if (!EsvmReadGuest(&copied, callbacks, sizeof(copied)) || !copied.complete || !copied.event ||
     !EsvmWritableGuest(mapping, sizeof(*mapping)) || bytes != EsvmDecoderMappingBytes) { return Invalid; }
 int result = 0; auto owner = GetOwner(result); if (!owner) { return result; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto source = Find(owner->contexts, context); if (!source) { return Invalid; }
 if (!Provenance(reinterpret_cast<uint64_t>(source->callbacks.event), reinterpret_cast<uint64_t>(copied.event),
                 reinterpret_cast<uint64_t>(copied.complete))) { return Failed; }
 ReapDead(*owner);
 if (owner->pending == PendingLimit || std::count_if(owner->decoders.begin(), owner->decoders.end(),
     [&](const auto& d) { return d && d->context == source; }) >= ChildrenLimit) { return Full; }
 auto slot = Free(owner->decoders); if (slot == owner->decoders.end()) { return Full; }
 Allocate(); auto decoder = std::make_shared<Decoder>();
 decoder->context = source; decoder->callbacks = copied;
  // Native regular-source input pool is 18 MiB (13.60 EsvmEngine 0x1a81d).
  // Keep a full 9 MiB admission window while small batches accumulate behind
  // paused output. Audio retains its independent 1 MiB bound in Consume/refill.
  decoder->limits.max_queued_packet_bytes = 18 * 1024 * 1024;
  decoder->packets = std::make_unique<EsvmPacketDecoder>(EsvmDecoderSchema::Firmware1360, decoder->limits);
 if (decoder->packets->Status().failure != PacketResult::Ok) { return Failed; }
 *slot = decoder; // Retain partially acquired resources if kernel rollback fails.
 if (!decoder->mapping.Create() || Fail(EsvmSourceFailure::Publish)) { ReapDead(*owner); return Failed; }
 decoder->handle = AllocateEsvmServiceHandle();
 if (!decoder->handle || !Valid(*owner)) { ReapDead(*owner); return Failed; }
 if (!EsvmStoreGuest(mapping, &decoder->mapping.address, sizeof(void*))) { ReapDead(*owner); return Invalid; }
 decoder->live = true;
 Pending pending(*owner);
 Trace("decoder-admitted-1360", decoder->handle, bytes, 0);
 StateTraceEntry(*source, "add-decoder", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
 TraceSourceState(*owner, *source, "decoder-admitted");
 Complete(*owner, copied.complete, decoder->handle, 0, copied.user);
 DispatchRefills(*owner, context);
 return 0;
} catch (const std::bad_alloc&) { return Failed; }

int SOURCE_ABI EsvmSubmitDecoderBytes(int handle, uint64_t bytes, EsvmDecoderComplete complete, void* user) try {
 Drain drain;
 Trace("submit-enter", handle, bytes, 0);
 if (!complete || bytes == 0 || bytes > EsvmDecoderMappingBytes) { return Invalid; }
 int result = 0; auto owner = GetOwner(result); if (!owner) { return result; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto decoder = Find(owner->decoders, handle); if (!decoder || !decoder->context->live) { return Invalid; }
 StateTraceEntry(*decoder->context, "submit", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
 if (owner->pending == PendingLimit) { return Full; }
 // Safe snapshot even if malicious guest code unmapped its arena. Consume then
 // makes its own padded packet copies before ANY guest completion is delivered.
 Allocate(); auto snapshot = std::make_unique<uint8_t[]>(static_cast<size_t>(bytes));
 Pending pending(*owner);
 int detail = 0;
 const char* stage = "service-state";
 bool snapshot_read = false;
 if (decoder->service_failure) { result = decoder->service_failure; }
 else if (!(snapshot_read = EsvmReadGuest(snapshot.get(), decoder->mapping.address, static_cast<size_t>(bytes)))) { result = Invalid; stage = "mapping-read"; }
 else if (!SdrInput({snapshot.get(), static_cast<size_t>(bytes)}, handle)) { result = Failed; detail = static_cast<int>(PacketResult::UnsupportedConfig); stage = "sdr-admission"; }
 else {
  stage = "packet-admission";
  TraceAacConfiguration(handle, {snapshot.get(), static_cast<size_t>(bytes)});
  uint32_t subtype = 0;
  if (bytes >= 16) { std::memcpy(&subtype, snapshot.get() + 4, sizeof(subtype)); }
  Trace(subtype == 0 ? "submit-wire-au" : "submit-wire-subtype", handle, bytes, 0, static_cast<int>(subtype));
  const auto consumed = decoder->packets->Consume({snapshot.get(), static_cast<size_t>(bytes)}, decoder->context->input_started);
  if (consumed.config_barrier != EsvmDecoderConfigBarrier::None) {
   // Independent error-only budget: this barrier can become a native fatal
   // push error, even when routine media trace budgets are exhausted.
   static std::atomic_uint captures {0};
   auto count = captures.load(std::memory_order_relaxed);
   while (count < 16) {
    if (!captures.compare_exchange_weak(count, count + 1, std::memory_order_relaxed)) { continue; }
    const auto status = decoder->packets->Status();
    LOGF("MediaFramework config barrier: pid=%d source=%d child=%d identical=%u codec=%u generation=%llu "
         "batch-bytes=%llu packets=%llu packet-bytes=%llu decoded=%llu held=%u eos=%u\n",
         owner->pid, decoder->context->handle, handle,
         unsigned(consumed.config_barrier == EsvmDecoderConfigBarrier::Identical), unsigned(status.codec),
         static_cast<unsigned long long>(status.generation), static_cast<unsigned long long>(bytes),
         static_cast<unsigned long long>(status.queued_packets), static_cast<unsigned long long>(status.queued_packet_bytes),
         static_cast<unsigned long long>(status.queued_frames), unsigned(status.held_codec_frame), unsigned(status.eos_accepted));
    break;
   }
  }
  if (StateTraceEnabled()) {
   ++decoder->trace_batch; decoder->trace_units = consumed.access_units; // Includes EOS-only control units.
   decoder->trace_subtype = subtype; decoder->trace_result = static_cast<uint64_t>(consumed.result);
  }
  detail = static_cast<int>(consumed.result);
  result = consumed.result == PacketResult::Backpressure ? Full : PacketError(consumed.result);
  if (consumed.result == PacketResult::InvalidState && !decoder->context->input_started) { result = InputNotStarted; }
  if (consumed.result == PacketResult::Ok) {
   if (consumed.configuration_queued) { Trace("video-configuration-queued", handle, bytes, 0, 0, owner->pid); }
   result = FinishInitialConfiguration(*owner, decoder->context);
   decoder->refill.Accepted(decoder->packets->Status().generation);
   // Packet status owns the verified media kind; source does not infer it from
   // the first outer record, public AU size or the order children were created.
    SyncVideoGeneration(*owner, decoder);
    stage = "audio-generation";
    if (!result) { result = SyncAudioGeneration(*owner, decoder); }
    const auto pumped = decoder->packets->Pump();
    detail = static_cast<int>(pumped); if (!result) { result = PacketError(pumped); stage = "decode"; }
    if (result) { RetireVideo(*owner, decoder); RetireAudio(*owner, decoder); }
   if (!result) {
    try {
     const int selected = SelectConfiguredVideo(*owner, decoder->context);
     if (selected) { Trace("render-deferred-selection-failed", decoder->context->handle, handle, selected); }
     // Bytes already admitted: a geometry/stream-selection failure is NOT a
     // failed packet copy. Render reports it; never invite resubmitting the AU.
      stage = "video-publish";
      result = PublishDue(*owner, decoder, Clock::now());
      if (!result && EsvmIsAudioCodec(decoder->packets->Status().codec)) {
       stage = "audio-publish";
      if (!StartScheduler()) { result = Failed; }
      else { result = PublishAudio(*owner, decoder, Clock::now()); }
     }
    } catch (const std::bad_alloc&) {
     // Consumption already owns bounded bytes/decoded pixels. A temporary
     // mailbox control-block allocation failure is not failed byte admission;
     // retain the staged frame and let the clock worker retry publication.
     Trace("mailbox-allocation-deferred", handle, bytes, Failed);
    }
   }
   Trace("decoded-frames-queued", handle, decoder->packets->Status().queued_frames, result);
  }
 }
 // Consume or Pump may have entered sticky failure after the pre-pump sync.
 // Cleanup precedes completion/reentry, including the no-output-lease case.
 if (decoder->packets->Status().state == EsvmDecoderState::Failed) {
  RetireVideo(*owner, decoder); SyncAudioGeneration(*owner, decoder);
  if (!result) { result = Failed; }
 }
 Trace("submit-complete", handle, bytes, result, detail);
 TraceSubmissionFailure(handle, bytes, result, stage, detail, decoder->packets->Status(),
                        snapshot_read ? std::span<const uint8_t>(snapshot.get(), static_cast<size_t>(bytes)) : std::span<const uint8_t> {});
 // Record the output transition BEFORE completion can reenter Play/Flush.
 // Observing it only during dispatch would let an earlier output rearm Sync
 // after the guest's Play has already cleared it.
 ObserveReadiness(*owner, decoder->context->handle);
 Complete(*owner, complete, handle, result, user);
 DispatchRefills(*owner, decoder->context->handle);
 return 0;
} catch (const std::bad_alloc&) { return Failed; }

int SOURCE_ABI EsvmRemoveDecoder(int handle, EsvmDecoderComplete complete, void* user) try {
 Drain drain;
 if (!complete) { return Invalid; }
 int result = 0; auto owner = GetOwner(result); if (!owner) { return result; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto decoder = Find(owner->decoders, handle); if (!decoder) { return Invalid; }
 StateTraceEntry(*decoder->context, "remove-decoder", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
 TraceSourceState(*owner, *decoder->context, "retire-before");
 if (owner->pending == PendingLimit) { return Full; }
 Pending pending(*owner);
 RetireVideo(*owner, decoder);
 const bool audio_released = RetireAudio(*owner, decoder);
 decoder->refill.Retire(); decoder->live = false; decoder->callbacks = {}; decoder->packets.reset();
 TraceSourceState(*owner, *decoder->context, "retire-after");
 const bool released = decoder->mapping.Reset() && audio_released;
 ReapDead(*owner);
 Trace("decoder-removed", handle, 0, released ? 0 : Failed);
 Complete(*owner, complete, handle, released ? 0 : Failed, user);
 return 0;
} catch (const std::bad_alloc&) { return Failed; }

int EsvmDeleteSource(int handle, uintptr_t caller) try {
 Drain drain;
 int result = 0; auto owner = GetOwner(result); if (!owner) { return result; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto source = Find(owner->contexts, handle); if (!source) { return Invalid; }
 source->readiness.Retire(); source->live = false; source->callbacks = {};
 StateTraceEntry(*source, "delete-source", caller);
 TraceSourceState(*owner, *source, "source-retired");
 bool released = true;
 for (auto& decoder: owner->decoders) {
   if (decoder && decoder->context == source) {
    RetireVideo(*owner, decoder);
    released = RetireAudio(*owner, decoder) && released;
   decoder->refill.Retire(); decoder->live = false; decoder->callbacks = {}; decoder->packets.reset();
   released = decoder->mapping.Reset() && released;
  }
 }
 ReapDead(*owner);
 return released ? 0 : Failed;
} catch (const std::bad_alloc&) { return Failed; }

int EsvmControlSource(int handle, EsvmSourceControl control, double value, uintptr_t caller) try {
 Drain drain;
 int result = 0; auto owner = GetOwner(result); if (!owner) { return result; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto source = Find(owner->contexts, handle); if (!source) { return Invalid; }
 StateTraceEntry(*source, control == EsvmSourceControl::Pause ? "pause" : control == EsvmSourceControl::Play ? "play" : "control", caller);
 TraceSourceState(*owner, *source, "control-before");
 struct TraceControlExit { Owner& owner; Context& source; ~TraceControlExit() { TraceSourceState(owner, source, "control-after"); } } trace_exit {*owner, *source};
 switch (control) {
  case EsvmSourceControl::Play:
   source->readiness.Play();
   if (!source->playing) { source->anchor = Clock::now(); source->playing = true; }
   Service().scheduler_wake.notify_all();
   { const int applied = ApplyAudioControls(*owner, source); DispatchRefills(*owner, handle); return applied; }
  case EsvmSourceControl::Pause:
   source->readiness.Pause();
   source->elapsed_us = PlayedUs(*source, Clock::now()); source->playing = false;
   { const int applied = ApplyAudioControls(*owner, source); DispatchRefills(*owner, handle); return applied; }
  case EsvmSourceControl::Rate: case EsvmSourceControl::AudibleRate:
   if (!std::isfinite(value) || value < 0 || value > 16) { return Invalid; }
   // No time-stretch/audio clock implementation yet: don't report it applied.
   if (value != 1) { return Failed; }
   if (control == EsvmSourceControl::Rate) { source->rate = value; } else { source->audible_rate = value; }
   return 0;
  case EsvmSourceControl::Volume:
   if (!std::isfinite(value) || value < 0 || value > 1) { return Invalid; }
   source->volume = value; return ApplyAudioControls(*owner, source);
  case EsvmSourceControl::Mute:
   if (value != 0 && value != 1) { return Invalid; }
   source->muted = value != 0; return ApplyAudioControls(*owner, source);
  default: return Failed;
 }
} catch (const std::bad_alloc&) { return Failed; }
int SOURCE_ABI EsvmSetAudibleRate(int context, double rate) {
 return EsvmControlSource(context, EsvmSourceControl::AudibleRate, rate, reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
}

int SOURCE_ABI EsvmGetSourcePts(int context, uint64_t* pts, uint16_t* sequence) try {
 Drain drain;
 if (!EsvmWritableGuest(pts, sizeof(*pts)) || !EsvmWritableGuest(sequence, sizeof(*sequence))) { return Invalid; }
 int result = 0; auto owner = GetOwner(result); if (!owner) { return result; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto source = Find(owner->contexts, context); if (!source) { return Invalid; }
 std::shared_ptr<Decoder> video;
 for (const auto& decoder: owner->decoders) {
  if (decoder && decoder->live && decoder->context == source && decoder->video && !decoder->video->IsRetired()) {
   if (video) { return Failed; } video = decoder;
  }
 }
 if (!video) { return Failed; }
 const auto shown = video->video->Presented();
 // Queue-present acknowledgment is the strongest available host evidence, NOT
 // physical scanout timing feedback. Decode/upload/elapsed clock never suffice.
 if (!shown.serial || !shown.has_pts || shown.pts < 0 || shown.generation != video->packets->Status().generation ||
     !Valid(*owner)) { return Failed; }
 const auto value = static_cast<uint64_t>(shown.pts);
 if (!EsvmStoreGuest(pts, &value, sizeof(value)) || !EsvmStoreGuest(sequence, &shown.sequence, sizeof(shown.sequence))) { return Invalid; }
 return 0;
} catch (const std::bad_alloc&) { return Failed; }
int SOURCE_ABI EsvmRenderSource(int context, float x, float y, float width, float height) try {
 Drain drain;
 if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
     x < -1 || y < -1 || x > 2 || y > 2 || width < 0 || height < 0 || width > 2 || height > 2 || x + width > 2 || y + height > 2) { return Invalid; }
 int result = 0; auto owner = GetOwner(result); if (!owner) { return result; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto source = Find(owner->contexts, context); if (!source) { return Invalid; }
 StateTraceEntry(*source, "render", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
 // Current mailbox implements contained [0,1] rectangles. Newer firmware's
 // offscreen range remains explicitly unsupported, never silently clamped.
 if (x < 0 || y < 0 || double(x) + width > 1 || double(y) + height > 1) { return Failed; }
 source->rectangle = {x, y, width, height};
 source->rectangle_set = true;
 const int accepted = SelectConfiguredVideo(*owner, source);
 if (accepted) { return accepted; }
 for (const auto& video: owner->decoders) {
  if (!video || video->context != source || !video->video) { continue; }
  try { const int published = PublishDue(*owner, video, Clock::now()); if (published) { return published; } }
  catch (const std::bad_alloc&) { RetireVideo(*owner, video); return Failed; }
 }
 Service().scheduler_wake.notify_all();
 DispatchRefills(*owner, context);
 return accepted;
} catch (const std::bad_alloc&) { return Failed; }

int SOURCE_ABI EsvmFlushContext(int context, EsvmDecoderComplete complete, void* user) try {
 Drain drain;
 if (!complete) { return Invalid; }
 int result = 0; auto owner = GetOwner(result); if (!owner) { return result; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return Failed; }
 auto source = Find(owner->contexts, context); if (!source) { return Invalid; }
 StateTraceEntry(*source, "flush", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
 TraceSourceState(*owner, *source, "flush-before");
 if (owner->pending == PendingLimit) { return Full; }
 Pending pending(*owner);
 std::array<std::shared_ptr<Nv::NativeVideoStream>, ChildrenLimit> clearing {};
 size_t clearing_count = 0;
 // Source Flush has no sequence argument. Native initial Flush is independent
 // of AU sequence establishment; preserve the decoder's actual sequence state.
 for (const auto& decoder: owner->decoders) {
  if (decoder && decoder->live && decoder->context == source) {
   const auto status = decoder->packets->Status();
   if (status.state == EsvmDecoderState::Failed) { result = Failed; break; }
  }
 }
 if (!result) {
  source->readiness.Discontinuity(); source->readiness_waiting_output = false;
  for (const auto& decoder: owner->decoders) {
   if (decoder && decoder->live && decoder->context == source) {
     result = PacketError(decoder->packets->FlushCurrent());
    if (decoder->video && clearing_count < clearing.size()) { clearing[clearing_count++] = decoder->video; }
     SyncVideoGeneration(*owner, decoder);
     if (!result) { result = SyncAudioGeneration(*owner, decoder); }
    if (result) { break; }
   }
  }
 }
 // Bind the flushed generation before requesting its first output. A concurrent
 // lifecycle/control operation while renderer-clear releases the gate must not
 // let this completion recreate an obsolete synchronization request.
 ObserveReadiness(*owner, context);
 const auto readiness_revision = source->readiness.Revision();
 if (!result && clearing_count != 0 && owner->callbacks != 0) {
  // Reentry still holds an outer recursive dispatch gate. Unlocking only this
  // level would NOT satisfy the no-wait-under-source-lock contract.
  result = Failed;
 } else if (!result && clearing_count != 0) {
  // Native async completion does not authorize reporting renderer-clear from a
  // CPU bool Flush. Keep the callback on this guest entry thread (valid TLS),
  // but wait only OUTSIDE source/lifecycle locks. The renderer commits revisions
  // independently; unavailable output gets a bounded honest failure, not a hang.
  gate.unlock();
   struct Relock { EsvmDispatchLock& lock; ~Relock() { lock.lock(); } } relock {gate};
  try {
   const auto deadline = Clock::now() + std::chrono::milliseconds(100);
   for (;;) {
    bool cleared = true;
    for (size_t i = 0; i < clearing_count; ++i) { cleared = clearing[i]->ClearAcknowledged() && cleared; }
    if (cleared) { break; }
    if (!owner->live.load() || Clock::now() >= deadline) { result = Failed; break; }
    auto& registry = Service(); std::unique_lock wait_lock(registry.scheduler_mutex);
    registry.scheduler_wake.wait_for(wait_lock, std::chrono::milliseconds(2));
   }
  } catch (const std::system_error&) { result = Failed; }
 }
 if (!source->live || !Valid(*owner)) { result = Failed; }
 if (!result && source->readiness.Revision() == readiness_revision) {
  source->readiness_waiting_output = true;
 }
 Trace("flush-complete", context, 0, result);
 TraceSourceState(*owner, *source, "flush-after");
 Complete(*owner, complete, context, result, user);
 DispatchRefills(*owner, context);
 return 0;
} catch (const std::bad_alloc&) { return Failed; }
int SOURCE_ABI EsvmRejectContextOperation(int context, const void* params, EsvmDecoderComplete complete, void*) {
 if (!params || !complete) { return Invalid; }
 // Parameter schema unrecovered: reject transport synchronously, no callback owed.
 return EsvmControlSource(context, EsvmSourceControl::Unsupported);
}

bool TakeEsvmDecodedFrame(int pid, int context, int handle, EsvmDecodedFrame* output) {
 if (!output) { return false; }
 Drain drain; std::shared_ptr<Owner> owner;
 { Common::HostLockCriticalGuard critical; auto& registry = Service(); std::lock_guard lock(registry.mutex);
   auto it = registry.owners.find(pid); if (it == registry.owners.end()) { return false; } owner = it->second; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return false; }
 auto source = Find(owner->contexts, context); auto decoder = Find(owner->decoders, handle);
 if (!source || !source->playing || !decoder || decoder->context != source || decoder->video) { return false; }
 if (!PopReadinessFrame(*owner, decoder, output)) {
  decoder->packets->Pump(); return PopReadinessFrame(*owner, decoder, output);
 }
 decoder->packets->Pump(); ObserveReadiness(*owner, context); return true;
}

void InvalidateEsvmSourcesOwner(int pid) {
 Common::HostLockCriticalGuard critical; auto& registry = Service(); std::lock_guard lock(registry.mutex);
 auto it = registry.owners.find(pid); if (it == registry.owners.end()) { return; }
 Retire(registry, it->second); registry.owners.erase(it);
}
bool DrainRetiredEsvmSources(bool wait) {
 wait = wait && !EsvmDispatchScopeActive();
 bool cleanup_ok = true;
 std::shared_ptr<Owner> owner;
 { Common::HostLockCriticalGuard critical; auto& registry = Service(); std::lock_guard lock(registry.mutex); owner = registry.retired; }
 while (owner) {
  std::shared_ptr<Owner> next;
  { EsvmDispatchLock gate(*owner->dispatch, wait);
   bool released = gate.owns_lock() && owner->callbacks == 0;
   if (released) {
    for (auto& decoder: owner->decoders) {
     if (decoder) { RetireVideo(*owner, decoder); released = RetireAudio(*owner, decoder) && released; decoder->refill.Retire(); decoder->live = false; decoder->callbacks = {}; decoder->packets.reset(); released = decoder->mapping.Reset() && released; }
    }
    if (released) { for (auto& decoder: owner->decoders) { decoder.reset(); } for (auto& source: owner->contexts) { source.reset(); } }
    else { cleanup_ok = false; }
   }
   Common::HostLockCriticalGuard critical; auto& registry = Service(); std::lock_guard lock(registry.mutex);
   next = owner->retired_next;
    // Keep visible while waiting OR deferring a contended/callback gate, so an
    // external blocking retirement cannot miss an admitted callback's owner.
   if (released) { auto* link = &registry.retired; while (*link && *link != owner) { link = &(*link)->retired_next; } if (*link) { *link = owner->retired_next; } }
  }
  owner = std::move(next);
 }
 return cleanup_ok;
}

void StopEsvmSourceScheduler() {
 auto& registry = Service(); std::thread worker;
 { std::lock_guard lock(registry.scheduler_mutex);
   registry.scheduler_stopping.store(true); registry.scheduler_wake.notify_all(); worker = std::move(registry.scheduler); }
 if (worker.joinable()) { worker.join(); }
}

LIB_DEFINE(InitMediaFrameworkSource_1) {
 LIB_FUNC("Q6VTUcSi2kg", EsvmAddDecoder);
 LIB_FUNC("WBBO8yQhDP4", EsvmSubmitDecoderBytes);
 LIB_FUNC("I27jBmJyEWA", EsvmRemoveDecoder);
 LIB_FUNC("tVi7LPlxmL0", EsvmFlushContext);
 LIB_FUNC("PLJs4B6fCk0", EsvmGetSourcePts);
 LIB_FUNC("jDow6uu9KS0", EsvmRenderSource);
 LIB_FUNC("dlFQzvKJ2rk", EsvmSetAudibleRate);
 LIB_FUNC("onZVvMJVfUc", EsvmRejectContextOperation);
}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void SetEsvmSourceProvenanceForTest(EsvmSourceProvenanceProbe probe) { provenance_probe.store(probe); }
void EsvmSourceFailNextForTest(EsvmSourceFailure point) { failure = point; }
EsvmSourceResources EsvmSourceResourcesForTest() {
 std::array<std::shared_ptr<Owner>, 128> owners {}; size_t count = 0;
 size_t closing_audio = 0;
 size_t retired_owners = 0;
 { auto& registry = Service(); std::lock_guard lock(registry.mutex);
   for (const auto& [pid, owner]: registry.owners) { if (count < owners.size()) { owners[count++] = owner; } }
   for (auto owner = registry.retired; owner; owner = owner->retired_next) {
    ++retired_owners; if (count < owners.size()) { owners[count++] = owner; }
   }
   closing_audio = registry.closing_audio_count.load(std::memory_order_relaxed); }
 EsvmSourceResources result {};
 result.closing_audio = closing_audio;
 result.retired_owners = retired_owners;
 for (const auto& owner: owners) {
  if (!owner) { continue; } EsvmDispatchLock gate(*owner->dispatch);
  result.pending += owner->pending;
  for (const auto& source: owner->contexts) { if (source) { ++result.contexts; } }
  for (const auto& decoder: owner->decoders) {
    if (decoder) { ++result.decoders; result.mappings += decoder->mapping.address != nullptr; result.descriptors += decoder->mapping.descriptor >= 0;
     result.audio_leases += decoder->audio != nullptr; result.staged_audio_bytes += decoder->staged_audio.bytes;
    if (decoder->packets) { result.decoded_frames += decoder->packets->Status().queued_frames; } }
  }
 }
 return result;
}
EsvmSourceAudioSnapshot EsvmSourceAudioForTest(int handle) {
 Drain drain;
 int result = 0; auto owner = GetOwner(result); if (!owner) { return {}; }
 EsvmDispatchLock gate(*owner->dispatch);
 if (!Valid(*owner)) { return {}; }
 auto decoder = Find(owner->decoders, handle); if (!decoder || !decoder->packets) { return {}; }
 return {decoder->audio, decoder->packets->Status().generation, decoder->audio_accepted_frames, decoder->staged_audio.bytes, decoder->audio_result,
         decoder->packets->Status().state};
}
#endif
} // namespace Libs::MediaFramework
