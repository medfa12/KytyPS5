#pragma once

#include <cstdint>
#include <cstring>
#include <span>

namespace Loader {

enum class ProcessAbi { Unknown, Legacy, Prospero };

// Shared-module ABI values never establish an executable process ABI.
inline ProcessAbi ReadProcessAbi(uint8_t abi, bool shared) {
	if (shared || abi > 2) { return ProcessAbi::Unknown; }
	return abi == 2 ? ProcessAbi::Prospero : ProcessAbi::Legacy;
}

// The executable's legacy SDK field, not the adjacent Prospero SDK field.
// Missing or malformed process metadata leaves the kernel's initial value zero.
inline uint32_t ReadCompiledSdkVersion(std::span<const uint8_t> header) {
	if (header.size() < 0x14) { return 0; }
	uint64_t size = 0;
	uint32_t magic = 0, sdk = 0;
	std::memcpy(&size, header.data(), sizeof(size));
	std::memcpy(&magic, header.data() + 8, sizeof(magic));
	if (size < 0x14 || magic != 0x4942524f) { return 0; }
	std::memcpy(&sdk, header.data() + 0x10, sizeof(sdk));
	return sdk;
}

inline uint32_t ReadProsperoCompiledSdkVersion(std::span<const uint8_t> header) {
	if (header.size() < 0x18) { return 0; }
	uint64_t size = 0;
	uint32_t magic = 0, version = 0, sdk = 0;
	std::memcpy(&size, header.data(), sizeof(size));
	std::memcpy(&magic, header.data() + 8, sizeof(magic));
	std::memcpy(&version, header.data() + 0xc, sizeof(version));
	// Native format validation requires declared size >=0x14, not >=0x18.
	// Actual supplied bytes must still cover the complete four-byte field.
	if (size < 0x14 || magic != 0x4942524f || version < 4) { return 0; }
	std::memcpy(&sdk, header.data() + 0x14, sizeof(sdk));
	return sdk;
}

} // namespace Loader
