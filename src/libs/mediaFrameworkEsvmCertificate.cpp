#include "libs/mediaFrameworkEsvmCertificate.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace Libs::MediaFramework {
namespace {
using Bytes = std::span<const uint8_t>;
using Result = EsvmCertificateResult;

// Public Widevine root key. Independently matched to root_signed_cert in
// devine-dl/pywidevine cdm.py at 25e03529f6bbfbf7701fd554f89b873fd398b0ef.
// See tests/fixtures/esvm-certificate/README.md for source URL and hashes.
// Public certificate data only; no private or device credential.
constexpr uint8_t RootKey[] = {
	0x30, 0x82, 0x01, 0x8a, 0x02, 0x82, 0x01, 0x81, 0x00, 0xb4, 0xfe, 0x39, 0xc3, 0x65, 0x90, 0x03,
	0xdb, 0x3c, 0x11, 0x97, 0x09, 0xe8, 0x68, 0xcd, 0xf2, 0xc3, 0x5e, 0x9b, 0xf2, 0xe7, 0x4d, 0x23,
	0xb1, 0x10, 0xdb, 0x87, 0x65, 0xdf, 0xdc, 0xfb, 0x9f, 0x35, 0xa0, 0x57, 0x03, 0x53, 0x4c, 0xf6,
	0x6d, 0x35, 0x7d, 0xa6, 0x78, 0xdb, 0xb3, 0x36, 0xd2, 0x3f, 0x9c, 0x40, 0xa9, 0x95, 0x26, 0x72,
	0x7f, 0xb8, 0xbe, 0x66, 0xdf, 0xc5, 0x21, 0x98, 0x78, 0x15, 0x16, 0x68, 0x5d, 0x2f, 0x46, 0x0e,
	0x43, 0xcb, 0x8a, 0x84, 0x39, 0xab, 0xfb, 0xb0, 0x35, 0x80, 0x22, 0xbe, 0x34, 0x23, 0x8b, 0xab,
	0x53, 0x5b, 0x72, 0xec, 0x4b, 0xb5, 0x48, 0x69, 0x53, 0x3e, 0x47, 0x5f, 0xfd, 0x09, 0xfd, 0xa7,
	0x76, 0x13, 0x8f, 0x0f, 0x92, 0xd6, 0x4c, 0xdf, 0xae, 0x76, 0xa9, 0xba, 0xd9, 0x22, 0x10, 0xa9,
	0x9d, 0x71, 0x45, 0xd6, 0xd7, 0xe1, 0x19, 0x25, 0x85, 0x9c, 0x53, 0x9a, 0x97, 0xeb, 0x84, 0xd7,
	0xcc, 0xa8, 0x88, 0x82, 0x20, 0x70, 0x26, 0x20, 0xfd, 0x7e, 0x40, 0x50, 0x27, 0xe2, 0x25, 0x93,
	0x6f, 0xbc, 0x3e, 0x72, 0xa0, 0xfa, 0xc1, 0xbd, 0x29, 0xb4, 0x4d, 0x82, 0x5c, 0xc1, 0xb4, 0xcb,
	0x9c, 0x72, 0x7e, 0xb0, 0xe9, 0x8a, 0x17, 0x3e, 0x19, 0x63, 0xfc, 0xfd, 0x82, 0x48, 0x2b, 0xb7,
	0xb2, 0x33, 0xb9, 0x7d, 0xec, 0x4b, 0xba, 0x89, 0x1f, 0x27, 0xb8, 0x9b, 0x88, 0x48, 0x84, 0xaa,
	0x18, 0x92, 0x0e, 0x65, 0xf5, 0xc8, 0x6c, 0x11, 0xff, 0x6b, 0x36, 0xe4, 0x74, 0x34, 0xca, 0x8c,
	0x33, 0xb1, 0xf9, 0xb8, 0x8e, 0xb4, 0xe6, 0x12, 0xe0, 0x02, 0x98, 0x79, 0x52, 0x5e, 0x45, 0x33,
	0xff, 0x11, 0xdc, 0xeb, 0xc3, 0x53, 0xba, 0x7c, 0x60, 0x1a, 0x11, 0x3d, 0x00, 0xfb, 0xd2, 0xb7,
	0xaa, 0x30, 0xfa, 0x4f, 0x5e, 0x48, 0x77, 0x5b, 0x17, 0xdc, 0x75, 0xef, 0x6f, 0xd2, 0x19, 0x6d,
	0xdc, 0xbe, 0x7f, 0xb0, 0x78, 0x8f, 0xdc, 0x82, 0x60, 0x4c, 0xbf, 0xe4, 0x29, 0x06, 0x5e, 0x69,
	0x8c, 0x39, 0x13, 0xad, 0x14, 0x25, 0xed, 0x19, 0xb2, 0xf2, 0x9f, 0x01, 0x82, 0x0d, 0x56, 0x44,
	0x88, 0xc8, 0x35, 0xec, 0x1f, 0x11, 0xb3, 0x24, 0xe0, 0x59, 0x0d, 0x37, 0xe4, 0x47, 0x3c, 0xea,
	0x4b, 0x7f, 0x97, 0x31, 0x1c, 0x81, 0x7c, 0x94, 0x8a, 0x4c, 0x7d, 0x68, 0x15, 0x84, 0xff, 0xa5,
	0x08, 0xfd, 0x18, 0xe7, 0xe7, 0x2b, 0xe4, 0x47, 0x27, 0x12, 0x11, 0xb8, 0x23, 0xec, 0x58, 0x93,
	0x3c, 0xac, 0x12, 0xd2, 0x88, 0x6d, 0x41, 0x3d, 0xc5, 0xfe, 0x1c, 0xdc, 0xb9, 0xf8, 0xd4, 0x51,
	0x3e, 0x07, 0xe5, 0x03, 0x6f, 0xa7, 0x12, 0xe8, 0x12, 0xf7, 0xb5, 0xce, 0xa6, 0x96, 0x55, 0x3f,
	0x78, 0xb4, 0x64, 0x82, 0x50, 0xd2, 0x33, 0x5f, 0x91, 0x02, 0x03, 0x01, 0x00, 0x01,
};
static_assert(sizeof(RootKey) == 398);
#ifdef _WIN32
constexpr uint8_t RootSha256[] = {
	0x4d, 0x37, 0x51, 0x79, 0x67, 0xd9, 0x3c, 0x1f, 0xe3, 0x9e, 0xa8, 0x41, 0xf1, 0x7a, 0x88, 0xda,
	0x28, 0xc4, 0xab, 0xa7, 0xf3, 0xf0, 0x95, 0x05, 0x22, 0x4c, 0xaf, 0xdf, 0x33, 0xf7, 0x5c, 0x32,
};
#endif

struct Budget {
	size_t fields = 0;
	Result failure = Result::InvalidWire;
	bool Field() noexcept {
		if (++fields <= EsvmCertificateMaxFields) { return true; }
		failure = Result::LimitExceeded;
		return false;
	}
	bool Depth(unsigned depth) noexcept {
		if (depth <= EsvmCertificateMaxDepth) { return true; }
		failure = Result::LimitExceeded;
		return false;
	}
};

class Wire {
public:
	Wire(Bytes bytes, Budget& budget): m_bytes(bytes), m_budget(budget) {}
	bool Empty() const noexcept { return m_pos == m_bytes.size(); }
	bool Varint(uint64_t& value) noexcept {
		value = 0;
		for (unsigned i = 0; i < 10; ++i) {
			if (Empty()) { return false; }
			const auto byte = m_bytes[m_pos++];
			if (i == 9 && byte > 1) { return false; } // No 64-bit overflow or eleventh byte.
			value |= uint64_t(byte & 0x7f) << (7 * i);
			if ((byte & 0x80) == 0) { return true; }
		}
		return false;
	}
	bool Tag(uint32_t& field, unsigned& type) noexcept {
		uint64_t tag = 0;
		if (!m_budget.Field() || !Varint(tag) || tag > UINT32_MAX || (tag >> 3) == 0) { return false; }
		field = static_cast<uint32_t>(tag >> 3);
		type = static_cast<unsigned>(tag & 7);
		return type <= 5;
	}
	bool Data(Bytes& out) noexcept {
		uint64_t count = 0;
		if (!Varint(count) || count > m_bytes.size() - m_pos) { return false; }
		out = m_bytes.subspan(m_pos, static_cast<size_t>(count));
		m_pos += static_cast<size_t>(count);
		return true;
	}
	bool Skip(uint32_t field, unsigned type, unsigned depth) noexcept {
		uint64_t scalar = 0;
		Bytes data;
		switch (type) {
			case 0: return Varint(scalar);
			case 1: return Advance(8);
			case 2: return Data(data);
			case 3: {
				if (!m_budget.Depth(depth + 1)) { return false; }
				while (!Empty()) {
					uint32_t nested = 0; unsigned wire = 0;
					if (!Tag(nested, wire)) { return false; }
					if (wire == 4) { return nested == field; }
					if (!Skip(nested, wire, depth + 1)) { return false; }
				}
				return false;
			}
			case 5: return Advance(4);
			default: return false; // Unmatched end-group, invalid wire type.
		}
	}

private:
	bool Advance(size_t n) noexcept {
		if (n > m_bytes.size() - m_pos) { return false; }
		m_pos += n;
		return true;
	}
	Bytes m_bytes;
	Budget& m_budget;
	size_t m_pos = 0;
};

struct Signed {
	Bytes payload, signature;
};

Result ParseSigned(Bytes bytes, Budget& budget, Signed& out, unsigned depth = 0) noexcept {
	if (!budget.Depth(depth)) { return budget.failure; }
	Wire wire(bytes, budget);
	while (!wire.Empty()) {
		uint32_t field = 0; unsigned type = 0;
		if (!wire.Tag(field, type)) { return budget.failure; }
		if ((field == 1 || field == 2) && type == 2) {
			if (!wire.Data(field == 1 ? out.payload : out.signature)) { return budget.failure; }
		} else if (field == 3 && type == 2) {
			Bytes signer;
			if (!wire.Data(signer)) { return budget.failure; }
			Signed ignored;
			const auto parsed = ParseSigned(signer, budget, ignored, depth + 1);
			if (parsed != Result::Ok) { return parsed; }
			// Structurally parsed only. NEVER used as a root or intermediate.
		} else if (field == 4 && type == 0) {
			uint64_t algorithm = 0;
			if (!wire.Varint(algorithm)) { return budget.failure; }
			// Newer optional hash selector: only unspecified/SHA-1 supported.
			// Reject other selectors rather than silently downgrade to SHA-1.
			if (algorithm > 1) { return Result::UnsupportedAlgorithm; }
		} else if (!wire.Skip(field, type, depth)) { return budget.failure; }
	}
	return Result::Ok;
}

Result Unwrap(Bytes bytes, Budget& budget, Bytes& raw, uint32_t& server_error) noexcept {
	uint64_t message_type = 1; // Native protobuf enum default.
	Bytes message;
	Wire wire(bytes, budget);
	while (!wire.Empty()) {
		uint32_t field = 0; unsigned type = 0;
		if (!wire.Tag(field, type)) { return budget.failure; }
		if (field == 1 && type == 0) {
			uint64_t value = 0;
			if (!wire.Varint(value)) { return budget.failure; }
			if (value >= 1 && value <= 10) { message_type = value; }
		} else if (field == 2 && type == 2) {
			if (!wire.Data(message)) { return budget.failure; }
		} else if (!wire.Skip(field, type, 0)) { return budget.failure; }
	}
	if (message_type == 3) {
		uint32_t error = 1;
		Wire errors(message, budget);
		while (!errors.Empty()) {
			uint32_t field = 0; unsigned type = 0;
			if (!errors.Tag(field, type)) { return budget.failure; }
			if (field == 1 && type == 0) {
				uint64_t value = 0;
				if (!errors.Varint(value)) { return budget.failure; }
				if (value >= 1 && value <= 3) { error = static_cast<uint32_t>(value); }
			} else if (!errors.Skip(field, type, 0)) { return budget.failure; }
		}
		server_error = error;
		return Result::ServerError;
	}
	if (message_type != 5) { return Result::UnexpectedMessageType; }
	if (message.empty()) { return Result::MissingCertificate; }
	raw = message;
	return Result::Ok;
}

struct Payload {
	Bytes key, serial, provider;
	std::optional<uint32_t> creation, system;
	std::optional<bool> test;
};

Result ParsePayload(Bytes bytes, Budget& budget, Payload& out) noexcept {
	uint64_t certificate_type = 0;
	Wire wire(bytes, budget);
	while (!wire.Empty()) {
		uint32_t field = 0; unsigned type = 0;
		if (!wire.Tag(field, type)) { return budget.failure; }
		if ((field == 1 || field == 3 || field == 5 || field == 6 || field == 9) && type == 0) {
			uint64_t value = 0;
			if (!wire.Varint(value)) { return budget.failure; }
			switch (field) {
				case 1: if (value <= 4) { certificate_type = value; } break;
				case 3: out.creation = static_cast<uint32_t>(value); break;
				case 5: out.system = static_cast<uint32_t>(value); break;
				case 6: out.test = value != 0; break;
				case 9: if (value != 1) { return Result::UnsupportedAlgorithm; } break;
			}
		} else if ((field == 2 || field == 4 || field == 7) && type == 2) {
			Bytes value;
			if (!wire.Data(value)) { return budget.failure; }
			if (field == 2) { out.serial = value; }
			if (field == 4) { out.key = value; }
			if (field == 7) { out.provider = value; }
		} else if (!wire.Skip(field, type, 0)) { return budget.failure; }
	}
	if (certificate_type != 3) { return Result::WrongCertificateType; }
	return out.key.empty() ? Result::InvalidPublicKey : Result::Ok;
}

class Der {
public:
	explicit Der(Bytes bytes): m_bytes(bytes) {}
	bool Empty() const noexcept { return m_pos == m_bytes.size(); }
	bool Value(uint8_t tag, Bytes& value) noexcept {
		if (m_bytes.size() - m_pos < 2 || m_bytes[m_pos++] != tag) { return false; }
		size_t length = m_bytes[m_pos++];
		if ((length & 0x80) != 0) {
			const size_t count = length & 0x7f;
			// Input is at most 16 KiB; >2 length octets cannot be minimal DER.
			if (count == 0 || count > 2 || count > m_bytes.size() - m_pos || m_bytes[m_pos] == 0) { return false; }
			length = 0;
			for (size_t i = 0; i < count; ++i) { length = (length << 8) | m_bytes[m_pos++]; }
			if (length < 128) { return false; }
		}
		if (length > m_bytes.size() - m_pos) { return false; }
		value = m_bytes.subspan(m_pos, length);
		m_pos += length;
		return true;
	}
	bool PositiveInteger(Bytes& value) noexcept {
		if (!Value(2, value) || value.empty() || (value.front() & 0x80) != 0) { return false; }
		if (value.front() == 0) {
			if (value.size() < 2 || (value[1] & 0x80) == 0) { return false; }
			value = value.subspan(1);
		}
		return true;
	}
private:
	Bytes m_bytes;
	size_t m_pos = 0;
};

struct Rsa {
	Bytes modulus, exponent;
	uint32_t bits = 0;
};

Result ParseRsa(Bytes der, Rsa& out) noexcept {
	if (der.empty() || der.size() > EsvmCertificateMaxBytes) { return Result::InvalidPublicKey; }
	Der outer(der);
	Bytes sequence;
	if (!outer.Value(0x30, sequence) || !outer.Empty()) { return Result::InvalidPublicKey; }
	Der inner(sequence);
	if (!inner.PositiveInteger(out.modulus) || !inner.PositiveInteger(out.exponent) || !inner.Empty() ||
	    (out.modulus.back() & 1) == 0 || (out.exponent.back() & 1) == 0) { return Result::InvalidPublicKey; }
	if (out.exponent.size() > 8) { return Result::UnsupportedPublicKey; }
	uint64_t exponent = 0;
	for (const auto byte: out.exponent) { exponent = (exponent << 8) | byte; }
	if (exponent < 3) { return Result::InvalidPublicKey; }
	unsigned leading = 0;
	for (uint8_t byte = out.modulus.front(); (byte & 0x80) == 0; byte <<= 1) { ++leading; }
	out.bits = static_cast<uint32_t>(out.modulus.size() * 8 - leading);
	// CNG RSA support bound, not a serial/provider or native certificate policy.
	if (out.bits < 512 || out.bits > EsvmCertificateMaxRsaBits) { return Result::UnsupportedPublicKey; }
	return Result::Ok;
}

#ifdef _WIN32
struct Algorithm {
	BCRYPT_ALG_HANDLE handle = nullptr;
	~Algorithm() { if (handle) { BCryptCloseAlgorithmProvider(handle, 0); } }
	Algorithm() = default;
	Algorithm(const Algorithm&) = delete;
	Algorithm& operator=(const Algorithm&) = delete;
};
struct Key {
	BCRYPT_KEY_HANDLE handle = nullptr;
	~Key() { if (handle) { BCryptDestroyKey(handle); } }
	Key() = default;
	Key(const Key&) = delete;
	Key& operator=(const Key&) = delete;
};
struct Hash {
	BCRYPT_HASH_HANDLE handle = nullptr;
	~Hash() { if (handle) { BCryptDestroyHash(handle); } }
};

Result Digest(LPCWSTR name, Bytes data, std::span<uint8_t> output) noexcept {
	Algorithm algorithm;
	if (BCryptOpenAlgorithmProvider(&algorithm.handle, name, MS_PRIMITIVE_PROVIDER, 0) < 0) {
		return Result::CryptoUnavailable;
	}
	Hash hash;
	// Windows 7+ CNG owns the hash object allocation; destroyed on every path.
	if (BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0 ||
	    BCryptHashData(hash.handle, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) < 0 ||
	    BCryptFinishHash(hash.handle, output.data(), static_cast<ULONG>(output.size()), 0) < 0) {
		return Result::CryptoFailure;
	}
	return Result::Ok;
}

Result Import(const Rsa& rsa, Algorithm& algorithm, Key& key) noexcept {
	if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_RSA_ALGORITHM, MS_PRIMITIVE_PROVIDER, 0) < 0) {
		return Result::CryptoUnavailable;
	}
	std::array<uint8_t, sizeof(BCRYPT_RSAKEY_BLOB) + 8 + EsvmCertificateMaxRsaBits / 8> blob {};
	const BCRYPT_RSAKEY_BLOB header {BCRYPT_RSAPUBLIC_MAGIC, rsa.bits,
	    static_cast<ULONG>(rsa.exponent.size()), static_cast<ULONG>(rsa.modulus.size()), 0, 0};
	std::memcpy(blob.data(), &header, sizeof(header));
	std::memcpy(blob.data() + sizeof(header), rsa.exponent.data(), rsa.exponent.size());
	std::memcpy(blob.data() + sizeof(header) + rsa.exponent.size(), rsa.modulus.data(), rsa.modulus.size());
	const auto size = static_cast<ULONG>(sizeof(header) + rsa.exponent.size() + rsa.modulus.size());
	const auto status = BCryptImportKeyPair(algorithm.handle, nullptr, BCRYPT_RSAPUBLIC_BLOB,
	    &key.handle, blob.data(), size, 0);
	if (status >= 0) { return Result::Ok; }
	const auto code = static_cast<uint32_t>(status);
	if (code == 0xc000000du || code == 0xc00000bbu) { // INVALID_PARAMETER / NOT_SUPPORTED
		return Result::UnsupportedPublicKey;
	}
	return Result::CryptoFailure; // Includes CNG allocation failure; not a bad signature.
}
#endif

