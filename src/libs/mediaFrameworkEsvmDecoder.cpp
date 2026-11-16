#include "libs/mediaFrameworkEsvmDecoder.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <atomic>
#include <limits>
#include <new>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/ac3_parser.h>
#include <libavutil/buffer.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

namespace Libs::MediaFramework {
namespace {
using Result = EsvmDecoderResult;
using State = EsvmDecoderState;
using Codec = EsvmDecoderCodec;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
constexpr size_t MiB = 1024 * 1024;
constexpr size_t MaxPackets = 60;
constexpr size_t MaxFrames = 8;

// Byte-wise loads: firmware wire fields/next records need not be aligned.
uint64_t Little(std::span<const uint8_t> bytes) {
	uint64_t value = 0;
	for (size_t i = 0; i < bytes.size(); ++i) value |= uint64_t(bytes[i]) << (8 * i);
	return value;
}
struct Reader {
	std::span<const uint8_t> bytes;
	bool Take(size_t count, std::span<const uint8_t>& out) {
		if (count > bytes.size()) return false;
		out = bytes.first(count);
		bytes = bytes.subspan(count);
		return true;
	}
	bool U64(uint64_t& out) {
		std::span<const uint8_t> part;
		if (!Take(8, part)) return false;
		out = Little(part);
		return true;
	}
};
struct Config {
	Codec codec = Codec::None;
	uint32_t sample_rate = 0;
	uint16_t bottom = 0, top = 0, left = 0, right = 0;
	uint8_t matrix = 2, transfer = 2, primaries = 2;
	uint8_t nal_length = 0;
	bool byte_stream = true;
	size_t extra_size = 0;
	std::array<uint8_t, 4096> extra {};
	bool operator==(const Config&) const = default;
};
struct AuView {
	std::span<const uint8_t> data;
	size_t copied_bytes = 0;
	int64_t pts = 0;
	uint16_t sequence = 0;
	uint64_t attached = 0;
	bool eos = false;
	bool flush = false;
	bool exclusion = false;
	uint32_t trim_start = 0, trim_end = 0; // MICROSECONDS, not sample counts.
};
struct Batch {
	Config config;
	bool has_config = false;
	bool flush = false;
	bool has_sequence = false;
	uint16_t sequence = 0;
	bool eos = false;
	size_t count = 0;
	size_t packet_bytes = 0;
	std::array<AuView, MaxPackets> aus {};
};
struct PacketMeta {
	uint64_t generation;
	uint64_t attached;
	uint16_t sequence;
	int64_t pts;
	uint32_t trim_start, trim_end;
};
struct Slot {
	AVPacket* packet = nullptr;
	bool eos = false;
	Slot() = default;
	~Slot() { av_packet_free(&packet); }
	Slot(const Slot&) = delete;
	Slot& operator=(const Slot&) = delete;
	Slot(Slot&& other) noexcept : packet(std::exchange(other.packet, nullptr)), eos(other.eos) {}
	Slot& operator=(Slot&& other) noexcept {
		if (this != &other) {
			av_packet_free(&packet);
			packet = std::exchange(other.packet, nullptr);
			eos = other.eos;
		}
		return *this;
	}
};
struct CodecDeleter {
	void operator()(AVCodecContext* ptr) const { avcodec_free_context(&ptr); }
};
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;

bool ValidLimits(const EsvmDecoderLimits& l) {
	return l.max_batch_bytes > 0 && l.max_batch_bytes <= 10 * MiB &&
	       l.max_packet_bytes > 0 && l.max_packet_bytes <= 9 * MiB &&
	       l.max_queued_packet_bytes >= l.max_packet_bytes && l.max_queued_packet_bytes <= 18 * MiB &&
	       l.max_queued_packets > 0 && l.max_queued_packets <= MaxPackets &&
	       l.max_queued_frames > 0 && l.max_queued_frames <= MaxFrames &&
	       l.max_queued_frame_bytes >= 4 && l.max_queued_frame_bytes <= 64 * MiB &&
	       l.max_width > 0 && l.max_width <= 4096 && l.max_height > 0 && l.max_height <= 2304 &&
	       l.max_steps_per_pump > 0 && l.max_steps_per_pump <= 4096 &&
	       l.max_audio_channels > 0 && l.max_audio_channels <= 2 &&
	       l.max_audio_samples_per_frame > 0 && l.max_audio_samples_per_frame <= 2048;
}

Result ParseConfig(std::span<const uint8_t> bytes, Config& config) {
	if (bytes.size() != 24 || Little(bytes.first(8)) != 24) return Result::InvalidWire;
	config.codec = Codec::Vp9;
	config.bottom = static_cast<uint16_t>(Little(bytes.subspan(8, 2)));
	config.top = static_cast<uint16_t>(Little(bytes.subspan(10, 2)));
	config.left = static_cast<uint16_t>(Little(bytes.subspan(12, 2)));
	config.right = static_cast<uint16_t>(Little(bytes.subspan(14, 2)));
	config.matrix = bytes[16];
	config.transfer = bytes[17];
	config.primaries = bytes[18];
	// This foundation handles SDR YUV matrix conversions, not arbitrary HDR/RGB.
	if (config.matrix != 1 && config.matrix != 2 && config.matrix != 5 && config.matrix != 6)
		return Result::UnsupportedConfig;
	if (std::any_of(bytes.begin() + 19, bytes.end(), [](uint8_t b) { return b != 0; }))
		return Result::UnsupportedConfig;
	return Result::Ok;
}

constexpr std::array<uint32_t, 13> AacRates {
	96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350
};
Result ParseAacConfig(std::span<const uint8_t> bytes, Config& config) {
	// 13.60 public codec 100 -> serializer 0x2db60, outer subtype FOUR.
	// Opus is subtype SEVEN (0x2e070), not this fixed-size record.
	if (bytes.size() != 16 || Little(bytes.first(8)) != 16) return Result::InvalidWire;
	config.codec = Codec::Aac;
	config.sample_rate = static_cast<uint32_t>(Little(bytes.subspan(8, 4)));
	const auto container = Little(bytes.subspan(12, 4));
	// No ASC/channel/profile fields exist here. Do not invent them for RAW.
	if (container != uint32_t(EsvmAacContainer::Adts) ||
	    std::find(AacRates.begin(), AacRates.end(), config.sample_rate) == AacRates.end())
		return Result::UnsupportedConfig;
	return Result::Ok;
}

Result CheckAdts(std::span<const uint8_t> bytes, const Config& config,
                 const EsvmDecoderLimits& limits) {
	// One complete AAC-LC ADTS frame/one raw_data_block per native AU. No
	// cross-AU parser state, guessed RAW headers, LATM, or multi-frame trim.
	if (bytes.size() < 7 || bytes[0] != 0xff || (bytes[1] & 0xf6) != 0xf0)
		return Result::UnsupportedAccessUnit;
	const size_t header = (bytes[1] & 1) ? 7 : 9;
	const size_t length = (size_t(bytes[3] & 3) << 11) | (size_t(bytes[4]) << 3) | (bytes[5] >> 5);
	if (bytes.size() < header || length != bytes.size()) return Result::InvalidWire;
	if ((bytes[2] >> 6) != 1 || (bytes[6] & 3) != 0) return Result::UnsupportedAccessUnit;
	const size_t rate_index = (bytes[2] >> 2) & 15;
	const uint32_t channels = ((bytes[2] & 1) << 2) | (bytes[3] >> 6);
	if (rate_index >= AacRates.size() || AacRates[rate_index] != config.sample_rate ||
	    channels == 0 || channels > limits.max_audio_channels)
		return Result::UnsupportedAccessUnit;
	return Result::Ok;
}

Result ParseAc3Config(std::span<const uint8_t> bytes, Codec codec, Config& config) {
	// 13.60 serializers 0x2dc50/0x2de50 copy only the public size word.
	// Public size is 16, wire payload is 8. Rate/layout come from syncframes.
	if (bytes.size() != 8 || Little(bytes) != 16) return Result::InvalidWire;
	config.codec = codec;
	return Result::Ok;
}

Result CheckAc3(std::span<const uint8_t> bytes, Codec codec) {
	uint8_t bsid = 0;
	uint16_t frame_size = 0;
	if (bytes.size() < 7 || av_ac3_parse_header(bytes.data(), bytes.size(), &bsid, &frame_size) < 0)
		return Result::InvalidWire;
	if (frame_size != bytes.size()) return Result::InvalidWire; // One complete syncframe per AU.
	if ((codec == Codec::Ac3 && bsid > 10) || (codec == Codec::Eac3 && bsid <= 10))
		return Result::UnsupportedAccessUnit;
	// Dependent substreams need aggregation and different metadata accounting.
	// Do not decode them independently or silently drop additional channels.
	if (codec == Codec::Eac3 && (bytes[2] & 0xf8) != 0) return Result::UnsupportedAccessUnit;
	return Result::Ok;
}

size_t StartCode(std::span<const uint8_t> bytes) {
	if (bytes.size() >= 4 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0 && bytes[3] == 1) return 4;
	if (bytes.size() >= 3 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 1) return 3;
	return 0;
}

Result ParseNalConfig(std::span<const uint8_t> bytes, Codec codec, Config& config) {
	// 13.60: AVC 0x2dd30 / HEVC 0x2df30 serialize size + nalUnitLengthSize,
	// then length-prefixed SPS/PPS (HEVC: VPS/SPS/PPS), then a one-byte
	// isByteStreamFormat blob. These first 16 bytes are NOT crop coordinates.
	Reader reader {bytes};
	uint64_t size = 0, nal_length = 0;
	if (!reader.U64(size) || !reader.U64(nal_length) || size != (codec == Codec::Avc ? 0x38u : 0x48u))
		return Result::InvalidWire;
	if (nal_length > 4) return Result::UnsupportedConfig;
	config.codec = codec;
	config.nal_length = static_cast<uint8_t>(nal_length);
	const size_t count = codec == Codec::Avc ? 2 : 3;
	for (size_t i = 0; i < count; ++i) {
		uint64_t length = 0;
		std::span<const uint8_t> nal;
		if (!reader.U64(length) || length > reader.bytes.size() || !reader.Take(static_cast<size_t>(length), nal))
			return Result::InvalidWire;
		if (nal.empty()) continue; // In-band parameter sets remain the decoder's responsibility.
		nal = nal.subspan(StartCode(nal));
		if (nal.empty() || (nal[0] & 0x80) ||
		    (codec == Codec::Avc ? (nal[0] & 31) != 7 + i : (nal.size() < 2 || ((nal[0] >> 1) & 63) != 32 + i)))
			return Result::UnsupportedConfig;
		if (nal.size() + 4 > config.extra.size() - config.extra_size) return Result::LimitExceeded;
		config.extra[config.extra_size + 3] = 1;
		std::memcpy(config.extra.data() + config.extra_size + 4, nal.data(), nal.size());
		config.extra_size += nal.size() + 4;
	}
	uint64_t length = 0;
	std::span<const uint8_t> format;
	if (!reader.U64(length) || length != 1 || !reader.Take(1, format) || !reader.bytes.empty() || format[0] > 1)
		return Result::InvalidWire;
	config.byte_stream = format[0] != 0;
	if (!config.byte_stream && !nal_length) return Result::UnsupportedConfig;
	return Result::Ok;
}

Result CheckNalAu(std::span<const uint8_t> bytes, const Config& config, size_t& copied) {
	copied = bytes.size();
	if (bytes.empty()) return Result::Ok;
	if (config.byte_stream) return StartCode(bytes) ? Result::Ok : Result::UnsupportedAccessUnit;
	Reader reader {bytes};
	copied = 0;
	for (size_t n = 0; !reader.bytes.empty(); ++n) {
		if (n == 4096) return Result::LimitExceeded;
		std::span<const uint8_t> prefix, nal;
		if (!reader.Take(config.nal_length, prefix)) return Result::InvalidWire;
		uint32_t length = 0;
		for (auto b: prefix) length = (length << 8) | b;
		if (!length || !reader.Take(length, nal)) return Result::InvalidWire;
		copied += 4 + size_t(length);
	}
	return Result::Ok;
}

Result Parse(std::span<const uint8_t> bytes, const EsvmDecoderLimits& limits,
             const EsvmDecoderStatus& status, const Config& current_config, Batch& batch) {
	if (bytes.empty()) return Result::InvalidWire;
	if (bytes.size() > limits.max_batch_bytes) return Result::LimitExceeded;
	Reader outer {bytes};
	bool saw_au_record = false;
	size_t records = 0;
	batch.has_sequence = status.has_sequence;
	batch.sequence = status.sequence;
	batch.eos = status.eos_accepted;
	batch.config = current_config;
	while (!outer.bytes.empty()) {
		if (++records > 64) return Result::LimitExceeded;
		std::span<const uint8_t> header, payload;
		if (!outer.Take(16, header)) return Result::InvalidWire;
		const uint64_t magic = Little(header.first(4));
		const uint64_t subtype = Little(header.subspan(4, 4));
		const uint64_t length = Little(header.subspan(8, 8));
		if (magic != 100 || length > outer.bytes.size()) return Result::InvalidWire;
		if (!outer.Take(static_cast<size_t>(length), payload)) return Result::InvalidWire;
		if (subtype == 8) {
			if (length != 0 || !outer.bytes.empty()) return Result::InvalidWire;
			break; // Optional in-count sentinel; native normally excludes it.
		}
		if (subtype >= 1 && subtype <= 6) {
			// Config must precede AUs; multi-config/batch reordering is not guessed.
			if (batch.has_config || saw_au_record) return Result::UnsupportedConfig;
			batch.config = {};
			const auto result = subtype <= 2 ? ParseNalConfig(payload, subtype == 1 ? Codec::Avc : Codec::Hevc, batch.config)
			                  : subtype == 3 ? ParseConfig(payload, batch.config)
			                  : subtype == 4 ? ParseAacConfig(payload, batch.config)
			                  : ParseAc3Config(payload, subtype == 5 ? Codec::Ac3 : Codec::Eac3, batch.config);
			if (result != Result::Ok) return result;
			batch.has_config = true;
			batch.has_sequence = false;
			batch.eos = false;
			continue;
		}
		if (subtype != 0) return subtype <= 7 ? Result::UnsupportedCodec : Result::InvalidWire;
		saw_au_record = true;
		Reader aus {payload};
		while (!aus.bytes.empty()) {
			if (batch.count == MaxPackets) return Result::LimitExceeded;
			std::span<const uint8_t> fixed;
			if (!aus.Take(0x30, fixed) || Little(fixed.first(8)) != 0x40) return Result::InvalidWire;
			// No protected state or audio-exclusion metadata is silently discarded.
			if (Little(fixed.subspan(0x14, 4)) != 0 || fixed[0x28] != 0) return Result::ProtectedContent;
			if (fixed[0x29] != 0 && !EsvmIsAudioCodec(batch.config.codec)) return Result::UnsupportedAccessUnit;
			if (fixed[0x29] > 1) return Result::InvalidWire;
			if (fixed[0x18] > 1 || fixed[0x19] > 1) return Result::InvalidWire;
			AuView au;
			const uint64_t pts = Little(fixed.subspan(8, 8));
			if (pts > uint64_t(std::numeric_limits<int64_t>::max())) return Result::UnsupportedTimestamp;
			au.pts = static_cast<int64_t>(pts);
			au.sequence = static_cast<uint16_t>(Little(fixed.subspan(0x10, 2)));
			au.attached = Little(fixed.subspan(0x20, 8));
			au.eos = fixed[0x18] != 0;
			au.flush = fixed[0x19] != 0;
			au.exclusion = fixed[0x29] != 0;
			if (au.exclusion) {
				std::span<const uint8_t> trim;
				if (!aus.Take(8, trim)) return Result::InvalidWire;
				au.trim_start = static_cast<uint32_t>(Little(trim.first(4)));
				au.trim_end = static_cast<uint32_t>(Little(trim.subspan(4, 4)));
			}
			if (au.sequence == 0) return Result::SequenceMismatch;
			// A first-AU discontinuity is supported. Mid-batch flush semantics need
			// further native scheduling audit; reject instead of dropping prior AUs.
			if (au.flush) {
				if (batch.count != 0) return Result::UnsupportedAccessUnit;
				batch.flush = true;
				batch.has_sequence = false;
				batch.eos = false;
			}
			if (batch.eos) return Result::InvalidState;
			if (batch.has_sequence && au.sequence != batch.sequence) return Result::SequenceMismatch;
			batch.has_sequence = true;
			batch.sequence = au.sequence;
			uint64_t length = 0;
			if (!aus.U64(length) || length > aus.bytes.size()) return Result::InvalidWire;
			if (length > limits.max_packet_bytes || length > 9 * MiB - batch.packet_bytes)
				return Result::LimitExceeded;
			if (!aus.Take(static_cast<size_t>(length), au.data)) return Result::InvalidWire;
			au.copied_bytes = au.data.size();
			if (batch.config.codec == Codec::Avc || batch.config.codec == Codec::Hevc) {
				const auto result = CheckNalAu(au.data, batch.config, au.copied_bytes);
				if (result != Result::Ok) return result;
				if (au.copied_bytes > limits.max_packet_bytes || au.copied_bytes > 9 * MiB - batch.packet_bytes)
					return Result::LimitExceeded;
			}
			if (EsvmIsAudioCodec(batch.config.codec)) {
				if (batch.packet_bytes + au.data.size() > MiB) return Result::LimitExceeded;
				if (!au.data.empty()) {
					const auto result = batch.config.codec == Codec::Aac ? CheckAdts(au.data, batch.config, limits)
					                                                     : CheckAc3(au.data, batch.config.codec);
					if (result != Result::Ok) return result;
				} else if (au.exclusion) return Result::UnsupportedAccessUnit;
			}
			if (au.data.empty() && !au.eos && !au.flush) return Result::UnsupportedAccessUnit;
			batch.packet_bytes += au.copied_bytes;
			batch.eos = au.eos;
			batch.aus[batch.count++] = au;
		}
	}
	if (!batch.has_config && status.state == State::Unconfigured) return Result::InvalidState;
	if (batch.has_config && batch.count == 0 && !saw_au_record) return Result::Ok;
	if (!batch.has_config && status.eos_accepted && !batch.flush) return Result::InvalidState;
	return Result::Ok;
}
} // namespace

struct EsvmPacketDecoder::Impl {
	EsvmDecoderSchema schema;
	EsvmDecoderLimits limits;
	EsvmDecoderStatus status;
	Config config;
	CodecPtr codec;
	Config pending_config;
	CodecPtr pending_codec;
	size_t configuration_after_packets = 0;
	bool configuration_drain_sent = false;
	uint16_t codec_sequence = 0;
	AVFrame* scratch = nullptr;
	SwsContext* scaler = nullptr;
	bool cropped = false;
	bool send_blocked = false;
	std::array<Slot, MaxPackets> input {};
	std::array<EsvmDecodedFrame, MaxFrames> output {};
	std::array<EsvmDecodedAudio, MaxFrames> audio_output {};
	size_t input_head = 0, output_head = 0;

