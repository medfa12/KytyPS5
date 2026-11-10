#include "loader/rnpsBundle.h"

#include "common/common.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace Loader {

namespace {

constexpr char     kRnpsMagic[]   = "RNPSHEDR";
constexpr uint32_t kRnpsVersion   = 2;
constexpr uint32_t kRnpsHeaderLen = 0x1D0;

constexpr uint8_t kHermesMagic[] = {0x1F, 0x19, 0x03, 0xC1, 0x03, 0xBC, 0x1F, 0xC6};
constexpr uint8_t kHbcMagicLE[]  = {0xC6, 0x1F, 0xBC, 0x03, 0xC1, 0x03, 0x19, 0x1F};
constexpr char    kHermesTag[]   = "HERMES";

constexpr uint64_t kBytecodeScanLimit = 2u * 1024u * 1024u;

template <typename T>
void ReadU32LE(const uint8_t* base, size_t offset, T* out) {
	static_assert(sizeof(T) == sizeof(uint32_t));
	std::memcpy(out, base + offset, sizeof(uint32_t));
}

bool ReadPrefix(const std::filesystem::path& file_name, std::vector<std::byte>& buf, uint64_t max_size) {
	Common::File f;

	f.Open(file_name, Common::File::Mode::Read);

	if (f.IsInvalid()) {
		LOGF("Can't open %s\n", Common::PathToString(file_name).c_str());
		return false;
	}

	const uint64_t size  = std::min(f.Size(), max_size);
	const uint32_t bytes = static_cast<uint32_t>(size);

	buf.resize(bytes);

	uint32_t bytes_read = 0;
	f.Read(buf.data(), bytes, &bytes_read);
	f.Close();

	if (bytes_read != bytes) {
		LOGF("Can't read %s\n", Common::PathToString(file_name).c_str());
		return false;
	}

	return true;
}

bool ContainsSequence(const std::vector<std::byte>& buf, const void* seq, size_t seq_len) {
	if (seq_len == 0 || buf.size() < seq_len) {
		return false;
	}
	const auto* begin = reinterpret_cast<const uint8_t*>(buf.data());
	const auto* end   = begin + buf.size();
	const auto* pat   = reinterpret_cast<const uint8_t*>(seq);
	return std::search(begin, end, pat, pat + seq_len) != end;
}

// ps5-vsh: plaintext Hermes bundles carry readable string tables; encrypted
// payloads don't. Counts ASCII runs >= 16 bytes (>= 5 to accept); random
// data trips this at ~0.08 expected runs, plaintext HBC at hundreds.
bool HasReadableStringTables(const std::vector<std::byte>& buf) {
	const auto* begin = reinterpret_cast<const uint8_t*>(buf.data());
	const auto* end   = begin + buf.size();
	size_t      run   = 0;
	int         runs  = 0;
	for (auto* p = begin; p != end; ++p) {
		if (*p >= 0x20 && *p < 0x7F) {
			if (++run == 16) {
				if (++runs >= 5) {
					return true;
				}
			}
		} else {
			run = 0;
		}
	}
	return false;
}

} // namespace

static_assert(sizeof(RnpsHeader) == 48, "RnpsHeader must match the 48-byte bundle prefix layout");

bool IsRawHbcBundle(const std::filesystem::path& file_name) {
	std::vector<std::byte> buf;

	if (!ReadPrefix(file_name, buf, 8)) {
		return false;
	}
	if (buf.size() < sizeof(kHbcMagicLE)) {
		return false;
	}

	const auto* data = reinterpret_cast<const uint8_t*>(buf.data());

	return std::memcmp(data, kHbcMagicLE, sizeof(kHbcMagicLE)) == 0;
}

bool GetHbcVersion(const std::filesystem::path& file_name, uint32_t* out) {
	if (out == nullptr) {
		return false;
	}

	std::vector<std::byte> buf;

	if (!ReadPrefix(file_name, buf, 12)) {
		return false;
	}
	if (buf.size() < sizeof(kHbcMagicLE) + sizeof(uint32_t)) {
		return false;
	}

	const auto* data = reinterpret_cast<const uint8_t*>(buf.data());

	if (std::memcmp(data, kHbcMagicLE, sizeof(kHbcMagicLE)) != 0) {
		return false;
	}

	uint32_t version = 0;
	ReadU32LE(data, sizeof(kHbcMagicLE), &version);
	*out = version;

	return true;
}

