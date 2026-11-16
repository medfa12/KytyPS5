// Full-emulator fixture, include inside VirtualMemoryAllocationTests namespace.
// Top-level include libs/mediaFrameworkEsvmSource.h, graphics/presentation/nativeVideo.h
// libs/mediaFrameworkEsvmKeys.h and fixtures/esvm-vp9/packets.h. Link the
// parent-owned nativeVideo.cpp mailbox. Paired lifecycle tests require the same
// dedicated KYTY_ESVM_KEYS_TESTS target as the keys suite; NEVER production.
// Dispatch TestMediaFrameworkEsvmSource() after InitSubsystems in its own process.
// Source/packet cpp files are production glob inputs; parent owns integration.
#include "fixtures/esvm-aac/packets.h"
#include "fixtures/esvm-ac3/packets.h"
#include "fixtures/esvm-nal/packets.h"
#include "fixtures/esvm-nal/transition.h"
namespace MediaFrameworkEsvmSourceTests {
namespace Mf = Libs::MediaFramework;
constexpr const char* Test = "MediaFrameworkEsvmSource";
constexpr int Invalid = static_cast<int>(0x81790001u), Failed = static_cast<int>(0x81790fffu), Full = static_cast<int>(0x81790007u);
constexpr int InputNotStarted = static_cast<int>(0x81780002u);
struct Api {
 Loader::SymbolDatabase symbols;
 using Add = KYTY_SYSV_ABI int (*)(const void*, const Mf::EsvmSourceCallbacks24*, int32_t*);
 using Unary = KYTY_SYSV_ABI int (*)(int);
 using Scalar = KYTY_SYSV_ABI int (*)(int, double);
 using Mute = KYTY_SYSV_ABI int (*)(int, bool);
 Add add;
 Unary remove, play, pause;
 Scalar rate, volume;
 Mute mute;
 decltype(&Mf::EsvmAddDecoder) decoder;
 decltype(&Mf::EsvmSubmitDecoderBytes) submit;
 decltype(&Mf::EsvmRemoveDecoder) remove_decoder;
 decltype(&Mf::EsvmFlushContext) flush;
 decltype(&Mf::EsvmGetSourcePts) pts;
 decltype(&Mf::EsvmRenderSource) render;
 decltype(&Mf::EsvmRejectContextOperation) unsupported;
 decltype(&Mf::EsvmRequestKeySystemAccess) access;
 decltype(&Mf::EsvmCreateMediaKeys2) keys;
 decltype(&Mf::EsvmQueryProvisioning) query;
 template<class F> F Function(const char* nid) {
  const auto* record = symbols.Find({nid, "MediaFrameworkInterface", 1, "MediaFrameworkInterface", 1, 1, Loader::SymbolType::Func});
  const auto* fallback = symbols.FindByNid(nid, Loader::SymbolType::Func);
  Check(Test, record && fallback && record->vaddr == fallback->vaddr, "versioned production export routing");
  return reinterpret_cast<F>(record->vaddr);
 }
 Api() {
  Mf::InitMediaFramework_1(&symbols);
  add = Function<Add>("O9kOx8wYA-Y"); remove = Function<Unary>("kgxp80kqgrA");
  play = Function<Unary>("XMjmgYrWzls"); pause = Function<Unary>("NlSpa23Wdh0");
  rate = Function<Scalar>("c-AJTsu0v00"); volume = Function<Scalar>("4PQhrMOSD6U"); mute = Function<Mute>("TS2pCViUFos");
  decoder = Function<decltype(decoder)>("Q6VTUcSi2kg"); submit = Function<decltype(submit)>("WBBO8yQhDP4");
  remove_decoder = Function<decltype(remove_decoder)>("I27jBmJyEWA"); flush = Function<decltype(flush)>("tVi7LPlxmL0");
  pts = Function<decltype(pts)>("PLJs4B6fCk0"); render = Function<decltype(render)>("jDow6uu9KS0");
  unsupported = Function<decltype(unsupported)>("onZVvMJVfUc");
  access = Function<decltype(access)>("Bn34VVQIB-g"); keys = Function<decltype(keys)>("UMuQN3ysX9o");
  query = Function<decltype(query)>("O20dcRTe2KE");
 }
};
static int source_events = 0;
static int progress_events = 0;
static Api* progress_api = nullptr;
static int progress_context = 0;
static uint64_t progress_pts = 0;
static std::thread::id progress_thread;
static uint64_t progress_critical = 0;
static std::atomic_uint decoder_events {0};
struct ReadinessProbe {
 Api* api = nullptr;
 int context = 0, calls = 0, action = 0;
 int pid = Loader::GetCurrentGuestProcessId();
 std::thread::id thread = std::this_thread::get_id();
 uint64_t critical = Common::GetHostLockCriticalCounter()->load();
};
static thread_local ReadinessProbe* readiness_probe = nullptr;
static KYTY_SYSV_ABI void SourceEvent(int event, const void* payload, void* user) {
 if (event == 0xf) {
  ++progress_events;
  uint64_t pts = 0; uint16_t sequence = 0;
  Check(Test, !payload && progress_api && Mf::EsvmDispatchScopeActive() &&
              std::this_thread::get_id() == progress_thread && Loader::GetCurrentGuestProcessId() == 100 &&
              Common::GetHostLockCriticalCounter()->load() == progress_critical &&
              progress_api->pts(progress_context, &pts, &sequence) == 0 && pts == progress_pts && sequence == 7,
        "native progress callback reenters PTS query with acknowledged timestamp and sequence");
  Check(Test, progress_api->render(progress_context, 0, 0, 1, 1) == 0,
        "progress reentry cannot recursively dispatch the claimed receipt");
  return;
 }
 ++source_events;
 Check(Test, event == 0x2001 && !payload, "source readiness uses recovered source ABI, not citron 0x2000 or public event 7");
 if (auto* probe = readiness_probe) {
  ++probe->calls;
  Check(Test, user == probe && Loader::GetCurrentGuestProcessId() == probe->pid &&
              std::this_thread::get_id() == probe->thread && Mf::EsvmDispatchScopeActive() &&
              Common::GetHostLockCriticalCounter()->load() == probe->critical,
        "readiness preserves copied user pointer and owning guest TLS outside host-critical locks");
  const int action = probe->action; probe->action = 0;
  if (action == 1) {
   Check(Test, probe->api->pause(probe->context) == 0 && probe->api->play(probe->context) == 0,
         "readiness callback may reenter controls without recursive delivery");
  }
  if (action == 2) { Check(Test, probe->api->remove(probe->context) == 0, "readiness callback may delete its source"); }
  if (action == 3) { Check(Test, Mf::EsvmUnregisterClient("citroncore") == 0, "readiness callback may retire its owning epoch"); }
 }
}
void ReadinessPolicyModel() {
 // Host policy checks against disassembled semantics; no firmware execution.
 Mf::EsvmReadiness state;
 Mf::EsvmReadinessObservation observation;
 observation.decoded = 4;
 observation.input_count = 8;
 observation.buffered = 7;
 state.Pause(); state.SyncVideo(); state.Observe(observation);
 Check(Test, !state.Pending(), "four decoded plus seven buffered units do not meet normal-rate native threshold");
 observation.decoding = 1; state.Observe(observation);
 const auto first = state.Pending();
 Check(Test, first && state.Claim(first) && !state.Claim(first), "policy mixed-queue predicate yields exactly one claim");
 state.Observe(observation);
 Check(Test, !state.Pending(), "unchanged sufficient queues cannot rearm native sync flag");
 state.SyncVideo(); state.Observe(observation);
 Check(Test, state.Pending() && !state.Claim(first), "rearmed observation cannot be consumed with an earlier snapshot ticket");
 state.Play();
 Check(Test, !state.Pending(), "Play revokes deferred paused readiness");

 observation = {}; observation.decoded = 1; observation.input_count = 2;
 observation.buffered = 2; observation.allocation_state = 1;
 observation.free_bytes = Mf::EsvmReadiness::FreeByteThreshold;
 state.Pause(); state.SyncVideo(); state.Observe(observation);
 Check(Test, !state.Pending(), "allocation stall at exactly nine MiB available is insufficient");
 --observation.free_bytes; state.Observe(observation);
 const auto blocked = state.Pending();
 Check(Test, blocked != 0, "native allocation fallback uses available bytes and all-input coverage");
 state.Discontinuity(); state.SyncVideo();
 observation.allocation_state = 0; observation.input_ended = true; observation.buffered = 1;
 state.Observe(observation);
 Check(Test, !state.Pending() && !state.Claim(blocked), "flush revokes old ticket and EOS cannot bypass uncovered input");
 observation.buffered = 2; observation.decoded = 0; state.Observe(observation);
 Check(Test, !state.Pending(), "EOS without actual decoded video is insufficient");
 observation.decoded = 1; state.Observe(observation);
 Check(Test, state.Pending() != 0, "short EOS input can satisfy native readiness without twelve decoded frames");
 state.Timestamp(100); state.Timestamp(101);
 Check(Test, !state.Pending(), "advancing native timestamp invalidates local readiness arm and pending delivery");
 state.Pause(); state.SyncVideo(); state.Observe(observation); state.Retire();
 Check(Test, !state.Pending(), "retirement revokes deferred event");
}
struct ReentrantRetirement {
 int pid = 0;
 bool terminate = false;
 std::binary_semaphore entered {0}, proceed {0}, returned {0}, finish {0}, done {0};
 void Run() {
  Check(Test, Mf::EsvmDispatchScopeActive(), "callback is within shared keys/source dispatch scope");
  // Put BOTH genuine owners on BOTH retired lists before either global drain.
  // This makes the old A->B/B->A lock inversion deterministic, not probabilistic.
  Mf::InvalidateEsvmKeysOwner(pid); Mf::InvalidateEsvmSourcesOwner(pid);
  entered.release();
  Check(Test, proceed.try_acquire_for(std::chrono::seconds(5)), "start paired reentrant lifecycle calls");
  Check(Test, (terminate ? Mf::EsvmTerminate() : Mf::EsvmUnregisterClient("citroncore")) == 0,
        "reentrant lifecycle closes admission without waiting for another callback's gate");
  returned.release();
  // Keep this outer gate held until BOTH calls returned. Routine exit drains
  // must defer too, even if the other thread has already left its lifecycle call.
  Check(Test, finish.try_acquire_for(std::chrono::seconds(5)), "release paired outer callback gates");
 }
 static KYTY_SYSV_ABI void Query(bool provisioned, int result, void* user) {
  Check(Test, !provisioned && result == 0, "real typed Query still truthfully reports absent credentials");
  static_cast<ReentrantRetirement*>(user)->Run();
 }
};
static thread_local ReentrantRetirement* reentrant_retirement = nullptr;
struct RefillProbe {
 Api* api = nullptr;
 int context = 0, calls = 0, action = 0;
 std::thread::id thread = std::this_thread::get_id();
 uint64_t critical = Common::GetHostLockCriticalCounter()->load();
 std::binary_semaphore* entered = nullptr;
 std::binary_semaphore* resume = nullptr;
};
static thread_local RefillProbe* refill_probe = nullptr;
static KYTY_SYSV_ABI void DecoderEvent(int decoder, int result) {
 ++decoder_events;
 Check(Test, decoder > 0 && result == 0, "Q6 refill is exact decoder/result ABI, never a source-ready event");
 if (auto* probe = refill_probe) {
  ++probe->calls;
  Check(Test, std::this_thread::get_id() == probe->thread && Common::GetHostLockCriticalCounter()->load() == probe->critical,
        "refill callback stays on eligible guest entry thread, outside registry/lifecycle/codec operations");
  const int action = probe->action; probe->action = 0;
  if (action == 1) { Check(Test, probe->api->pause(probe->context) == 0 && probe->api->play(probe->context) == 0,
                                "refill reenters controls without redispatching the same credit"); }
  if (action == 2) { Check(Test, probe->api->remove(probe->context) == 0, "refill may delete its source and both children"); }
  if (action == 3) {
   probe->entered->release();
   Check(Test, probe->resume->try_acquire_for(std::chrono::seconds(5)), "release held refill callback");
  }
  if (action == 4) { reentrant_retirement->Run(); }
 }
}
struct Done {
 int calls = 0, handle = -1, result = 0x1234, action = 0;
 Api* api = nullptr;
 int context = 0;
 void* poison_mapping = nullptr;
 size_t poison_bytes = 0;
 uint64_t critical = 0;
 std::binary_semaphore* entered = nullptr;
 std::binary_semaphore* resume = nullptr;
 static KYTY_SYSV_ABI void Complete(int handle, int result, void* user) {
  auto& out = *static_cast<Done*>(user);
  ++out.calls; out.handle = handle; out.result = result;
  Check(Test, Common::GetHostLockCriticalCounter()->load() == out.critical, "no registry/lifecycle host-critical lock across callback");
  if (out.poison_mapping) { std::memset(out.poison_mapping, 0xa5, out.poison_bytes); }
  if (out.action == 1) { Check(Test, out.api->remove(out.context) == 0, "reentrant context deletion from completion"); }
  if (out.action == 2) { Check(Test, Mf::EsvmUnregisterClient("citroncore") == 0, "reentrant owner unregister from completion"); }
  if (out.action == 3) { Check(Test, out.api->pause(out.context) == 0 && out.api->play(out.context) == 0, "reentrant shared control routing"); }
   if (out.action == 4) {
   out.entered->release();
   Check(Test, out.resume->try_acquire_for(std::chrono::seconds(5)), "release held source completion");
   }
   if (out.action == 5) { reentrant_retirement->Run(); }
   if (out.action == 6) {
    const auto failed = Mf::EsvmSourceAudioForTest(handle);
    Check(Test, result == Failed && failed.packet_state == Mf::EsvmDecoderState::Failed &&
                !failed.lease && !failed.staged_bytes && failed.result == Libs::Audio::AudioInternal::NativeQueueResult::Failed,
          "fatal PCM/lease cleanup is already complete when guest completion reenters");
   }
 }
};
static bool schema_allowed = true;
static bool Provenance(uint64_t source, uint64_t event, uint64_t complete) {
 // Only this explicit host-only test seam supplies synthetic module provenance.
 // Production uses loaded ELF backing SHA256 + exact native callback RVAs.
 return schema_allowed && source == reinterpret_cast<uint64_t>(&SourceEvent) &&
        (!event || event == reinterpret_cast<uint64_t>(&DecoderEvent)) &&
        (!complete || complete == reinterpret_cast<uint64_t>(&Done::Complete));
}
template<class T> void Put(std::vector<uint8_t>& bytes, size_t offset, T value) {
 Check(Test, offset + sizeof(T) <= bytes.size(), "fixture writer bound");
 std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
std::vector<uint8_t> Config() {
 std::vector<uint8_t> bytes(Mf::EsvmNativeSourceBytes);
 Put(bytes, 0x2948, UINT64_C(0x0000000200000001)); Put(bytes, 0x295c, UINT64_C(0x0000000200000003));
 Put(bytes, 0x2964, uint32_t {3}); Put(bytes, 0x296c, uint32_t {2});
 std::memcpy(bytes.data() + 0x1000, "pass_through=false", 19);
 return bytes;
}
int Context(Api& api) {
 auto config = Config();
 Mf::EsvmSourceCallbacks24 callbacks {SourceEvent, reinterpret_cast<void*>(0x1234), 0xb};
 int context = -1;
 Check(Test, api.add(config.data(), &callbacks, &context) == 0 && context > 0 &&
             Mf::IsEsvmServiceHandle(context), "registered O9 creates owned native context, not filename/canvas");
 std::fill(config.begin(), config.end(), 0xa5); callbacks = {}; // Service must own both copies.
 return context;
}
int Decoder(Api& api, int context, void** mapping, Done& done) {
 Mf::EsvmDecoderCallbacks24 callbacks {DecoderEvent, Done::Complete, &done};
 Check(Test, api.decoder(context, Mf::EsvmDecoderMappingBytes, &callbacks, mapping) == 0 && done.calls == 1 &&
             done.result == 0 && done.handle > 0 && done.handle != context && *mapping,
       "Q6 publishes real 10 MiB mapping and exact three-register completion");
 callbacks = {}; return done.handle;
}
std::vector<uint8_t> Record(uint32_t subtype, const std::vector<uint8_t>& payload) {
 std::vector<uint8_t> bytes(16 + payload.size());
 Put(bytes, 0, uint32_t {100}); Put(bytes, 4, subtype); Put(bytes, 8, uint64_t(payload.size()));
 std::copy(payload.begin(), payload.end(), bytes.begin() + 16); return bytes;
}
std::vector<uint8_t> Vp9Config() {
 std::vector<uint8_t> bytes(24); Put(bytes, 0, uint64_t {24}); bytes[16] = bytes[17] = bytes[18] = 6;
 return Record(3, bytes);
}
std::vector<uint8_t> Packet(std::span<const uint8_t> encoded = EsvmVp9Fixture::Black, uint64_t pts = 1234567) {
 std::vector<uint8_t> bytes(0x38 + encoded.size());
 Put(bytes, 0, uint64_t {0x40}); Put(bytes, 8, pts); Put(bytes, 0x10, uint16_t {7});
 Put(bytes, 0x30, uint64_t(encoded.size()));
 std::copy(encoded.begin(), encoded.end(), bytes.begin() + 0x38);
 return Record(0, bytes);
}
void Submit(Api& api, int decoder, void* mapping, const std::vector<uint8_t>& bytes, int expected = 0, int context = 0, int action = 0) {
 std::memcpy(mapping, bytes.data(), bytes.size());
 Done done; done.api = &api; done.context = context; done.action = context ? 3 : action;
 done.critical = Common::GetHostLockCriticalCounter()->load(); done.poison_mapping = mapping; done.poison_bytes = bytes.size();
 Check(Test, api.submit(decoder, bytes.size(), Done::Complete, &done) == 0 && done.calls == 1 &&
             done.handle == decoder && done.result == expected, "WBBO always completes accepted success/failure with exact ABI");
}
void Empty() {
 const auto r = Mf::EsvmSourceResourcesForTest();
 Check(Test, r.contexts == 0 && r.decoders == 0 && r.mappings == 0 && r.descriptors == 0 && r.pending == 0 && r.decoded_frames == 0 &&
              r.audio_leases == 0 && r.closing_audio == 0 && r.staged_audio_bytes == 0 && r.retired_owners == 0,
        "no retained context/SHM/packet/frame/operation resources");
}
struct StartupCompletion {
 Api* api = nullptr;
 int audio = 0, calls = 0;
 void* mapping = nullptr;
 unsigned events = 0;
 static KYTY_SYSV_ABI void Complete(int, int result, void* user) {
  auto& self = *static_cast<StartupCompletion*>(user);
  ++self.calls;
  Check(Test, result == 0 && decoder_events.load() == self.events,
        "last configuration completes before startup credits dispatch");
  // A nested accepted AU must survive both mapping reuse and outer completion.
  Submit(*self.api, self.audio, self.mapping, Packet(EsvmAacFixture::Packet0));
  Check(Test, Mf::EsvmSourceResourcesForTest().decoded_frames == 1 && decoder_events.load() == self.events,
        "completed internal Flush precedes completion reentry; real AAC is retained without recursive credit dispatch");
 }
};
void InitialConfigurationBarrier(Api& api) {
 std::vector<uint8_t> payload(16); Put(payload, 0, uint64_t {16});
 Put(payload, 8, uint32_t {48000}); Put(payload, 12, uint32_t {1});
 const auto aac = Record(4, payload);
 for (bool audio_first: {true, false}) {
  for (bool reconfigure: {false, true}) {
   const int context = Context(api);
   Check(Test, api.pause(context) == 0, "initial readiness requires explicit Pause");
   Done audio_done, video_done;
   audio_done.critical = video_done.critical = Common::GetHostLockCriticalCounter()->load();
   void* audio_mapping = nullptr; void* video_mapping = nullptr;
   const int audio = Decoder(api, context, &audio_mapping, audio_done);
   const int video = Decoder(api, context, &video_mapping, video_done);
   const auto events = decoder_events.load(); const int ready = source_events;
   const int first = audio_first ? audio : video;
   void* first_mapping = audio_first ? audio_mapping : video_mapping;
   const auto first_config = audio_first ? aac : Vp9Config();
   const auto first_packet = audio_first ? Packet(EsvmAacFixture::Packet0) : Packet();
   auto mixed = first_config; mixed.insert(mixed.end(), first_packet.begin(), first_packet.end());
   Submit(api, first, first_mapping, mixed, InputNotStarted);
   Check(Test, Mf::EsvmSourceAudioForTest(first).packet_state == Mf::EsvmDecoderState::Unconfigured,
         "pre-barrier AU-bearing batch rejects atomically without committing its configuration");
   Submit(api, first, first_mapping, first_config);
   const auto generation = Mf::EsvmSourceAudioForTest(first).generation;
   Submit(api, first, first_mapping, first_packet, InputNotStarted);
   Check(Test, decoder_events.load() == events && source_events == ready &&
               Mf::EsvmSourceResourcesForTest().decoded_frames == 0 && Mf::EsvmSourceAudioForTest(first).generation == generation,
         "audio-before-video and video-before-audio withhold credits and reject input before copying or decoding");
   const auto last_config = audio_first ? Vp9Config() : aac;
   void* last_mapping = audio_first ? video_mapping : audio_mapping;
   const int last = audio_first ? video : audio;
   auto invalid_config = last_config; invalid_config[0] = 0;
   Submit(api, last, last_mapping, invalid_config, Invalid);
   Check(Test, decoder_events.load() == events && Mf::EsvmSourceAudioForTest(first).generation == generation,
         "failed final configuration cannot produce initial Flush or release input credits");
   std::memcpy(last_mapping, last_config.data(), last_config.size());
   StartupCompletion completion {&api, audio, 0, audio_mapping, events};
   Check(Test, api.submit(last, last_config.size(), StartupCompletion::Complete, &completion) == 0 && completion.calls == 1 &&
               decoder_events.load() == events + 2 && source_events == ready,
         "all-stream configuration completes genuine initial Flush then releases one credit per child, without readiness");
   const auto audio_generation = Mf::EsvmSourceAudioForTest(audio).generation;
   if (reconfigure) {
    Submit(api, video, video_mapping, Vp9Config());
    Check(Test, Mf::EsvmSourceAudioForTest(audio).generation == audio_generation &&
                Mf::EsvmSourceResourcesForTest().decoded_frames == 1,
          "later video configuration neither repeats all-stream Flush nor discards accepted AAC PCM");
   }
   std::vector<uint8_t> video_input;
   for (int n = 0; n < 12; ++n) {
    const auto packet = Packet(EsvmVp9Fixture::Black, 1000000 + n * 40000);
    video_input.insert(video_input.end(), packet.begin(), packet.end());
   }
   Submit(api, video, video_mapping, video_input);
    Check(Test, source_events == ready + 1 &&
               Mf::EsvmSourceAudioForTest(audio).generation == audio_generation &&
               Mf::EsvmSourceResourcesForTest().decoded_frames == 5,
          "initial producer awaits actual video across configuration replacement and preserves accepted audio");
   Check(Test, api.remove(context) == 0, "remove two-stream startup fixture"); Empty();
  }
 }
 for (bool delete_in_completion: {true, false}) {
  const int context = Context(api);
  Done first_done, last_done;
  first_done.critical = last_done.critical = Common::GetHostLockCriticalCounter()->load();
  void* first_mapping = nullptr; void* last_mapping = nullptr;
  const int first = Decoder(api, context, &first_mapping, first_done);
  const int last = Decoder(api, context, &last_mapping, last_done);
  Submit(api, first, first_mapping, aac);
  const auto events = decoder_events.load();
  Done completion; completion.api = &api; completion.context = context;
  completion.critical = first_done.critical; completion.action = delete_in_completion ? 1 : 0;
  RefillProbe probe; probe.api = &api; probe.context = context; probe.action = delete_in_completion ? 0 : 2;
  refill_probe = &probe;
  const auto config = Vp9Config(); std::memcpy(last_mapping, config.data(), config.size());
  Check(Test, api.submit(last, config.size(), Done::Complete, &completion) == 0 && completion.calls == 1 && completion.result == 0,
        "startup completion remains exactly once when callback deletes source");
  refill_probe = nullptr;
  Check(Test, decoder_events.load() == events + (delete_in_completion ? 0 : 1) && api.pause(context) == Invalid,
        "completion deletion cancels all startup credits; first refill deletion cancels second child credit");
  Empty();
 }
}
void SourceReadiness(Api& api) {
 ReadinessProbe probe; probe.api = &api; readiness_probe = &probe;
 struct RestoreProbe { ~RestoreProbe() { readiness_probe = nullptr; } } restore;
 auto create = [&] {
  auto config = Config();
  Mf::EsvmSourceCallbacks24 callbacks {SourceEvent, &probe, 0xb};
  Check(Test, api.add(config.data(), &callbacks, &probe.context) == 0, "readiness fixture owns real source callback record");
  return probe.context;
 };
 auto batch = [](size_t count) {
  std::vector<uint8_t> result;
  for (size_t n = 0; n < count; ++n) {
   const auto packet = Packet(EsvmVp9Fixture::Black, 1000000 + n * 40000);
   result.insert(result.end(), packet.begin(), packet.end());
  }
  return result;
 };
 auto flush = [&](int context) {
  Done done; done.critical = probe.critical;
  Check(Test, api.flush(context, Done::Complete, &done) == 0 && done.calls == 1 && done.result == 0,
        "source Flush succeeds without inventing an AU sequence");
 };
 // No codec/output exists: changed controls and repeated polls must never
 // manufacture source readiness, even after the diagnostic budget saturates.
 const int trace_context = create();
 for (int n = 0; n < 40; ++n) {
  Check(Test, api.pause(trace_context) == 0 && api.play(trace_context) == 0,
        "diagnostic saturation preserves actual Pause/Play operations");
 }
 for (int n = 0; n < 128; ++n) Check(Test, api.pause(trace_context) == 0, "unchanged diagnostic polling remains inert");
 Check(Test, probe.calls == 0 && api.remove(trace_context) == 0, "trace cap cannot invent readiness or obstruct retirement");
 std::printf("MediaFramework trace fixture: capped-source=%d\n", trace_context);
 Empty();
 for (bool initial_flush: {false, true}) {
  const int initial = create();
  if (initial_flush) { flush(initial); }
  Done added; added.critical = probe.critical; void* mapping = nullptr;
  const int decoder = Decoder(api, initial, &mapping, added);
  Submit(api, decoder, mapping, Vp9Config());
  Submit(api, decoder, mapping, batch(12));
  Check(Test, probe.calls == 0, "sufficient output after admission supplies no implicit Pause, even with Flush synchronization");
  if (!initial_flush) {
   Check(Test, api.pause(initial) == 0 && probe.calls == 1, "explicit Pause uses genuine all-stream startup Flush and actual video output");
  }
  Check(Test, api.remove(initial) == 0, "remove unarmed initial source");
  Empty();
  probe.calls = 0;
 }
 const int context = create();
 flush(context); // No children yet: first configuration must retain this request.
 Done added; added.critical = probe.critical; void* mapping = nullptr;
 const int decoder = Decoder(api, context, &mapping, added);
 Submit(api, decoder, mapping, Vp9Config());
 Check(Test, api.pause(context) == 0, "explicit guest Pause arms readiness");
 Check(Test, probe.calls == 0, "configuration/refill is not readiness");
 Submit(api, decoder, mapping, batch(11));
 Check(Test, probe.calls == 0, "eleven real buffered/decoded video units remain below native threshold");
 probe.action = 1;
 Submit(api, decoder, mapping, Packet(EsvmVp9Fixture::Black, 1440000));
 Check(Test, probe.calls == 1, "twelfth real unit delivers source-ready on submitting guest thread exactly once");
 for (int n = 0; n < 3; ++n) { Check(Test, api.pause(context) == 0, "poll source without manufacturing new sync arm"); }
 Check(Test, probe.calls == 1, "repeated pause and callback reentry cannot repeat source-ready");
 Done flushed; flushed.critical = probe.critical;
 Check(Test, api.flush(context, Done::Complete, &flushed) == 0 && flushed.result == 0, "flush invalidates ready generation");
 Submit(api, decoder, mapping, batch(12), 0, context); // Completion reenters Pause/Play before notification dispatch.
 Check(Test, probe.calls == 1, "completion Play invalidates readiness before deferred source delivery");
 Check(Test, api.pause(context) == 0, "pause for fresh generation");
 Done flushed_again; flushed_again.critical = probe.critical;
 Check(Test, api.flush(context, Done::Complete, &flushed_again) == 0 && flushed_again.result == 0, "second generation flush");
 probe.action = 2;
 Submit(api, decoder, mapping, batch(12));
 Check(Test, probe.calls == 2 && api.pause(context) == Invalid, "ready callback deletes source without use-after-free or revival");
 Empty();

 // Six frames plus an EOS-only marker cannot reach codec EOF while paused:
 // four outputs and one held frame stop Pump before the trailing marker.
 // Native input-end readiness must break that cycle without enlarging queues.
 auto short_input = batch(6);
 auto end = Packet({}); end[16 + 0x18] = 1;
 short_input.insert(short_input.end(), end.begin(), end.end());
 Mf::EsvmPacketDecoder short_decoder(Mf::EsvmDecoderSchema::Firmware1360);
 Check(Test, short_decoder.Consume(Vp9Config()).result == Mf::EsvmDecoderResult::Ok &&
             short_decoder.Consume(short_input).result == Mf::EsvmDecoderResult::Ok &&
             short_decoder.Pump() == Mf::EsvmDecoderResult::Backpressure,
       "short EOS fixture actually blocks before codec EOF");
 const auto short_status = short_decoder.Status();
 Check(Test, short_status.eos_accepted && short_status.state != Mf::EsvmDecoderState::Ended &&
             short_status.queued_frames == 4 && short_status.held_codec_frame &&
             short_status.queued_packets == 2 && short_status.queued_access_units == 1,
       "EOS marker is excluded from payload accounting while output remains bounded");
 const int short_context = create();
 Done short_added; short_added.critical = probe.critical; mapping = nullptr;
 const int short_handle = Decoder(api, short_context, &mapping, short_added);
 Submit(api, short_handle, mapping, Vp9Config());
 flush(short_context); // Configured but no AU sequence yet.
 Check(Test, api.pause(short_context) == 0, "pause short-input source explicitly");
  Submit(api, short_handle, mapping, batch(6));
  Check(Test, probe.calls == 2, "short backpressured input without EOS remains below native threshold");
  for (int n = 0; n < 16; ++n) Check(Test, api.pause(short_context) == 0, "poll blocked source without altering threshold or Sync");
  Check(Test, probe.calls == 2, "observing blocked output cannot manufacture ready delivery");
  std::printf("MediaFramework trace fixture: blocked-source=%d child=%d\n", short_context, short_handle);
 Submit(api, short_handle, mapping, end);
 Check(Test, probe.calls == 3, "admitted EOS releases short-input readiness before output EOF");
 Check(Test, api.remove(short_context) == 0, "remove short-input readiness fixture");
 Empty();

 // Exercise the shared production retrieval observer through both the owning
 // mailbox publisher and the diagnostic transfer path, without a present ack.
 for (bool mailbox: {false, true}) {
  for (bool advancing: {false, true}) {
   const int timestamp_context = create();
   Done added; added.critical = probe.critical; mapping = nullptr;
   const int handle = Decoder(api, timestamp_context, &mapping, added);
   flush(timestamp_context); // Child exists but has no codec configuration.
   Submit(api, handle, mapping, Vp9Config());
   Check(Test, api.play(timestamp_context) == 0, "retrieve output while playing after Flush");
   if (mailbox) { Check(Test, api.render(timestamp_context, 0, 0, 1, 1) == 0, "select timestamp fixture mailbox"); }
   std::vector<uint8_t> input;
   for (int n = 0; n < 14; ++n) {
    const auto packet = Packet(EsvmVp9Fixture::Black, 1000000 + (advancing ? n * 40000 : 0));
    input.insert(input.end(), packet.begin(), packet.end());
   }
   const int before = probe.calls;
   Submit(api, handle, mapping, input);
   if (mailbox) {
    // Submit retrieves one output; Render or the scheduler retrieves the next
    // into the bounded staged slot even while the first mailbox stays full.
    Check(Test, api.render(timestamp_context, 0, 0, 1, 1) == 0, "retrieve second timestamp before any presentation acknowledgement");
   } else {
    Mf::EsvmDecodedFrame first, second;
    Check(Test, Mf::TakeEsvmDecodedFrame(probe.pid, timestamp_context, handle, &first) &&
                Mf::TakeEsvmDecodedFrame(probe.pid, timestamp_context, handle, &second) &&
                first.has_pts && second.has_pts && (first.pts != second.pts) == advancing,
          "owning retrieval observes two actual decoded microsecond timestamps");
   }
   Check(Test, probe.calls == before && api.pause(timestamp_context) == 0 &&
               probe.calls == before + (advancing ? 0 : 1),
         "changed retrieved PTS clears Sync; identical PTS preserves it until explicit Pause");
   Check(Test, api.remove(timestamp_context) == 0, "remove timestamp fixture");
   Empty();
  }
 }
 {
  const int discontinuity = create();
  Done added; added.critical = probe.critical; mapping = nullptr;
  const int handle = Decoder(api, discontinuity, &mapping, added);
  Submit(api, handle, mapping, Vp9Config());
  Check(Test, api.pause(discontinuity) == 0, "pause discontinuity fixture");
  flush(discontinuity);
  Submit(api, handle, mapping, Vp9Config()); // Replaces a configured generation.
  const int before = probe.calls;
  Submit(api, handle, mapping, batch(12));
   Check(Test, probe.calls == before + 1, "same-child configuration preserves unfinished explicit Flush until actual output");
  Check(Test, api.remove(discontinuity) == 0, "remove reconfiguration fixture");
  Empty();
 }
 {
  const int failed_flush = create();
  Done added; added.critical = probe.critical; mapping = nullptr;
  const int handle = Decoder(api, failed_flush, &mapping, added);
  Submit(api, handle, mapping, Vp9Config());
  Check(Test, api.pause(failed_flush) == 0 && api.render(failed_flush, 0, 0, 1, 1) == 0,
        "paused mailbox fixture has no simulated renderer-clear acknowledgement");
  Done done; done.critical = probe.critical;
  Check(Test, api.flush(failed_flush, Done::Complete, &done) == 0 && done.calls == 1 && done.result == Failed,
        "failed renderer-clear completion cannot arm first-output readiness");
  const int before = probe.calls;
  Submit(api, handle, mapping, batch(12));
  Check(Test, probe.calls == before, "actual output after failed Flush cannot synthesize synchronization");
  Check(Test, api.remove(failed_flush) == 0, "remove failed Flush fixture");
  Empty();
 }
  // mapple3: AC3 config, Pause, HEVC config/startup Flush, then another
  // HEVC config in the first payload batch. The same child changes gen 3->4
  // before its first output, while the source still owns an unfinished Sync.
  for (int variant = 0; variant < 4; ++variant) {
   const int repeated = create();
   Done video_added; video_added.critical = probe.critical; void* video_mapping = nullptr;
   const int video = Decoder(api, repeated, &video_mapping, video_added);
   Done audio_added; audio_added.critical = probe.critical; void* audio_mapping = nullptr;
   const int audio = Decoder(api, repeated, &audio_mapping, audio_added);
   std::vector<uint8_t> audio_config(8); Put(audio_config, 0, uint64_t {16});
   Submit(api, audio, audio_mapping, Record(5, audio_config));
   Check(Test, api.pause(repeated) == 0, "pause before HEVC startup configuration");
   std::vector<uint8_t> config(16); Put(config, 0, uint64_t {0x48}); Put(config, 8, uint64_t {4});
   auto append = [&](std::span<const uint8_t> bytes) {
    const auto at = config.size(); config.resize(at + 8); Put(config, at, uint64_t(bytes.size()));
    config.insert(config.end(), bytes.begin(), bytes.end());
   };
   append(EsvmNalFixture::Hevc10Vps); append(EsvmNalFixture::Hevc10Sps); append(EsvmNalFixture::Hevc10Pps);
   const uint8_t annex = 1; append({&annex, 1});
   const auto wire_config = Record(2, config);
   Submit(api, video, video_mapping, wire_config);
   const int before = probe.calls;
   std::vector<uint8_t> idr {0, 0, 0, 1};
   idr.insert(idr.end(), EsvmNalFixture::Hevc10Idr.begin(), EsvmNalFixture::Hevc10Idr.end());
   auto frames = [&](int count, int start = 0) {
    std::vector<uint8_t> out;
    for (int n = 0; n < count; ++n) {
     const auto au = Packet(idr, 1000000 + (start + n) * 40000);
     out.insert(out.end(), au.begin(), au.end());
    }
    return out;
   };
   if (variant == 1) {
    Submit(api, video, video_mapping, wire_config);
    Check(Test, probe.calls == before, "configuration alone does not finish source Sync");
   }
   if (variant == 3) Check(Test, api.play(repeated) == 0, "Play revokes paused readiness before replacement");
   auto mixed = variant == 1 ? std::vector<uint8_t> {} : wire_config;
   const auto units = frames(variant == 2 ? 6 : 23);
   mixed.insert(mixed.end(), units.begin(), units.end());
   Submit(api, video, video_mapping, mixed);
   if (variant == 2) {
    Check(Test, probe.calls == before, "repeated config with six real frames remains below threshold");
    Submit(api, video, video_mapping, frames(17, 6));
   }
   Check(Test, probe.calls == before + (variant == 3 ? 0 : 1),
         "same-child first output completes pending startup Sync across HEVC configuration replacement");
   for (int n = 0; n < 8; ++n) {
    Check(Test, (variant == 3 ? api.play(repeated) : api.pause(repeated)) == 0, "poll replacement without repeating delivery");
   }
   Check(Test, probe.calls == before + (variant == 3 ? 0 : 1), "replacement readiness is delivered at most once");
   std::printf("MediaFramework trace fixture: repeated-hevc-source=%d child=%d variant=%d\n", repeated, video, variant);
   if (variant == 0) {
    Check(Test, api.play(repeated) == 0, "Play permits real output retrieval after readiness");
    Mf::EsvmDecodedFrame frame;
    int retrieved = 0;
    while (Mf::TakeEsvmDecodedFrame(probe.pid, repeated, video, &frame)) ++retrieved;
    Check(Test, retrieved > 0, "drain actual old-generation output before later replacement");
    Check(Test, api.pause(repeated) == 0, "Pause alone cannot recreate consumed Sync");
    Submit(api, video, video_mapping, mixed);
    Check(Test, probe.calls == before + 1, "replacement after completed first-output Sync cannot manufacture another arm");
   }
   Check(Test, api.remove(repeated) == 0, "retire repeated HEVC fixture");
   Empty();
  }
  const int before_hevc = probe.calls;

  // Apple's demonstrated AC3-before-HEVC path, real HEVC 10-bit SDR and
  // short-input EOS. No synthetic queue counts or injected readiness override.
  const int hevc_context = create();
  Done hevc_added; hevc_added.critical = probe.critical; mapping = nullptr;
  const int hevc_decoder = Decoder(api, hevc_context, &mapping, hevc_added);
  Done ac3_added; ac3_added.critical = probe.critical; void* ac3_mapping = nullptr;
  const int ac3_decoder = Decoder(api, hevc_context, &ac3_mapping, ac3_added);
  std::vector<uint8_t> ac3_config(8); Put(ac3_config, 0, uint64_t {16});
  const auto before_refill = decoder_events.load();
  Submit(api, ac3_decoder, ac3_mapping, Record(5, ac3_config));
  Submit(api, ac3_decoder, ac3_mapping, Packet(EsvmAc3Fixture::ac3Surround[0]), InputNotStarted);
  Check(Test, decoder_events.load() == before_refill && Mf::EsvmSourceResourcesForTest().decoded_frames == 0,
        "actual AC3-before-HEVC ordering cannot grant premature audio credit or consume audio");
  // No public Flush: initial HEVC configuration must produce it internally.
 Check(Test, api.pause(hevc_context) == 0, "pause HEVC source explicitly");
 std::vector<uint8_t> payload(16); Put(payload, 0, uint64_t {0x48}); Put(payload, 8, uint64_t {4});
 auto blob = [&](std::span<const uint8_t> bytes) {
  const auto at = payload.size(); payload.resize(at + 8); Put(payload, at, uint64_t(bytes.size()));
  payload.insert(payload.end(), bytes.begin(), bytes.end());
 };
 blob(EsvmNalFixture::Hevc10Vps); blob(EsvmNalFixture::Hevc10Sps); blob(EsvmNalFixture::Hevc10Pps);
 const uint8_t annex_b = 1; blob({&annex_b, 1});
  Submit(api, hevc_decoder, mapping, Record(2, payload));
  Submit(api, ac3_decoder, ac3_mapping, Packet(EsvmAc3Fixture::ac3Surround[0]));
  Check(Test, Mf::EsvmSourceResourcesForTest().decoded_frames == 1 && probe.calls == before_hevc,
        "same AC3 input is accepted exactly once after barrier without manufacturing video readiness");
 std::vector<uint8_t> idr {0, 0, 0, 1}; idr.insert(idr.end(), EsvmNalFixture::Hevc10Idr.begin(), EsvmNalFixture::Hevc10Idr.end());
 auto protected_au = Packet(idr); Put(protected_au, 16 + 0x14, int32_t {1});
 Submit(api, hevc_decoder, mapping, protected_au, Failed);
 Check(Test, probe.calls == before_hevc, "protected HEVC rejection cannot manufacture readiness");
 auto eos = Packet(idr); eos[16 + 0x18] = 1;
 probe.action = 3;
 Submit(api, hevc_decoder, mapping, eos);
 Check(Test, probe.calls == before_hevc + 1 && api.pause(hevc_context) != 0, "real short HEVC EOS delivers readiness and reentrant unregister retires owner");
 Empty();
 Mf::EsvmInit24 init {24, 0, 0x1fff, 700, 0};
 Check(Test, Mf::EsvmInitialize(&init, false) == 0 && Mf::EsvmRegisterClient("citroncore") == 0 &&
             api.pause(hevc_context) == Invalid, "new registration cannot revive a readiness ticket from retired epoch");
}
void PausedVideoSmallBatches(Api& api) {
 for (bool hevc: {false, true}) {
 const int context = Context(api);
 Done added; added.critical = Common::GetHostLockCriticalCounter()->load();
 void* mapping = nullptr; const int decoder = Decoder(api, context, &mapping, added);
 Check(Test, api.pause(context) == 0, "small-batch source explicitly paused");
 auto config = Vp9Config();
 std::vector<uint8_t> encoded(EsvmVp9Fixture::Black.begin(), EsvmVp9Fixture::Black.end());
 if (hevc) {
  std::vector<uint8_t> payload(16); Put(payload, 0, uint64_t {0x48}); Put(payload, 8, uint64_t {4});
  auto blob = [&](std::span<const uint8_t> bytes) {
   const auto at = payload.size(); payload.resize(at + 8); Put(payload, at, uint64_t(bytes.size()));
   payload.insert(payload.end(), bytes.begin(), bytes.end());
  };
  blob(EsvmNalFixture::Hevc10Vps); blob(EsvmNalFixture::Hevc10Sps); blob(EsvmNalFixture::Hevc10Pps);
  const uint8_t annex_b = 1; blob({&annex_b, 1}); config = Record(2, payload);
  encoded = {0, 0, 0, 1}; encoded.insert(encoded.end(), EsvmNalFixture::Hevc10Idr.begin(), EsvmNalFixture::Hevc10Idr.end());
 }
 Submit(api, decoder, mapping, config);
 const int ready_before = source_events;
 auto credit = decoder_events.load();
 for (unsigned n = 0; n < 16; ++n) {
  Submit(api, decoder, mapping, Packet(encoded, 1000000 + n * 40000));
  Check(Test, decoder_events.load() == credit + 1,
        "bounded encoded capacity grants next small batch while paused output is full");
  credit = decoder_events.load();
  if (n < 11) Check(Test, source_events == ready_before, "four full output slots cannot fabricate threshold readiness");
 }
 Check(Test, source_events == ready_before + 1, "real buffered input reaches unchanged threshold without Play or EOS");
 for (int n = 0; n < 4; ++n) Check(Test, api.pause(context) == 0, "poll paused input credit");
 Check(Test, decoder_events.load() == credit && source_events == ready_before + 1, "polls duplicate neither refill nor readiness");
 auto eos = Packet({}); eos[16 + 0x18] = 1; Submit(api, decoder, mapping, eos);
 Check(Test, decoder_events.load() == credit && source_events == ready_before + 1, "EOS neither rearms refill nor duplicates prior readiness");
 Check(Test, api.play(context) == 0, "play genuinely buffered small batches");
 Mf::EsvmDecodedFrame frame; unsigned count = 0;
 while (Mf::TakeEsvmDecodedFrame(100, context, decoder, &frame)) {
  Check(Test, frame.pts == 1000000 + count * 40000 && frame.width == 64 && frame.height == 48,
        "paused prefetch retains every decoded frame and owned AU");
  ++count;
 }
 Check(Test, count == 16, "all small-batch frames drain without loss");
 Check(Test, api.remove(context) == 0, "retire small-batch fixture"); Empty();
 }
}

void OrderedAvcMailbox(Api& api) {
 using namespace EsvmNalTransition;
 const int context = Context(api);
 progress_api = &api; progress_context = context; progress_pts = 1000000; progress_events = 0;
 progress_thread = std::this_thread::get_id(); progress_critical = Common::GetHostLockCriticalCounter()->load();
 Done added; added.critical = Common::GetHostLockCriticalCounter()->load();
 void* mapping = nullptr; const int decoder = Decoder(api, context, &mapping, added);
 auto config = [&](bool changed) {
  std::vector<uint8_t> payload(16); Put(payload, 0, uint64_t {0x38}); Put(payload, 8, uint64_t {4});
  auto blob = [&](std::span<const uint8_t> bytes) {
   const auto at = payload.size(); payload.resize(at + 8); Put(payload, at, uint64_t(bytes.size()));
   payload.insert(payload.end(), bytes.begin(), bytes.end());
  };
  blob(changed ? std::span<const uint8_t>(NewSps) : std::span<const uint8_t>(OldSps));
  blob(changed ? std::span<const uint8_t>(NewPps) : std::span<const uint8_t>(OldPps));
  const uint8_t annex_b = 1; blob({&annex_b, 1}); return Record(1, payload);
 };
 auto frames = [&](bool changed) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < 6; ++i) {
   auto au = Packet((changed ? NewPackets : OldPackets)[i], 1000000 + (changed ? 240000 : 0) + 40000 * PresentationOrder[i]);
   if (changed && i == 5) au[16 + 0x18] = 1;
   out.insert(out.end(), au.begin(), au.end());
  }
  return out;
 };
 Submit(api, decoder, mapping, config(false));
 Submit(api, decoder, mapping, frames(false));
 Check(Test, api.play(context) == 0 && api.render(context, 0, 0, 1, 1) == 0, "publish old AVC before changed configuration");
 auto first = Libs::Graphics::ReadNativeVideoSnapshot();
 Check(Test, first.stream && first.frame && first.frame->pts == 1000000, "actual old AVC frame awaits receipt");
 auto changed = config(true); const auto next = frames(true); changed.insert(changed.end(), next.begin(), next.end());
 Submit(api, decoder, mapping, changed); // Completion overwrites every submitted guest byte.
 auto pending = Libs::Graphics::ReadNativeVideoSnapshot();
 Check(Test, pending.serial == first.serial && pending.stream == first.stream && first.stream->IsCurrent(first),
       "changed configuration preserves unacknowledged mailbox and presentation lease");
 uint64_t pts = 0; uint16_t sequence = 0;
 auto current = first;
 for (size_t i = 0; i < 12; ++i) {
  progress_pts = 1000000 + i * 40000;
  Check(Test, current.frame && current.frame->pts == 1000000 + int64_t(i) * 40000 &&
              current.frame->width == (i < 6 ? 64 : 96) && current.stream == first.stream &&
              current.stream->Acknowledge(current), "mailbox presents every old/delayed/new AVC frame in PTS order");
  Check(Test, api.pts(context, &pts, &sequence) == 0 && pts == 1000000 + i * 40000 && sequence == 7,
        "acknowledged timeline survives ordered configuration");
  if (i == 11) break;
  const auto serial = current.serial;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  do {
   Check(Test, api.render(context, 0, 0, 1, 1) == 0, "pump ordered AVC source");
   std::this_thread::yield(); current = Libs::Graphics::ReadNativeVideoSnapshot();
  } while (current.serial == serial && std::chrono::steady_clock::now() < deadline);
  Check(Test, current.serial > serial, "clock advances through configuration without another input submission");
 }
 Done flushed; flushed.critical = Common::GetHostLockCriticalCounter()->load();
 Check(Test, api.flush(context, Done::Complete, &flushed) == 0 && flushed.calls == 1 &&
             !first.stream->IsCurrent(first) && !current.stream->Acknowledge(current),
       "explicit Flush invalidates old and new presentation receipts");
 Check(Test, api.remove(context) == 0, "retire changed AVC mailbox fixture"); Empty();
}