Result CheckPin() noexcept {
#ifdef _WIN32
	std::array<uint8_t, 32> digest {};
	const auto hashed = Digest(BCRYPT_SHA256_ALGORITHM, RootKey, digest);
	if (hashed != Result::Ok) { return hashed; }
	return std::equal(digest.begin(), digest.end(), RootSha256) ? Result::Ok : Result::TrustAnchorMismatch;
#else
	return Result::CryptoUnavailable;
#endif
}

Result VerifyPss(Bytes der, Bytes payload, Bytes signature) noexcept {
	Rsa root;
	const auto parsed = ParseRsa(der, root);
	if (parsed != Result::Ok) { return parsed; }
	if (signature.size() != root.modulus.size()) { return Result::BadSignature; }
#ifdef _WIN32
	Algorithm algorithm;
	Key key;
	auto result = Import(root, algorithm, key);
	if (result != Result::Ok) { return result; }
	std::array<uint8_t, 20> digest {};
	result = Digest(BCRYPT_SHA1_ALGORITHM, payload, digest);
	if (result != Result::Ok) { return result; }
	BCRYPT_PSS_PADDING_INFO padding {BCRYPT_SHA1_ALGORITHM, 20};
	// CNG PSS uses the specified hash for MGF1 as well. No autodetected salt,
	// alternate hash, v1.5 fallback, or supplied signer trust is permitted.
	const NTSTATUS status = BCryptVerifySignature(key.handle, &padding, digest.data(), static_cast<ULONG>(digest.size()),
	    const_cast<PUCHAR>(signature.data()), static_cast<ULONG>(signature.size()), BCRYPT_PAD_PSS);
	if (status >= 0) { return Result::Ok; }
	if (static_cast<uint32_t>(status) == 0xc000a000u) { return Result::BadSignature; } // STATUS_INVALID_SIGNATURE
	return Result::CryptoFailure;
#else
	(void)payload;
	return Result::CryptoUnavailable;
#endif
}

