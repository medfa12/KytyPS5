#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Libs::MediaFramework {

// Host-only, allocation-free protocol/crypto backend. No guest pointers,
// handles, callbacks, role storage, device credentials or provisioning state.
// These are implementation safety bounds, NOT firmware ABI limits.
inline constexpr size_t EsvmCertificateMaxBytes = 16 * 1024;
inline constexpr size_t EsvmCertificateMaxFields = 2048;
inline constexpr unsigned EsvmCertificateMaxDepth = 8;
inline constexpr uint32_t EsvmCertificateMaxRsaBits = 8192;

enum class EsvmCertificateFormat { SignedCertificate, ServiceCertificateResponse };
// Host results, NOT public MediaFramework/engine error numbers.
enum class EsvmCertificateResult {
	Ok,
	InvalidInput,
	LimitExceeded,
	InvalidWire,
	MissingCertificate,
	UnexpectedMessageType,
	ServerError, // ErrorResponse code is untrusted diagnostic information only.
	UnsupportedAlgorithm,
	WrongCertificateType,
	InvalidPublicKey,
	UnsupportedPublicKey,
	BadSignature,
	CryptoUnavailable,
	CryptoFailure,
	TrustAnchorMismatch,
};

struct EsvmCertificateVerification;
[[nodiscard]] EsvmCertificateVerification VerifyEsvmServiceCertificate(
    std::span<const uint8_t> input, EsvmCertificateFormat format) noexcept;

// The only constructor is private: parsing alone cannot produce this value.
// All returned byte views are immutable and owned by this value, not by input.
// Acceptance means a SERVICE certificate signed directly by the firmware pin,
// with a usable RSA public key. It does NOT mean device provisioned/licensed.
class EsvmVerifiedServiceCertificate final {
public:
	EsvmVerifiedServiceCertificate(const EsvmVerifiedServiceCertificate&) = default;
	EsvmVerifiedServiceCertificate& operator=(const EsvmVerifiedServiceCertificate&) = default;
	EsvmVerifiedServiceCertificate(EsvmVerifiedServiceCertificate&&) = default;
	EsvmVerifiedServiceCertificate& operator=(EsvmVerifiedServiceCertificate&&) = default;

	[[nodiscard]] std::span<const uint8_t> SignedCertificate() const noexcept { return {m_bytes.data(), m_size}; }
	[[nodiscard]] std::span<const uint8_t> SignedPayload() const noexcept { return View(m_payload); }
	[[nodiscard]] std::span<const uint8_t> PublicKeyDer() const noexcept { return View(m_key); }
	// Provider/serial are length-delimited bytes. No origin, UTF-8, NUL, serial
	// length or nonempty-provider policy is invented. Never pass to %s/strlen.
	[[nodiscard]] std::span<const uint8_t> ProviderId() const noexcept { return View(m_provider); }
	[[nodiscard]] std::span<const uint8_t> SerialNumber() const noexcept { return View(m_serial); }
	[[nodiscard]] uint32_t RsaBits() const noexcept { return m_rsa_bits; }
	[[nodiscard]] std::optional<uint32_t> CreationTimeSeconds() const noexcept { return m_creation; }
	[[nodiscard]] std::optional<uint32_t> SystemId() const noexcept { return m_system; }
	[[nodiscard]] std::optional<bool> LegacyTestDevice() const noexcept { return m_test; }

private:
	EsvmVerifiedServiceCertificate() = default;
	friend EsvmCertificateVerification VerifyEsvmServiceCertificate(
	    std::span<const uint8_t>, EsvmCertificateFormat) noexcept;
	struct Slice { size_t offset = 0, size = 0; };
	std::span<const uint8_t> View(Slice s) const noexcept { return {m_bytes.data() + s.offset, s.size}; }
	std::array<uint8_t, EsvmCertificateMaxBytes> m_bytes {};
	size_t m_size = 0;
	Slice m_payload, m_key, m_provider, m_serial;
	uint32_t m_rsa_bits = 0;
	std::optional<uint32_t> m_creation, m_system;
	std::optional<bool> m_test;
};

struct EsvmCertificateVerification {
	EsvmCertificateResult result = EsvmCertificateResult::InvalidInput;
	// Engaged if and only if result == Ok; no partial/untrusted output on error.
	std::optional<EsvmVerifiedServiceCertificate> certificate;
	uint32_t server_error = 0;
};

// Audited immutable PKCS#1 DER (398 bytes), not a caller-selected root.
// SHA-256: 4d37517967d93c1fe39ea841f17a88da28c4aba7f3f09505224cafdf33f75c32
[[nodiscard]] std::span<const uint8_t> EsvmServiceCertificateRootKey() noexcept;

#if defined(KYTY_ESVM_CERTIFICATE_TESTS)
// Compile BOTH the backend and standalone tests with this definition. These
// diagnostics return bare status only: never a verified-certificate token.
// The production verification entry point never consults a test trust seam.
[[nodiscard]] EsvmCertificateResult EsvmCertificateSignatureForTest(
    std::span<const uint8_t> root_der, std::span<const uint8_t> payload,
    std::span<const uint8_t> signature) noexcept;
[[nodiscard]] EsvmCertificateResult EsvmCertificatePayloadForTest(std::span<const uint8_t> payload) noexcept;
[[nodiscard]] EsvmCertificateResult EsvmCertificateRsaForTest(std::span<const uint8_t> der) noexcept;
#endif

} // namespace Libs::MediaFramework