	Impl(EsvmDecoderSchema s, EsvmDecoderLimits l) : schema(s), limits(l) {
		if (!ValidLimits(l)) status.failure = Result::InvalidLimits;
		else if (s != EsvmDecoderSchema::Firmware1360) status.failure = Result::UnsupportedSchema;
		if (status.failure != Result::Ok) status.state = State::Failed;
	}
	~Impl() {
		av_frame_free(&scratch);
		sws_freeContext(scaler);
	}
	static int GetBuffer(AVCodecContext* context, AVFrame* frame, int flags) {
		const auto& d = *static_cast<const Impl*>(context->opaque);
		if (context->codec_type == AVMEDIA_TYPE_AUDIO) {
			if (frame->nb_samples <= 0 || uint32_t(frame->nb_samples) > d.limits.max_audio_samples_per_frame ||
			    frame->ch_layout.nb_channels <= 0 ||
			    uint32_t(frame->ch_layout.nb_channels) > d.limits.max_audio_channels ||
			    (frame->format != AV_SAMPLE_FMT_FLTP && frame->format != AV_SAMPLE_FMT_FLT))
				return AVERROR(EINVAL);
			return avcodec_default_get_buffer2(context, frame, flags);
		}
		// Enforce allocation dimensions/format BEFORE FFmpeg allocates reference
		// images, not just when publishing an already-decoded oversized frame.
		if (frame->width <= 0 || frame->height <= 0 ||
		    uint32_t(frame->width) > d.limits.max_width || uint32_t(frame->height) > d.limits.max_height ||
		    (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_YUV420P10LE)) {
			static std::atomic<unsigned> diagnostics {0};
			if (diagnostics.fetch_add(1, std::memory_order_relaxed) < 16)
				std::fprintf(stderr, "Esvm video buffer rejected: codec=%d width=%d height=%d pixel_format=%d max=%ux%u\n",
				             int(context->codec_id), frame->width, frame->height, frame->format,
				             d.limits.max_width, d.limits.max_height);
			return AVERROR(EINVAL);
		}
		return avcodec_default_get_buffer2(context, frame, flags);
	}
	void ClearQueues() {
		pending_codec.reset(); pending_config = {};
		configuration_after_packets = 0; configuration_drain_sent = false;
		status.configuration_pending = false;
		for (auto& slot: input) slot = Slot {};
		for (auto& frame: output) frame = {};
		for (auto& frame: audio_output) frame = {};
		input_head = output_head = 0;
		status.queued_packets = status.queued_access_units = status.queued_packet_bytes = 0;
		status.queued_frames = status.queued_frame_bytes = 0;
		status.held_codec_frame = false;
		status.audio_sample_rate = status.audio_channels = 0;
		cropped = false;
		send_blocked = false;
		if (scratch) av_frame_unref(scratch);
	}
	Result Fail(Result result, int error = 0) {
		ClearQueues();
		codec.reset();
		sws_freeContext(scaler);
		scaler = nullptr;
		status.state = State::Failed;
		status.failure = result;
		status.ffmpeg_error = error;
		return result;
	}
	Result Check() const {
		return status.state == State::Failed ? status.failure : Result::Ok;
	}
	bool CanBump() const { return status.generation != std::numeric_limits<uint64_t>::max(); }
	void PopInput() {
		if (status.configuration_pending && configuration_after_packets) --configuration_after_packets;
		input[input_head] = Slot {};
		input_head = (input_head + 1) % MaxPackets;
		--status.queued_packets;
	}
	void ActivateConfiguration() {
		codec = std::move(pending_codec);
		config = pending_config; pending_config = {};
		status.codec = config.codec;
		status.configured_sample_rate = config.sample_rate;
		status.configuration_pending = false;
		configuration_after_packets = 0; configuration_drain_sent = false;
		status.state = State::Ready;
		cropped = send_blocked = false;
		av_frame_unref(scratch);
		// Owned output retains pixels, PTS and sequence. Configuration is not Flush:
		// the source's generation, staged frame, mailbox and clock remain continuous.
	}
	Result Convert() {
		if (EsvmIsAudioCodec(config.codec)) return ConvertAudio();
		if (scratch->width <= 0 || scratch->height <= 0 ||
		    uint32_t(scratch->width) > limits.max_width || uint32_t(scratch->height) > limits.max_height)
			return Fail(Result::LimitExceeded);
		// swscale performs actual 8/10-bit SDR conversion; PQ/HLG need explicit
		// transfer/gamut/tone mapping rather than treating HDR values as SDR.
		if (scratch->format != AV_PIX_FMT_YUV420P && scratch->format != AV_PIX_FMT_YUV420P10LE)
			return Fail(Result::UnsupportedPixelFormat);
		if (scratch->color_trc == AVCOL_TRC_SMPTE2084 || scratch->color_trc == AVCOL_TRC_ARIB_STD_B67) {
			static std::atomic<unsigned> diagnostics {0};
			if (diagnostics.fetch_add(1, std::memory_order_relaxed) < 16)
				std::fprintf(stderr, "Esvm HDR conversion unsupported: format=%d matrix=%d transfer=%d primaries=%d size=%dx%d\n",
				             scratch->format, int(scratch->colorspace), int(scratch->color_trc),
				             int(scratch->color_primaries), scratch->width, scratch->height);
			return Fail(Result::UnsupportedConfig);
		}
		if (scratch->flags & AV_FRAME_FLAG_CORRUPT) return Fail(Result::DecodeFailed);
		if (!cropped) {
			if (scratch->crop_left > size_t(scratch->width) || scratch->crop_right > size_t(scratch->width) ||
			    scratch->crop_top > size_t(scratch->height) || scratch->crop_bottom > size_t(scratch->height))
				return Fail(Result::UnsupportedConfig);
			scratch->crop_left += config.left;
			scratch->crop_right += config.right;
			scratch->crop_top += config.top;
			scratch->crop_bottom += config.bottom;
			if (scratch->crop_left >= size_t(scratch->width) ||
			    scratch->crop_right >= size_t(scratch->width) - scratch->crop_left ||
			    scratch->crop_top >= size_t(scratch->height) ||
			    scratch->crop_bottom >= size_t(scratch->height) - scratch->crop_top)
				return Fail(Result::UnsupportedConfig);
			const int result = av_frame_apply_cropping(scratch, AV_FRAME_CROP_UNALIGNED);
			if (result < 0) return Fail(Result::DecodeFailed, result);
			cropped = true;
		}
		const size_t bytes = size_t(scratch->width) * size_t(scratch->height) * 4;
		if (bytes > limits.max_queued_frame_bytes) return Fail(Result::LimitExceeded);
		if (status.queued_frames == limits.max_queued_frames ||
		    bytes > limits.max_queued_frame_bytes - status.queued_frame_bytes)
			return Result::Backpressure;
		EsvmDecodedFrame frame;
		// Keep a small zeroed SIMD tail; bytes/stride remain the logical image.
		frame.rgba.reset(new (std::nothrow) uint8_t[bytes + AV_INPUT_BUFFER_PADDING_SIZE]);
		if (!frame.rgba) return Fail(Result::AllocationFailed);
		std::memset(frame.rgba.get() + bytes, 0, AV_INPUT_BUFFER_PADDING_SIZE);
		scaler = sws_getCachedContext(scaler, scratch->width, scratch->height, static_cast<AVPixelFormat>(scratch->format),
		                              scratch->width, scratch->height, AV_PIX_FMT_RGBA,
		                              SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!scaler) return Fail(Result::AllocationFailed);
		int matrix = SWS_CS_ITU601;
		if (config.matrix == 1 || (config.matrix == 2 && scratch->colorspace == AVCOL_SPC_BT709))
			matrix = SWS_CS_ITU709;
		else if (config.matrix == 2 && scratch->colorspace != AVCOL_SPC_UNSPECIFIED &&
		         scratch->colorspace != AVCOL_SPC_SMPTE170M && scratch->colorspace != AVCOL_SPC_BT470BG)
			return Fail(Result::UnsupportedConfig);
		const auto* coefficients = sws_getCoefficients(matrix);
		const int color_result = sws_setColorspaceDetails(scaler, coefficients,
		    scratch->color_range == AVCOL_RANGE_JPEG, coefficients, 1, 0, 1 << 16, 1 << 16);
		if (color_result < 0) return Fail(Result::DecodeFailed, color_result);
		uint8_t* planes[] = {frame.rgba.get(), nullptr, nullptr, nullptr};
		int strides[] = {scratch->width * 4, 0, 0, 0};
		const int rows = sws_scale(scaler, scratch->data, scratch->linesize, 0, scratch->height,
		                           planes, strides);
		if (rows != scratch->height) return Fail(Result::DecodeFailed, rows);
		frame.bytes = bytes;
		frame.width = static_cast<uint32_t>(scratch->width);
		frame.height = static_cast<uint32_t>(scratch->height);
		frame.stride = frame.width * 4;
		const int64_t timestamp = scratch->best_effort_timestamp != AV_NOPTS_VALUE
		                              ? scratch->best_effort_timestamp : scratch->pts;
		frame.has_pts = timestamp != AV_NOPTS_VALUE;
		frame.pts = frame.has_pts ? timestamp : 0;
		// Input admission may already describe the NEXT configuration. The old
		// codec still emits its original sequence while draining delayed frames.
		frame.pts_sequence = codec_sequence;
		frame.generation = status.generation;
		if (scratch->opaque_ref && scratch->opaque_ref->size == sizeof(PacketMeta)) {
			PacketMeta meta;
			std::memcpy(&meta, scratch->opaque_ref->data, sizeof(meta));
			if (meta.generation != status.generation || meta.sequence != codec_sequence)
				return Fail(Result::CodecProtocolError);
			frame.attached_data = meta.attached;
			frame.has_attached_data = true;
		}
		// AVC/HEVC wire configuration contains parameter sets, not colour
		// fields. Preserve their decoded VUI rather than publishing the wire
		// parser's unspecified defaults. Explicit VP9 metadata still wins;
		// an unspecified AVFrame remains unspecified and is not labelled SDR.
		auto effective = [](uint8_t configured, int decoded) -> uint8_t {
			return configured != 2 ? configured : decoded >= 0 && decoded <= UINT8_MAX
			                                           ? static_cast<uint8_t>(decoded) : 2;
		};
		frame.matrix_coefficients = effective(config.matrix, scratch->colorspace);
		frame.transfer_characteristics = effective(config.transfer, scratch->color_trc);
		frame.colour_primaries = effective(config.primaries, scratch->color_primaries);
		output[(output_head + status.queued_frames) % MaxFrames] = std::move(frame);
		++status.queued_frames;
		status.queued_frame_bytes += bytes;
		av_frame_unref(scratch);
		status.held_codec_frame = false;
		cropped = false;
		return Result::Ok;
	}
	Result ConvertAudio() {
		if (scratch->flags & AV_FRAME_FLAG_CORRUPT) return Fail(Result::DecodeFailed);
		if (scratch->format != AV_SAMPLE_FMT_FLTP && scratch->format != AV_SAMPLE_FMT_FLT)
			return Fail(Result::UnsupportedSampleFormat);
		if (scratch->nb_samples <= 0 || uint32_t(scratch->nb_samples) > limits.max_audio_samples_per_frame ||
		    scratch->ch_layout.nb_channels <= 0 ||
		    uint32_t(scratch->ch_layout.nb_channels) > limits.max_audio_channels)
			return Fail(Result::LimitExceeded);
		if ((config.codec == Codec::Aac && (scratch->sample_rate != int(config.sample_rate) || scratch->nb_samples != 1024)) ||
		    scratch->sample_rate < 7350 || scratch->sample_rate > 96000 ||
		    scratch->ch_layout.order != AV_CHANNEL_ORDER_NATIVE ||
		    (scratch->ch_layout.u.mask != AV_CH_LAYOUT_MONO && scratch->ch_layout.u.mask != AV_CH_LAYOUT_STEREO))
			return Fail(Result::UnsupportedConfig); // No implicit SBR/rate/layout conversion.
		if (!scratch->opaque_ref || scratch->opaque_ref->size != sizeof(PacketMeta))
			return Fail(Result::CodecProtocolError); // Cannot silently lose exclusion/attachment.
		PacketMeta meta;
		std::memcpy(&meta, scratch->opaque_ref->data, sizeof(meta));
		if (meta.generation != status.generation || meta.sequence != status.sequence)
			return Fail(Result::CodecProtocolError);
		const int64_t timestamp = scratch->pts;
		if (timestamp == AV_NOPTS_VALUE || timestamp != meta.pts) return Fail(Result::CodecProtocolError);
		const auto original = uint32_t(scratch->nb_samples);
		const auto channels = uint32_t(scratch->ch_layout.nb_channels);
		status.audio_sample_rate = static_cast<uint32_t>(scratch->sample_rate);
		status.audio_channels = channels;
		uint32_t start = 0, end = 0, samples = original;
		uint64_t pts_shift = 0;
		// Native common audio exclusion 0x1c5f90: period=(1/rate)*1000*1000,
		// start rounds UP to a sample, tail limit rounds DOWN from rounded AU
		// duration; effective start PTS shift rounds DOWN to microseconds.
		const double period = (1.0 / scratch->sample_rate) * 1000.0 * 1000.0;
		const double duration = original * period;
		if (double(uint64_t(meta.trim_start) + meta.trim_end) >= duration) samples = 0;
		else {
			if (meta.trim_start) {
				start = static_cast<uint32_t>(std::ceil(meta.trim_start / period));
				pts_shift = static_cast<uint64_t>(std::floor(start * period));
			}
			if (meta.trim_end) {
				const double rounded = std::trunc(duration + std::nextafter(0.5, 0.0));
				const auto limit = static_cast<uint32_t>(std::floor((rounded - meta.trim_end) / period));
				end = original - std::min(original, limit);
			}
			if (uint64_t(start) + end > original) return Fail(Result::UnsupportedAccessUnit);
			samples = original - start - end;
		}
		if (!samples) {
			av_frame_unref(scratch);
			status.held_codec_frame = false;
			return Result::Ok; // A fully excluded AU has no fabricated PCM block.
		}
		if (pts_shift > uint64_t(std::numeric_limits<int64_t>::max() - timestamp))
			return Fail(Result::UnsupportedTimestamp);
		const bool planar = scratch->format == AV_SAMPLE_FMT_FLTP;
		const size_t plane_bytes = size_t(original) * (planar ? 1 : channels) * sizeof(float);
		if (scratch->linesize[0] < 0 || size_t(scratch->linesize[0]) < plane_bytes)
			return Fail(Result::CodecProtocolError);
		const size_t bytes = size_t(samples) * channels * sizeof(float);
		if (bytes > limits.max_queued_frame_bytes) return Fail(Result::LimitExceeded);
		if (status.queued_frames == limits.max_queued_frames ||
		    bytes > limits.max_queued_frame_bytes - status.queued_frame_bytes) return Result::Backpressure;
		EsvmDecodedAudio frame;
		frame.pcm.reset(new (std::nothrow) float[size_t(samples) * channels]);
		if (!frame.pcm) return Fail(Result::AllocationFailed);
		for (uint32_t c = 0; c < channels; ++c) {
			if (!scratch->extended_data || !scratch->extended_data[planar ? c : 0])
				return Fail(Result::CodecProtocolError);
			const auto* plane = reinterpret_cast<const float*>(scratch->extended_data[planar ? c : 0]);
			for (uint32_t i = 0; i < samples; ++i) {
				const float value = plane[planar ? start + i : size_t(start + i) * channels + c];
				if (!std::isfinite(value)) return Fail(Result::DecodeFailed);
				frame.pcm[size_t(i) * channels + c] = value;
			}
		}
		frame.bytes = bytes;
		frame.samples = samples; frame.original_samples = original;
		frame.sample_rate = static_cast<uint32_t>(scratch->sample_rate); frame.channels = channels;
		frame.channel_mask = scratch->ch_layout.u.mask;
		frame.source_pts = timestamp; frame.pts = timestamp + int64_t(pts_shift); frame.has_pts = true;
		frame.pts_sequence = meta.sequence; frame.generation = meta.generation;
		frame.attached_data = meta.attached; frame.has_attached_data = true;
		frame.trim_start_samples = start; frame.trim_end_samples = end;
		audio_output[(output_head + status.queued_frames) % MaxFrames] = std::move(frame);
		++status.queued_frames; status.queued_frame_bytes += bytes;
		av_frame_unref(scratch); status.held_codec_frame = false;
		return Result::Ok;
	}
};

EsvmPacketDecoder::EsvmPacketDecoder(EsvmDecoderSchema schema, EsvmDecoderLimits limits)
    : m_impl(new (std::nothrow) Impl(schema, limits)) {}
EsvmPacketDecoder::~EsvmPacketDecoder() = default;

EsvmDecoderConsumeResult EsvmPacketDecoder::Consume(std::span<const uint8_t> bytes, bool allow_access_units) {
	auto rejected = [](Result result) { return EsvmDecoderConsumeResult {result, 0, 0}; };
	if (!m_impl) return rejected(Result::AllocationFailed);
	auto& d = *m_impl;
	if (const auto result = d.Check(); result != Result::Ok) return rejected(result);
	Batch batch;
	if (const auto result = Parse(bytes, d.limits, d.status, d.pending_codec ? d.pending_config : d.config, batch); result != Result::Ok)
		return rejected(result);
	if (!allow_access_units && batch.count) return rejected(Result::InvalidState);
	auto nal = [](Codec codec) { return codec == Codec::Avc || codec == Codec::Hevc; };
	const bool ordered = batch.has_config && !batch.flush && d.codec && !d.pending_codec &&
	                     nal(d.config.codec) && nal(batch.config.codec);
	const bool replace = (batch.has_config && !ordered) || batch.flush;
	if (replace && !d.CanBump()) return rejected(Result::LimitExceeded);
	if (batch.has_config && !ordered && !batch.flush &&
	    (d.pending_codec || d.status.queued_packets || d.status.queued_frames || d.status.held_codec_frame)) {
		auto result = rejected(Result::Backpressure);
		result.config_barrier = batch.config == d.config ? EsvmDecoderConfigBarrier::Identical : EsvmDecoderConfigBarrier::Changed;
		return result;
	}
	const size_t queued_count = replace ? 0 : d.status.queued_packets;
	const size_t queued_bytes = replace ? 0 : d.status.queued_packet_bytes;
	if (EsvmIsAudioCodec(batch.config.codec) && batch.packet_bytes > MiB - std::min(MiB, queued_bytes))
		return rejected(Result::Backpressure);
	if (batch.count > d.limits.max_queued_packets || batch.packet_bytes > d.limits.max_queued_packet_bytes)
		return rejected(Result::LimitExceeded);
	if (batch.count > d.limits.max_queued_packets - queued_count ||
	    batch.packet_bytes > d.limits.max_queued_packet_bytes - queued_bytes)
		return rejected(Result::Backpressure);

	// Allocate every fallible resource before mutation. No vector/deque growth,
	// exceptions, or guest-backed packet references are used.
	CodecPtr next_codec;
	if (batch.has_config) {
		const bool audio = EsvmIsAudioCodec(batch.config.codec);
		const char* name = batch.config.codec == Codec::Aac ? "aac" : batch.config.codec == Codec::Ac3 ? "ac3"
		                 : batch.config.codec == Codec::Eac3 ? "eac3" : batch.config.codec == Codec::Avc ? "h264"
		                 : batch.config.codec == Codec::Hevc ? "hevc" : "vp9";
		const AVCodec* decoder = avcodec_find_decoder_by_name(name);
		if (!decoder) return rejected(Result::UnsupportedCodec);
		next_codec.reset(avcodec_alloc_context3(decoder));
		if (!next_codec) return rejected(Result::AllocationFailed);
		next_codec->thread_count = 1;
		next_codec->thread_type = 0;
		next_codec->max_pixels = int64_t(d.limits.max_width) * d.limits.max_height;
		if (audio) {
			next_codec->sample_rate = static_cast<int>(batch.config.sample_rate);
			next_codec->pkt_timebase = {1, 1000000};
		}
		if (batch.config.codec == Codec::Ac3 || batch.config.codec == Codec::Eac3) {
			// Real codec downmix, including signaled center/surround coefficients;
			// output stays within the native sink's mono/stereo allocation bound.
			if (av_opt_set(next_codec->priv_data, "downmix", d.limits.max_audio_channels == 1 ? "mono" : "stereo", 0) < 0)
				return rejected(Result::UnsupportedConfig);
		}
		next_codec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;
		next_codec->err_recognition = AV_EF_CAREFUL | AV_EF_EXPLODE;
		next_codec->opaque = &d;
		next_codec->get_buffer2 = Impl::GetBuffer;
		if (batch.config.extra_size) {
			next_codec->extradata = static_cast<uint8_t*>(av_mallocz(batch.config.extra_size + AV_INPUT_BUFFER_PADDING_SIZE));
			if (!next_codec->extradata) return rejected(Result::AllocationFailed);
			std::memcpy(next_codec->extradata, batch.config.extra.data(), batch.config.extra_size);
			next_codec->extradata_size = static_cast<int>(batch.config.extra_size);
		}
		const int result = avcodec_open2(next_codec.get(), decoder, nullptr);
		if (result < 0) return rejected(result == AVERROR(ENOMEM) ? Result::AllocationFailed : Result::DecodeFailed);
	}
	std::unique_ptr<AVFrame, void (*)(AVFrame*)> next_scratch(nullptr, [](AVFrame* f) { av_frame_free(&f); });
	if (!d.scratch) {
		next_scratch.reset(av_frame_alloc());
		if (!next_scratch) return rejected(Result::AllocationFailed);
	}
	const uint64_t generation = d.status.generation + (replace ? 1 : 0);
	std::array<Slot, MaxPackets> prepared;
	for (size_t i = 0; i < batch.count; ++i) {
		const auto& au = batch.aus[i];
		auto& slot = prepared[i];
		slot.eos = au.eos;
		if (au.data.empty()) continue;
		slot.packet = av_packet_alloc();
		if (!slot.packet) return rejected(Result::AllocationFailed);
		const int result = av_new_packet(slot.packet, static_cast<int>(au.copied_bytes));
		if (result < 0) return rejected(Result::AllocationFailed);
		// av_new_packet supplies owned AV_INPUT_BUFFER_PADDING_SIZE zero bytes.
		if ((batch.config.codec == Codec::Avc || batch.config.codec == Codec::Hevc) && !batch.config.byte_stream) {
			Reader reader {au.data};
			size_t offset = 0;
			while (!reader.bytes.empty()) {
				std::span<const uint8_t> prefix, nal;
				reader.Take(batch.config.nal_length, prefix); // Entire AU already validated.
				uint32_t length = 0;
				for (auto b: prefix) length = (length << 8) | b;
				reader.Take(length, nal);
				std::memset(slot.packet->data + offset, 0, 3);
				slot.packet->data[offset + 3] = 1;
				std::memcpy(slot.packet->data + offset + 4, nal.data(), nal.size());
				offset += nal.size() + 4;
			}
		} else std::memcpy(slot.packet->data, au.data.data(), au.data.size());
		slot.packet->pts = au.pts;
		slot.packet->dts = AV_NOPTS_VALUE;
		slot.packet->opaque_ref = av_buffer_allocz(sizeof(PacketMeta));
		if (!slot.packet->opaque_ref) return rejected(Result::AllocationFailed);
		const PacketMeta meta {generation, au.attached, au.sequence, au.pts, au.trim_start, au.trim_end};
		std::memcpy(slot.packet->opaque_ref->data, &meta, sizeof(meta));
	}
	// Commit. Native may reuse/overwrite the entire mapping after Ok.
	if (ordered) {
		d.pending_codec = std::move(next_codec);
		d.pending_config = batch.config;
		d.configuration_after_packets = d.status.queued_packets;
		d.configuration_drain_sent = false;
		d.status.configuration_pending = true;
	} else if (replace) {
		if (d.pending_codec && !next_codec) d.ActivateConfiguration();
		d.ClearQueues();
		++d.status.generation;
		if (next_codec) {
			d.codec = std::move(next_codec);
			d.config = batch.config;
			d.status.codec = batch.config.codec;
			d.status.configured_sample_rate = batch.config.sample_rate;
		} else {
			avcodec_flush_buffers(d.codec.get());
		}
		d.status.state = State::Ready;
	}
	if (next_scratch) d.scratch = next_scratch.release();
	d.status.has_sequence = batch.has_sequence;
	d.status.sequence = batch.sequence;
	d.status.eos_accepted = batch.eos;
	for (size_t i = 0; i < batch.count; ++i) {
		if (prepared[i].packet) ++d.status.queued_access_units;
		d.input[(d.input_head + d.status.queued_packets) % MaxPackets] = std::move(prepared[i]);
		++d.status.queued_packets;
	}
	d.status.queued_packet_bytes += batch.packet_bytes;
	return {Result::Ok, bytes.size(), batch.count, EsvmDecoderConfigBarrier::None, ordered};
}

EsvmDecoderResult EsvmPacketDecoder::Pump() {
	if (!m_impl) return Result::AllocationFailed;
	auto& d = *m_impl;
	if (const auto result = d.Check(); result != Result::Ok) return result;
	if (!d.codec) return Result::InvalidState;
	if (d.status.state == State::Ended && !d.pending_codec) return Result::Ok;
	for (size_t step = 0; step < d.limits.max_steps_per_pump; ++step) {
		if (d.pending_codec && !d.configuration_after_packets && d.status.state == State::Ended) {
			d.ActivateConfiguration();
		}
		if (d.status.held_codec_frame) {
			const auto result = d.Convert();
			if (result != Result::Ok) return result;
			continue;
		}
		// A single codec-owned AVFrame can be held in addition to the bounded
		// output queue; packets stay owned until send_packet accepts them.
		int result = avcodec_receive_frame(d.codec.get(), d.scratch);
		if (result == 0) {
			d.status.held_codec_frame = true;
			d.send_blocked = false;
			continue;
		}
		if (result == AVERROR_EOF) {
			if (d.pending_codec && !d.configuration_after_packets &&
			    (d.configuration_drain_sent || d.status.state == State::Draining)) {
				d.ActivateConfiguration();
				continue;
			}
			if (d.status.state != State::Draining) return d.Fail(Result::CodecProtocolError, result);
			d.status.state = State::Ended;
			return Result::Ok;
		}
		if (result != AVERROR(EAGAIN)) return d.Fail(Result::DecodeFailed, result);
		if (d.send_blocked || d.status.state == State::Draining || d.configuration_drain_sent)
			return d.Fail(Result::CodecProtocolError, result); // Never busy-loop both-EAGAIN.
		if (d.pending_codec && !d.configuration_after_packets) {
			// Internal codec drain, NOT input EOS or a source discontinuity. Retain
			// every reordered old frame before admitting new packets to the new codec.
			result = avcodec_send_packet(d.codec.get(), nullptr);
			if (result < 0) return d.Fail(Result::CodecProtocolError, result);
			d.configuration_drain_sent = true;
			continue;
		}
		if (d.status.queued_packets == 0) return Result::Ok;
		auto& slot = d.input[d.input_head];
		if (!slot.packet && !slot.eos) {
			d.PopInput(); // Accepted first-AU flush-only marker; no empty codec packet.
			continue;
		}
		result = avcodec_send_packet(d.codec.get(), slot.packet);
		if (result == AVERROR(EAGAIN)) {
			d.send_blocked = true; // Drain then retry this SAME retained packet, across Yield too.
			continue;
		}
		if (result < 0) return d.Fail(Result::DecodeFailed, result);
		if (slot.packet) {
			PacketMeta meta;
			std::memcpy(&meta, slot.packet->opaque_ref->data, sizeof(meta));
			d.codec_sequence = meta.sequence;
			--d.status.queued_access_units;
			d.status.queued_packet_bytes -= static_cast<size_t>(slot.packet->size);
			av_packet_free(&slot.packet);
			if (!slot.eos) d.PopInput();
			// With EOS+payload, drain its output first, then send null next time.
		} else {
			d.PopInput();
			d.status.state = State::Draining;
		}
	}
	return Result::Yield;
}

bool EsvmPacketDecoder::PopFrame(EsvmDecodedFrame* out) {
	if (!m_impl || !out || !EsvmIsVideoCodec(m_impl->status.codec) || m_impl->status.queued_frames == 0) return false;
	auto& d = *m_impl;
	auto& front = d.output[d.output_head];
	d.status.queued_frame_bytes -= front.bytes;
	*out = std::move(front);
	front = {};
	d.output_head = (d.output_head + 1) % MaxFrames;
	--d.status.queued_frames;
	return true;
}

bool EsvmPacketDecoder::PopAudio(EsvmDecodedAudio* out) {
	if (!m_impl || !out || !EsvmIsAudioCodec(m_impl->status.codec) || m_impl->status.queued_frames == 0) return false;
	auto& d = *m_impl;
	auto& front = d.audio_output[d.output_head];
	d.status.queued_frame_bytes -= front.bytes;
	*out = std::move(front);
	front = {};
	d.output_head = (d.output_head + 1) % MaxFrames;
	--d.status.queued_frames;
	return true;
}

EsvmDecoderResult EsvmPacketDecoder::Flush(uint16_t sequence) {
	if (!m_impl) return Result::AllocationFailed;
	auto& d = *m_impl;
	if (const auto result = d.Check(); result != Result::Ok) return result;
	if (!d.codec) return Result::InvalidState;
	if (sequence == 0) return Result::SequenceMismatch;
	if (!d.CanBump()) return Result::LimitExceeded;
	if (d.pending_codec) d.ActivateConfiguration(); // Flush follows the already-owned configuration.
	d.ClearQueues();
	avcodec_flush_buffers(d.codec.get());
	++d.status.generation;
	d.status.state = State::Ready;
	d.status.eos_accepted = false;
	d.status.has_sequence = true;
	d.status.sequence = sequence;
	return Result::Ok;
}

EsvmDecoderResult EsvmPacketDecoder::FlushCurrent() {
	if (!m_impl) return Result::AllocationFailed;
	auto& d = *m_impl;
	if (const auto result = d.Check(); result != Result::Ok) return result;
	if (!d.CanBump()) return Result::LimitExceeded;
	if (d.pending_codec) d.ActivateConfiguration();
	d.ClearQueues();
	if (d.codec) avcodec_flush_buffers(d.codec.get());
	++d.status.generation;
	d.status.state = d.codec ? State::Ready : State::Unconfigured;
	d.status.eos_accepted = false;
	return Result::Ok;
}

EsvmDecoderResult EsvmPacketDecoder::Reset() {
	if (!m_impl) return Result::AllocationFailed;
	auto& d = *m_impl;
	if (d.schema != EsvmDecoderSchema::Firmware1360) return Result::UnsupportedSchema;
	if (!ValidLimits(d.limits)) return Result::InvalidLimits;
	if (!d.CanBump()) return Result::LimitExceeded;
	d.ClearQueues();
	d.codec.reset();
	sws_freeContext(d.scaler);
	d.scaler = nullptr;
	const uint64_t generation = d.status.generation + 1;
	d.status = {};
	d.status.generation = generation;
	d.config = {};
	return Result::Ok;
}

EsvmDecoderStatus EsvmPacketDecoder::Status() const {
	if (m_impl) return m_impl->status;
	EsvmDecoderStatus status;
	status.state = State::Failed;
	status.failure = Result::AllocationFailed;
	return status;
}

} // namespace Libs::MediaFramework
