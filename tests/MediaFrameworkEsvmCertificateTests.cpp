// Standalone: C++20; link ONLY mediaFrameworkEsvmCertificate.cpp (+ bcrypt on
// Windows). Define KYTY_ESVM_CERTIFICATE_TESTS for BOTH translation units.
// No emulator, guest process, device credential, network or test runner needed.
#include "libs/mediaFrameworkEsvmCertificate.h"
#include "fixtures/esvm-certificate/public_certificates.h"
#include "fixtures/esvm-certificate/synthetic_pss.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#if !defined(KYTY_ESVM_CERTIFICATE_TESTS)
#error Define KYTY_ESVM_CERTIFICATE_TESTS for the standalone test AND backend.
#endif

namespace {
using namespace Libs::MediaFramework;
using Result = EsvmCertificateResult;
using Format = EsvmCertificateFormat;
using Bytes = std::vector<uint8_t>;
using View = std::span<const uint8_t>;

static_assert(!std::is_default_constructible_v<EsvmVerifiedServiceCertificate>);
static_assert(!std::is_constructible_v<EsvmVerifiedServiceCertificate, View>);

void Check(bool condition, const char* message) {
	if (!condition) { std::fprintf(stderr, "EsvmCertificateTests: %s\n", message); std::abort(); }
}
bool Same(View a, View b) { return std::equal(a.begin(), a.end(), b.begin(), b.end()); }
View Text(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }
Bytes Copy(View s) { return {s.begin(), s.end()}; }
void Append(Bytes& a, View b) { a.insert(a.end(), b.begin(), b.end()); }
void Varint(Bytes& out, uint64_t n) {
	while (n >= 128) { out.push_back(static_cast<uint8_t>((n & 127) | 128)); n >>= 7; }
	out.push_back(static_cast<uint8_t>(n));
}
void Scalar(Bytes& out, uint32_t field, uint64_t n) { Varint(out, uint64_t(field) << 3); Varint(out, n); }
void Data(Bytes& out, uint32_t field, View data) {
	Varint(out, (uint64_t(field) << 3) | 2); Varint(out, data.size()); Append(out, data);
}
Bytes Envelope(uint32_t type, View raw) { Bytes b; Scalar(b, 1, type); Data(b, 2, raw); return b; }
Bytes Signed(View payload, View signature) { Bytes b; Data(b, 1, payload); Data(b, 2, signature); return b; }
Bytes Decode(std::string_view base64) {
	constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	Bytes out;
	uint32_t value = 0; unsigned bits = 0;
	for (const char c: base64) {
		if (c == '=') { break; }
		const auto digit = alphabet.find(c);
		Check(digit != alphabet.npos, "fixture base64 alphabet");
		value = (value << 6) | static_cast<uint32_t>(digit); bits += 6;
		if (bits >= 8) { bits -= 8; out.push_back(static_cast<uint8_t>(value >> bits)); }
	}
	return out;
}
uint64_t Read(View bytes, size_t& offset) {
	uint64_t value = 0;
	for (unsigned i = 0; i < 10; ++i) {
		Check(offset < bytes.size(), "fixture varint bounds");
		const auto c = bytes[offset++];
		Check(i != 9 || c <= 1, "fixture varint overflow");
		value |= uint64_t(c & 127) << (i * 7);
		if (c < 128) { return value; }
	}
	Check(false, "fixture varint termination"); return 0;
}
void Skip(View bytes, size_t& offset, uint64_t tag, unsigned depth = 0) {
	Check(depth <= EsvmCertificateMaxDepth, "fixture group depth");
	const auto type = tag & 7;
	if (type == 0) { Read(bytes, offset); return; }
	if (type == 3) {
		while (offset < bytes.size()) {
			const auto nested = Read(bytes, offset);
			if ((nested & 7) == 4) { Check((nested >> 3) == (tag >> 3), "fixture end-group"); return; }
			Skip(bytes, offset, nested, depth + 1);
		}
		Check(false, "fixture missing end-group"); return;
	}
	Check(type == 1 || type == 2 || type == 5, "fixture unknown wire type");
	const auto count = type == 2 ? Read(bytes, offset) : (type == 1 ? 8 : 4);
	Check(count <= bytes.size() - offset, "fixture skip bounds");
	offset += static_cast<size_t>(count);
}
View Field(View bytes, uint32_t wanted) {
	size_t offset = 0; View found;
	while (offset < bytes.size()) {
		const auto tag = Read(bytes, offset);
		if ((tag & 7) != 2) { Skip(bytes, offset, tag); continue; }
		const auto size = Read(bytes, offset);
		Check(size <= bytes.size() - offset, "fixture data bounds");
		if ((tag >> 3) == wanted) { found = bytes.subspan(offset, static_cast<size_t>(size)); }
		offset += static_cast<size_t>(size);
	}
	return found;
}
Result PlatformResult(Result crypto_result) {
#ifdef _WIN32
	return crypto_result;
#else
	(void)crypto_result;
	return Result::CryptoUnavailable;
#endif
}
void Reject(View bytes, Format format, Result expected) {
	const auto verified = VerifyEsvmServiceCertificate(bytes, format);
	Check(verified.result == expected, "specific failure classification");
	Check(!verified.certificate, "failure must not expose an authenticated/partial token");
}
void Accept(View bytes, Format format, View expected_raw, std::string_view provider) {
	const auto verified = VerifyEsvmServiceCertificate(bytes, format);
	Check(verified.result == PlatformResult(Result::Ok), "authentic public root-signed vector");
#ifdef _WIN32
	Check(verified.certificate.has_value(), "successful crypto produces verified token");
	const auto& cert = *verified.certificate;
	Check(Same(cert.SignedCertificate(), expected_raw), "own exact signed wrapper, not response envelope");
	Check(Same(cert.SignedPayload(), Field(expected_raw, 1)), "own exact signed payload without reserialization");
	Check(Same(cert.ProviderId(), Text(provider)), "authenticated provider bytes");
	Check(cert.SerialNumber().size() == 16 && cert.RsaBits() == 2048 && cert.PublicKeyDer().size() == 270,
	      "authentic serial/public RSA metadata");
	Check(cert.CreationTimeSeconds().has_value(), "creation time retained, not a validity assertion");
#else
	(void)expected_raw; (void)provider;
	Check(!verified.certificate, "unsupported platform NEVER returns parse-only trust");
#endif
}

void AuthenticAndOwnership() {
	const auto response = Decode(EsvmCertificateFixture::CommonResponseBase64);
	const auto raw = Copy(Field(response, 2));
	Check(response.size() == 716 && raw.size() == 711, "production public fixture sizes");
	Accept(response, Format::ServiceCertificateResponse, raw, "license.widevine.com");
	Accept(raw, Format::SignedCertificate, raw, "license.widevine.com");
	const auto staging = Decode(EsvmCertificateFixture::StagingResponseBase64);
	Check(staging.size() == 714, "staging public fixture size");
	Accept(staging, Format::ServiceCertificateResponse, Field(staging, 2), "staging.google.com");
	const auto root = Decode(EsvmCertificateFixture::SignedRootBase64);
	Check(root.size() == 802 && EsvmServiceCertificateRootKey().size() == 398, "firmware public root sizes");
	Check(Same(Field(Field(root, 1), 4), EsvmServiceCertificateRootKey()), "fixture root matches hard pin");
	Reject(root, Format::SignedCertificate, PlatformResult(Result::WrongCertificateType));
	Reject(raw, Format::ServiceCertificateResponse, Result::UnexpectedMessageType);
	Reject(response, Format::SignedCertificate, Result::MissingCertificate);
	Bytes unaligned(raw.size() + 2, 0xa5);
	std::copy(raw.begin(), raw.end(), unaligned.begin() + 1);
	const auto verified = VerifyEsvmServiceCertificate(View(unaligned).subspan(1, raw.size()), Format::SignedCertificate);
	Check(unaligned.front() == 0xa5 && unaligned.back() == 0xa5, "unaligned input canaries untouched");
	std::fill(unaligned.begin(), unaligned.end(), 0);
#ifdef _WIN32
	Check(verified.certificate && Same(verified.certificate->SignedCertificate(), raw), "retained bytes own storage");
	const auto copy = *verified.certificate;
	Check(Same(copy.ProviderId(), Text("license.widevine.com")) && Same(copy.SignedCertificate(), raw),
	      "copy rebases byte views into copied storage");
#else
	Check(verified.result == Result::CryptoUnavailable && !verified.certificate, "no backend fails closed");
#endif
}

void ProtobufAndBounds() {
	const auto response = Decode(EsvmCertificateFixture::CommonResponseBase64);
	const auto raw = Copy(Field(response, 2));
	Reject({}, Format::SignedCertificate, Result::InvalidInput);
	Reject(Bytes(EsvmCertificateMaxBytes + 1), Format::SignedCertificate, Result::LimitExceeded);
	Reject(raw, static_cast<Format>(99), Result::InvalidInput);
	for (const Bytes bad: {Bytes {0}, Bytes {0x0f}, Bytes {0x0c}, Bytes {0x0a, 0x80}, Bytes {0x0a, 9, 1},
	                      Bytes {0x7b}, Bytes {0x7b, 0x74}, Bytes {0x79, 0}, Bytes {0x7d, 0}}) {
		Reject(bad, Format::SignedCertificate, Result::InvalidWire);
	}
	Bytes overlong(11, 0x80); overlong.back() = 0;
	Reject(overlong, Format::SignedCertificate, Result::InvalidWire);
	Bytes overflow {0x0a}; overflow.insert(overflow.end(), 9, 0xff); overflow.push_back(2);
	Reject(overflow, Format::SignedCertificate, Result::InvalidWire);
	Bytes tag_overflow; Varint(tag_overflow, uint64_t(1) << 32);
	Reject(tag_overflow, Format::SignedCertificate, Result::InvalidWire);
	// Every strict prefix is invalid: never a partially authenticated result.
	for (size_t i = 0; i < raw.size(); ++i) {
		const auto result = VerifyEsvmServiceCertificate(View(raw).first(i), Format::SignedCertificate);
		Check(result.result != Result::Ok && !result.certificate, "truncation at every byte fails closed");
	}
	Reject(Envelope(4, {}), Format::ServiceCertificateResponse, Result::UnexpectedMessageType);
	Reject(Envelope(5, {}), Format::ServiceCertificateResponse, Result::MissingCertificate);
	for (uint32_t error: {1u, 2u, 3u}) {
		Bytes body; Scalar(body, 1, error);
		const auto result = VerifyEsvmServiceCertificate(Envelope(3, body), Format::ServiceCertificateResponse);
		Check(result.result == Result::ServerError && result.server_error == error && !result.certificate,
		      "server error cannot become certificate success");
	}
	Reject(Envelope(3, Bytes {8, 0x80}), Format::ServiceCertificateResponse, Result::InvalidWire);
	Bytes last = Envelope(2, raw); Scalar(last, 1, 5);
	Accept(last, Format::ServiceCertificateResponse, raw, "license.widevine.com");
	Scalar(last, 1, 99); // Unknown enum does not replace recognized value, as native protobuf.
	Accept(last, Format::ServiceCertificateResponse, raw, "license.widevine.com");
	Scalar(last, 1, 2);
	Reject(last, Format::ServiceCertificateResponse, Result::UnexpectedMessageType);
	Bytes noncanonical {8, 0x85, 0}; Data(noncanonical, 2, raw);
	Accept(noncanonical, Format::ServiceCertificateResponse, raw, "license.widevine.com");
	Bytes duplicate = Envelope(5, Bytes {0}); Data(duplicate, 2, raw);
	Accept(duplicate, Format::ServiceCertificateResponse, raw, "license.widevine.com");
	Bytes reversed; Data(reversed, 2, Field(raw, 2)); Data(reversed, 1, Field(raw, 1));
	Accept(reversed, Format::SignedCertificate, reversed, "license.widevine.com");
	Bytes replaced; Data(replaced, 1, Bytes {0}); Data(replaced, 2, Bytes {0}); Append(replaced, raw);
	Accept(replaced, Format::SignedCertificate, replaced, "license.widevine.com");
	Data(replaced, 2, {});
	Reject(replaced, Format::SignedCertificate, Result::MissingCertificate);
	Bytes unknown = raw;
	unknown.push_back(0x79); unknown.insert(unknown.end(), 8, 0xff); // fixed64
	unknown.push_back(0x7d); unknown.insert(unknown.end(), 4, 0xff); // fixed32
	unknown.push_back(0x7b); Scalar(unknown, 1, 2); unknown.push_back(0x7c); // matched group
	Data(unknown, 15, Bytes {0xff, 0, 0x80}); // Opaque unknown bytes need not be protobuf.
	Accept(unknown, Format::SignedCertificate, unknown, "license.widevine.com");
	Bytes many = raw;
	for (size_t i = 0; i < EsvmCertificateMaxFields; ++i) { Scalar(many, 15, 0); }
	Reject(many, Format::SignedCertificate, Result::LimitExceeded);
	Bytes signer;
	for (unsigned i = 0; i < EsvmCertificateMaxDepth; ++i) { Bytes next; Data(next, 3, signer); signer = next; }
	Bytes depth = raw; Data(depth, 3, signer); // One extra edge from top-level wrapper.
	Reject(depth, Format::SignedCertificate, Result::LimitExceeded);
	Bytes groups = raw;
	groups.insert(groups.end(), EsvmCertificateMaxDepth + 1, 0x7b);
	groups.insert(groups.end(), EsvmCertificateMaxDepth + 1, 0x7c);
	Reject(groups, Format::SignedCertificate, Result::LimitExceeded);
	Bytes hash = raw; Scalar(hash, 4, 2);
	Reject(hash, Format::SignedCertificate, Result::UnsupportedAlgorithm);
	hash = raw; Scalar(hash, 4, 1);
	Accept(hash, Format::SignedCertificate, hash, "license.widevine.com");
}

void TamperAndRootIsolation() {
	const auto response = Decode(EsvmCertificateFixture::CommonResponseBase64);
	const auto raw = Copy(Field(response, 2));
	const auto payload = Field(raw, 1), signature = Field(raw, 2);
	for (size_t i = 0; i < 16; ++i) {
		Bytes bad = raw;
		bad[static_cast<size_t>(payload.data() - raw.data()) + i] ^= 1;
		Reject(bad, Format::SignedCertificate, PlatformResult(Result::BadSignature));
		bad = raw; bad[static_cast<size_t>(signature.data() - raw.data()) + i] ^= 1;
		Reject(bad, Format::SignedCertificate, PlatformResult(Result::BadSignature));
	}
	Bytes wrong_root = Copy(EsvmServiceCertificateRootKey()); wrong_root[64] ^= 1;
	Check(EsvmCertificateRsaForTest(wrong_root) == Result::Ok, "wrong root is still a well-formed RSA public key");
	Check(EsvmCertificateSignatureForTest(wrong_root, payload, signature) == PlatformResult(Result::BadSignature),
	      "wrong root cannot verify authentic signature");
	const auto root = Decode(EsvmCertificateFixture::SignedRootBase64);
	Bytes signed_with_signer = raw; Data(signed_with_signer, 3, root);
	Accept(signed_with_signer, Format::SignedCertificate, signed_with_signer, "license.widevine.com");
	// A supplied signer is not used to replace the root or forgive tampering.
	const auto offset = static_cast<size_t>(Field(signed_with_signer, 1).data() - signed_with_signer.data());
	signed_with_signer[offset + 5] ^= 1;
	Reject(signed_with_signer, Format::SignedCertificate, PlatformResult(Result::BadSignature));
}

Bytes DerValue(uint8_t tag, View data) {
	Bytes bytes {tag};
	if (data.size() < 128) { bytes.push_back(static_cast<uint8_t>(data.size())); }
	else if (data.size() <= 255) { bytes.push_back(0x81); bytes.push_back(static_cast<uint8_t>(data.size())); }
	else { bytes.push_back(0x82); bytes.push_back(static_cast<uint8_t>(data.size() >> 8)); bytes.push_back(static_cast<uint8_t>(data.size())); }
	Append(bytes, data); return bytes;
}
void PayloadAndDerDiagnostics() {
	Bytes payload; Scalar(payload, 1, 3); Data(payload, 4, EsvmServiceCertificateRootKey());
	Check(EsvmCertificatePayloadForTest(payload) == Result::Ok, "native does not require serial/provider");
	Data(payload, 2, Bytes {0, 0xff}); Data(payload, 7, Bytes {0, 0xff, 0});
	Check(EsvmCertificatePayloadForTest(payload) == Result::Ok, "no invented serial size/origin/UTF8/NUL constraints");
	Scalar(payload, 1, 99);
	Check(EsvmCertificatePayloadForTest(payload) == Result::Ok, "unknown type enum does not replace recognized SERVICE");
	Scalar(payload, 1, 2);
	Check(EsvmCertificatePayloadForTest(payload) == Result::WrongCertificateType, "recognized wrong type replaces SERVICE");
	Scalar(payload, 1, 3); Scalar(payload, 9, 2);
	Check(EsvmCertificatePayloadForTest(payload) == Result::UnsupportedAlgorithm, "no ECC algorithm downgrade");
	Check(EsvmCertificatePayloadForTest(Bytes {8, 3}) == Result::InvalidPublicKey, "missing RSA key");
	for (Bytes der: {Bytes {}, Bytes {0x30, 0x80, 0, 0}, Bytes {0x30, 0x81, 0},
	                Bytes {0x30, 3, 0x30, 1, 0}, Bytes {0x30, 6, 2, 1, 0, 2, 1, 3}}) {
		Check(EsvmCertificateRsaForTest(der) == Result::InvalidPublicKey, "reject indefinite/nonminimal/zero/SPKI key");
	}
	Bytes der = Copy(EsvmServiceCertificateRootKey());
	der.push_back(0);
	Check(EsvmCertificateRsaForTest(der) == Result::InvalidPublicKey, "DER trailing garbage");
	der = Copy(EsvmServiceCertificateRootKey()); der[8] = 0x80;
	Check(EsvmCertificateRsaForTest(der) == Result::InvalidPublicKey, "negative DER modulus");
	der = Copy(EsvmServiceCertificateRootKey()); der.back() = 1; der[der.size() - 2] = 0; der[der.size() - 3] = 0;
	Check(EsvmCertificateRsaForTest(der) == Result::InvalidPublicKey, "zero-prefixed/invalid RSA exponent");
	der = Copy(EsvmServiceCertificateRootKey()); der[der.size() - 6] &= 0xfe;
	Check(EsvmCertificateRsaForTest(der) == Result::InvalidPublicKey, "even RSA modulus");
	Bytes redundant_modulus {0, 0};
	Append(redundant_modulus, EsvmServiceCertificateRootKey().subspan(9, 384));
	Bytes redundant = DerValue(2, redundant_modulus); Append(redundant, DerValue(2, Bytes {1, 0, 1}));
	Check(EsvmCertificateRsaForTest(DerValue(0x30, redundant)) == Result::InvalidPublicKey,
	      "DER integer rejects redundant sign padding");
	Bytes big_modulus(EsvmCertificateMaxRsaBits / 8 + 2, 0xff); big_modulus[0] = 0;
	Bytes integers = DerValue(2, big_modulus); Append(integers, DerValue(2, Bytes {1, 0, 1}));
	Check(EsvmCertificateRsaForTest(DerValue(0x30, integers)) == Result::UnsupportedPublicKey, "RSA work bound");
	Check(EsvmCertificateRsaForTest(Bytes {0x30, 6, 2, 1, 3, 2, 1, 3}) == Result::UnsupportedPublicKey,
	      "explicit unsupported tiny RSA size, never accepted as usable key");
}

void ExactPssParameters() {
	namespace Fixture = EsvmCertificateSynthetic;
	const auto key = Decode(Fixture::PublicKeyBase64), payload = Decode(Fixture::PayloadBase64);
	const auto signature = Decode(Fixture::PssSha1Salt20Base64);
	Check(EsvmCertificatePayloadForTest(payload) == Result::Ok, "noncanonical payload fixture is structurally valid");
	Check(EsvmCertificateSignatureForTest(key, payload, signature) == PlatformResult(Result::Ok),
	      "test-only trust seam checks real PSS without yielding authenticated token");
	for (const auto* wrong: {Fixture::PssSha1Salt0Base64, Fixture::PssSha1Salt32Base64,
	                        Fixture::PssSha1Mgf256Salt20Base64, Fixture::PssSha256Salt20Base64, Fixture::Pkcs1Sha1Base64}) {
		Check(EsvmCertificateSignatureForTest(key, payload, Decode(wrong)) == PlatformResult(Result::BadSignature),
		      "reject wrong salt/hash/MGF1/v1.5, no fallback/autodetection");
	}
	Bytes canonical = payload; canonical.erase(canonical.begin() + 2); canonical[1] = 3;
	Check(EsvmCertificatePayloadForTest(canonical) == Result::Ok, "same protobuf meaning after canonicalization");
	Check(EsvmCertificateSignatureForTest(key, canonical, signature) == PlatformResult(Result::BadSignature),
	      "same fields reserialized are NOT the signed byte sequence");
	Reject(Signed(payload, signature), Format::SignedCertificate, PlatformResult(Result::BadSignature));
	// Test root seam cannot alter a subsequent production verification.
	const auto response = Decode(EsvmCertificateFixture::CommonResponseBase64);
	Accept(response, Format::ServiceCertificateResponse, Field(response, 2), "license.widevine.com");
}
} // namespace

int main() {
	AuthenticAndOwnership();
	ProtobufAndBounds();
	TamperAndRootIsolation();
	PayloadAndDerDiagnostics();
	ExactPssParameters();
#ifdef _WIN32
	std::puts("EsvmCertificateTests: passed (real Windows CNG verification)");
#else
	std::puts("EsvmCertificateTests: parser tests passed; crypto unavailable path checked, RSA verification NOT run");
#endif
	return 0;
}
