#pragma once

#include "common/abi.h"
#include "graphics/presentation/nativeVideo.h"
#include "libs/mediaFrameworkEsvmDecoder.h"
#include "libs/mediaFrameworkEsvmRefill.h"
#include "libs/mediaFrameworkEsvmReadiness.h"
#include "libs/audio_internal.h"

#include <cstddef>
#include <cstdint>

namespace Loader { class SymbolDatabase; }
namespace Libs::MediaFramework {
using EsvmSourceEvent = KYTY_SYSV_ABI void (*)(int32_t event, const void* payload, void* user);
using EsvmDecoderComplete = KYTY_SYSV_ABI void (*)(int32_t handle, int32_t result, void* user);
using EsvmDecoderEvent = KYTY_SYSV_ABI void (*)(int32_t decoder, int32_t result);
struct EsvmSourceCallbacks24 { EsvmSourceEvent event; void* user; uint64_t mask; };
struct EsvmDecoderCallbacks24 { EsvmDecoderEvent event; EsvmDecoderComplete complete; void* user; };
static_assert(sizeof(EsvmSourceCallbacks24) == 24 && sizeof(EsvmDecoderCallbacks24) == 24);
inline constexpr size_t EsvmNativeSourceBytes = 0x2980;
inline constexpr size_t EsvmDecoderMappingBytes = 10 * 1024 * 1024;

// O9 discriminator: false ONLY for a safely read nonempty string prefix (local
// filename backend). Native candidates are gated by real registered ownership,
// copied callbacks/mask and bounded field reads BEFORE copying the full config.
bool TryAddEsvmSource(const void* config, const void* callbacks, int32_t* output, int* result);
bool ReadMediaFrameworkUri(const char* source, char (&output)[0x1000], size_t* length);
int EsvmDeleteSource(int context, uintptr_t caller = 0);
enum class EsvmSourceControl { Play, Pause, Rate, AudibleRate, Volume, Mute, Unsupported };
int EsvmControlSource(int context, EsvmSourceControl control, double value = 0, uintptr_t caller = 0);

int KYTY_SYSV_ABI EsvmAddDecoder(int context, uint64_t bytes, const EsvmDecoderCallbacks24* callbacks, void** mapping);
int KYTY_SYSV_ABI EsvmSubmitDecoderBytes(int decoder, uint64_t bytes, EsvmDecoderComplete complete, void* user);
int KYTY_SYSV_ABI EsvmRemoveDecoder(int decoder, EsvmDecoderComplete complete, void* user);
int KYTY_SYSV_ABI EsvmFlushContext(int context, EsvmDecoderComplete complete, void* user);
int KYTY_SYSV_ABI EsvmGetSourcePts(int context, uint64_t* pts, uint16_t* sequence);
int KYTY_SYSV_ABI EsvmRenderSource(int context, float x, float y, float width, float height);
int KYTY_SYSV_ABI EsvmSetAudibleRate(int context, double rate);
int KYTY_SYSV_ABI EsvmRejectContextOperation(int context, const void* params, EsvmDecoderComplete complete, void* user);
void InitMediaFrameworkSource_1(Loader::SymbolDatabase* symbols);
void InitMediaFramework_1(Loader::SymbolDatabase* symbols); // Shared production routing registrar.
void InvalidateEsvmSourcesOwner(int pid); // Nonblocking under lifecycle lock.
bool DrainRetiredEsvmSources(bool wait = false); // Outside locks; false retains failed kernel cleanup for retry.
void StopEsvmSourceScheduler(); // Host shutdown, outside lifecycle locks; no guest callbacks.

// Host-only diagnostic/fixture transfer before Render selects a mailbox stream.
// An activated stream owns consumption; graphics reads nativeVideo's selected
// mailbox instead, never guesses these handles. Transfer is not presentation.
bool TakeEsvmDecodedFrame(int owner_pid, int context, int decoder, EsvmDecodedFrame* output);

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
// Host-only provenance seam. Production always hashes the actual loaded module.
using EsvmSourceProvenanceProbe = bool (*)(uint64_t source_event, uint64_t decoder_event, uint64_t decoder_complete);
void SetEsvmSourceProvenanceForTest(EsvmSourceProvenanceProbe probe);
enum class EsvmSourceFailure { None, Allocate, ShmOpen, ShmResize, ShmMap, Publish };
void EsvmSourceFailNextForTest(EsvmSourceFailure failure);
struct EsvmSourceResources { size_t contexts, decoders, mappings, descriptors, pending, decoded_frames, audio_leases, closing_audio, staged_audio_bytes, retired_owners; };
EsvmSourceResources EsvmSourceResourcesForTest();
struct EsvmSourceAudioSnapshot {
 std::shared_ptr<Audio::AudioInternal::NativeOutput> lease;
 uint64_t generation = 0, accepted_frames = 0;
 size_t staged_bytes = 0;
  Audio::AudioInternal::NativeQueueResult result = Audio::AudioInternal::NativeQueueResult::Unavailable;
  EsvmDecoderState packet_state = EsvmDecoderState::Unconfigured;
};
EsvmSourceAudioSnapshot EsvmSourceAudioForTest(int decoder);
#endif
} // namespace Libs::MediaFramework
