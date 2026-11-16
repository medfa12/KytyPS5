// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef KYTY_LIBS_RUNTIME_USER_SERVICE_H_
#define KYTY_LIBS_RUNTIME_USER_SERVICE_H_

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace Loader { struct Program; }

namespace Libs::UserService {
// Host-only operations for explicitly selected local-account HLE. No NP
// enrollment or login is implied by creation. Guest records are session-only.
bool LocalAccountHleEnabled();
int ChangeEventCallback(uint64_t callback, uint64_t userdata, bool add);
int CreateLocalUser(std::string_view name, int type, bool incomplete, int* user);
int DestroyLocalUser(int user);
int SetLocalLoginFlag(int user, uint32_t mask, uint32_t value, uint32_t* output);
int GetLocalLoginFlag(int user, uint32_t* output);
int SetLocalUserStatus(int user, uint32_t mask, uint32_t value, uint32_t* output);
int GetLocalUserStatus(int user, uint32_t* output);
// Audited 32-bit UserService properties:3 auto-login,5 last-login-order.
int GetStoredScalar(int user, uint32_t property, uint32_t* output);
// Copies the persisted UTF-8 passcode, including NUL, to the guest buffer.
int GetStoredPasscode(int user, char* output, size_t size);
// Host-only local profile snapshot for the explicitly selected local NP HLE.
// Account ID zero means no enrollment; this never establishes online sign-in.
// Uses the application's prepared identities when application-owned.
int QueryLocalAccountId(int user, uint64_t* account_id);
// Close native-property callback admission before releasing boot mappings.
// False retains the owner until outstanding same-owner calls have drained.
bool ReleaseNativeUserServiceBackend(Loader::Program* owner);
// HOST ONLY. Call after session UserService identity has been loaded, before
// publishing/entering the application. Does not load a store or log anyone in.
// Captures the current foreground user as this application's initial user.
// Returns OK or a USER_SERVICE_ERROR_* code. Duplicate preparation is rejected.
int PrepareUserServiceOwner(Loader::Program* owner) noexcept;
// HOST ONLY. Call after closing admission and actual native-thread/lease drain.
// Refuses live owners; never stops execution or changes the session/profile.
int RetireUserServiceOwner(Loader::Program* owner) noexcept;
} // namespace Libs::UserService

#endif
