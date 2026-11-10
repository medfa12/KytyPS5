#include "common/abi.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace Libs {

// ps5-vsh: libSceRnpsBundle (RNPS bundle / CRL handling) HLE stubs.
// Real implementations come later (libressl X509 verify + bundle decrypt
// need real keys/certs); all init/load entry points report success so the
// caller proceeds instead of aborting. NIDs below are real export NIDs
// extracted from libSceRnpsBundle.sprx (13.60) dynamic symbol table
// (export lib id H = libSceRnpsBundle v1, export module id A, v1.1;
// "libSce" prefix is stripped by SymbolDatabase::GenerateName, so
// "RnpsBundle" matches).

namespace RnpsBundle {

LIB_VERSION("RnpsBundle", 1, "RnpsBundle", 1, 1);

namespace Rnps {

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsBundleInit(uint64_t a1, uint64_t a2) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 "\n", a1, a2);
	(void)a1;
	(void)a2;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsBundleInitEx(uint64_t a1, uint64_t a2) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 "\n", a1, a2);
	(void)a1;
	(void)a2;
	return 0;
}

// Default CRL path used by the real implementation: /mnt/rnps/crl.dat
// ("crl file not found, skipping crl load." / "no crl file specified,
// skipping crl load." are non-fatal there, so success is correct here).
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsBundleLoadCrlList(uint64_t a1, uint64_t a2) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " (default /mnt/rnps/crl.dat)\n",
	     a1, a2);
	(void)a1;
	(void)a2;
	return 0;
}

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsBundleLoadCrlData(uint64_t a1, uint64_t a2) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64 " (default /mnt/rnps/crl.dat)\n",
	     a1, a2);
	(void)a1;
	(void)a2;
	return 0;
}

// Fail closed: decrypting without keys must not pretend success, so this
// returns a NOT-FOUND placeholder error with a clear log instead of 0.
constexpr int RNPS_BUNDLE_ERROR_NOT_FOUND = static_cast<int>(0x80940001u);

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsBundleVerifyAndDecrypt(uint64_t a1, uint64_t a2) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " -> stub, no keys, failing closed with 0x80940001\n",
	     a1, a2);
	(void)a1;
	(void)a2;
	return RNPS_BUNDLE_ERROR_NOT_FOUND;
}

// ps5-vsh: QmgtzAvePXM teardown implementation; returns success.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsBundleTerm() {
	PRINT_NAME();

	return 0;
}

// ---------------------------------------------------------------------------
// ps5-vsh: recorded-plaintext replay for the bundle decryption service.
//
// The console's SM unwraps the bundle key inside the kernel driver
// ("/dev/rnps" ioctl 0xC0105203); the module itself only *verifies* (LibreSSL
// X509/CRL, no AES import), so a real decryption is impossible in userland.
// See docs/rnps-research/decrypt-contract.md.
//
// This entry point therefore stands in for the SM with a lookup table of
// genuinely obtained plaintexts, keyed on the container's own header fields and
// the first ciphertext bytes. It never invents output: a miss returns
// 0x80940001 and the guest reports the decryption failure. It is disabled
// unless KYTY_RNPS_REPLAY is set (any value other than "0"), so the default
// build keeps the faithful fail-closed behaviour.
//
// ABI (libSceRnpsBundle.sprx 0x14D0), verified against the 0x20030 wrapper:
//   int32 f(void* data, uint32_t size, uint32_t* out_payload_offset,
//           uint32_t* out_plain_size, const Opts* opts, void* crl_ctx)
// `data` holds the whole RNPSHEDR container; the plaintext is written IN PLACE
// at data + payloadOffset. The wrapper returns data + payloadOffset and the
// builder takes its length from *out_plain_size.
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t kRnpsPayloadOffsetField = 0x1c; // u32, = 0xb20 for all 13.60
constexpr uint32_t kRnpsStoredSizeField    = 0x20; // u32
constexpr uint32_t kRnpsPlainSizeField     = 0x24; // u32
constexpr uint32_t kRnpsHeaderBytes        = 0x40; // fields we read live below this

bool RnpsReplayEnabled() {
	static const bool enabled = [] {
		const char* v = std::getenv("KYTY_RNPS_REPLAY");
		return v != nullptr && *v != '\0' && std::strcmp(v, "0") != 0;
	}();
	return enabled;
}

std::string RnpsReplayDir() {
	if (const char* env = std::getenv("KYTY_RNPS_REPLAY_DIR"); env != nullptr && *env != '\0') {
		return env;
	}
	std::error_code ec;
	for (const char* candidate : {"_TempData/rnps_replay", "../_TempData/rnps_replay"}) {
		if (std::filesystem::is_directory(candidate, ec)) {
			return candidate;
		}
	}
	return "_TempData/rnps_replay";
}

uint32_t RnpsU32(const void* base, uint32_t offset) {
	uint32_t v = 0;
	std::memcpy(&v, static_cast<const uint8_t*>(base) + offset, sizeof(v));
	return v;
}

} // namespace

