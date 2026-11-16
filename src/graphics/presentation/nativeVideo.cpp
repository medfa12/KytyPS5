#include "graphics/presentation/nativeVideo.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Libs::Graphics {
namespace {
std::mutex selection_mutex;
std::weak_ptr<NativeVideoStream> selected;
std::atomic<uint64_t> revision {1};
std::atomic<uint64_t> presented_revision {0};
uint64_t Changed() { return revision.fetch_add(1, std::memory_order_acq_rel) + 1; }
bool ValidRect(NativeVideoRect r) {
	return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
	       std::isfinite(r.height) && r.x >= 0 && r.y >= 0 && r.width >= 0 && r.height >= 0 &&
	       double(r.x) + r.width <= 1.0 && double(r.y) + r.height <= 1.0;
}
}

NativeVideoStream::NativeVideoStream(int owner, int context, int decoder, uint64_t generation)
    : owner(owner), context(context), decoder(decoder), m_generation(generation) {}
NativeVideoStream::~NativeVideoStream() { Changed(); }

bool NativeVideoStream::Publish(NativeVideoFrame&& frame) {
	if (!frame.rgba || !frame.width || !frame.height || frame.width > 4096 || frame.height > 2304 ||
	    frame.stride < uint64_t(frame.width) * 4 || frame.stride % 4 != 0 ||
	    frame.bytes != uint64_t(frame.stride) * frame.height || frame.bytes > 64 * 1024 * 1024)
		return false;
	std::lock_guard lock(m_mutex);
	if (m_retired || m_pending || !m_generation || frame.generation != m_generation) return false;
	// Allocate the control block before moving the caller's only pixel owner.
	auto owned = std::make_shared<NativeVideoFrame>();
	*owned = std::move(frame);
	m_frame = std::move(owned);
	m_pending = true;
	++m_serial;
	++m_revision;
	Changed();
	return true;
}

bool NativeVideoStream::SetRect(NativeVideoRect rect) {
	if (!ValidRect(rect)) return false;
	std::lock_guard lock(m_mutex);
	if (m_retired) return false;
	if (m_rect.x == rect.x && m_rect.y == rect.y && m_rect.width == rect.width &&
	    m_rect.height == rect.height) return true;
	m_rect = rect;
	++m_revision;
	Changed();
	return true;
}

bool NativeVideoStream::Flush(uint64_t generation) {
	std::lock_guard lock(m_mutex);
	if (m_retired || generation <= m_generation) return false;
	m_generation = generation;
	m_frame.reset();
	m_pending = false;
	m_presented = {};
	++m_revision;
	m_clear_ticket = Changed();
	return true;
}

void NativeVideoStream::Retire() {
	std::lock_guard lock(m_mutex);
	if (m_retired) return;
	m_retired = true;
	m_frame.reset();
	m_pending = false;
	m_presented = {};
	++m_revision;
	m_clear_ticket = Changed();
}
bool NativeVideoStream::ClearAcknowledged() const {
	std::lock_guard lock(m_mutex);
	return !m_clear_ticket || presented_revision.load(std::memory_order_acquire) >= m_clear_ticket;
}
bool NativeVideoStream::IsRetired() const { std::lock_guard lock(m_mutex); return m_retired; }
NativeVideoPresentation NativeVideoStream::Presented() const {
	std::lock_guard lock(m_mutex);
	return m_presented;
}
NativeVideoSnapshot NativeVideoStream::Snapshot() {
	std::lock_guard lock(m_mutex);
	return {{}, m_retired ? nullptr : m_frame, m_rect, m_serial, m_revision};
}
bool NativeVideoStream::IsCurrent(const NativeVideoSnapshot& snapshot) const {
	std::lock_guard lock(m_mutex);
	return !m_retired && snapshot.stream.get() == this && snapshot.frame &&
	       snapshot.frame == m_frame && snapshot.revision == m_revision && snapshot.serial == m_serial;
}
bool NativeVideoStream::Acknowledge(const NativeVideoSnapshot& snapshot) {
	std::lock_guard lock(m_mutex);
	if (m_retired || !m_pending || snapshot.stream.get() != this || !snapshot.frame || snapshot.frame != m_frame ||
	    snapshot.revision != m_revision || snapshot.serial != m_serial) return false;
	m_presented = {m_generation, m_serial, m_frame->pts, m_frame->sequence, m_frame->has_pts};
	m_pending = false;
	return true;
}
bool SelectNativeVideoStream(const std::shared_ptr<NativeVideoStream>& stream) {
	if (!stream || stream->owner <= 0 || stream->context <= 0 || stream->decoder <= 0) return false;
	std::lock_guard lock(selection_mutex);
	if (stream->IsRetired()) return false;
	if (auto previous = selected.lock(); previous && previous != stream && !previous->IsRetired())
		return false;
	selected = stream;
	Changed();
	return true;
}
NativeVideoSnapshot ReadNativeVideoSnapshot() {
	std::shared_ptr<NativeVideoStream> stream;
	{ std::lock_guard lock(selection_mutex); stream = selected.lock(); }
	if (!stream) return {};
	auto snapshot = stream->Snapshot();
	snapshot.stream = std::move(stream);
	return snapshot;
}
uint64_t NativeVideoRevision() { return revision.load(std::memory_order_acquire); }
void AcknowledgeNativeVideoRevision(uint64_t value) {
	auto previous = presented_revision.load(std::memory_order_relaxed);
	while (previous < value && !presented_revision.compare_exchange_weak(previous, value,
	    std::memory_order_release, std::memory_order_relaxed)) {}
}

NativeVideoPixelRect FitNativeVideo(NativeVideoRect rect, uint32_t video_width,
                                  uint32_t video_height, NativeVideoPixelRect viewport) {
	if (!ValidRect(rect) || !video_width || !video_height || !viewport.width || !viewport.height ||
	    !rect.width || !rect.height || viewport.x < 0 || viewport.y < 0 ||
	    uint64_t(viewport.x) + viewport.width > INT32_MAX ||
	    uint64_t(viewport.y) + viewport.height > INT32_MAX) return {};
	const double box_w = rect.width * 3840.0, box_h = rect.height * 2160.0;
	const double scale = std::min(box_w / video_width, box_h / video_height);
	const double width = video_width * scale, height = video_height * scale;
	const double x = rect.x * 3840.0 + (box_w - width) * .5;
	const double y = rect.y * 2160.0 + (box_h - height) * .5;
	const auto left = std::llround(viewport.x + x * viewport.width / 3840.0);
	const auto top = std::llround(viewport.y + y * viewport.height / 2160.0);
	const auto right = std::llround(viewport.x + (x + width) * viewport.width / 3840.0);
	const auto bottom = std::llround(viewport.y + (y + height) * viewport.height / 2160.0);
	return {static_cast<int32_t>(left), static_cast<int32_t>(top),
	        static_cast<uint32_t>(right - left), static_cast<uint32_t>(bottom - top)};
}
} // namespace Libs::Graphics