void HevcColourMailbox(Api& api) {
 for (bool signalled_sdr: {false, true}) {
  const int context = Context(api);
  Done added; added.critical = Common::GetHostLockCriticalCounter()->load();
  void* mapping = nullptr; const int decoder = Decoder(api, context, &mapping, added);
  std::vector<uint8_t> config(16); Put(config, 0, uint64_t {0x48}); Put(config, 8, uint64_t {4});
  auto blob = [&](std::span<const uint8_t> data) {
   const auto at = config.size(); config.resize(at + 8); Put(config, at, uint64_t(data.size()));
   config.insert(config.end(), data.begin(), data.end());
  };
  if (signalled_sdr) {
   blob(EsvmNalFixture::Hevc10Vps); blob(EsvmNalFixture::Hevc10Sps); blob(EsvmNalFixture::Hevc10Pps);
  } else {
   blob(EsvmNalFixture::HevcVps); blob(EsvmNalFixture::HevcSps); blob(EsvmNalFixture::HevcPps);
  }
  const uint8_t annex_b = 1; blob({&annex_b, 1}); Submit(api, decoder, mapping, Record(2, config));
  std::vector<uint8_t> idr {0, 0, 0, 1};
  if (signalled_sdr) idr.insert(idr.end(), EsvmNalFixture::Hevc10Idr.begin(), EsvmNalFixture::Hevc10Idr.end());
  else idr.insert(idr.end(), EsvmNalFixture::HevcIdr.begin(), EsvmNalFixture::HevcIdr.end());
  auto eos = Packet(idr); eos[16 + 0x18] = 1; Submit(api, decoder, mapping, eos);
  Check(Test, api.play(context) == 0, "play actual decoded HEVC colour fixture");
  const int result = api.render(context, 0, 0, 1, 1);
  auto snapshot = Libs::Graphics::ReadNativeVideoSnapshot();
  if (signalled_sdr) {
   Check(Test, result == 0 && snapshot.stream && snapshot.frame && snapshot.stream->decoder == decoder &&
               snapshot.frame->width == 64 && snapshot.frame->height == 48,
         "BT.709 HEVC frame reaches mailbox instead of losing VUI at source gate");
   const auto& frame = *snapshot.frame;
   for (size_t n = 0; n < frame.bytes; n += 4)
    Check(Test, frame.rgba[n] > 230 && frame.rgba[n + 1] < 20 && frame.rgba[n + 2] < 20 && frame.rgba[n + 3] == 255,
          "mailbox contains actual decoded red pixels");
  } else {
   Check(Test, result == Failed && !snapshot.frame, "unspecified HEVC colour cannot be silently published as SDR");
  }
  Check(Test, api.remove(context) == 0, "retire HEVC colour fixture"); Empty();
 }
}
void RefillCoalescing() {
 // Pure state-machine cases only. Synthetic boolean observations here are NOT
 // a production capacity override, parser bypass, native-ready or GPU evidence.
 Mf::EsvmRefillCredit credit;
 Check(Test, !credit.Pending(), "new refill state does not invent an initial notification");
 credit.Observe(1, false);
 Check(Test, !credit.Pending(), "actual unavailable capacity keeps credit pending empty");
 credit.Observe(1, true); const auto first = credit.Pending();
 credit.Observe(1, true);
 Check(Test, first && credit.Pending().revision == first.revision && credit.Claim(first) && !credit.Claim(first),
       "repeated real capacity observations coalesce and one ticket can only be claimed once");
 credit.Observe(1, false); credit.Observe(1, true);
 Check(Test, !credit.Pending(), "poll/capacity oscillation alone does not rearm an already delivered credit");
 credit.Accepted(1); credit.Observe(1, false);
 Check(Test, !credit.Pending(), "accepted byte ownership is not a claim that input capacity is free");
 credit.Observe(1, true); const auto second = credit.Pending();
 Check(Test, second && second.revision != first.revision && !credit.Claim(first), "accepted input work rearms a distinct credit revision");
 credit.Observe(1, false);
 Check(Test, !credit.Pending() && !credit.Claim(second), "lost capacity cancels a captured but undelivered ticket");
 credit.Observe(1, true); const auto before_flush = credit.Pending();
 Check(Test, !credit.Claim(second), "capacity returning cannot revive the ticket invalidated when capacity was lost");
 credit.Observe(2, true); const auto after_flush = credit.Pending();
 Check(Test, after_flush && after_flush.generation == 2 && !credit.Claim(before_flush), "generation invalidation rejects old pending work");
 credit.Observe(1, true); credit.Accepted(1);
 Check(Test, credit.Pending().generation == 2 && credit.Claim(after_flush), "stale worker/operation observations cannot revive an earlier generation");
 credit.Accepted(2); credit.Observe(2, true); const auto before_close = credit.Pending();
 credit.Retire(); credit.Observe(3, true); credit.Accepted(3);
 Check(Test, !credit.Pending() && !credit.Claim(before_close), "retirement permanently closes pending/admission even for a new generation");
}
void ActualInputCapacity() {
 Mf::EsvmDecoderLimits limits; limits.max_queued_packets = limits.max_queued_frames = 1;
 Mf::EsvmPacketDecoder packet(Mf::EsvmDecoderSchema::Firmware1360, limits);
 Check(Test, !Mf::ReadEsvmRefillCapacity(packet.Status(), limits).can_refill, "unconfigured mapping has no codec input credit");
 Check(Test, packet.Consume(Vp9Config()).result == Mf::EsvmDecoderResult::Ok &&
             Mf::ReadEsvmRefillCapacity(packet.Status(), limits).can_refill, "opened real VP9 codec and actual empty bounded queues allow input");
 Check(Test, packet.Consume(Packet()).result == Mf::EsvmDecoderResult::Ok &&
             !Mf::ReadEsvmRefillCapacity(packet.Status(), limits).can_refill,
       "actual occupied packet slot suppresses credit before pumping");
 Check(Test, packet.Pump() == Mf::EsvmDecoderResult::Backpressure || packet.Status().queued_frames == 1,
       "real decoding reaches its bounded one-frame output queue");
 Check(Test, Mf::ReadEsvmRefillCapacity(packet.Status(), limits).can_refill, "full video output does not consume the empty encoded input slot");
 Mf::EsvmDecodedFrame frame;
 Check(Test, packet.PopFrame(&frame) && frame.rgba && Mf::ReadEsvmRefillCapacity(packet.Status(), limits).can_refill,
        "popping actual output preserves independent encoded capacity");
 auto status = packet.Status(); status.eos_accepted = true;
 Check(Test, !Mf::ReadEsvmRefillCapacity(status, limits).can_refill, "EOS suppression does not emit a fake terminal refill");
 status = packet.Status(); status.codec = Mf::EsvmDecoderCodec::Aac; status.queued_packet_bytes = 1;
 const auto aac = Mf::ReadEsvmRefillCapacity(status, limits);
 Check(Test, aac.available_bytes == 1024 * 1024 - 1 && !aac.can_refill,
       "AAC credit uses its enforced 1 MiB encoded budget, never VP9's 9 MiB allowance");
}
void RefillEntryLifetime(Api& api) {
 const int context = Context(api);
 Done created; created.critical = Common::GetHostLockCriticalCounter()->load();
 void* mapping = nullptr; const int decoder = Decoder(api, context, &mapping, created);
 RefillProbe probe; probe.api = &api; probe.context = context; probe.action = 1; refill_probe = &probe;
 Submit(api, decoder, mapping, Vp9Config());
 Check(Test, probe.calls == 1, "successful config permits exactly one real capacity callback, including control reentry");
 for (int i = 0; i < 8; ++i) { Check(Test, api.pause(context) == 0 && api.play(context) == 0, "eligible entry polling"); }
 Check(Test, probe.calls == 1, "repeated controls do not rearm input credit");
 auto bad = Vp9Config(); bad[0] = 0;
 Submit(api, decoder, mapping, bad, Invalid);
 Check(Test, probe.calls == 1, "rejected bytes cannot create a new refill credit");
 Submit(api, decoder, mapping, Packet());
 Check(Test, probe.calls == 2, "actual accepted AU rearm reevaluates real available capacity");
 probe.action = 2;
 Submit(api, decoder, mapping, Packet(EsvmVp9Fixture::Gray));
 Check(Test, probe.calls == 3 && api.play(context) == Invalid, "claimed refill lifetime tolerates reentrant source deletion without stale redispatch");
 refill_probe = nullptr; Empty();
}
static KYTY_SYSV_ABI void UnexpectedKeysEvent(int, int, const void*, uint64_t) {
 Check(Test, false, "lifecycle/Query regression must not fabricate a keys data event");
}
void PairedReentrantRetirement(Api& api) {
 // Exact native public-message fixture, admitted ONLY by the keys test scope.
 // This creates real copied configurations/keys to clean, never DRM capability.
 std::vector<uint8_t> configuration(Mf::EsvmAccessConfigurationBytes, 0);
 for (const auto& [offset, text]: std::array<std::pair<size_t, const char*>, 7> {{
        {0x20, "cenc"}, {0x30, "webm"}, {0xa4, "audio/mp4"}, {0x200a8, "video/mp4"},
        {0x400b4, "temporary"}, {0x400d4, "persistent-license"}, {0x400f4, "persistent-usage-record"}}}) {
  std::memcpy(configuration.data() + offset, text, std::strlen(text));
 }
 configuration[0xa0] = 2; configuration[0x200a4] = 1; configuration[0x400a8] = 1; configuration[0x401b4] = 3;
 const Mf::EsvmInit24 init {24, 0, 0x1fff, 700, 0};
 for (bool terminate: {false, true}) {
  // Source/source, keys/keys and both cross-service completion/refill variants.
  for (int variant = 0; variant < 4; ++variant) {
   std::array<Loader::Program*, 2> programs {};
   std::array<int, 2> contexts {}, decoders {}, keys {};
   std::array<void*, 2> mappings {};
   std::array<ReentrantRetirement, 2> probes;
   for (size_t i = 0; i < programs.size(); ++i) {
    // Runtime identity retains its owner; match the existing runtime fixtures.
    programs[i] = new Loader::Program;
    programs[i]->unique_id = 795000 + int(terminate) * 100 + variant * 2 + int(i);
    programs[i]->application_owner = programs[i];
    probes[i].pid = Loader::RegisterRuntimeProcessIdentity(programs[i], programs[i]->unique_id);
    probes[i].terminate = terminate;
    Loader::RuntimeLinker::SetCurrentProgram(programs[i]);
    Check(Test, probes[i].pid > 100 && Mf::EsvmInitialize(&init, false) == 0 && Mf::EsvmRegisterClient("citroncore") == 0,
          "paired owner admission uses actual runtime identities and SHM/semaphores");
    contexts[i] = Context(api);
    Done created; created.critical = Common::GetHostLockCriticalCounter()->load();
    decoders[i] = Decoder(api, contexts[i], &mappings[i], created);
    Done access; access.critical = created.critical;
    {
     Mf::EsvmKeysPublicMessageFixtureScope fixture;
     Check(Test, api.access("com.widevine.alpha.citroncore", configuration.data(), 1, Done::Complete, &access) == 0 &&
                 access.calls == 1 && access.result == 0 && access.handle > 0, "test-only owned public-message access");
    }
    Done key; key.critical = created.critical;
    Check(Test, api.keys(access.handle, true, UnexpectedKeysEvent, Done::Complete, &key) == 0 && key.calls == 1 &&
                key.result == 0 && key.handle > 0, "genuine keys objects accompany both source owners");
    keys[i] = key.handle;
    const auto codec = Vp9Config(); std::memcpy(mappings[i], codec.data(), codec.size());
   }
   Loader::RuntimeLinker::SetCurrentProgram(nullptr);
   const auto before = Mf::EsvmKeysResourcesForTest();
   Check(Test, before.keys == 2 && before.accesses == 2 && before.configuration_bytes == 2 * configuration.size(),
         "paired regression really owns keys/configuration allocations, not empty registry placeholders");
   std::array<std::thread, 2> callers;
   for (size_t i = 0; i < callers.size(); ++i) {
    callers[i] = std::thread([&, i] {
     Loader::RuntimeLinker::SetCurrentProgram(programs[i]); reentrant_retirement = &probes[i];
     const bool use_keys = variant == 1 || (variant >= 2 && i == 1);
     if (use_keys) {
      Check(Test, api.query(keys[i], ReentrantRetirement::Query, &probes[i]) == 0, "held real keys Query entry");
     } else {
      Done held; held.critical = Common::GetHostLockCriticalCounter()->load(); held.action = variant == 3 ? 0 : 5;
      RefillProbe refill; refill.thread = std::this_thread::get_id(); refill.critical = held.critical;
      refill.action = variant == 3 ? 4 : 0; refill_probe = &refill;
      Check(Test, api.submit(decoders[i], Vp9Config().size(), Done::Complete, &held) == 0 && held.calls == 1,
            "held genuine source completion/refill entry");
      refill_probe = nullptr;
     }
     Check(Test, !Mf::EsvmDispatchScopeActive(), "all nested dispatch levels unwind before outer exit");
     reentrant_retirement = nullptr; Loader::RuntimeLinker::SetCurrentProgram(nullptr); probes[i].done.release();
    });
   }
   for (auto& probe: probes) { Check(Test, probe.entered.try_acquire_for(std::chrono::seconds(5)), "both callbacks hold their own gates with both owners retired"); }
   for (auto& probe: probes) { probe.proceed.release(); }
   for (auto& probe: probes) { Check(Test, probe.returned.try_acquire_for(std::chrono::seconds(5)), "bounded reentrant calls return while BOTH foreign callback gates remain held"); }
   for (auto& probe: probes) {
    uint64_t epoch = 0;
    Check(Test, Mf::GetEsvmClientEpoch(probe.pid, &epoch) != 0, "reentrant nonblocking cleanup still closes admission");
    probe.finish.release();
   }
   for (auto& probe: probes) { Check(Test, probe.done.try_acquire_for(std::chrono::seconds(5)), "bounded outer exit/drainer completion before join"); }
   for (auto& caller: callers) { caller.join(); }
   Empty();
   const auto after = Mf::EsvmKeysResourcesForTest();
   Check(Test, after.owners == 0 && after.retired_owners == 0 && after.keys == 0 && after.accesses == 0 &&
               after.configuration_bytes == 0 && after.pending == 0, "outer exits genuinely free BOTH services, including retired-list allocations");
   for (size_t i = 0; i < programs.size(); ++i) {
    Check(Test, Mf::ReleaseEsvmOwner(probes[i].pid) && Loader::DeactivateRuntimeProcessIdentity(programs[i]), "paired runtime teardown");
   }
  }
 }
}
void FatalAudioStaging(Api& api) {
 namespace Ai = Libs::Audio::AudioInternal;
 const int context = Context(api);
 Done created; created.critical = Common::GetHostLockCriticalCounter()->load();
 void* mapping = nullptr; const int decoder = Decoder(api, context, &mapping, created);
 std::vector<uint8_t> aac(16); Put(aac, 0, uint64_t {16}); Put(aac, 8, uint32_t {48000}); Put(aac, 12, uint32_t {1});
 const auto configuration = Record(4, aac);
 Submit(api, decoder, mapping, configuration);
 Submit(api, decoder, mapping, Packet(EsvmAacFixture::Packet0));
 // Audio has not been initialized in this process. The real production opener
 // returns unavailable; no mock PCM, queue-success or decoder-failure seam.
 Check(Test, api.play(context) == 0, "start actual AAC without an output backend");
 auto staged = Mf::EsvmSourceAudioForTest(decoder);
 const auto stage_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
 while (!staged.staged_bytes && std::chrono::steady_clock::now() < stage_deadline) {
  std::this_thread::yield(); staged = Mf::EsvmSourceAudioForTest(decoder);
 }
 Check(Test, staged.staged_bytes != 0 && staged.accepted_frames == 0 && !staged.lease &&
             staged.result == Ai::NativeQueueResult::Unavailable && staged.packet_state != Mf::EsvmDecoderState::Failed,
       "real decoded owned PCM is staged with no lease before fatal input");
 // Valid AAC-LC ADTS framing, deliberately invalid compressed syntax. This is
 // the same real-codec failure vector as the independent packet regression.
 std::vector<uint8_t> corrupt(20, 0);
 std::copy_n(EsvmAacFixture::Packet0.begin(), 7, corrupt.begin());
 corrupt[3] &= 0xfc; corrupt[4] = 20 >> 3; corrupt[5] = ((20 & 7) << 5) | 31;
 Submit(api, decoder, mapping, Packet(corrupt), Failed, 0, 6);
 const auto failed = Mf::EsvmSourceAudioForTest(decoder);
 Check(Test, failed.packet_state == Mf::EsvmDecoderState::Failed && failed.generation == staged.generation &&
             !failed.staged_bytes && !failed.lease && failed.accepted_frames == 0 && failed.result == Ai::NativeQueueResult::Failed,
       "actual same-generation fatal Pump discards preexisting PCM BEFORE completion, even with a null lease");

 Libs::Audio::Initialize();
 struct AudioShutdown { ~AudioShutdown() { Libs::Audio::Shutdown(); } } audio_shutdown;
 // Prove later availability with an independent healthy source using the SAME
 // actual native backend, rather than asserting that Initialize implies output.
 const int healthy_context = Context(api);
 created = {}; created.critical = Common::GetHostLockCriticalCounter()->load();
 void* healthy_mapping = nullptr; const int healthy = Decoder(api, healthy_context, &healthy_mapping, created);
 Submit(api, healthy, healthy_mapping, configuration);
 Submit(api, healthy, healthy_mapping, Packet(EsvmAacFixture::Packet0));
 Check(Test, api.play(healthy_context) == 0, "healthy control may open newly initialized real output");
 auto healthy_audio = Mf::EsvmSourceAudioForTest(healthy);
 const auto retry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
 do {
  const auto retry = Mf::EsvmSourceAudioForTest(decoder);
  Check(Test, retry.packet_state == Mf::EsvmDecoderState::Failed && retry.generation == staged.generation &&
              retry.accepted_frames == 0 && retry.staged_bytes == 0 && !retry.lease && retry.result == Ai::NativeQueueResult::Failed,
        "worker/output retries after the old 250ms backoff can NEVER open/queue failed decoder PCM");
  healthy_audio = Mf::EsvmSourceAudioForTest(healthy); std::this_thread::yield();
 } while (std::chrono::steady_clock::now() < retry_deadline);
 auto healthy_lease = healthy_audio.lease;
  if (healthy_lease) {
  std::puts("MediaFrameworkEsvmSource: fatal AAC regression verified with later real SDL acceptance");
  Check(Test, healthy_audio.accepted_frames > 0 && healthy_audio.result == Ai::NativeQueueResult::Accepted,
        "real later output is demonstrated by healthy PCM acceptance while failed source remains empty");
  Submit(api, healthy, healthy_mapping, Packet(corrupt), Failed, 0, 6);
  const auto live_failed = Mf::EsvmSourceAudioForTest(healthy);
   Check(Test, live_failed.packet_state == Mf::EsvmDecoderState::Failed && healthy_lease->Status().result == Ai::NativeQueueResult::Retired &&
              !live_failed.lease && !live_failed.staged_bytes && live_failed.accepted_frames == healthy_audio.accepted_frames,
        "fatal Pump also retires an existing native lease without reporting new accepted frames");
 } else {
  std::puts("MediaFrameworkEsvmSource: fatal AAC staging cleared; later output unavailable (real-sink branch skipped)");
  Check(Test, healthy_audio.staged_bytes != 0 && healthy_audio.accepted_frames == 0 && healthy_audio.result == Ai::NativeQueueResult::Unavailable,
        "headless branch explicitly lacks later real output, never claims a silent successful queue");
 }
 Submit(api, decoder, mapping, Packet(EsvmAacFixture::Packet0), Failed);
 Check(Test, api.remove(context) == 0 && api.remove(healthy_context) == 0, "remove both fatal-audio regression contexts");
 const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
 while (Mf::EsvmSourceResourcesForTest().closing_audio && std::chrono::steady_clock::now() < close_deadline) { std::this_thread::yield(); }
 Check(Test, !healthy_lease || healthy_lease->IsClosed(), "fatal active lease is genuinely closed by host drainer");
 Empty();
}
void AudioLeaseIntegration(Api& api, uint32_t subtype) {
  namespace Ai = Libs::Audio::AudioInternal;
 Libs::Audio::Initialize();
 struct AudioShutdown { ~AudioShutdown() { Libs::Audio::Shutdown(); } } audio_shutdown;
 const int context = Context(api);
 Check(Test, api.render(context, 0, 0, 1, 1) == 0, "audio/video context owns geometry before either config");
 Done audio_done; audio_done.critical = Common::GetHostLockCriticalCounter()->load();
 Done video_done; video_done.critical = audio_done.critical;
 void* audio_mapping = nullptr; void* video_mapping = nullptr;
 const int audio = Decoder(api, context, &audio_mapping, audio_done);
 const int video = Decoder(api, context, &video_mapping, video_done);
  std::vector<uint8_t> config(subtype == 4 ? 16 : 8); Put(config, 0, uint64_t {16});
  if (subtype == 4) { Put(config, 8, uint32_t {48000}); Put(config, 12, uint32_t {1}); }
  std::span<const std::span<const uint8_t>> packets = EsvmAacFixture::Packets;
  if (subtype == 5) { packets = EsvmAc3Fixture::ac3Surround; }
  if (subtype == 6) { packets = EsvmAc3Fixture::eac3Surround; }
  std::printf("MediaFrameworkEsvmSource: audio wire subtype %u\n", subtype);
  Submit(api, audio, audio_mapping, Record(subtype, config));
  Check(Test, !Mf::EsvmSourceAudioForTest(audio).lease, "audio config does not invent decoded channel count or open a fake audio device");
 Submit(api, video, video_mapping, Vp9Config());
 auto snapshot = Libs::Graphics::ReadNativeVideoSnapshot();
 Check(Test, snapshot.stream && snapshot.stream->decoder == video && !snapshot.frame,
        "unique video selection ignores a genuinely configured audio child");
  Submit(api, audio, audio_mapping, Packet(packets[0]));
 Check(Test, !Mf::EsvmSourceAudioForTest(audio).lease && api.play(context) == 0,
       "paused source retains real decoded PCM until Play, without fictitious output acceptance");
 auto audio_state = Mf::EsvmSourceAudioForTest(audio);
 const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
 while (!audio_state.accepted_frames && !audio_state.staged_bytes && std::chrono::steady_clock::now() < ready_deadline) {
  std::this_thread::yield(); audio_state = Mf::EsvmSourceAudioForTest(audio);
 }
 Check(Test, audio_state.accepted_frames || audio_state.staged_bytes, "host progression owns actual PCM or reports genuine native queue acceptance");
 auto lease = audio_state.lease;
 if (audio_state.accepted_frames) {
  std::puts("MediaFrameworkEsvmSource: real SDL PCM sink acquired; checking backpressure/EOS/retirement");
  Check(Test, lease && audio_state.result == Ai::NativeQueueResult::Accepted && api.pause(context) == 0 && lease->Status().paused &&
              api.volume(context, 0.5) == 0 && api.mute(context, true) == 0 && api.mute(context, false) == 0 && api.play(context) == 0,
        "real native lease receives exact PCM and source Pause/Volume/Mute/Play control its logical stream");
  // Deliberately freeze only the host sink while the source keeps playing to
  // exercise actual queue-full retention, not a fake packet/device capacity.
  Check(Test, lease->SetPaused(true), "freeze real sink for deterministic source backpressure");
  for (int i = 0; i < 24 && Mf::EsvmSourceAudioForTest(audio).result != Ai::NativeQueueResult::Backpressure; ++i) {
   Submit(api, audio, audio_mapping, Packet(packets[size_t(i) % packets.size()]));
  }
  const auto full = Mf::EsvmSourceAudioForTest(audio);
  Check(Test, full.result == Ai::NativeQueueResult::Backpressure && full.staged_bytes != 0,
        "actual full native queue preserves one exact owned staged PCM block");
  const auto before_worker_events = decoder_events.load();
  Check(Test, lease->SetPaused(false), "resume real sink without another source Submit");
  const auto progress_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (Mf::EsvmSourceAudioForTest(audio).accepted_frames <= full.accepted_frames && std::chrono::steady_clock::now() < progress_deadline) { std::this_thread::yield(); }
  Check(Test, Mf::EsvmSourceAudioForTest(audio).accepted_frames > full.accepted_frames && decoder_events.load() == before_worker_events,
        "host worker retries retained PCM after sink capacity returns but NEVER calls a guest refill callback");
  auto eos = Packet(std::span<const uint8_t> {}); eos[16 + 0x18] = 1;
  Submit(api, audio, audio_mapping, eos);
  const auto end_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!lease->Status().input_ended && std::chrono::steady_clock::now() < end_deadline) { std::this_thread::yield(); }
  Check(Test, lease->Status().input_ended, "actual decoder EOS plus accepted PCM ends input once to release the real resampler tail");
 } else {
  std::puts("MediaFrameworkEsvmSource: SDL output unavailable; checking retained PCM (sink checks skipped)");
  Check(Test, !lease && audio_state.staged_bytes != 0 &&
              (audio_state.result == Ai::NativeQueueResult::Unavailable || audio_state.result == Ai::NativeQueueResult::Failed),
        "headless/failed output retains owned PCM and never counts a silent fallback as successful submission");
 }
 uint64_t pts = UINT64_C(0xa5a5a5a5a5a5a5a5); uint16_t sequence = 0x5a5a;
 Check(Test, api.pts(context, &pts, &sequence) == Failed && pts == UINT64_C(0xa5a5a5a5a5a5a5a5) && sequence == 0x5a5a,
       "actual PCM queue acceptance, EOS and SDL queue length never fabricate presented PTS");
 Done removed; removed.critical = Common::GetHostLockCriticalCounter()->load();
 Check(Test, api.remove_decoder(video, Done::Complete, &removed) == 0 && removed.result == 0, "remove unpresented video child before audio-only flush");
 Done flushed; flushed.critical = removed.critical;
 Check(Test, api.pause(context) == 0 && api.flush(context, Done::Complete, &flushed) == 0 && flushed.result == 0,
       "audio-only flush uses actual established sequence and clears staged/codec PCM");
 if (lease) {
  const auto cleared = lease->Status();
  Check(Test, cleared.generation > audio_state.generation && cleared.queued_frames == 0 && !cleared.input_ended && cleared.paused,
        "strict generation Flush really clears SDL future PCM and resets input-ended without claiming speaker clear");
 }
 Check(Test, Mf::EsvmSourceAudioForTest(audio).staged_bytes == 0 && api.remove(context) == 0, "removal releases staged PCM and atomically retires its audio lease");
 const auto close_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
 while (((lease && !lease->IsClosed()) || Mf::EsvmSourceResourcesForTest().closing_audio) && std::chrono::steady_clock::now() < close_deadline) { std::this_thread::yield(); }
 Check(Test, !lease || lease->IsClosed(), "host close drainer destroys the retired stream outside source/lifecycle locks");
 Empty();
}
} // namespace MediaFrameworkEsvmSourceTests

