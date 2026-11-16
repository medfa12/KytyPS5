#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace Loader {
// Review inventory ONLY: not a selection predicate or production admission list.
// Core-imported family from avsh4-alternate-np-closure-addendum-20261106.md.
// RVAs apply solely to this alternate libSceNpCommon.sprx SHA256.
inline constexpr std::string_view NpSynchronizationImageSha256 =
    "81de809c26e4cd1a73d6268c7e89d1e41cf811397f1ac7be17f70e8e14a26a35";
enum class NpSynchronizationFamily { Raw, EventClass, MutexClass, RegistryQuery };
struct NpSynchronizationExport {
	std::string_view nid;
	uint32_t rva;
	NpSynchronizationFamily family;
};
inline constexpr std::array<NpSynchronizationExport, 42> NpSynchronizationExports {{
	{"in19gH7G040", 0x6450, NpSynchronizationFamily::Raw},
	{"q2tsVO3lM4A", 0x6620, NpSynchronizationFamily::Raw},
	{"1a+iY5YUJcI", 0x66a0, NpSynchronizationFamily::Raw},
	{"fZShld2PQ7w", 0x66b0, NpSynchronizationFamily::Raw},
	{"ss2xO9IJxKQ", 0x66c0, NpSynchronizationFamily::Raw},
	{"uMJFOA62mVU", 0x66f0, NpSynchronizationFamily::Raw},
	{"bsjWg59A7aE", 0x6700, NpSynchronizationFamily::Raw},
	{"5DkyduAF2rs", 0x6860, NpSynchronizationFamily::Raw},
	{"+nmn+Z0nWDo", 0x6880, NpSynchronizationFamily::Raw},
	{"29ftOGIrUCo", 0x68f0, NpSynchronizationFamily::Raw},
	{"uEwag-0YZPc", 0x6950, NpSynchronizationFamily::Raw},
	{"lQ11BpMM4LU", 0x69e0, NpSynchronizationFamily::Raw},
	{"r9Bet+s6fKc", 0x69f0, NpSynchronizationFamily::Raw},
	{"DuslmoqQ+nk", 0x6a10, NpSynchronizationFamily::Raw},
	{"oZyb9ktuCpA", 0x6a30, NpSynchronizationFamily::Raw},
	{"1CiXI-MyEKs", 0x6a50, NpSynchronizationFamily::Raw},
	{"4zxevggtYrQ", 0x6ae0, NpSynchronizationFamily::Raw},
	{"18j+qk6dRwk", 0x6af0, NpSynchronizationFamily::Raw},
	{"CQG2oyx1-nM", 0x6b30, NpSynchronizationFamily::Raw},
	{"LHZtCT2W1Pw", 0x6b50, NpSynchronizationFamily::Raw},
	{"xPrF2nGPBXQ", 0x6b80, NpSynchronizationFamily::Raw},
	{"6adrFGe2cpU", 0x6be0, NpSynchronizationFamily::Raw},
	{"-W28+9p1CKI", 0x6c40, NpSynchronizationFamily::Raw},
	{"fhJ5uKzcn0w", 0x6c60, NpSynchronizationFamily::Raw},
	{"EjMsfO3GCIA", 0x6d80, NpSynchronizationFamily::Raw},
	{"yvaNTRiKXmo", 0x6da0, NpSynchronizationFamily::Raw},
	{"cMOgkE2M2e8", 0x1d910, NpSynchronizationFamily::EventClass},
	{"WWW4bvT-rSw", 0x1d930, NpSynchronizationFamily::EventClass},
	{"st-oQvV7HVI", 0x1d9e0, NpSynchronizationFamily::EventClass},
	{"amFi-Av19hU", 0x1d9f0, NpSynchronizationFamily::EventClass},
	{"5Wy+JxpCBxg", 0x1da30, NpSynchronizationFamily::EventClass},
	{"QlaBcxSFPZI", 0x1da60, NpSynchronizationFamily::EventClass},
	{"8i-vOVRVt5w", 0x1daa0, NpSynchronizationFamily::EventClass},
	{"37Rd2JS+FCM", 0x1db00, NpSynchronizationFamily::EventClass},
	{"1s+c3SG0WYc", 0x1db80, NpSynchronizationFamily::EventClass},
	{"O1AvlQU33pI", 0x1e410, NpSynchronizationFamily::MutexClass},
	{"9zi9FTPol74", 0x1e430, NpSynchronizationFamily::MutexClass},
	{"Yc+qj4TIEY0", 0x1e4b0, NpSynchronizationFamily::MutexClass},
	{"aTNOl9EB4V4", 0x1e4c0, NpSynchronizationFamily::MutexClass},
	{"RgGW4f0ox1g", 0x1e500, NpSynchronizationFamily::MutexClass},
	{"VM+CXTW4F-s", 0x1e5f0, NpSynchronizationFamily::MutexClass},
	{"eYgHIWx0Hco", 0x1e6a0, NpSynchronizationFamily::MutexClass},
}};
// Separate startup query; neither a synchronization primitive nor registry proof.
inline constexpr NpSynchronizationExport NpSynchronizationRegistryQuery {
	"GRvK1ZE+FEQ", 0x28020, NpSynchronizationFamily::RegistryQuery
};
} // namespace Loader
