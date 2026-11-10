#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Libs::MediaFramework {

// Host-only packet foundation. No handles, guest callbacks, SHM admission,
// threads, demux, clock-unit conversion or presentation are implemented here.
// All calls on an instance must be serialized by its owning context service.
enum class EsvmDecoderSchema { Firmware1360, Firmware403 };
enum class EsvmDecoderResult {
	Ok,
	Backpressure, // Nothing from Consume was accepted; pop frames/pump and retry.
	Yield,        // Pump's bounded work budget expired; call Pump again.
	InvalidLimits,
	UnsupportedSchema,
	InvalidWire,
	UnsupportedCodec,
	UnsupportedConfig,
	UnsupportedAccessUnit,
	ProtectedContent,
	InvalidState,
	SequenceMismatch,
	UnsupportedTimestamp,
	LimitExceeded,
	AllocationFailed,
	DecodeFailed,
	UnsupportedPixelFormat,
	CodecProtocolError,
	UnsupportedSampleFormat,
};
// Ready means an opened codec, not native can-play or presentation readiness.
enum class EsvmDecoderState { Unconfigured, Ready, Draining, Ended, Failed };
enum class EsvmDecoderCodec { None, Vp9, Aac, Ac3, Eac3, Avc, Hevc };
constexpr bool EsvmIsAudioCodec(EsvmDecoderCodec codec) {
	return codec == EsvmDecoderCodec::Aac || codec == EsvmDecoderCodec::Ac3 || codec == EsvmDecoderCodec::Eac3;
}
constexpr bool EsvmIsVideoCodec(EsvmDecoderCodec codec) {
	return codec == EsvmDecoderCodec::Vp9 || codec == EsvmDecoderCodec::Avc || codec == EsvmDecoderCodec::Hevc;
}
enum class EsvmAacContainer : uint32_t { Raw = 0, Adts = 1 };

struct EsvmDecoderLimits {
	size_t max_batch_bytes = 10 * 1024 * 1024;
	size_t max_packet_bytes = 9 * 1024 * 1024;
	size_t max_queued_packet_bytes = 9 * 1024 * 1024;
	size_t max_queued_packets = 60;
	size_t max_queued_frames = 4;
	size_t max_queued_frame_bytes = 64 * 1024 * 1024;
	uint32_t max_width = 4096;
	uint32_t max_height = 2304;
	size_t max_steps_per_pump = 256;
	uint32_t max_audio_channels = 2;
	// Includes codec buffer allocation extent: AAC may allocate 2048 samples
	// while publishing a 1024-sample frame. A smaller bound can reject decode.
	uint32_t max_audio_samples_per_frame = 2048;
};

struct EsvmDecodedFrame {
	std::unique_ptr<uint8_t[]> rgba;
	size_t bytes = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t stride = 0;
	// From the received AVFrame, NOT the most recently submitted packet/clock.
	// Values retain native microsecond ticks. Missing timestamps stay missing.
	// This is decoded PTS, NEVER evidence that scanout displayed this frame.
	int64_t pts = 0;
	bool has_pts = false;
	uint16_t pts_sequence = 0;
	uint64_t generation = 0;
	uint64_t attached_data = 0;
	bool has_attached_data = false;
	uint8_t matrix_coefficients = 0;
	uint8_t transfer_characteristics = 0;
	uint8_t colour_primaries = 0;
};

enum class EsvmDecoderConfigBarrier { None, Identical, Changed };
struct EsvmDecoderConsumeResult {
	EsvmDecoderResult result = EsvmDecoderResult::InvalidState;
	// Nonzero only on Ok: every referenced byte has been copied/consumed. The
	// caller can overwrite the entire submitted mapping at that point, even
	// before Pump. This is NOT decode/presentation completion.
	size_t consumed_bytes = 0;
	size_t access_units = 0;
	// Diagnostic only: validated configuration rejected behind pending output/input.
	// Identical compares parsed fields and codec extra data, never struct padding.
	EsvmDecoderConfigBarrier config_barrier = EsvmDecoderConfigBarrier::None;
	bool configuration_queued = false;
};