void TestMediaFrameworkEsvmSource() {
 using namespace MediaFrameworkEsvmSourceTests;
 namespace Fs = Libs::LibKernel::FileSystem;
 using Loader::RuntimeLinker;
 Fs::Initialize();
 auto* previous = RuntimeLinker::GetCurrentProgram(); RuntimeLinker::SetCurrentProgram(nullptr);
 struct Restore {
  Loader::Program* previous;
  ~Restore() { Mf::SetEsvmSourceProvenanceForTest(nullptr); RuntimeLinker::SetCurrentProgram(previous); Fs::Shutdown(); }
 } restore {previous};
 Check(Test, Mf::ReleaseEsvmOwner(100), "fresh direct owner");
 RefillCoalescing();
 ReadinessPolicyModel();
 ActualInputCapacity();
 Mf::SetEsvmSourceProvenanceForTest(Provenance);
 Api api; auto config = Config(); Mf::EsvmSourceCallbacks24 callbacks {SourceEvent, nullptr, 0xb};
 int context = 99;
 Check(Test, api.add(config.data(), &callbacks, &context) != 0 && context == 0, "native structured config requires initialized registered owner");
 Mf::EsvmInit24 init {24, 0, 0x1fff, 700, 0};
 Check(Test, Mf::EsvmInitialize(&init, false) == 0 && Mf::EsvmRegisterClient("citroncore") == 0, "real framework SHM/semaphore registration");
 Check(Test, api.add(reinterpret_cast<void*>(1), &callbacks, &context) == Invalid &&
             api.add(config.data(), reinterpret_cast<Mf::EsvmSourceCallbacks24*>(1), &context) == Invalid,
       "bad config/callback pointers do not trigger blind string/full-record reads");
 const char empty[] = "";
 Check(Test, api.add(empty, &callbacks, &context) == Invalid, "empty filename is not alone a native format discriminator");
 callbacks.mask = 1;
 Check(Test, api.add(config.data(), &callbacks, &context) == Invalid, "native copied mask must be 0xb"); callbacks.mask = 0xb;
 config[0x2964] = 4;
 Check(Test, api.add(config.data(), &callbacks, &context) == Invalid, "verified structural fields precede full record admission"); config = Config();
 schema_allowed = false;
 Check(Test, api.add(config.data(), &callbacks, &context) == Failed, "unknown actual module provenance cannot select AU schema"); schema_allowed = true;
 config[0x2976] = 1;
 Check(Test, api.add(config.data(), &callbacks, &context) == Failed, "unimplemented decoder-mode context is not normal Render"); config = Config();
 RefillEntryLifetime(api);
 PairedReentrantRetirement(api);
 FatalAudioStaging(api);
  for (uint32_t subtype: {4u, 5u, 6u}) { AudioLeaseIntegration(api, subtype); }
 // Render is context geometry, and native calls it before codec config. No
 // stream/frame/readiness is invented while retaining that genuine rectangle.
 const int early = Context(api);
 Check(Test, api.render(early, 0.25f, 0.125f, 0.5f, 0.75f) == 0 && !Libs::Graphics::ReadNativeVideoSnapshot().stream,
       "valid pre-decoder Render stores context geometry without a fake active video child");
 Done early_done; early_done.critical = Common::GetHostLockCriticalCounter()->load();
 void* early_mapping = nullptr; const int early_decoder = Decoder(api, early, &early_mapping, early_done);
 Check(Test, api.render(early, 0, 0, 1, 1) == 0 && !Libs::Graphics::ReadNativeVideoSnapshot().stream,
       "unconfigured decoder also leaves geometry pending without failure or selection");
 Check(Test, api.render(early, -0.25f, 0, 1, 1) == Failed && api.render(early, 0.25f, 0.125f, 0.5f, 0.75f) == 0,
       "unsupported geometry is rejected, while valid pre-config geometry remains context-owned");
 Submit(api, early_decoder, early_mapping, Vp9Config());
 auto early_snapshot = Libs::Graphics::ReadNativeVideoSnapshot();
 Check(Test, early_snapshot.stream && early_snapshot.stream->context == early && early_snapshot.stream->decoder == early_decoder &&
             !early_snapshot.frame && early_snapshot.rect.x == 0.25f && early_snapshot.rect.y == 0.125f &&
             early_snapshot.rect.width == 0.5f && early_snapshot.rect.height == 0.75f,
       "accepted real VP9 config activates the saved geometry without another Render or manufactured pixels");
 uint64_t early_pts = UINT64_C(0xa5a5a5a5a5a5a5a5); uint16_t early_sequence = 0x5a5a;
 Check(Test, api.pts(early, &early_pts, &early_sequence) == Failed && early_pts == UINT64_C(0xa5a5a5a5a5a5a5a5) &&
             early_sequence == 0x5a5a, "config plus accepted geometry establishes no presented PTS");
 Done ambiguous_done; ambiguous_done.critical = Common::GetHostLockCriticalCounter()->load();
 void* ambiguous_mapping = nullptr; const int ambiguous = Decoder(api, early, &ambiguous_mapping, ambiguous_done);
 Submit(api, ambiguous, ambiguous_mapping, Vp9Config());
 Check(Test, early_snapshot.stream->IsRetired() && api.render(early, 0, 0, 1, 1) == Failed,
       "arrival of a second configured video child retires the formerly unique selection instead of guessing ordering");
 Check(Test, api.remove(early) == 0 && early_snapshot.stream->IsRetired(),
       "deletion retires deferred/activated geometry lifetime");
 Empty();
 const int c = Context(api);
 Check(Test, api.play(c) == 0 && api.pause(c) == 0 && api.rate(c, 1) == 0 && api.rate(c, 2) == Failed &&
             api.volume(c, 0.5) == 0 && api.mute(c, true) == 0, "verified shared controls route to owned context; unsupported time stretch fails");
 uint64_t pts = UINT64_C(0xa5a5a5a5a5a5a5a5); uint16_t sequence = 0x5a5a;
 Check(Test, api.pts(c, &pts, &sequence) == Failed && pts == UINT64_C(0xa5a5a5a5a5a5a5a5) && sequence == 0x5a5a,
       "no fake presentation binding, video handle or displayed PTS");
 Done done; done.api = &api; done.context = c; done.critical = Common::GetHostLockCriticalCounter()->load();
 void* mapping = nullptr;
 Mf::EsvmDecoderCallbacks24 decoder_callbacks {DecoderEvent, Done::Complete, &done};
 Check(Test, api.decoder(c, Mf::EsvmDecoderMappingBytes - 1, &decoder_callbacks, &mapping) == Invalid && !mapping && !done.calls,
       "exact admitted mapping capacity, no zero-return/unbacked pointer success");
 for (auto point: {Mf::EsvmSourceFailure::Allocate, Mf::EsvmSourceFailure::ShmOpen, Mf::EsvmSourceFailure::ShmResize,
                   Mf::EsvmSourceFailure::ShmMap, Mf::EsvmSourceFailure::Publish}) {
  Mf::EsvmSourceFailNextForTest(point);
  Check(Test, api.decoder(c, Mf::EsvmDecoderMappingBytes, &decoder_callbacks, &mapping) == Failed && !mapping && !done.calls,
        "each allocation/SHM/publication failure rejects before callback/pointer publication");
  const auto r = Mf::EsvmSourceResourcesForTest();
  Check(Test, r.contexts == 1 && r.decoders == 0 && r.mappings == 0 && r.descriptors == 0, "real partial-resource rollback");
 }
 const int d = Decoder(api, c, &mapping, done);
 auto r = Mf::EsvmSourceResourcesForTest();
 Check(Test, r.contexts == 1 && r.decoders == 1 && r.mappings == 1 && r.descriptors == 0, "descriptor closes while actual mapping remains retained");
 schema_allowed = false;
 void* unknown_mapping = nullptr;
 Check(Test, api.decoder(c, Mf::EsvmDecoderMappingBytes, &decoder_callbacks, &unknown_mapping) == Failed && !unknown_mapping,
       "Q6 cannot infer unknown firmware schema from a valid context or requested buffer size"); schema_allowed = true;
 Done second; second.critical = Common::GetHostLockCriticalCounter()->load();
 void* extra_mapping = nullptr; const int extra = Decoder(api, c, &extra_mapping, second);
 Check(Test, api.decoder(c, Mf::EsvmDecoderMappingBytes, &decoder_callbacks, &unknown_mapping) == Full && !unknown_mapping,
       "two distinct child decoders admitted, third bounded before mapping allocation");
 Done removed; removed.critical = Common::GetHostLockCriticalCounter()->load();
 Check(Test, api.remove_decoder(extra, Done::Complete, &removed) == 0 && removed.calls == 1 && removed.handle == extra,
       "independent second decoder removal");
 Check(Test, api.submit(c, 40, Done::Complete, &done) == Invalid && api.play(d) == Invalid &&
             api.decoder(d, Mf::EsvmDecoderMappingBytes, &decoder_callbacks, &mapping) == Invalid,
       "typed context/decoder objects cannot alias despite shared nonreusing handle namespace");
 Submit(api, d, mapping, Vp9Config(), 0, c);
 auto hdr = Vp9Config(); hdr[16 + 17] = 16; hdr[16 + 18] = 9;
 Submit(api, d, mapping, hdr, Failed);
 hdr[16 + 17] = 18; Submit(api, d, mapping, hdr, Failed);
 hdr[16 + 17] = 2; hdr[16 + 18] = 2; Submit(api, d, mapping, hdr, Failed);
 auto protected_packet = Packet(); Put(protected_packet, 16 + 0x14, int32_t {1});
 Submit(api, d, mapping, protected_packet, Failed);
  auto unsupported_codec = Vp9Config(); Put(unsupported_codec, 4, uint32_t {7}); // Opus remains unsupported.
 Submit(api, d, mapping, unsupported_codec, Failed);
 Submit(api, d, mapping, Packet(), 0, c);
 Mf::EsvmDecodedFrame frame;
 Check(Test, api.pause(c) == 0 && !Mf::TakeEsvmDecodedFrame(100, c, d, &frame), "pause gates real frame transfer");
 Submit(api, d, mapping, Vp9Config(), Full); // Diagnostic must retain queued output and the exact completion result.
 Check(Test, api.play(c) == 0 && Mf::TakeEsvmDecodedFrame(100, c, d, &frame) && frame.rgba && frame.width == 64 &&
             frame.height == 48 && frame.has_pts && frame.pts == 1234567 && frame.pts_sequence == 7,
       "real FFmpeg VP9 frame survives immediate mapping poison in completion; PTS stays microseconds");
 for (size_t i = 0; i < frame.bytes; i += 4) {
  Check(Test, frame.rgba[i] <= 2 && frame.rgba[i + 1] <= 2 && frame.rgba[i + 2] <= 2 && frame.rgba[i + 3] == 255,
        "decoded purpose-made fixture pixels, no substituted media or blank synthesis");
 }
 Check(Test, api.pts(c, &pts, &sequence) == Failed && pts == UINT64_C(0xa5a5a5a5a5a5a5a5), "decoded output never masquerades as displayed PTS");
 done = {}; done.critical = Common::GetHostLockCriticalCounter()->load();
 Check(Test, api.flush(c, Done::Complete, &done) == 0 && done.calls == 1 && done.handle == c && done.result == 0,
       "flush uses real established sequence, clears real queues and completes exact ABI");
 // Host mailbox contract only: Acknowledge below simulates a present receipt,
 // not a project/GPU test or proof of a real scanout. No guessed active handles.
 Submit(api, d, mapping, Packet());
 progress_api = &api; progress_context = c; progress_pts = 1234567; progress_events = 0;
 progress_thread = std::this_thread::get_id(); progress_critical = Common::GetHostLockCriticalCounter()->load();
 Check(Test, api.render(c, 0, 0, 1, 1) == 0, "Render activates explicit configured video mailbox lease");
 Check(Test, progress_events == 0, "decoded or published output alone cannot emit progress");
 auto presented = Libs::Graphics::ReadNativeVideoSnapshot();
 Check(Test, presented.stream && presented.frame && presented.stream->owner == 100 &&
             presented.stream->context == c && presented.stream->decoder == d &&
             presented.frame->has_pts && presented.frame->pts == 1234567,
       "coherent owner/context/decoder/generation/PTS/rect snapshot carries actual decoded pixels");
 Check(Test, api.pts(c, &pts, &sequence) == Failed && pts == UINT64_C(0xa5a5a5a5a5a5a5a5),
       "publication/upload alone is not presented PTS");
 Submit(api, d, mapping, Packet(EsvmVp9Fixture::Gray, 1235567));
 Check(Test, Libs::Graphics::ReadNativeVideoSnapshot().serial == presented.serial,
       "full pending mailbox keeps staged owned frame instead of replacing unpresented pixels");
 Check(Test, presented.stream->Acknowledge(presented) && api.pts(c, &pts, &sequence) == 0 && pts == 1234567 && sequence == 7,
       "only matching host present acknowledgement supplies public microsecond PTS and exact u16 sequence");
 Check(Test, progress_events == 0 && api.render(c, 0, 0, 1, 1) == 0 && progress_events == 1 &&
              api.render(c, 0, 0, 1, 1) == 0 && progress_events == 1,
        "guest pump delivers exactly one progress event per acknowledged receipt, never the host worker");
 auto progressed = Libs::Graphics::ReadNativeVideoSnapshot();
 const auto frame_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
 while (progressed.serial == presented.serial && std::chrono::steady_clock::now() < frame_deadline) {
  std::this_thread::yield(); progressed = Libs::Graphics::ReadNativeVideoSnapshot();
 }
 Check(Test, progressed.frame && progressed.serial > presented.serial && progressed.frame->pts == 1235567 &&
             progressed.stream->Acknowledge(progressed) && api.pts(c, &pts, &sequence) == 0 && pts == 1235567,
       "clock worker retries after host ack without another Submit or graphics-to-guest callback");
 progress_pts = 1235567;
 Check(Test, api.render(c, 0, 0, 1, 1) == 0 && progress_events == 2,
        "subsequent acknowledged frame advances native public progress");
 const int conflict = Context(api); Done conflict_done; conflict_done.critical = Common::GetHostLockCriticalCounter()->load();
 void* conflict_mapping = nullptr; const int conflict_decoder = Decoder(api, conflict, &conflict_mapping, conflict_done);
 Submit(api, conflict_decoder, conflict_mapping, Vp9Config());
 Check(Test, api.render(conflict, 0, 0, 1, 1) == Failed &&
             Libs::Graphics::ReadNativeVideoSnapshot().stream == presented.stream,
       "second live video context cannot silently overwrite selected stream ordering");
 Check(Test, api.remove(conflict) == 0, "conflicting context cleanup");
 done = {}; done.critical = Common::GetHostLockCriticalCounter()->load();
 Check(Test, api.flush(c, Done::Complete, &done) == 0 && done.result == Failed &&
             !presented.stream->IsCurrent(presented) && !presented.stream->Acknowledge(presented) &&
             !progressed.stream->Acknowledge(progressed),
       "strict new generation invalidates CPU frames but cannot report renderer clear without a successful host commit");
 Check(Test, api.render(c, 0, 0, 1, 1) == 0 && progress_events == 2,
        "flushed presentation receipt cannot generate stale progress");
 // Explicit simulated host clear commit, not a GPU test. A later flush must
 // wait for a NEW captured revision; this prior commit cannot complete it.
 Libs::Graphics::AcknowledgeNativeVideoRevision(Libs::Graphics::NativeVideoRevision());
 Check(Test, presented.stream->ClearAcknowledged(), "host clear commit acknowledges old pending clear");
 std::binary_semaphore committer_ready {0};
 std::thread host_committer([&] {
  const auto before = Libs::Graphics::NativeVideoRevision(); committer_ready.release();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (Libs::Graphics::NativeVideoRevision() == before && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  Check(Test, Libs::Graphics::NativeVideoRevision() != before, "new mailbox flush invalidation observed");
  Libs::Graphics::AcknowledgeNativeVideoRevision(Libs::Graphics::NativeVideoRevision());
 });
 Check(Test, committer_ready.try_acquire_for(std::chrono::seconds(2)), "host clear observer ready");
 done = {}; done.critical = Common::GetHostLockCriticalCounter()->load();
 const int flushed = api.flush(c, Done::Complete, &done); host_committer.join();
 Check(Test, flushed == 0 && done.calls == 1 && done.result == 0 && presented.stream->ClearAcknowledged(),
       "flush success follows new host clear commit outside source locks, callback remains on original guest thread");
 pts = UINT64_C(0xa5a5a5a5a5a5a5a5); sequence = 0x5a5a;
 Check(Test, api.pts(c, &pts, &sequence) == Failed && pts == UINT64_C(0xa5a5a5a5a5a5a5a5) && sequence == 0x5a5a,
       "flush clears previous presented PTS without fabricating replacement");
 done = {}; done.critical = Common::GetHostLockCriticalCounter()->load();
 Check(Test, api.unsupported(c, config.data(), Done::Complete, &done) == Failed && !done.calls,
       "unknown parameter-schema operation rejects synchronously, no forever-wait acceptance");
 done.api = &api; done.context = c; done.action = 1;
 Submit(api, d, mapping, Vp9Config());
 Check(Test, api.remove_decoder(d, Done::Complete, &done) == 0 && done.calls == 1 && done.result == 0,
       "remove completes after SHM release; completion reenters and deletes parent"); Empty();
 Check(Test, presented.stream->IsRetired() && !presented.stream->Acknowledge(presented),
       "removed source lease cannot resurrect a captured in-flight frame");
 Check(Test, api.play(c) == Invalid && api.submit(d, 40, Done::Complete, &done) == Invalid, "stale context/decoder handles never route local backend");

 const int other = Context(api); done = {}; done.critical = Common::GetHostLockCriticalCounter()->load();
 void* second_mapping = nullptr; Decoder(api, other, &second_mapping, done);
 Loader::Program owner_program; owner_program.application_owner = &owner_program;
 const int owner_pid = Loader::RegisterRuntimeProcessIdentity(&owner_program, 794000);
 RuntimeLinker::SetCurrentProgram(&owner_program);
 Check(Test, Mf::EsvmInitialize(&init, false) == 0 && Mf::EsvmRegisterClient("citroncore") == 0 && api.play(other) == Invalid,
       "cross-PID source isolation");
 const int owned = Context(api);
 Check(Test, !Mf::TakeEsvmDecodedFrame(owner_pid, other, d, &frame), "host transfer checks context/decoder/owner relation");
 Check(Test, Mf::ReleaseEsvmOwner(owner_pid) && Loader::DeactivateRuntimeProcessIdentity(&owner_program), "runtime owner retirement");
 RuntimeLinker::SetCurrentProgram(nullptr);
 done = {}; done.api = &api; done.context = other; done.action = 2; done.critical = Common::GetHostLockCriticalCounter()->load();
 Decoder(api, other, &mapping, done);
 Empty();
 Check(Test, Mf::EsvmRegisterClient("citroncore") == 0 && api.play(other) == Invalid && api.play(owned) == Invalid,
       "owner epoch cannot revive old handles");
 // Both completion and genuine refill callbacks share retirement's owner gate.
 for (bool hold_refill: {false, true}) {
  const int held_context = Context(api);
  done = {}; done.critical = Common::GetHostLockCriticalCounter()->load();
  void* held_mapping = nullptr; const int held_decoder = Decoder(api, held_context, &held_mapping, done);
  const auto codec = Vp9Config(); std::memcpy(held_mapping, codec.data(), codec.size());
  std::binary_semaphore entered {0}, resume {0};
  Done held; held.action = hold_refill ? 0 : 4; held.entered = &entered; held.resume = &resume;
  RefillProbe probe; probe.api = &api; probe.context = held_context; probe.action = hold_refill ? 3 : 0;
  probe.entered = &entered; probe.resume = &resume;
  std::atomic_bool retired {false};
  std::thread caller([&] {
   RuntimeLinker::SetCurrentProgram(nullptr);
   held.critical = Common::GetHostLockCriticalCounter()->load();
   probe.critical = held.critical; probe.thread = std::this_thread::get_id(); refill_probe = &probe;
   Check(Test, api.submit(held_decoder, codec.size(), Done::Complete, &held) == 0, "held submit accepted");
   refill_probe = nullptr;
  });
  Check(Test, entered.try_acquire_for(std::chrono::seconds(5)), "source completion/refill entered on original guest thread");
  std::thread retire([&] { Check(Test, Mf::ReleaseEsvmOwner(100), "concurrent source-owner retirement"); retired.store(true); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  uint64_t epoch = 0;
  while (Mf::GetEsvmClientEpoch(100, &epoch) == 0 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  Check(Test, Mf::GetEsvmClientEpoch(100, &epoch) != 0 && !retired.load(),
        "retirement invalidates admission then waits for active source completion/refill gate");
  resume.release(); caller.join(); retire.join(); Empty();
  Check(Test, held.calls == 1 && probe.calls == int(hold_refill), "retirement cancels undelivered refill without reviving a queued generation");
  Check(Test, Mf::EsvmInitialize(&init, false) == 0 && Mf::EsvmRegisterClient("citroncore") == 0 &&
              api.play(held_context) == Invalid, "retired source never revives in new session");
 }
  Check(Test, source_events == 0 && decoder_events.load() != 0, "only actual capacity refill is emitted, no guessed source-ready/can-play/EOS/presentation events");
 InitialConfigurationBarrier(api);
  SourceReadiness(api);
  HevcColourMailbox(api);
  OrderedAvcMailbox(api);
  PausedVideoSmallBatches(api);
 Check(Test, Mf::EsvmTerminate() == 0 && Mf::ReleaseEsvmOwner(100) && Mf::ShutdownEsvmService(),
       "final owned teardown joins service-owned clock worker outside lifecycle locks"); Empty();
 Check(Test, frame.rgba && frame.width == 64, "host-owned decoded frame remains valid after entire service teardown");
}
