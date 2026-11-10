#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace Libs::Graphics {

// Host-owned RGBA pixels. Admission/decoding and the playback clock belong to
// the media service; the presenter never invokes guest code or reads guest SHM.
struct NativeVideoFrame {
	std::unique_ptr<uint8_t[]> rgba;
	size_t bytes = 0;
	uint32_t width = 0, height = 0, stride = 0;
	uint64_t generation = 0;
	int64_t pts = 0;
	uint16_t sequence = 0;
	bool has_pts = false;
};
struct NativeVideoRect { float x = 0, y = 0, width = 1, height = 1; };
struct NativeVideoPresentation {
	uint64_t generation = 0, serial = 0;
	int64_t pts = 0;
	uint16_t sequence = 0;
	bool has_pts = false;
};
class NativeVideoStream;
struct NativeVideoSnapshot {
	std::shared_ptr<NativeVideoStream> stream;
	std::shared_ptr<const NativeVideoFrame> frame;
	NativeVideoRect rect;
	uint64_t serial = 0, revision = 0;
};

class NativeVideoStream final {
public:
	NativeVideoStream(int owner, int context, int decoder, uint64_t generation);
	~NativeVideoStream();
	const int owner, context, decoder;
	// One pending frame plus the presenter's retained GPU image. Backpressure
	// leaves the caller's pixels intact. Never replaces an unpresented frame.
	bool Publish(NativeVideoFrame&& frame);
	bool SetRect(NativeVideoRect rect);
	// Requests invalidation. Return is NOT renderer-clear completion. Poll
	// ClearAcknowledged outside source/lifecycle locks if completion is needed.
	bool Flush(uint64_t generation);
	void Retire();
	bool ClearAcknowledged() const;
	bool IsRetired() const;
	NativeVideoPresentation Presented() const;
	NativeVideoSnapshot Snapshot();
	bool IsCurrent(const NativeVideoSnapshot& snapshot) const;
	// Called only after successful host presentation of this backdrop, not
	// after upload/decode or while an opaque MAIN plane hides it. This reports
	// queue-present acceptance; it is not physical-display timing feedback.
	bool Acknowledge(const NativeVideoSnapshot& snapshot);

private:
	mutable std::mutex m_mutex;
	bool m_retired = false, m_pending = false;
	uint64_t m_generation, m_serial = 0, m_revision = 1;
	uint64_t m_clear_ticket = 0;
	NativeVideoRect m_rect;
	std::shared_ptr<const NativeVideoFrame> m_frame;
	NativeVideoPresentation m_presented;
};

// Explicit single-backdrop admission until native multiple-player ordering is
// implemented. A different live stream is rejected rather than silently hidden.
bool SelectNativeVideoStream(const std::shared_ptr<NativeVideoStream>& stream);
NativeVideoSnapshot ReadNativeVideoSnapshot();
uint64_t NativeVideoRevision();
// Presenter-only commit boundary: after successful host queue presentation,
// acknowledge the revision captured BEFORE reading its video snapshot. Thus a
// flush racing an old recorded/submitted frame cannot be reported cleared.
void AcknowledgeNativeVideoRevision(uint64_t revision);

// Pure geometry: normalized top-left bounds in the 3840x2160 logical service
// viewport, aspect-fit the video within those bounds, then map to host viewport.
struct NativeVideoPixelRect { int32_t x = 0, y = 0; uint32_t width = 0, height = 0; };
NativeVideoPixelRect FitNativeVideo(NativeVideoRect rect, uint32_t video_width,
                                  uint32_t video_height, NativeVideoPixelRect viewport);
} // namespace Libs::Graphics