// Separate from video: interleaved normalized float PCM, no resampling/mixing.
// samples counts sample FRAMES (per channel); bytes = samples * channels * 4.
// PTS is native microseconds after sample-aligned audio exclusion, NOT playback
// completion. source_pts preserves the untrimmed AU timestamp. No implicit
// demux/container encoder-delay removal is performed.
struct EsvmDecodedAudio {
	std::unique_ptr<float[]> pcm;
	size_t bytes = 0;
	uint32_t samples = 0, original_samples = 0;
	uint32_t sample_rate = 0, channels = 0;
	uint64_t channel_mask = 0;
	int64_t pts = 0, source_pts = 0;
	bool has_pts = false;
	uint16_t pts_sequence = 0;
	uint64_t generation = 0, attached_data = 0;
	bool has_attached_data = false;
	uint32_t trim_start_samples = 0, trim_end_samples = 0;
};

struct EsvmDecoderStatus {
	EsvmDecoderState state = EsvmDecoderState::Unconfigured;
	EsvmDecoderResult failure = EsvmDecoderResult::Ok;
	int ffmpeg_error = 0;
	uint64_t generation = 1;
	bool has_sequence = false;
	uint16_t sequence = 0;
	bool eos_accepted = false;
	// One owned AVC/HEVC configuration boundary; old codec output drains first.
	bool configuration_pending = false;
	size_t queued_packets = 0; // Includes an EOS-only entry until it is sent.
	size_t queued_access_units = 0; // Payload-bearing packets only; no EOS/flush markers.
	size_t queued_packet_bytes = 0;
	size_t queued_frames = 0;
	size_t queued_frame_bytes = 0;
	bool held_codec_frame = false; // At most one, waiting for output capacity.
	EsvmDecoderCodec codec = EsvmDecoderCodec::None;
	// queued_frames/queued_frame_bytes count the selected codec's output queue.
	// Audio format is unknown (zero) until actually decoded; never guessed from
	// AAC config has no channel count; AC3/EAC3 wire config has no sample rate.
	uint32_t configured_sample_rate = 0;
	uint32_t audio_sample_rate = 0, audio_channels = 0;
};

class EsvmPacketDecoder final {
public:
	explicit EsvmPacketDecoder(EsvmDecoderSchema schema, EsvmDecoderLimits limits = {});
	~EsvmPacketDecoder();
	EsvmPacketDecoder(const EsvmPacketDecoder&) = delete;
	EsvmPacketDecoder& operator=(const EsvmPacketDecoder&) = delete;
	EsvmPacketDecoder(EsvmPacketDecoder&&) = delete;
	EsvmPacketDecoder& operator=(EsvmPacketDecoder&&) = delete;

	// Input must be an accessible host span bounded to the decoder's logical
	// SHM mapping by the caller. Never dereferences embedded guest pointers.
	// Entire batch validates/admit-copies atomically; rejection changes no state.
	// The source's initial all-stream barrier withholds AU admission. Configuration
	// submissions remain legal; an AU-bearing batch is rejected before mutation.
	EsvmDecoderConsumeResult Consume(std::span<const uint8_t> batch, bool allow_access_units = true);
	EsvmDecoderResult Pump();
	bool PopFrame(EsvmDecodedFrame* out);
	bool PopAudio(EsvmDecodedAudio* out);
	// Explicit discontinuity: discards all pending packets/frames and codec
	// references, retains codec config, establishes a new sequence/generation.
	// Required for a sequence change or restart after EOS; Failed needs Reset.
	EsvmDecoderResult Flush(uint16_t new_sequence);
	// Source-level Flush carries no new sequence and is valid before the first AU
	// or codec configuration. Retain configuration and any established sequence.
	EsvmDecoderResult FlushCurrent();
	// Closes codec and clears failure/config/queues. Generation never resets.
	EsvmDecoderResult Reset();
	[[nodiscard]] EsvmDecoderStatus Status() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::MediaFramework