bool IsRnpsBundle(const std::filesystem::path& file_name) {
	std::vector<std::byte> buf;

	if (!ReadPrefix(file_name, buf, 12)) {
		return false;
	}
	if (buf.size() < 8) {
		return false;
	}

	const auto* data = reinterpret_cast<const uint8_t*>(buf.data());

	if (std::memcmp(data, kHbcMagicLE, sizeof(kHbcMagicLE)) == 0) {
		return true;
	}

	if (buf.size() < 12) {
		return false;
	}

	if (std::memcmp(data, kRnpsMagic, sizeof(RnpsHeader::magic)) != 0) {
		return false;
	}

	uint32_t version = 0;
	ReadU32LE(data, 8, &version);

	return version == kRnpsVersion;
}

bool ParseRnpsHeader(const std::filesystem::path& file_name, RnpsHeader* header) {
	if (header == nullptr) {
		return false;
	}

	std::vector<std::byte> buf;

	if (!ReadPrefix(file_name, buf, kRnpsHeaderLen)) {
		return false;
	}
	if (buf.size() < kRnpsHeaderLen) {
		LOGF("invalid RNPS bundle (too small): %s\n", Common::PathToString(file_name).c_str());
		return false;
	}

	const auto* data = reinterpret_cast<const uint8_t*>(buf.data());

	if (std::memcmp(data, kHbcMagicLE, sizeof(kHbcMagicLE)) == 0) {
		return false;
	}

	if (std::memcmp(data, kRnpsMagic, sizeof(header->magic)) != 0) {
		LOGF("invalid RNPS bundle (bad magic): %s\n", Common::PathToString(file_name).c_str());
		return false;
	}

	RnpsHeader parsed {};

	std::memcpy(parsed.magic, data, sizeof(parsed.magic));
	ReadU32LE(data, 8, &parsed.version);
	ReadU32LE(data, 12, &parsed.headerSize);
	ReadU32LE(data, 16, &parsed.fieldA);
	ReadU32LE(data, 20, &parsed.fieldB);
	ReadU32LE(data, 24, &parsed.variant0);
	ReadU32LE(data, 28, &parsed.variant1);
	ReadU32LE(data, 32, &parsed.size0);
	ReadU32LE(data, 36, &parsed.size1);
	ReadU32LE(data, 40, &parsed.size2);
	ReadU32LE(data, 44, &parsed.flags);

	if (parsed.version != kRnpsVersion || parsed.headerSize != kRnpsHeaderLen) {
		LOGF("invalid RNPS bundle header (ver=%" PRIu32 " size=0x%" PRIx32 "): %s\n", parsed.version,
		     parsed.headerSize, Common::PathToString(file_name).c_str());
		return false;
	}

	*header = parsed;

	return true;
}

bool HasHermesBytecode(const std::filesystem::path& file_name) {
	std::vector<std::byte> buf;

	if (!ReadPrefix(file_name, buf, kBytecodeScanLimit)) {
		return false;
	}

	const bool has_magic = ContainsSequence(buf, kHbcMagicLE, sizeof(kHbcMagicLE)) ||
	                             ContainsSequence(buf, kHermesMagic, sizeof(kHermesMagic));
	const bool has_tag   = ContainsSequence(buf, kHermesTag, sizeof(kHermesTag) - 1);

	if (has_magic && has_tag) {
		return true;
	}

	// ps5-vsh: 13.60 ships plaintext Hermes v89 bundles with the magic but
	// without the HERMES tag (docs/shellui-1360.md: 68/71 titles). The old
	// tag-only check mislabeled them "likely encrypted". Accept magic +
	// readable string tables; encrypted payloads have no long ASCII runs
	// (false-positive rate ~0 at the thresholds below).
	if (has_magic && HasReadableStringTables(buf)) {
		return true;
	}

	LOGF("no Hermes bytecode found in %s (likely encrypted bundle)\n",
	     Common::PathToString(file_name).c_str());

	return false;
}

} // namespace Loader
