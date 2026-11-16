// Standalone: link ONLY mediaFrameworkEsvmDecoder.cpp + avcodec,avutil,swscale.
// No emulator/kernel setup, MediaFramework exports, SHM, network or demux.
#include "libs/mediaFrameworkEsvmDecoder.h"
#include "fixtures/esvm-vp9/packets.h"
#include "fixtures/esvm-aac/packets.h"
#include "fixtures/esvm-ac3/packets.h"
#include "fixtures/esvm-nal/packets.h"
#include "fixtures/esvm-nal/transition.h"
#include "libs/mediaFrameworkEsvmRefill.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace {
using namespace Libs::MediaFramework;
using Result = EsvmDecoderResult;
using State = EsvmDecoderState;
using Bytes = std::vector<uint8_t>;
constexpr auto Schema = EsvmDecoderSchema::Firmware1360;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "MediaFrameworkEsvmDecoderTests: %s\n", message);
		std::abort();
	}
}
void Put(Bytes& bytes, size_t offset, uint64_t value, size_t width) {
	Check(offset <= bytes.size() && width <= bytes.size() - offset, "test writer bounds");
	for (size_t i = 0; i < width; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
Bytes Record(uint32_t type, std::span<const uint8_t> payload) {
	Bytes bytes(16 + payload.size());
	Put(bytes, 0, 100, 4);
	Put(bytes, 4, type, 4);
	Put(bytes, 8, payload.size(), 8);
	std::copy(payload.begin(), payload.end(), bytes.begin() + 16);
	return bytes;
}
Bytes Config(uint16_t bottom = 0, uint16_t top = 0, uint16_t left = 0, uint16_t right = 0) {
	Bytes payload(24);
	Put(payload, 0, 24, 8);
	Put(payload, 8, bottom, 2);
	Put(payload, 10, top, 2);
	Put(payload, 12, left, 2);
	Put(payload, 14, right, 2);
	payload[16] = payload[17] = payload[18] = 6;
	return Record(3, payload);
}
Bytes Au(std::span<const uint8_t> packet, uint64_t pts = 17, uint16_t sequence = 7,
         bool eos = false, bool flush = false, uint64_t attached = 0x12345678) {
	Bytes payload(0x38 + packet.size());
	Put(payload, 0, 0x40, 8);
	Put(payload, 8, pts, 8);
	Put(payload, 0x10, sequence, 2);
	payload[0x18] = eos;
	payload[0x19] = flush;
	Put(payload, 0x20, attached, 8);
	Put(payload, 0x30, packet.size(), 8);
	std::copy(packet.begin(), packet.end(), payload.begin() + 0x38);
	return Record(0, payload);
}
void Append(Bytes& to, const Bytes& from) { to.insert(to.end(), from.begin(), from.end()); }
void Configure(EsvmPacketDecoder& decoder, Bytes config = Config()) {
	const auto accepted = decoder.Consume(config);
	Check(accepted.result == Result::Ok && accepted.consumed_bytes == config.size() &&
	          accepted.access_units == 0, "admit exact VP9 config");
	Check(decoder.Status().state == State::Ready, "configured state");
}
bool SameStatus(const EsvmDecoderStatus& a, const EsvmDecoderStatus& b) {
	return a.state == b.state && a.failure == b.failure && a.ffmpeg_error == b.ffmpeg_error &&
	       a.generation == b.generation && a.has_sequence == b.has_sequence && a.sequence == b.sequence &&
	       a.eos_accepted == b.eos_accepted && a.queued_packets == b.queued_packets &&
	       a.queued_access_units == b.queued_access_units &&
	       a.queued_packet_bytes == b.queued_packet_bytes && a.queued_frames == b.queued_frames &&
	       a.queued_frame_bytes == b.queued_frame_bytes && a.held_codec_frame == b.held_codec_frame &&
	       a.codec == b.codec && a.configured_sample_rate == b.configured_sample_rate &&
	       a.audio_sample_rate == b.audio_sample_rate && a.audio_channels == b.audio_channels;
}
void Reject(EsvmPacketDecoder& decoder, const Bytes& bytes, Result expected) {
	const auto before = decoder.Status();
	const auto rejected = decoder.Consume(bytes);
	Check(rejected.result == expected, "specific rejection reason");
	Check(rejected.consumed_bytes == 0 && rejected.access_units == 0, "rejection accepts no bytes");
	Check(SameStatus(before, decoder.Status()), "rejection must be transactional");
}
void Pixels(const EsvmDecodedFrame& frame, int level, uint32_t width = 64, uint32_t height = 48) {
	Check(frame.rgba && frame.width == width && frame.height == height && frame.stride == width * 4 &&
	          frame.bytes == size_t(width) * height * 4, "owned RGBA dimensions/extent");
	for (size_t i = 0; i < frame.bytes; i += 4) {
		for (size_t c = 0; c < 3; ++c)
			Check(std::abs(int(frame.rgba[i + c]) - level) <= 2, "actual generated grayscale pixels");
		Check(frame.rgba[i + 3] == 255, "decoded alpha");
	}
}
void PumpToIdle(EsvmPacketDecoder& decoder) {
	for (size_t i = 0; i < 256; ++i) {
		const auto result = decoder.Pump();
		if (result == Result::Ok) return;
		Check(result == Result::Yield, "pump result while output has room");
	}
	Check(false, "bounded pump makes progress");
}

void RealPacketsAndOwnership() {
	EsvmPacketDecoder decoder(Schema);
	Bytes batch = Config();
	for (size_t i = 0; i < 3; ++i)
		Append(batch, Au(EsvmVp9Fixture::Packets[i], 17 + i * 1009, 7, i == 2, false, 123 + i));
	// Third record follows an odd-sized packet: no record/field alignment assumption.
	Bytes mapping(batch.size() + 2, 0xcc);
	std::copy(batch.begin(), batch.end(), mapping.begin() + 1);
	const auto accepted = decoder.Consume(std::span(mapping).subspan(1, batch.size()));
	Check(accepted.result == Result::Ok && accepted.access_units == 3 &&
	          accepted.consumed_bytes == batch.size(), "unaligned real native wire admission");
	Check(mapping.front() == 0xcc && mapping.back() == 0xcc, "no writes to mapping/canaries");
	std::fill(mapping.begin(), mapping.end(), 0xa5);
	std::fill(batch.begin(), batch.end(), 0x5a); // Reuse before any decode.
	PumpToIdle(decoder);
	Check(decoder.Status().state == State::Ended, "real FFmpeg EOS drain reaches EOF");
	Check(decoder.Status().queued_packet_bytes == 0 && decoder.Status().queued_packets == 0,
	      "codec consumed all owned packets");
	const auto generation = decoder.Status().generation;
	std::array<EsvmDecodedFrame, 3> frames;
	for (size_t i = 0; i < frames.size(); ++i) {
		Check(decoder.PopFrame(&frames[i]), "receive actual decoded frame");
		Pixels(frames[i], EsvmVp9Fixture::GrayLevels[i]);
		Check(frames[i].has_pts && frames[i].pts == int64_t(17 + i * 1009), "received frame raw PTS");
		Check(frames[i].pts_sequence == 7 && frames[i].generation == generation,
		      "frame sequence/generation");
		Check(frames[i].has_attached_data && frames[i].attached_data == 123 + i,
		      "FFmpeg per-packet attachment propagation");
	}
	Check(!decoder.PopFrame(nullptr), "null pop");
	EsvmDecodedFrame none;
	Check(!decoder.PopFrame(&none), "no fabricated fourth frame");
	Check(decoder.Reset() == Result::Ok, "reset after EOS");
	Pixels(frames[1], 93); // Frames remain owned after codec/mapping destruction/reset.
}

void MalformedAndUnsupported() {
	EsvmPacketDecoder decoder(Schema);
	Configure(decoder);
	const Bytes valid = Au(EsvmVp9Fixture::Black);
	for (size_t n = 0; n < valid.size(); ++n)
		Reject(decoder, Bytes(valid.begin(), valid.begin() + n), Result::InvalidWire);
	const Bytes config = Config();
	for (size_t n = 0; n < config.size(); ++n)
		Reject(decoder, Bytes(config.begin(), config.begin() + n), Result::InvalidWire);
	// Repair the outer extent so truncation reaches the inner AU fields/bytes.
	// n == 16 is a legitimate zero-AU record, not a truncated AU.
	for (size_t n = 17; n < valid.size(); ++n) {
		Bytes short_inner(valid.begin(), valid.begin() + n);
		Put(short_inner, 8, n - 16, 8);
		Reject(decoder, short_inner, Result::InvalidWire);
	}
	Bytes bad = valid;
	Put(bad, 8, std::numeric_limits<uint64_t>::max(), 8);
	Reject(decoder, bad, Result::InvalidWire);
	bad = valid; Put(bad, 16 + 0x30, std::numeric_limits<uint64_t>::max(), 8);
	Reject(decoder, bad, Result::InvalidWire);
	bad = valid; bad[0] = 99;
	Reject(decoder, bad, Result::InvalidWire);
	bad = valid; Put(bad, 16, 0x30, 8);
	Reject(decoder, bad, Result::InvalidWire); // No schema inference from thisSize.
	bad = valid; Put(bad, 16 + 0x14, 1, 4);
	Reject(decoder, bad, Result::ProtectedContent);
	bad = valid; bad[16 + 0x28] = 1;
	Reject(decoder, bad, Result::ProtectedContent);
	bad = valid; bad[16 + 0x29] = 1;
	Reject(decoder, bad, Result::UnsupportedAccessUnit);
	bad = valid; bad[16 + 0x18] = 2;
	Reject(decoder, bad, Result::InvalidWire);
	bad = valid; bad[16 + 0x19] = 2;
	Reject(decoder, bad, Result::InvalidWire);
	bad = valid; Put(bad, 16 + 8, uint64_t(1) << 63, 8);
	Reject(decoder, bad, Result::UnsupportedTimestamp);
	bad = valid; Put(bad, 16 + 0x10, 0, 2);
	Reject(decoder, bad, Result::SequenceMismatch);
	for (uint32_t type: {7u})
		Reject(decoder, Record(type, {}), Result::UnsupportedCodec); // Opus is SEVEN, not four.
	Reject(decoder, Record(4, {}), Result::InvalidWire); // AAC requires its actual 16-byte config.
	Reject(decoder, Record(9, {}), Result::InvalidWire);
	bad = Config(); bad[16 + 16] = 9;
	Reject(decoder, bad, Result::UnsupportedConfig);
	bad = Config(); bad.back() = 1;
	Reject(decoder, bad, Result::UnsupportedConfig);
	bad = valid; Append(bad, Config());
	Reject(decoder, bad, Result::UnsupportedConfig);
	bad = Config(); Append(bad, Record(7, {}));
	Reject(decoder, bad, Result::UnsupportedCodec); // No partial config commit.
	bad = valid; bad.push_back(0xff);
	Reject(decoder, bad, Result::InvalidWire); // No partial packet ownership/queue mutation.
	bad = Record(8, {}); Append(bad, valid);
	Reject(decoder, bad, Result::InvalidWire);
	bad = Au({}, 17, 7, true); Append(bad, valid);
	Reject(decoder, bad, Result::InvalidState);
	bad = valid; Append(bad, Au(EsvmVp9Fixture::Gray, 18, 8, false, true));
	Reject(decoder, bad, Result::UnsupportedAccessUnit); // Mid-batch flush not guessed.
	bad = valid;
	for (size_t offset: {size_t(16 + 0x12), size_t(16 + 0x1a), size_t(16 + 0x2a)}) bad[offset] = 0xff;
	Check(decoder.Consume(bad).result == Result::Ok, "wire padding is NOT a reserved-zero requirement");
	PumpToIdle(decoder);
	EsvmDecodedFrame frame;
	Check(decoder.PopFrame(&frame), "padding-tolerant real frame");
	Pixels(frame, 0);
}

void LimitsBackpressureAndFlush() {
	{
		EsvmPacketDecoder initial(Schema);
		const auto generation = initial.Status().generation;
		Check(initial.FlushCurrent() == Result::Ok && initial.Status().generation > generation &&
		          initial.Status().state == State::Unconfigured && !initial.Status().has_sequence,
		      "source flush before configuration bumps generation without fabricating codec or sequence");
		Configure(initial);
		Check(initial.FlushCurrent() == Result::Ok && initial.Status().state == State::Ready &&
		          !initial.Status().has_sequence && initial.Status().sequence == 0,
		      "configured source flush retains codec without requiring first AU sequence");
		Check(initial.Consume(Au(EsvmVp9Fixture::Black, 123, 7, true)).result == Result::Ok,
		      "first real AU establishes sequence after initial source flush");
		PumpToIdle(initial);
		const auto ended_generation = initial.Status().generation;
		Check(initial.FlushCurrent() == Result::Ok && initial.Status().generation > ended_generation &&
		          initial.Status().has_sequence && initial.Status().sequence == 7 && !initial.Status().eos_accepted &&
		          initial.Status().queued_frames == 0 && initial.Status().queued_access_units == 0,
		      "source flush preserves established sequence while clearing EOS and decoded ownership");
		Check(initial.Consume(Au(EsvmVp9Fixture::Gray, 456, 7)).result == Result::Ok,
		      "retained codec accepts same sequence after source flush");
		PumpToIdle(initial);
		EsvmDecodedFrame frame;
		Check(initial.PopFrame(&frame) && frame.pts == 456 && frame.generation > ended_generation,
		      "source flush never relabels old output into new generation");
		Pixels(frame, 93);
	}
	EsvmDecoderLimits limits;
	limits.max_queued_frames = 1;
	limits.max_queued_frame_bytes = 64 * 48 * 4;
	limits.max_queued_packets = 3;
	EsvmPacketDecoder decoder(Schema, limits);
	Configure(decoder);
	Bytes batch;
	for (size_t i = 0; i < 3; ++i) Append(batch, Au(EsvmVp9Fixture::Packets[i], 101 + i));
	Check(decoder.Consume(batch).result == Result::Ok, "bounded packet queue fills");
	Check(decoder.Status().queued_access_units == 3, "owned payload packets count toward buffered readiness");
	Reject(decoder, Au(EsvmVp9Fixture::Black), Result::Backpressure);
	Check(decoder.Consume(Au(EsvmVp9Fixture::Black)).config_barrier == EsvmDecoderConfigBarrier::None,
	      "ordinary packet capacity pressure is not labeled a configuration barrier");
	Check(decoder.Pump() == Result::Backpressure, "output stops producer, no frame loss");
	Check(decoder.Status().queued_frames == 1 && decoder.Status().held_codec_frame,
	      "one bounded extra AVFrame held for output backpressure");
	const uint64_t old_generation = decoder.Status().generation;
	Check(decoder.Flush(8) == Result::Ok, "flush with pending packets/output");
	Check(decoder.Status().generation > old_generation && !decoder.Status().held_codec_frame &&
	          decoder.Status().queued_frames == 0 && decoder.Status().queued_packets == 0 && decoder.Status().queued_access_units == 0,
	      "flush discards queued AND codec-held old-sequence frames/packets");
	Reject(decoder, Au(EsvmVp9Fixture::Black, 11, 7), Result::SequenceMismatch);
	Check(decoder.Consume(Au(EsvmVp9Fixture::Light, 999, 8, true)).result == Result::Ok,
	      "new sequence EOS with payload");
	Check(decoder.Status().queued_access_units == 1, "EOS with payload counts the payload until send accepts it");
	PumpToIdle(decoder);
	Check(decoder.Status().queued_access_units == 0, "sending EOS payload removes its buffered-unit count");
	EsvmDecodedFrame frame;
	Check(decoder.PopFrame(&frame), "new-sequence frame only");
	Pixels(frame, 186);
	Check(frame.pts_sequence == 8 && frame.pts == 999 && frame.generation > old_generation,
	      "old sequence never relabeled/presented");
	Reject(decoder, Au(EsvmVp9Fixture::Black, 1000, 8), Result::InvalidState);
	const auto eos_generation = decoder.Status().generation;
	Check(decoder.Consume(Au(EsvmVp9Fixture::Gray, 2000, 9, false, true)).result == Result::Ok,
	      "first AU decoderFlush restarts after EOS");
	PumpToIdle(decoder);
	Check(decoder.PopFrame(&frame), "flush AU decoded");
	Pixels(frame, 93);
	Check(frame.pts_sequence == 9 && frame.generation > eos_generation, "AU flush generation");
	Check(decoder.Consume(Au({}, 2001, 9, true)).result == Result::Ok, "separate empty EOS marker");
	Check(decoder.Status().queued_packets == 1 && decoder.Status().queued_access_units == 0,
	      "empty EOS occupies an input slot without counting as buffered payload");
	PumpToIdle(decoder);
	Check(decoder.Status().state == State::Ended && !decoder.PopFrame(&frame), "EOS has no invented pixels");
	Check(decoder.Flush(10) == Result::Ok, "restart ended codec");
	Bytes four;
	for (size_t i = 0; i < 4; ++i) Append(four, Au(EsvmVp9Fixture::Black, i, 10));
	Reject(decoder, four, Result::LimitExceeded);
	Bytes too_many;
	for (size_t i = 0; i < 61; ++i) Append(too_many, Au(EsvmVp9Fixture::Black, i, 10));
	Reject(decoder, too_many, Result::LimitExceeded);
	Reject(decoder, Bytes(10 * 1024 * 1024 + 1), Result::LimitExceeded);
	// O9/config replacement cannot discard pending output without a discontinuity.
	Check(decoder.Consume(Au(EsvmVp9Fixture::Black, 1, 10)).result == Result::Ok, "queue for config BP");
	Reject(decoder, Config(), Result::Backpressure);

	limits = {};
	limits.max_packet_bytes = EsvmVp9Fixture::Black.size();
	limits.max_queued_packet_bytes = EsvmVp9Fixture::Black.size();
	EsvmPacketDecoder byte_bounded(Schema, limits);
	Configure(byte_bounded);
	Check(byte_bounded.Consume(Au(EsvmVp9Fixture::Black)).result == Result::Ok, "byte queue fills");
	Reject(byte_bounded, Au(EsvmVp9Fixture::Gray), Result::Backpressure);
	Bytes oversized(EsvmVp9Fixture::Black.size() + 1, 0);
	Reject(byte_bounded, Au(oversized), Result::LimitExceeded);
}

void BackpressureResumesWithoutLoss() {
	EsvmDecoderLimits limits;
	limits.max_queued_frames = 1;
	limits.max_queued_frame_bytes = 64 * 48 * 4;
	EsvmPacketDecoder decoder(Schema, limits);
	Configure(decoder);
	Bytes batch;
	for (size_t i = 0; i < 3; ++i)
		Append(batch, Au(EsvmVp9Fixture::Packets[i], 123456 + i, 11, i == 2));
	Check(decoder.Consume(batch).result == Result::Ok, "backpressure resume admission");
	std::fill(batch.begin(), batch.end(), 0xcc);
	size_t received = 0;
	bool blocked = false;
	for (size_t step = 0; step < 32 && decoder.Status().state != State::Ended; ++step) {
		const auto result = decoder.Pump();
		Check(result == Result::Ok || result == Result::Yield || result == Result::Backpressure,
		      "real codec output resumes after backpressure");
		blocked |= result == Result::Backpressure;
		Check(decoder.Status().queued_frames <= 1 && decoder.Status().queued_frame_bytes <= 64 * 48 * 4,
		      "output remains bounded across drain/retry");
		EsvmDecodedFrame frame;
		while (decoder.PopFrame(&frame)) {
			Check(received < 3 && frame.has_pts && frame.pts == int64_t(123456 + received),
			      "backpressure does not drop/relabel packets");
			Pixels(frame, EsvmVp9Fixture::GrayLevels[received]);
			++received;
		}
	}
	Check(blocked && decoder.Status().state == State::Ended && received == 3,
	      "all three actual frames survive bounded output and actual EOS drain");
}

void DecodeFailureCropSchemaAndYield() {
	EsvmPacketDecoder decoder(Schema);
	Configure(decoder, Config(2, 2, 2, 2));
	Check(decoder.Consume(Au(EsvmVp9Fixture::Gray)).result == Result::Ok, "crop packet");
	PumpToIdle(decoder);
	EsvmDecodedFrame frame;
	Check(decoder.PopFrame(&frame), "cropped frame");
	Pixels(frame, 93, 60, 44);
	Check(decoder.Reset() == Result::Ok, "reset crop config");
	Configure(decoder);
	const std::array<uint8_t, 7> garbage {0,1,2,3,4,5,6};
	Check(decoder.Consume(Au(garbage)).result == Result::Ok, "owned malformed CODEC packet admitted");
	Check(decoder.Pump() == Result::DecodeFailed && decoder.Status().state == State::Failed,
	      "actual codec failure is sticky, not fake successful decode");
	Check(decoder.Status().queued_packets == 0 && decoder.Status().queued_frames == 0,
	      "failure releases queued resources");
	Reject(decoder, Config(), Result::DecodeFailed);
	Check(decoder.Flush(8) == Result::DecodeFailed, "flush cannot hide a fatal codec error");
	Check(decoder.FlushCurrent() == Result::DecodeFailed, "source flush cannot hide a fatal codec error");
	Check(decoder.Reset() == Result::Ok, "reset explicitly recovers failure");
	Configure(decoder);
	Check(decoder.Consume(Au(EsvmVp9Fixture::Black)).result == Result::Ok, "recover with real packet");
	PumpToIdle(decoder);
	Check(decoder.PopFrame(&frame), "real frame after recovery");
	Pixels(frame, 0);

	EsvmPacketDecoder old(EsvmDecoderSchema::Firmware403);
	Reject(old, Config(), Result::UnsupportedSchema);
	Check(old.Pump() == Result::UnsupportedSchema, "explicit unsupported 4.03 schema");
	EsvmDecoderLimits invalid;
	invalid.max_queued_packets = 61;
	EsvmPacketDecoder bad_limits(Schema, invalid);
	Reject(bad_limits, Config(), Result::InvalidLimits);
	EsvmPacketDecoder empty(Schema);
	Reject(empty, Au(EsvmVp9Fixture::Black), Result::InvalidState);
	Check(empty.Pump() == Result::InvalidState && empty.Flush(7) == Result::InvalidState,
	      "unconfigured codec cannot decode/flush");
	EsvmDecoderLimits bounded;
	bounded.max_width = 32;
	EsvmPacketDecoder dimension_bounded(Schema, bounded);
	Configure(dimension_bounded);
	Check(dimension_bounded.Consume(Au(EsvmVp9Fixture::Black)).result == Result::Ok, "dimension admission");
	const auto dimension_result = dimension_bounded.Pump();
	Check(dimension_result == Result::DecodeFailed || dimension_result == Result::LimitExceeded,
	      "real oversized decoded dimensions fail without publishing pixels");
	Check(!dimension_bounded.PopFrame(&frame), "no frame exceeds pixel budget");
	bounded = {};
	bounded.max_queued_frame_bytes = 4;
	EsvmPacketDecoder image_byte_bounded(Schema, bounded);
	Configure(image_byte_bounded);
	Check(image_byte_bounded.Consume(Au(EsvmVp9Fixture::Black)).result == Result::Ok,
	      "frame-byte budget admission");
	Check(image_byte_bounded.Pump() == Result::LimitExceeded && !image_byte_bounded.PopFrame(&frame),
	      "an image too large ever to fit fails instead of permanent backpressure");
	EsvmPacketDecoder bad_crop(Schema);
	Configure(bad_crop, Config(48, 0, 0, 0));
	Check(bad_crop.Consume(Au(EsvmVp9Fixture::Black)).result == Result::Ok, "crop extent input");
	Check(bad_crop.Pump() == Result::UnsupportedConfig && !bad_crop.PopFrame(&frame),
	      "crop cannot escape decoded frame extent");
	bounded = {};
	bounded.max_steps_per_pump = 1;
	EsvmPacketDecoder yielding(Schema, bounded);
	Configure(yielding);
	Check(yielding.Consume(Au(EsvmVp9Fixture::Gray, 42000, 7, true)).result == Result::Ok, "bounded work input");
	Check(yielding.Pump() == Result::Yield, "pump work budget");
	PumpToIdle(yielding);
	Check(yielding.PopFrame(&frame) && frame.pts == 42000, "yield retains packet/frame progress");
	Pixels(frame, 93);
}

Bytes AacConfig(uint32_t rate = 48000, uint32_t container = 1) {
	Bytes payload(16);
	Put(payload, 0, 16, 8);
	Put(payload, 8, rate, 4);
	Put(payload, 12, container, 4);
	return Record(4, payload);
}
Bytes AudioAu(std::span<const uint8_t> packet, uint64_t pts, bool eos = false,
              bool exclusion = false, uint32_t start_us = 0, uint32_t end_us = 0,
              uint16_t sequence = 13) {
	Bytes bytes = Au(packet, pts, sequence, eos, false, pts + 17);
	if (exclusion) {
		bytes[16 + 0x29] = 1;
		bytes.insert(bytes.begin() + 16 + 0x30, 8, 0);
		Put(bytes, 16 + 0x30, start_us, 4);
		Put(bytes, 16 + 0x34, end_us, 4);
		Put(bytes, 8, bytes.size() - 16, 8);
	}
	return bytes;
}
void AudioFormat(const EsvmDecodedAudio& frame, uint32_t samples = 1024) {
	Check(frame.pcm && frame.samples == samples && frame.original_samples == 1024 &&
	          frame.channels == 2 && frame.channel_mask == 3 && frame.sample_rate == 48000 &&
	          frame.bytes == size_t(samples) * 2 * sizeof(float), "real owned stereo float PCM format");
	for (size_t i = 0; i < frame.bytes / sizeof(float); ++i)
		Check(std::isfinite(frame.pcm[i]) && std::abs(frame.pcm[i]) < 1.0f, "finite actual waveform samples");
}
double ToneAmplitude(const EsvmDecodedAudio& frame, uint32_t channel, double frequency) {
	double sine = 0, cosine = 0;
	for (uint32_t i = 0; i < frame.samples; ++i) {
		const double phase = 6.283185307179586 * frequency * i / frame.sample_rate;
		const double value = frame.pcm[size_t(i) * frame.channels + channel];
		sine += value * std::sin(phase);
		cosine += value * std::cos(phase);
	}
	return 2 * std::sqrt(sine * sine + cosine * cosine) / frame.samples;
}
constexpr size_t AudioPackets = EsvmAacFixture::Packets.size();
using AudioFrames = std::array<EsvmDecodedAudio, AudioPackets>;
AudioFrames DecodeAudioReference() {
	EsvmDecoderLimits limits;
	limits.max_queued_frames = 8;
	EsvmPacketDecoder decoder(Schema, limits);
	Bytes batch = AacConfig();
	for (size_t i = 0; i < AudioPackets; ++i)
		Append(batch, AudioAu(EsvmAacFixture::Packets[i], 100000 + i * 30001, i + 1 == AudioPackets));
	Bytes mapping(batch.size() + 2, 0xab);
	std::copy(batch.begin(), batch.end(), mapping.begin() + 1);
	const auto accepted = decoder.Consume(std::span(mapping).subspan(1, batch.size()));
	Check(accepted.result == Result::Ok && accepted.access_units == AudioPackets &&
	          accepted.consumed_bytes == batch.size(), "unaligned AAC config + AU ownership");
	Check(mapping.front() == 0xab && mapping.back() == 0xab, "audio mapping canaries intact");
	std::fill(mapping.begin(), mapping.end(), 0xcc);
	std::fill(batch.begin(), batch.end(), 0xdd);
	Check(decoder.Status().codec == EsvmDecoderCodec::Aac && decoder.Status().configured_sample_rate == 48000 &&
	          decoder.Status().audio_channels == 0, "opened AAC decoder does not invent channel count");
	PumpToIdle(decoder);
	Check(decoder.Status().state == State::Ended && decoder.Status().audio_channels == 2 &&
	          decoder.Status().audio_sample_rate == 48000, "actual audio EOS and discovered format");
	EsvmDecodedFrame video;
	Check(!decoder.PopFrame(&video) && decoder.Status().queued_frames == AudioPackets,
	      "video pop cannot steal/interpret audio output");
	AudioFrames frames;
	for (size_t i = 0; i < AudioPackets; ++i) {
		Check(decoder.PopAudio(&frames[i]), "receive each actual AAC PCM block");
		AudioFormat(frames[i]);
		Check(frames[i].has_pts && frames[i].pts == int64_t(100000 + i * 30001) &&
		          frames[i].source_pts == frames[i].pts && frames[i].pts_sequence == 13 &&
		          frames[i].generation == decoder.Status().generation && frames[i].has_attached_data &&
		          frames[i].attached_data == uint64_t(frames[i].pts) + 17, "audio packet metadata and microsecond PTS");
		Check(frames[i].trim_start_samples == 0 && frames[i].trim_end_samples == 0,
		      "no invented container priming or tail exclusion");
	}
	Check(!decoder.PopAudio(nullptr), "null audio pop");
	EsvmDecodedAudio none;
	Check(!decoder.PopAudio(&none), "no fabricated audio after EOS");
	// Steady blocks, excluding AAC's encoder priming and trailing partial block.
	// Both frequencies/amplitudes must be real and independently located in the
	// expected channels; silence/random noise/duplicated mono cannot pass.
	for (size_t i: {size_t(2), size_t(3), size_t(4)}) {
		Check(std::abs(ToneAmplitude(frames[i], 0, 440) - 0.25) < 0.04 &&
		          std::abs(ToneAmplitude(frames[i], 1, 880) - 0.25) < 0.04,
		      "decoded PCM contains purpose-generated left/right tone amplitudes");
		Check(ToneAmplitude(frames[i], 0, 880) < 0.04 && ToneAmplitude(frames[i], 1, 440) < 0.04,
		      "real stereo separation, not duplicated or exchanged channels");
	}
	Check(decoder.Reset() == Result::Ok && decoder.Status().codec == EsvmDecoderCodec::None,
	      "reset closes AAC and clears discriminant");
	AudioFormat(frames[3]); // Survives decoder reset and destruction on return.
	return frames;
}

void AudioTrimAndOwnership(const AudioFrames& reference) {
	EsvmDecoderLimits limits;
	limits.max_queued_frames = 8;
	EsvmPacketDecoder decoder(Schema, limits);
	Bytes batch = AacConfig();
	for (size_t i = 0; i < AudioPackets; ++i) {
		const uint32_t start = i == 1 ? 50000 : i == 2 ? 1001 : 0;
		const uint32_t end = i == 4 ? 30000 : i == 2 ? 2001 : 0;
		Append(batch, AudioAu(EsvmAacFixture::Packets[i], 100000 + i * 30001,
		                     i + 1 == AudioPackets, true, start, end));
	}
	Check(decoder.Consume(batch).result == Result::Ok, "admit actual microsecond audio-exclusion wire");
	std::fill(batch.begin(), batch.end(), 0xee);
	PumpToIdle(decoder);
	Check(decoder.Status().state == State::Ended && decoder.Status().queued_frames == AudioPackets - 2,
	      "fully excluded AUs produce no PCM but still decode/drain statefully");
	EsvmDecodedAudio frame;
	for (size_t index: {size_t(0), size_t(2), size_t(3), size_t(5)}) {
		Check(decoder.PopAudio(&frame), "pop nonempty trimmed audio only");
		const uint32_t start = index == 2 ? 49 : 0;
		const uint32_t end = index == 2 ? 97 : 0;
		AudioFormat(frame, 1024 - start - end);
		Check(frame.trim_start_samples == start && frame.trim_end_samples == end &&
		          frame.source_pts == reference[index].pts &&
		          frame.pts == reference[index].pts + (index == 2 ? 1020 : 0),
		      "native exclusion rounds sample boundaries and shifts PTS by effective start");
		for (size_t i = 0; i < size_t(frame.samples) * 2; ++i)
			Check(frame.pcm[i] == reference[index].pcm[size_t(start) * 2 + i],
			      "trim selects exact decoded PCM samples without changing history or dropping metadata");
	}
	const float retained = frame.pcm[99];
	Check(decoder.Flush(14) == Result::Ok && decoder.Reset() == Result::Ok && frame.pcm[99] == retained,
	      "popped audio remains independently owned across flush/reset");
}

void AudioRejectionBoundsAndFailure() {
	EsvmPacketDecoder decoder(Schema);
	Check(decoder.Consume(AacConfig()).result == Result::Ok, "AAC config opens actual codec");
	Reject(decoder, AacConfig(48000, 0), Result::UnsupportedConfig); // RAW has no guessed ASC/channels.
	Reject(decoder, AacConfig(48000, 2), Result::UnsupportedConfig);
	Reject(decoder, AacConfig(0), Result::UnsupportedConfig);
	Reject(decoder, AacConfig(12345), Result::UnsupportedConfig);
	Bytes bad = AacConfig(); Put(bad, 16, 24, 8);
	Reject(decoder, bad, Result::InvalidWire);
	bad = AacConfig(); bad.push_back(0); Put(bad, 8, 17, 8);
	Reject(decoder, bad, Result::InvalidWire);
	for (size_t n = 0; n < 32; ++n) {
		const auto config = AacConfig();
		Reject(decoder, Bytes(config.begin(), config.begin() + n), Result::InvalidWire);
	}
	const auto valid = AudioAu(EsvmAacFixture::Packets[0], 1000, false, true, 1001, 2001);
	for (size_t n = 16 + 0x30; n < 16 + 0x38; ++n) {
		bad.assign(valid.begin(), valid.begin() + n); Put(bad, 8, n - 16, 8);
		Reject(decoder, bad, Result::InvalidWire);
	}
	bad = valid; bad[16 + 0x28] = 1;
	Reject(decoder, bad, Result::ProtectedContent);
	bad = valid; Put(bad, 16 + 0x14, 1, 4);
	Reject(decoder, bad, Result::ProtectedContent);
	bad = valid; bad[16 + 0x29] = 2;
	Reject(decoder, bad, Result::InvalidWire);
	Reject(decoder, AudioAu({}, 1000, true, true), Result::UnsupportedAccessUnit);
	Bytes packet(EsvmAacFixture::Packets[0].begin(), EsvmAacFixture::Packets[0].end());
	packet[0] = 0;
	Reject(decoder, AudioAu(packet, 1), Result::UnsupportedAccessUnit);
	packet.assign(EsvmAacFixture::Packets[0].begin(), EsvmAacFixture::Packets[0].end());
	packet[6] |= 1;
	Reject(decoder, AudioAu(packet, 1), Result::UnsupportedAccessUnit); // Multiple raw_data_blocks.
	packet[6] &= 0xfc; packet[2] &= 0x3f;
	Reject(decoder, AudioAu(packet, 1), Result::UnsupportedAccessUnit); // AAC Main, not LC.
	packet.assign(EsvmAacFixture::Packets[0].begin(), EsvmAacFixture::Packets[0].end());
	packet[2] = (packet[2] & 0xc3) | (4 << 2);
	Reject(decoder, AudioAu(packet, 1), Result::UnsupportedAccessUnit); // Header/config rate mismatch.
	packet.assign(EsvmAacFixture::Packets[0].begin(), EsvmAacFixture::Packets[0].end());
	packet.pop_back();
	Reject(decoder, AudioAu(packet, 1), Result::InvalidWire);
	bad = AacConfig(); Append(bad, Record(7, {}));
	Reject(decoder, bad, Result::UnsupportedCodec); // No partial config commit; real Opus unsupported.

	// Valid ADTS framing around deliberately invalid compressed AAC syntax.
	packet.assign(20, 0);
	std::copy_n(EsvmAacFixture::Packets[0].begin(), 7, packet.begin());
	packet[3] &= 0xfc; packet[4] = 20 >> 3; packet[5] = ((20 & 7) << 5) | 31;
	Check(decoder.Consume(AudioAu(packet, 1000)).result == Result::Ok, "owned compressed AAC syntax admitted");
	Check(decoder.Pump() == Result::DecodeFailed && decoder.Status().state == State::Failed,
	      "invalid AAC syntax fails actual decoder truthfully");
	Reject(decoder, AacConfig(), Result::DecodeFailed);
	Check(decoder.Reset() == Result::Ok, "recover AAC failure explicitly");
	Check(decoder.Consume(AacConfig()).result == Result::Ok, "reinitialize after AAC failure");
	Check(decoder.Consume(AudioAu(EsvmAacFixture::Packets[0], uint64_t(std::numeric_limits<int64_t>::max()) - 10,
	                              false, true, 1001)).result == Result::Ok, "bounded input PTS near overflow");
	Check(decoder.Pump() == Result::UnsupportedTimestamp, "trim PTS addition cannot overflow");

	EsvmDecoderLimits limits;
	limits.max_audio_channels = 1;
	EsvmPacketDecoder mono_only(Schema, limits);
	Check(mono_only.Consume(AacConfig()).result == Result::Ok, "bounded AAC channels config");
	Reject(mono_only, AudioAu(EsvmAacFixture::Packets[0], 1), Result::UnsupportedAccessUnit);
	limits = {}; limits.max_queued_frame_bytes = 4;
	EsvmPacketDecoder tiny_pcm(Schema, limits);
	Check(tiny_pcm.Consume(AacConfig()).result == Result::Ok &&
	          tiny_pcm.Consume(AudioAu(EsvmAacFixture::Packets[0], 1)).result == Result::Ok,
	      "PCM budget input admission");
	Check(tiny_pcm.Pump() == Result::LimitExceeded, "PCM too large ever to fit fails without permanent BP");
}

void AudioBackpressureFlushAndDiscrimination() {
	EsvmDecoderLimits limits;
	// One stereo PCM block fits, as does the later 64x48 RGBA replacement.
	limits.max_queued_frames = 1; limits.max_queued_frame_bytes = 64 * 48 * 4;
	limits.max_queued_packets = 3;
	EsvmPacketDecoder decoder(Schema, limits);
	Check(decoder.Consume(AacConfig()).result == Result::Ok, "bounded PCM queue configure");
	Bytes batch;
	for (size_t i = 0; i < 3; ++i) Append(batch, AudioAu(EsvmAacFixture::Packets[i], 500 + i, i == 2));
	Check(decoder.Consume(batch).result == Result::Ok, "audio input queue fills");
	Reject(decoder, AudioAu(EsvmAacFixture::Packets[3], 503), Result::InvalidState); // EOS already accepted.
	Check(decoder.Pump() == Result::Backpressure && decoder.Status().queued_frames == 1 &&
	          decoder.Status().held_codec_frame, "one bounded codec-owned audio block held under BP");
	Reject(decoder, Config(), Result::Backpressure);
	size_t received = 0;
	for (size_t step = 0; step < 32 && received < 3; ++step) {
		EsvmDecodedAudio frame;
		if (decoder.PopAudio(&frame)) {
			AudioFormat(frame);
			Check(frame.pts == int64_t(500 + received++), "audio BP preserves PCM block order/PTS");
		}
		const auto result = decoder.Pump();
		Check(result == Result::Ok || result == Result::Backpressure || result == Result::Yield,
		      "audio bounded pump resumes without loss");
		Check(decoder.Status().queued_frame_bytes <= 8192, "PCM bytes remain bounded");
	}
	Check(received == 3 && decoder.Status().state == State::Ended, "audio EOS after BP drain");
	const auto old = decoder.Status().generation;
	Check(decoder.Flush(14) == Result::Ok && decoder.Status().generation > old, "AAC flush retains real codec/new generation");
	Reject(decoder, AudioAu(EsvmAacFixture::Packets[0], 1), Result::SequenceMismatch);
	Check(decoder.Consume(AudioAu(EsvmAacFixture::Packets[0], 900, false, false, 0, 0, 14)).result == Result::Ok,
	      "audio restart after EOS");
	PumpToIdle(decoder);
	Check(decoder.Consume(AudioAu(EsvmAacFixture::Packets[1], 901, false, false, 0, 0, 14)).result == Result::Ok &&
	          decoder.Pump() == Result::Backpressure, "AAC output held before flush");
	const auto generation = decoder.Status().generation;
	Check(decoder.Flush(15) == Result::Ok && decoder.Status().queued_frames == 0 &&
	          !decoder.Status().held_codec_frame && decoder.Status().queued_packets == 0 &&
	          decoder.Status().generation > generation, "flush discards pending/held audio without relabeling");
	Check(decoder.Consume(AudioAu({}, 1000, true, false, 0, 0, 15)).result == Result::Ok, "separate empty audio EOS");
	PumpToIdle(decoder);
	EsvmDecodedAudio none;
	Check(decoder.Status().state == State::Ended && !decoder.PopAudio(&none), "empty audio EOS invents no PCM");
	Configure(decoder);
	Check(decoder.Status().codec == EsvmDecoderCodec::Vp9 && decoder.Status().configured_sample_rate == 0,
	      "idle codec replacement is explicitly discriminated");
	Check(decoder.Consume(Au(EsvmVp9Fixture::Gray)).result == Result::Ok, "VP9 preserved after AAC replacement");
	PumpToIdle(decoder);
	Check(!decoder.PopAudio(&none) && decoder.Status().queued_frames == 1, "audio pop cannot steal RGBA frame");
	EsvmDecodedFrame video;
	Check(decoder.PopFrame(&video), "receive independent VP9 output");
	Pixels(video, 93);
}

void AudioInputBoundsYieldAndIndependentVideo() {
	EsvmDecoderLimits limits;
	limits.max_queued_packets = 1;
	EsvmPacketDecoder bounded(Schema, limits);
	Check(bounded.Consume(AacConfig()).result == Result::Ok, "audio input-count bound configure");
	Check(bounded.Consume(AudioAu(EsvmAacFixture::Packets[0], 1000)).result == Result::Ok,
	      "one audio packet admitted");
	Reject(bounded, AudioAu(EsvmAacFixture::Packets[1], 2000), Result::Backpressure);
	Bytes batch = AudioAu(EsvmAacFixture::Packets[0], 1);
	Append(batch, AudioAu(EsvmAacFixture::Packets[1], 2));
	Reject(bounded, batch, Result::LimitExceeded);
	limits = {};
	limits.max_packet_bytes = EsvmAacFixture::Packets[0].size();
	limits.max_queued_packet_bytes = limits.max_packet_bytes;
	EsvmPacketDecoder byte_bounded(Schema, limits);
	Check(byte_bounded.Consume(AacConfig()).result == Result::Ok, "audio input-byte bound configure");
	Reject(byte_bounded, AudioAu(EsvmAacFixture::Packets[1], 1), Result::LimitExceeded);
	Check(byte_bounded.Consume(AudioAu(EsvmAacFixture::Packets[0], 1)).result == Result::Ok,
	      "audio byte queue fills");
	Reject(byte_bounded, AudioAu(EsvmAacFixture::Packets[0], 2), Result::Backpressure);
	limits = {}; limits.max_audio_samples_per_frame = 1;
	EsvmPacketDecoder allocation_bounded(Schema, limits);
	Check(allocation_bounded.Consume(AacConfig()).result == Result::Ok &&
	          allocation_bounded.Consume(AudioAu(EsvmAacFixture::Packets[0], 1)).result == Result::Ok,
	      "audio get-buffer allocation bound input");
	Check(allocation_bounded.Pump() == Result::DecodeFailed, "audio allocation bound checked before FFmpeg buffer allocation");

	EsvmPacketDecoder video(Schema);
	Configure(video);
	Check(video.Consume(Au(EsvmVp9Fixture::Gray, 123)).result == Result::Ok, "independent VP9 input");
	PumpToIdle(video);
	const auto video_status = video.Status();
	limits = {}; limits.max_steps_per_pump = 1;
	EsvmPacketDecoder yielding(Schema, limits);
	Check(yielding.Consume(AacConfig()).result == Result::Ok &&
	          yielding.Consume(AudioAu(EsvmAacFixture::Packets[0], 5000, true)).result == Result::Ok,
	      "yielding audio input and EOS");
	Check(yielding.Pump() == Result::Yield, "bounded AAC pump work yields");
	PumpToIdle(yielding);
	EsvmDecodedAudio audio;
	Check(yielding.PopAudio(&audio) && audio.pts == 5000, "yield preserves owned AAC metadata/PCM");
	AudioFormat(audio);
	const auto generation = yielding.Status().generation;
	Bytes discontinuity = AudioAu(EsvmAacFixture::Packets[0], 6000, true, true, 1001, 2001, 14);
	discontinuity[16 + 0x19] = 1;
	Check(yielding.Consume(discontinuity).result == Result::Ok, "first audio AU flush restarts ended codec");
	PumpToIdle(yielding);
	Check(yielding.PopAudio(&audio) && audio.pts_sequence == 14 && audio.pts == 7020 &&
	          audio.generation > generation && audio.samples == 878, "audio AU flush keeps trim/new sequence/generation");
	Check(yielding.Reset() == Result::Ok && SameStatus(video_status, video.Status()),
	      "audio lifecycle never changes independent video decoder/queue");
	EsvmDecodedFrame frame;
	Check(video.PopFrame(&frame) && frame.pts == 123, "independent RGBA output still owned/pending");
	Pixels(frame, 93);
}
} // namespace

void Ac3DecodeAndBounds() {
	for (bool enhanced: {false, true}) {
		Bytes payload(8); Put(payload, 0, 16, 8);
		const auto config = Record(enhanced ? 6 : 5, payload);
		const auto& stereo = enhanced ? EsvmAc3Fixture::eac3Stereo : EsvmAc3Fixture::ac3Stereo;
		for (bool surround: {false, true}) {
			const auto& packets = surround ? (enhanced ? EsvmAc3Fixture::eac3Surround : EsvmAc3Fixture::ac3Surround) : stereo;
			EsvmDecoderLimits limits; limits.max_queued_frames = 1;
			EsvmPacketDecoder decoder(Schema, limits);
			const auto configured = decoder.Consume(config).result;
			if (configured != Result::Ok) std::fprintf(stderr, "AC3 configure enhanced=%d result=%d\n", enhanced, int(configured));
			Check(configured == Result::Ok && decoder.Status().configured_sample_rate == 0,
			      "AC3 wire does not invent a sample rate");
			Bytes batch;
			for (size_t i = 0; i < packets.size(); ++i)
				Append(batch, AudioAu(packets[i], 100000 + i * 32000, i == 2, i == 2, 1000, 2000));
			Check(decoder.Consume(batch).result == Result::Ok, "complete AC3 syncframes admitted");
			std::fill(batch.begin(), batch.end(), 0xcc);
			for (size_t i = 0; i < packets.size(); ++i) {
				const auto result = decoder.Pump();
				Check(result == Result::Ok || result == Result::Backpressure, "bounded real AC3 decode");
				EsvmDecodedAudio frame;
				Check(decoder.PopAudio(&frame) && frame.original_samples == 1536 && frame.sample_rate == 48000 &&
				          frame.channels == 2 && frame.channel_mask == 3 && frame.has_attached_data &&
				          frame.attached_data == 100017 + i * 32000 && frame.pts_sequence == 13,
				      "AC3 actual PCM format and per-packet metadata");
				Check(frame.samples == (i == 2 ? 1392u : 1536u) && frame.pts == int64_t(100000 + i * 32000 + (i == 2 ? 1000 : 0)),
				      "AC3 sample-rate-derived exclusion and PTS");
				if (i == 1) {
					if (surround) {
						// Independent FFmpeg CLI reference: 0.071096 in both
						// channels after the signaled, normalized center downmix.
						Check(std::abs(ToneAmplitude(frame, 0, 660) - 0.071096) < 0.01 &&
						      std::abs(ToneAmplitude(frame, 1, 660) - 0.071096) < 0.01,
						      "5.1 center tone really downmixed to both channels");
					}
					else Check(ToneAmplitude(frame, 0, 440) > 0.18 && ToneAmplitude(frame, 1, 880) > 0.18 &&
					           ToneAmplitude(frame, 0, 880) < 0.04 && ToneAmplitude(frame, 1, 440) < 0.04,
					           "AC3 stereo tones retain channel separation");
				}
			}
			Check(decoder.Pump() == Result::Ok && decoder.Status().state == State::Ended, "AC3 EOS drained");
			Check(decoder.Flush(13) == Result::Ok, "AC3 restart after EOS");
			Bytes bad = config; Put(bad, 16, 8, 8); Reject(decoder, bad, Result::InvalidWire);
			Reject(decoder, AudioAu(stereo[0].first(767), 1), Result::InvalidWire);
			bad.assign(stereo[0].begin(), stereo[0].end()); bad.insert(bad.end(), stereo[0].begin(), stereo[0].end());
			Reject(decoder, AudioAu(bad, 1), Result::InvalidWire);
			bad = AudioAu(stereo[0], 1); bad[16 + 0x28] = 1; Reject(decoder, bad, Result::ProtectedContent);
			const auto& wrong = enhanced ? EsvmAc3Fixture::ac3Stereo : EsvmAc3Fixture::eac3Stereo;
			Reject(decoder, AudioAu(wrong[0], 1), Result::UnsupportedAccessUnit);
			Check(decoder.Consume(AudioAu(stereo[1], 5000, true)).result == Result::Ok && decoder.Pump() == Result::Ok,
			      "AC3 decode still works after rejected packets and flush");
		}
	}
}

void NalVideoDecode() {
	using namespace EsvmNalFixture;
	for (int variant: {0, 1, 2, 3}) {
		const bool hevc = variant != 0;
		for (bool byte_stream: {false, true}) {
			Bytes payload(16); Put(payload, 0, hevc ? 0x48 : 0x38, 8); Put(payload, 8, 4, 8);
			auto blob = [&](std::span<const uint8_t> data) {
				const size_t offset = payload.size(); payload.resize(offset + 8); Put(payload, offset, data.size(), 8);
				payload.insert(payload.end(), data.begin(), data.end());
			};
			if (variant == 3) { blob(HevcPqVps); blob(HevcPqSps); blob(HevcPqPps); }
			else if (variant == 2) { blob(Hevc10Vps); blob(Hevc10Sps); blob(Hevc10Pps); }
			else if (hevc) { blob(HevcVps); blob(HevcSps); blob(HevcPps); }
			else { blob(AvcSps); blob(AvcPps); }
			const uint8_t flag = byte_stream; blob({&flag, 1});
			const auto config = Record(hevc ? 2 : 1, payload);
			const std::span<const uint8_t> idr = variant == 3 ? std::span<const uint8_t>(HevcPqIdr)
			    : variant == 2 ? std::span<const uint8_t>(Hevc10Idr) : hevc ? std::span<const uint8_t>(HevcIdr) : std::span<const uint8_t>(AvcIdr);
			Bytes data(4); data[3] = byte_stream ? 1 : static_cast<uint8_t>(idr.size());
			data.insert(data.end(), idr.begin(), idr.end());
			EsvmPacketDecoder decoder(Schema);
			Bytes batch = config; Append(batch, Au(data, 98765, 7, true, false, 0x12345678));
			Check(decoder.Consume(batch).result == Result::Ok, "native NAL config and AU accepted atomically");
			std::fill(batch.begin(), batch.end(), 0xcc);
			if (variant == 3) {
				Check(decoder.Pump() == Result::UnsupportedConfig, "PQ is not silently interpreted as SDR");
				continue;
			}
			PumpToIdle(decoder);
			EsvmDecodedFrame frame;
			Check(decoder.Status().state == State::Ended && decoder.PopFrame(&frame) && frame.width == 64 && frame.height == 48 &&
			          frame.pts == 98765 && frame.has_pts && frame.pts_sequence == 7,
			      "actual AVC/HEVC image and reordered decoder timestamp");
			if (variant == 2)
				Check(frame.matrix_coefficients == 1 && frame.transfer_characteristics == 1 && frame.colour_primaries == 1,
				      "HEVC decoded BT.709 VUI reaches frame rather than unspecified wire defaults");
			if (variant == 1)
				Check(frame.transfer_characteristics == 2 && frame.colour_primaries == 2,
				      "HEVC unspecified VUI remains unknown, not relabelled SDR");
			for (size_t i = 0; i < frame.bytes; i += 4)
				Check(frame.rgba[i] > 230 && frame.rgba[i + 1] < 20 && frame.rgba[i + 2] < 20 && frame.rgba[i + 3] == 255,
				      "decoded red pixels, not fabricated video readiness");
			Check(decoder.Flush(7) == Result::Ok, "NAL decoder flush");
			Bytes bad = config; bad.pop_back(); Put(bad, 8, bad.size() - 16, 8);
			Reject(decoder, bad, Result::InvalidWire);
			bad = Au(data, 100, 7); bad[16 + 0x28] = 1; Reject(decoder, bad, Result::ProtectedContent);
			if (!byte_stream) {
				Bytes short_nal = data; short_nal.pop_back();
				Reject(decoder, Au(short_nal, 100, 7), Result::InvalidWire);
			}
			Check(decoder.Consume(Au(data, 100, 7, true)).result == Result::Ok, "NAL decoder reusable after flush/rejection");
			PumpToIdle(decoder);
			Check(decoder.PopFrame(&frame) && frame.pts == 100, "fresh post-flush frame survives");
		}
	}
}

Bytes TransitionConfig(bool changed, bool byte_stream = true) {
	using namespace EsvmNalTransition;
	Bytes payload(16); Put(payload, 0, 0x38, 8); Put(payload, 8, 4, 8);
	auto blob = [&](std::span<const uint8_t> data) {
		const auto offset = payload.size(); payload.resize(offset + 8); Put(payload, offset, data.size(), 8);
		payload.insert(payload.end(), data.begin(), data.end());
	};
	blob(changed ? std::span<const uint8_t>(NewSps) : std::span<const uint8_t>(OldSps));
	blob(changed ? std::span<const uint8_t>(NewPps) : std::span<const uint8_t>(OldPps));
	const uint8_t flag = byte_stream; blob({&flag, 1});
	return Record(1, payload);
}

void OrderedNalConfiguration() {
	using namespace EsvmNalTransition;
	// Exercise queued input, already-converted output, and a full queue/held frame.
	for (int before: {0, 1, 2, 3}) {
		EsvmDecoderLimits limits;
		if (before == 2) limits.max_queued_frames = 1;
		EsvmPacketDecoder decoder(Schema, limits);
		auto batch = TransitionConfig(false);
		for (size_t i = 0; i < 6; ++i) Append(batch, Au(OldPackets[i], 1000000 + 40000 * PresentationOrder[i], 7, before == 3 && i == 5));
		Check(decoder.Consume(batch).result == Result::Ok, "old B-frame sequence owned");
		if (before == 1 || before == 2) {
			const auto r = decoder.Pump();
			Check(r == Result::Ok || r == Result::Backpressure, "old output pre-pumped");
			Check(decoder.Status().queued_frames > 0 && decoder.Status().queued_frames < 6,
			      "old codec really retains delayed frames before boundary");
		}
		const auto generation = decoder.Status().generation;
		batch = TransitionConfig(true);
		for (size_t i = 0; i < 6; ++i) Append(batch, Au(NewPackets[i], 1240000 + 40000 * PresentationOrder[i], 8, i == 5));
		const auto accepted = decoder.Consume(batch);
		Check(accepted.result == Result::Ok && accepted.consumed_bytes == batch.size() && accepted.configuration_queued,
		      "changed SPS and AUs owned behind old output, not rejected or discarded");
		std::fill(batch.begin(), batch.end(), 0xcc);
		Check(decoder.Status().generation == generation && decoder.Status().configuration_pending,
		      "configuration preserves presentation generation");
		Check(!ReadEsvmRefillCapacity(decoder.Status(), limits).can_refill, "pending boundary withholds refill");
		Reject(decoder, TransitionConfig(false), Result::Backpressure);
		size_t count = 0;
		for (size_t step = 0; step < 100 && decoder.Status().state != State::Ended; ++step) {
			const auto r = decoder.Pump();
			if (r != Result::Ok && r != Result::Yield && r != Result::Backpressure)
				std::fprintf(stderr, "transition before=%d count=%zu result=%d ffmpeg=%d\n", before, count, int(r), decoder.Status().ffmpeg_error);
			Check(r == Result::Ok || r == Result::Yield || r == Result::Backpressure, "boundary drain pumps without protocol failure");
			EsvmDecodedFrame frame;
			while (decoder.PopFrame(&frame)) {
				Check(count < 12 && frame.pts == 1000000 + int64_t(count) * 40000 && frame.has_pts &&
				      frame.pts_sequence == (count < 6 ? 7 : 8) && frame.generation == generation &&
				      frame.width == (count < 6 ? 64 : 96) && frame.height == (count < 6 ? 48 : 64),
				      "all old reordered and new frames retain exact order, PTS, sequence and dimensions");
				Check(frame.rgba[0] > 230 && frame.rgba[1] < 20 && frame.rgba[2] < 20, "transition decodes real pixels");
				++count;
			}
		}
		Check(count == 12 && !decoder.Status().configuration_pending, "no delayed or queued output lost at configuration");
		// Repeated transition after EOF: no second null drain of an ended codec.
		batch = TransitionConfig(false); Append(batch, Au(OldPackets[0], 2000000, 9, true));
		Check(decoder.Consume(batch).result == Result::Ok, "configuration after EOS owned");
		PumpToIdle(decoder);
		EsvmDecodedFrame frame;
		Check(decoder.PopFrame(&frame) && frame.pts == 2000000 && frame.width == 64 && frame.generation == generation,
		      "repeated boundary after EOS retains generation");
	}
	for (int flush: {0, 1, 2, 3}) {
		EsvmPacketDecoder decoder(Schema);
		auto batch = TransitionConfig(false); Append(batch, Au(OldPackets[0], 100, 7));
		Check(decoder.Consume(batch).result == Result::Ok, "flush setup owns old delayed input");
		PumpToIdle(decoder);
		Check(ReadEsvmRefillCapacity(decoder.Status(), {}).can_refill, "capacity available before configuration-only boundary");
		Check(decoder.Consume(TransitionConfig(true, false)).result == Result::Ok, "config-only boundary accepted");
		Check(!decoder.Status().eos_accepted && !ReadEsvmRefillCapacity(decoder.Status(), {}).can_refill,
		      "pending configuration alone suppresses refill despite available packet/output space");
		Bytes data(NewPackets[0].begin(), NewPackets[0].end());
		const auto size = data.size() - 4;
		for (size_t i = 0; i < 4; ++i) data[i] = static_cast<uint8_t>(size >> (24 - i * 8));
		Check(decoder.Consume(Au(data, 200, 8)).result == Result::Ok, "subsequent AU parses using owned future framing");
		const auto generation = decoder.Status().generation;
		if (flush == 3) {
			Check(decoder.Reset() == Result::Ok && !decoder.Status().configuration_pending &&
			      decoder.Status().state == State::Unconfigured, "reset releases pending config and AUs");
			continue;
		}
		if (flush == 0) Check(decoder.Flush(8) == Result::Ok, "explicit flush accepts owned pending configuration");
		if (flush == 1) Check(decoder.FlushCurrent() == Result::Ok, "source flush accepts pending configuration");
		if (flush == 2) Check(decoder.Consume(Au({}, 0, 8, false, true)).result == Result::Ok, "wire flush follows pending configuration");
		Check(!decoder.Status().configuration_pending && decoder.Status().generation == generation + 1,
		      "only flush advances generation and clears pending boundary");
		Check(decoder.Consume(Au(data, 300, 8, true)).result == Result::Ok, "latest owned configuration survives flush");
		PumpToIdle(decoder); EsvmDecodedFrame frame;
		Check(decoder.PopFrame(&frame) && frame.width == 96 && frame.pts == 300 && !decoder.PopFrame(&frame),
		      "flush discards both generations of queued input, retaining new codec");
	}
	EsvmDecoderLimits limits; limits.max_queued_packets = 6;
	EsvmPacketDecoder bounded(Schema, limits);
	auto batch = TransitionConfig(false);
	for (auto packet: OldPackets) Append(batch, Au(packet, 100, 7));
	Check(bounded.Consume(batch).result == Result::Ok, "bounded old packet queue filled");
	batch = TransitionConfig(true); Append(batch, Au(NewPackets[0], 200, 8));
	Reject(bounded, batch, Result::Backpressure);
	Check(!bounded.Status().configuration_pending, "over-capacity new config is transactional");
	// A real old-codec decode failure releases both sides of the owned boundary.
	EsvmPacketDecoder broken(Schema);
	batch = TransitionConfig(false); const Bytes corrupt {0, 0, 0, 1, 0x65, 0xff};
	Append(batch, Au(corrupt, 100, 7));
	Check(broken.Consume(batch).result == Result::Ok && broken.Consume(TransitionConfig(true)).result == Result::Ok,
	      "malformed encoded input can precede an owned configuration");
	Check(broken.Pump() == Result::DecodeFailed && broken.Status().state == State::Failed &&
	      !broken.Status().configuration_pending && broken.Status().queued_packets == 0,
	      "decode failure clears pending resources and remains visible");
}

void VideoPrefetchBytes() {
	EsvmDecoderLimits limits; limits.max_queued_packet_bytes = 18 * 1024 * 1024;
	EsvmPacketDecoder decoder(Schema, limits);
	// Admission is opaque encoded ownership; these padded packets are never
	// decoded. Exercise the real aggregate byte bound without fabricated status.
	Check(decoder.Consume(Config()).result == Result::Ok, "18 MiB bounded video prefetch configuration");
	Bytes payload(limits.max_packet_bytes); std::copy(EsvmVp9Fixture::Black.begin(), EsvmVp9Fixture::Black.end(), payload.begin());
	const auto packet = Au(payload, 100, 7);
	Check(decoder.Consume(packet).result == Result::Ok && ReadEsvmRefillCapacity(decoder.Status(), limits).can_refill,
	      "one full 9 MiB packet leaves exactly one bounded admission window");
	Check(decoder.Consume(packet).result == Result::Ok && decoder.Status().queued_packet_bytes == limits.max_queued_packet_bytes &&
	      !ReadEsvmRefillCapacity(decoder.Status(), limits).can_refill, "second admission exhausts actual byte capacity");
	Reject(decoder, Au(EsvmVp9Fixture::Black, 100, 7), Result::Backpressure);
	Check(decoder.Flush(7) == Result::Ok && ReadEsvmRefillCapacity(decoder.Status(), limits).can_refill,
	      "Flush releases both bounded encoded windows");
	++limits.max_queued_packet_bytes; EsvmPacketDecoder oversized(Schema, limits);
	Check(oversized.Consume(Config()).result == Result::InvalidLimits, "prefetch bound cannot grow past 18 MiB");
}

int main() {
	VideoPrefetchBytes();
	OrderedNalConfiguration();
	RealPacketsAndOwnership();
	MalformedAndUnsupported();
	LimitsBackpressureAndFlush();
	BackpressureResumesWithoutLoss();
	DecodeFailureCropSchemaAndYield();
	const auto audio_reference = DecodeAudioReference();
	AudioTrimAndOwnership(audio_reference);
	AudioRejectionBoundsAndFailure();
	AudioBackpressureFlushAndDiscrimination();
	AudioInputBoundsYieldAndIndependentVideo();
	Ac3DecodeAndBounds();
	NalVideoDecode();
	std::puts("MediaFrameworkEsvmDecoderTests: passed");
	return 0;
}
