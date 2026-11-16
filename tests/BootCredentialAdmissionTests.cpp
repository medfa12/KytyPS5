// Isolated host-only test; uses retained local inputs, never executes firmware.
// g++ -std=c++20 -Isrc tests/BootCredentialAdmissionTests.cpp -lcrypto -o <local.exe>
#include "loader/bootCredentialAdmission.h"
#include <openssl/sha.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool condition, const char* message) {
	if (!condition) { throw std::runtime_error(message); }
}
std::vector<uint8_t> Read(const char* path) {
	std::ifstream file(path, std::ios::binary);
	Check(bool(file), "input unavailable");
	return {std::istreambuf_iterator<char>(file), {}};
}
bool Hash(std::span<const uint8_t> bytes, Loader::BootCredentialDigest& out) {
	return SHA256(bytes.data(), bytes.size(), out.data()) != nullptr;
}
uint64_t U64(const std::vector<uint8_t>& bytes, size_t at) {
	uint64_t value = 0;
	for (size_t i = 0; i < 8; ++i) { value |= uint64_t {bytes.at(at + i)} << (8 * i); }
	return value;
}
} // namespace

int main(int argc, char** argv) try {
	using namespace Loader;
	Check(argc == 3, "expected original alternate Core ELF and raw verified 136-byte record");
	auto image = Read(argv[1]);
	auto record = Read(argv[2]);
	Check(image.size() == 35337516 && record.size() == 136, "wrong retained input sizes");
	std::vector<std::vector<uint8_t>> storage;
	std::vector<BootMappedSegment> mapped;
	storage.reserve(14);
	for (size_t i = 0; i < 14; ++i) {
		const size_t at = 64 + i * 56;
		if ((U64(image, at) & UINT32_MAX) != 1) { continue; }
		const auto off = U64(image, at + 8), filesz = U64(image, at + 32), memsz = U64(image, at + 40);
		storage.emplace_back(memsz, 0);
		std::copy_n(image.begin() + off, filesz, storage.back().begin());
		mapped.push_back({U64(image, at + 16), storage.back()});
	}
	int owner = 1, other = 2;
	auto lifetime = std::make_shared<BootCredentialLifetime>();
	auto admit = [&] { return BootCredentialAdmission::Admit(&owner, lifetime, image, mapped, record, Hash); };
	auto receipt = admit();
	Check(bool(receipt), "exact original/mapping/record must admit");
	Check(!BootCredentialAdmission::NativeMailboxPolicyExecuted, "must not claim native policy");
	Check(receipt->AuthorizesRegistration(&owner, "SceAppDbIpcmsg"), "source bit62 authorizes exact AppDb gate");
	Check(!receipt->AuthorizesRegistration(&owner, "SceOther") &&
	      !receipt->AuthorizesRegistration(&other, "SceAppDbIpcmsg"), "scope/owner isolation");
	std::array<uint64_t, 17> auth {};
	Check(receipt->CopyRecord(&owner, &auth) && auth[1] == UINT64_C(0x40001c0000000000), "exact auth query");
	record[16] ^= 1;
	Check(!admit(), "altered non-capability record field accepted");
	record[16] ^= 1;
	image[0x4000] ^= 1;
	Check(!admit(), "modified original accepted");
	image[0x4000] ^= 1;
	storage[0][0] ^= 1;
	Check(!admit(), "patched mapped code accepted despite pristine file");
	storage[0][0] ^= 1;
	const size_t last = storage.size() - 2; // PT_LOAD7 has BSS
	storage[last].back() = 1;
	Check(!admit(), "nonzero mapped BSS accepted");
	storage[last].back() = 0;
	mapped[0].vaddr += 1;
	Check(!admit(), "different mapping address accepted");
	mapped[0].vaddr -= 1;
	auto saved = mapped.back(); mapped.pop_back();
	Check(!admit(), "missing mapped segment accepted");
	mapped.push_back(saved);
	Check(bool(admit()), "restored inputs rejected");
	const auto before = auth;
	lifetime.reset();
	Check(!receipt->CopyRecord(&owner, &auth) && auth == before &&
	      !receipt->AuthorizesRegistration(&owner, "SceAppDbIpcmsg"), "expired owner accepted or output modified");
	lifetime = std::make_shared<BootCredentialLifetime>();
	Check(!receipt->IsLiveFor(&owner), "owner address reuse revived old receipt");
	receipt = admit(); Check(bool(receipt), "new owner lease failed");
	receipt->Revoke();
	Check(!receipt->CopyRecord(&owner, &auth) && auth == before, "revoked receipt accepted");
	std::cout << "BootCredentialAdmission: source, mapping, record, policy scope, lifetime and revocation checks passed\n";
	return 0;
} catch (const std::exception& error) {
	std::cerr << error.what() << '\n';
	return 1;
}