static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsBundleDecrypt(
    void* data, uint32_t size, uint32_t* out_payload_offset, uint32_t* out_plain_size, const void* opts,
    void* crl_ctx) {
	PRINT_NAME();
	Log::Flush();
	(void)opts;
	(void)crl_ctx;

	constexpr int kError = static_cast<int>(0x80940001u); // not found / no key

	if (data == nullptr || out_payload_offset == nullptr || out_plain_size == nullptr || size < kRnpsHeaderBytes ||
	    std::memcmp(data, "RNPSHEDR", 8) != 0) {
		LOGF("\t -> not an RNPSHEDR container (size=%u), failing closed with 0x80940001\n", size);
		return kError;
	}

	const uint32_t payload_offset = RnpsU32(data, kRnpsPayloadOffsetField);
	const uint32_t stored_size    = RnpsU32(data, kRnpsStoredSizeField);
	const uint32_t plain_size     = RnpsU32(data, kRnpsPlainSizeField);

	if (payload_offset < kRnpsHeaderBytes || payload_offset > size || plain_size > size - payload_offset) {
		LOGF("\t -> bad header (payload=%u stored=%u plain=%u size=%u), failing closed\n", payload_offset,
		     stored_size, plain_size, size);
		return kError;
	}

	if (!RnpsReplayEnabled()) {
		LOGF("\t -> container payload=%u stored=%u plain=%u: no key, replay disabled "
		     "(set KYTY_RNPS_REPLAY=1), failing closed with 0x80940001\n",
		     payload_offset, stored_size, plain_size);
		return kError;
	}

	uint64_t head = 0;
	std::memcpy(&head, static_cast<const uint8_t*>(data) + payload_offset,
	            size - payload_offset < sizeof(head) ? size - payload_offset : sizeof(head));

	char key[64] {};
	std::snprintf(key, sizeof(key), "%08x-%08x-%016" PRIx64, stored_size, plain_size, head);

	const std::string path = RnpsReplayDir() + "/" + key + ".bin";
	FILE*             f    = std::fopen(path.c_str(), "rb");
	if (f == nullptr) {
		LOGF("\t -> no recorded plaintext for %s, failing closed with 0x80940001\n", key);
		return kError;
	}
	std::vector<uint8_t> replay(plain_size);
	const size_t         got = std::fread(replay.data(), 1, replay.size(), f);
	std::fclose(f);
	if (got != replay.size()) {
		LOGF("\t -> recorded plaintext %s is %zu/%u bytes, failing closed\n", key, got, plain_size);
		return kError;
	}

	// ps5-vsh: REPLAY, not decryption. The bytes come from a genuinely obtained
	// plaintext whose size matches this container's plainSize field.
	std::memcpy(static_cast<uint8_t*>(data) + payload_offset, replay.data(), replay.size());
	*out_payload_offset = payload_offset;
	*out_plain_size     = plain_size;
	LOGF("\t -> REPLAYED recorded plaintext %s (%u bytes at offset %u); this is not a decryption\n", key,
	     plain_size, payload_offset);
	return 0;
}

// ps5-vsh: WgPr--ikm3U VerifyChain implementation. Same fail-closed
// posture as VerifyAndDecrypt: no keys, no pretend success.
static int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI SceRnpsBundleVerifyChain(uint64_t a1, uint64_t a2) {
	PRINT_NAME();
	LOGF("\t a1 = 0x%016" PRIx64 " a2 = 0x%016" PRIx64
	     " -> stub, no keys, failing closed with 0x80940001\n",
	     a1, a2);
	(void)a1;
	(void)a2;
	return RNPS_BUNDLE_ERROR_NOT_FOUND;
}

} // namespace Rnps

LIB_DEFINE(InitRnpsBundle_1) {
	LIB_FUNC("K2u0v4WEueU", Rnps::SceRnpsBundleInit);
	LIB_FUNC("hwS49XQ5OuA", Rnps::SceRnpsBundleInitEx);
	// Export labels below retain the existing mapping; their naming provenance
	// remains a release-review item independently of the fail-closed behavior.
	LIB_FUNC("UP2Nl8PPwFE", Rnps::SceRnpsBundleLoadCrlList);
	LIB_FUNC("UFNv+Aet+lE", Rnps::SceRnpsBundleLoadCrlData);
	LIB_FUNC("eqd58iI3rv8", Rnps::SceRnpsBundleVerifyAndDecrypt);
	LIB_FUNC("QmgtzAvePXM", Rnps::SceRnpsBundleTerm);
	LIB_FUNC("WgPr--ikm3U", Rnps::SceRnpsBundleVerifyChain);
	// ps5-vsh: bundle decrypt entry used by libNativeExtensions 0x20030
	// (PLT 0x5f7b0). Recorded-plaintext replay, see the comment above.
	LIB_FUNC("t8toeP54Mx8", Rnps::SceRnpsBundleDecrypt);
}

} // namespace RnpsBundle

} // namespace Libs

// Orchestrator registration snippet for src/libs/libs.cpp (add exactly this):
//
//   namespace RnpsBundle {
//   LIB_DEFINE(InitRnpsBundle_1);
//   } // namespace RnpsBundle
//
// ... and inside InitAll(Loader::SymbolDatabase* s), alongside the other
// ps5-vsh entries:
//
//   RnpsBundle::InitRnpsBundle_1(s);
