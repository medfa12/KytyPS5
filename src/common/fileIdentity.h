#ifndef KYTY_COMMON_FILEIDENTITY_H_
#define KYTY_COMMON_FILEIDENTITY_H_

#include <compare>
#include <cstdint>
#include <optional>
#include <string>

namespace Common {

// Keep the complete host identity: Windows volume + 128-bit file ID, or
// POSIX st_dev + st_ino. Never truncate these to the guest stat field widths.
struct FileIdentity {
	uint64_t device = 0;
	uint64_t inode_low = 0;
	uint64_t inode_high = 0;
	// Archive members have their own identity within the backing host file.
	// nullopt denotes the host file itself; a value is the archive's entry ID.
	std::optional<std::string> member;
	auto operator<=>(const FileIdentity&) const = default;
};

} // namespace Common
#endif
