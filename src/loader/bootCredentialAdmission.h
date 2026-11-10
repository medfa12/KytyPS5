#pragma once

#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace Loader {

// Host-owned lifetime, independent of receipt copies. Never exposed to guest code.
struct BootCredentialLifetime {};
struct BootMappedSegment {
	uint64_t vaddr = 0; // ELF-relative address, not host base address
	std::span<const uint8_t> bytes; // Complete p_memsz, before relocations/patches
};
using BootCredentialDigest = std::array<uint8_t, 32>;
using BootCredentialHasher = bool (*)(std::span<const uint8_t>, BootCredentialDigest&);

// Emulator admission policy: exact audited alternate Core/UI bindings.
// NOT a native mailbox0x16 acceptance token. No JSON assertions are consumed.
class BootCredentialAdmission final {
public:
	static std::shared_ptr<BootCredentialAdmission> Admit(
	    const void* owner, const std::shared_ptr<BootCredentialLifetime>& lifetime,
	    std::span<const uint8_t> original, std::span<const BootMappedSegment> mapped,
	    std::span<const uint8_t> auth, BootCredentialHasher hash) {
		if (!owner || !lifetime || !hash || auth.size() != 136) { return {}; }
		BootCredentialDigest digest {};
		if (!hash(original, digest)) { return {}; }
		const bool core = original.size() == 35337516 && digest == Hex("f2e569cebe324129a8be6a40a429e6f6652021f261ded3c02a5469cc81f5a434");
		bool ui = original.size() == 21878244 && digest == Hex("f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060");
		if (original.size() == 21878244 && digest == Hex("b4ce830af247ae6611af81dece472710fa728a5c6855280d609168b62d556f4c")) {
			// Reviewed PSM dimensions transform, RVA122fd (file162fd), only
			// 3840x2160 -> 1920x1080. Verify both complete image bindings.
			std::vector<uint8_t> restored(original.begin(), original.end());
			constexpr std::array<uint8_t, 8> before {0, 15, 0, 0, 112, 8, 0, 0};
			constexpr std::array<uint8_t, 8> after {128, 7, 0, 0, 56, 4, 0, 0};
			if (!std::equal(after.begin(), after.end(), restored.begin() + 0x162fd)) { return {}; }
			std::copy(before.begin(), before.end(), restored.begin() + 0x162fd);
			ui = hash(restored, digest) && digest == Hex("f3068cd9769711f9962367026f0bbba28ac2fac24e34fad1f40714979c89e060");
		}
		if ((!core && !ui) || !hash(auth, digest) || digest != Hex(core ?
		    "5d9c1daacc67e91fbe1ddfdd0eac992270edc0e55986564cf579da89a95cc57a" :
		    "b35057cfe2d2d844f287325c438b39950ed6555f686592af546eb12fb950e0aa")) { return {}; }
		// Digest pins authenticate the whole PHDR table. Independently compare the
		// actual mapped PT_LOAD bytes and zero-initialized BSS, not a path reopened
		// after loading. Reject missing/extra/reordered/modified mappings.
		size_t next = 0;
		for (size_t i = 0; i < 14; ++i) {
			const size_t at = 64 + i * 56;
			if ((U64(original.subspan(at, 8)) & UINT32_MAX) != 1) { continue; }
			const auto off = U64(original.subspan(at + 8, 8));
			const auto va = U64(original.subspan(at + 16, 8));
			const auto filesz = U64(original.subspan(at + 32, 8));
			const auto memsz = U64(original.subspan(at + 40, 8));
			if (next == mapped.size() || off > original.size() || filesz > original.size() - off || filesz > memsz) { return {}; }
			const auto& segment = mapped[next++];
			if (segment.vaddr != va || segment.bytes.size() != memsz ||
			    !std::equal(original.begin() + off, original.begin() + off + filesz, segment.bytes.begin()) ||
			    !std::all_of(segment.bytes.begin() + filesz, segment.bytes.end(), [](uint8_t b) { return b == 0; })) { return {}; }
		}
		if (next != mapped.size()) { return {}; }
		auto result = std::shared_ptr<BootCredentialAdmission>(new BootCredentialAdmission(owner, lifetime));
		result->core_image = core;
		for (size_t i = 0; i < result->record.size(); ++i) { result->record[i] = U64(auth.subspan(i * 8, 8)); }
		return result;
	}
	bool IsLiveFor(const void* candidate) const {
		return candidate == owner && !lifetime.expired() && !revoked.load();
	}
	bool IsCoreImage() const { return core_image; }
	bool CopyRecord(const void* candidate, std::array<uint64_t, 17>* out) const {
		if (!out || !IsLiveFor(candidate)) { return false; }
		*out = record;
		return true;
	}
	bool AuthorizesRegistration(const void* candidate, std::string_view name) const {
		return IsLiveFor(candidate) && name == "SceAppDbIpcmsg" && ((record[1] >> 62) & 1) != 0;
	}
	void Revoke() { revoked.store(true); }
	static constexpr bool NativeMailboxPolicyExecuted = false;

private:
	BootCredentialAdmission(const void* owner, const std::shared_ptr<BootCredentialLifetime>& lifetime):
	    owner(owner), lifetime(lifetime) {}
	static uint64_t U64(std::span<const uint8_t> b) {
		uint64_t value = 0;
		for (size_t i = 0; i < 8; ++i) { value |= uint64_t {b[i]} << (8 * i); }
		return value;
	}
	static BootCredentialDigest Hex(std::string_view value) {
		BootCredentialDigest result {};
		auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
		for (size_t i = 0; i < result.size(); ++i) { result[i] = uint8_t((digit(value[2 * i]) << 4) | digit(value[2 * i + 1])); }
		return result;
	}
	const void* owner;
	std::weak_ptr<BootCredentialLifetime> lifetime;
	std::array<uint64_t, 17> record {};
	std::atomic_bool revoked {false};
	bool core_image = false;
};
} // namespace Loader
