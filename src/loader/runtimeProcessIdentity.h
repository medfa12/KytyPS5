#pragma once

#include "loader/processMetadata.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include "loader/bootCredentialAdmission.h"

namespace Loader {
struct Program;

inline constexpr int BootGuestProcessId = 100;

struct GuestProcessIdentity {
	int pid = -1;
	std::optional<int32_t> app_id;
	std::optional<uint32_t> category;
	// Title ATTRIBUTE3, not authinfo capabilities. GetAppInfo publishes this
	// word at +0x4c; missing metadata must not grant any attribute bits.
	std::optional<uint32_t> attribute3;
	std::optional<uint64_t> authority;
	uint64_t capabilities = 0;
	ProcessAbi abi = ProcessAbi::Unknown;
	uint32_t compiled_sdk_version = 0;
	uint32_t prospero_compiled_sdk_version = 0;
};

// Call after SystemContent/LNC registration, before starting guest threads.
// Runtime owners and their records remain resident; PIDs are never recycled.
int RegisterRuntimeProcessIdentity(Program* owner, uint32_t app_id);
// Optional boot setup after its metadata/LNC registration. Explicit PID 100
// then addresses this snapshot even when queried by a runtime application.
bool ConfigureBootProcessIdentity(Program* program);
// Explicit cohost identity, distinct from PID100; no capabilities inferred.
int RegisterCohostProcessIdentity(Program* program, uint32_t app_id);
// Host loader only, BEFORE any relocation, compatibility patch, or execution.
// Caller supplies the actual mapping from the same immutable source snapshot.
bool AdmitOriginalCoreBootCredentials(Program* program, std::span<const uint8_t> original,
                                     std::span<const BootMappedSegment> mapped,
                                     std::span<const uint8_t> record);
void RevokeBootCredentials(Program* program);
bool AdmitSourceBoundBootCredentials(Program* program, std::span<const uint8_t> source,
                                    std::span<const BootMappedSegment> mapped, std::span<const uint8_t> record);
// Strict policy seam. Never uses compatibility PID100 capabilities or JSON flags.
bool TryGetSourceBoundProcessAuthInfo(int64_t pid, std::array<uint64_t, 17>* out);
bool AuthorizeSourceBoundAppDbRegistration(int64_t pid, std::string_view name);
// Refuses retirement while owned guest threads remain. Does not unload anything.
bool DeactivateRuntimeProcessIdentity(const Program* owner);

// Explicit cohosts use their retained identity. Other null-owner/direct-game
// callers retain PID100; inactive registered owners return -1, never PID100.
int GetCurrentGuestProcessId();
// Native transport must not turn an unknown null-owner caller into boot PID100.
bool TryGetCurrentRetainedGuestProcessId(int* out);
bool IsGuestProcessIdentityActive(int pid);
// PID zero means the calling application. Every other PID is explicit.
// All failed queries preserve outputs.
bool TryGetGuestProcessIdentity(int64_t pid, GuestProcessIdentity* out);
bool TryGetGuestProcessCategory(int64_t pid, uint32_t* out);
bool TryGetGuestProcessAppId(int64_t pid, int32_t* out);
bool TryGetGuestProcessAuthInfo(int64_t pid, std::array<uint64_t, 17>* out);
} // namespace Loader
