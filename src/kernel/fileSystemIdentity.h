#ifndef KYTY_KERNEL_FILESYSTEMIDENTITY_H_
#define KYTY_KERNEL_FILESYSTEMIDENTITY_H_

#include "common/fileIdentity.h"

#include <limits>
#include <map>
#include <optional>

namespace Libs::LibKernel::FileSystem::Detail {

struct GuestFileIdentity {
	uint32_t device;
	uint32_t inode;
	auto operator<=>(const GuestFileIdentity&) const = default;
};

// Caller serializes access. Retain entries for the process lifetime so reopening
// a file cannot change its guest identity. Full host keys prevent truncation and
// hash collisions; zero is reserved, and exhaustion is reported, never wrapped.
class FileIdentityMap {
public:
	// Anonymous objects share the monotonic inode domain with host files;
	// device zero is reserved for them, never a fabricated host identity key.
	std::optional<GuestFileIdentity> AllocateAnonymous() {
		if (m_next_inode > std::numeric_limits<uint32_t>::max()) { return std::nullopt; }
		return GuestFileIdentity {0, static_cast<uint32_t>(m_next_inode++)};
	}
	std::optional<GuestFileIdentity> Get(const Common::FileIdentity& host) {
		if (const auto it = m_files.find(host); it != m_files.end()) { return it->second; }
		if (m_next_inode > std::numeric_limits<uint32_t>::max()) { return std::nullopt; }
		auto device = m_devices.find(host.device);
		if (device == m_devices.end()) {
			if (m_next_device > std::numeric_limits<uint32_t>::max()) { return std::nullopt; }
			device = m_devices.emplace(host.device, static_cast<uint32_t>(m_next_device++)).first;
		}
		const GuestFileIdentity guest {device->second, static_cast<uint32_t>(m_next_inode++)};
		return m_files.emplace(host, guest).first->second;
	}

private:
	std::map<uint64_t, uint32_t> m_devices;
	std::map<Common::FileIdentity, GuestFileIdentity> m_files;
	uint64_t m_next_device = 1;
	uint64_t m_next_inode = 1;
};

} // namespace Libs::LibKernel::FileSystem::Detail
#endif