Result UsableServiceKey(const Rsa& rsa) noexcept {
#ifdef _WIN32
	Algorithm algorithm;
	Key key;
	return Import(rsa, algorithm, key);
#else
	(void)rsa;
	return Result::CryptoUnavailable;
#endif
}
} // namespace

std::span<const uint8_t> EsvmServiceCertificateRootKey() noexcept { return RootKey; }

EsvmCertificateVerification VerifyEsvmServiceCertificate(Bytes input, EsvmCertificateFormat format) noexcept {
	EsvmCertificateVerification out;
	if (input.empty() || !input.data()) { return out; }
	if (input.size() > EsvmCertificateMaxBytes) { out.result = Result::LimitExceeded; return out; }
	// Snapshot before parsing/hashing. Never publish different bytes if the
	// caller reuses the input backing after verification. Caller must supply a
	// readable host span; guest-memory preflight/copy is outside this backend.
	std::array<uint8_t, EsvmCertificateMaxBytes> snapshot {};
	std::memcpy(snapshot.data(), input.data(), input.size());
	const Bytes owned(snapshot.data(), input.size());
	Budget budget;
	Bytes raw = owned;
	if (format == EsvmCertificateFormat::ServiceCertificateResponse) {
		out.result = Unwrap(owned, budget, raw, out.server_error);
		if (out.result != Result::Ok) { return out; }
	} else if (format != EsvmCertificateFormat::SignedCertificate) { return out; }
	Signed signed_certificate;
	out.result = ParseSigned(raw, budget, signed_certificate);
	if (out.result != Result::Ok) { return out; }
	if (signed_certificate.payload.empty() || signed_certificate.signature.empty()) {
		out.result = Result::MissingCertificate;
		return out;
	}
	out.result = CheckPin();
	if (out.result != Result::Ok) { return out; }
	out.result = VerifyPss(RootKey, signed_certificate.payload, signed_certificate.signature);
	if (out.result != Result::Ok) { return out; }
	Payload payload;
	out.result = ParsePayload(signed_certificate.payload, budget, payload);
	if (out.result != Result::Ok) { return out; }
	Rsa rsa;
	out.result = ParseRsa(payload.key, rsa);
	if (out.result != Result::Ok) { return out; }
	out.result = UsableServiceKey(rsa); // Deliberately stricter than native's lazy DER decode.
	if (out.result != Result::Ok) { return out; }
	EsvmVerifiedServiceCertificate verified;
	std::memcpy(verified.m_bytes.data(), raw.data(), raw.size());
	verified.m_size = raw.size();
	const auto slice = [raw](Bytes bytes) -> EsvmVerifiedServiceCertificate::Slice {
		return bytes.empty() ? EsvmVerifiedServiceCertificate::Slice {} :
		    EsvmVerifiedServiceCertificate::Slice {static_cast<size_t>(bytes.data() - raw.data()), bytes.size()};
	};
	verified.m_payload = slice(signed_certificate.payload);
	verified.m_key = slice(payload.key);
	verified.m_serial = slice(payload.serial);
	verified.m_provider = slice(payload.provider);
	verified.m_rsa_bits = rsa.bits;
	verified.m_creation = payload.creation;
	verified.m_system = payload.system;
	verified.m_test = payload.test;
	out.certificate = verified;
	out.result = Result::Ok;
	return out;
}

#if defined(KYTY_ESVM_CERTIFICATE_TESTS)
EsvmCertificateResult EsvmCertificateSignatureForTest(Bytes root, Bytes payload, Bytes signature) noexcept {
	if (root.size() > EsvmCertificateMaxBytes || payload.size() > EsvmCertificateMaxBytes ||
	    signature.size() > EsvmCertificateMaxRsaBits / 8) { return Result::LimitExceeded; }
	return VerifyPss(root, payload, signature);
}
EsvmCertificateResult EsvmCertificatePayloadForTest(Bytes bytes) noexcept {
	if (bytes.size() > EsvmCertificateMaxBytes) { return Result::LimitExceeded; }
	Budget budget;
	Payload payload;
	auto result = ParsePayload(bytes, budget, payload);
	if (result != Result::Ok) { return result; }
	Rsa rsa;
	return ParseRsa(payload.key, rsa); // Parse-only; cannot construct a verified token.
}
EsvmCertificateResult EsvmCertificateRsaForTest(Bytes der) noexcept {
	Rsa rsa;
	return ParseRsa(der, rsa);
}
#endif
} // namespace Libs::MediaFramework
