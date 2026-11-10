#pragma once

#include "loader/staticExecutableLayout.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace Loader {

enum class StaticExecutableImageError {
	None,
	InvalidLayout,
	UnsupportedFlags,
	InvalidMappingRange,
	InputOverlapsMapping,
	AllocationFailed,
	ProtectionFailed,
	InstructionCacheFlushFailed,
};

struct StaticExecutableImageResult;

// Sole owner of the separately allocated PT_LOAD page intervals. The layout's
// enclosing owner span is metadata only: gaps in it are never owned or freed.
// Destroy/reset only after every user of these mappings has stopped.
class StaticExecutableImage {
public:
	StaticExecutableImage() = default;
	~StaticExecutableImage();
	StaticExecutableImage(const StaticExecutableImage&) = delete;
	StaticExecutableImage& operator=(const StaticExecutableImage&) = delete;
	StaticExecutableImage(StaticExecutableImage&& other) noexcept;
	StaticExecutableImage& operator=(StaticExecutableImage&& other) noexcept;

	[[nodiscard]] bool IsMapped() const noexcept { return m_owned_count != 0; }
	[[nodiscard]] const StaticExecutableLayout& Layout() const noexcept { return m_layout; }
	void Reset() noexcept;

private:
	friend StaticExecutableImageResult MapStaticExecutableImage(std::span<const uint8_t> bytes);
	StaticExecutableLayout m_layout;
	std::size_t m_owned_count = 0;
};

struct StaticExecutableImageResult {
	StaticExecutableImageError error = StaticExecutableImageError::None;
	// Set only when error == InvalidLayout.
	StaticExecutableLayoutError layout_error = StaticExecutableLayoutError::None;
	StaticExecutableImage image;
};

// Maps a complete plain ELF image; never invokes its entry. This is not SELF
// decoding/authentication, process or TLS setup, or runtime execution admission.
// Ignored program headers and ABI requirements retain the planner's limited
// semantics. Layout().tls describes an image template, not a TCB or TLS instance.
// Known ELF flags are applied through ProtectGuestMemory (subject to host OS
// protection capabilities). Availability/collisions are resolved transactionally
// during allocation. Failure releases every mapping created by this call; source
// bytes are unchanged and must remain valid and stable for the duration of the call.
[[nodiscard]] StaticExecutableImageResult MapStaticExecutableImage(std::span<const uint8_t> bytes);

} // namespace Loader
