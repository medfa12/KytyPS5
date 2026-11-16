#pragma once

#include "libs/npNativeTransport.h"

namespace Libs::LoginMgrNativeTransport {

// Explicitly installed service, independent from SceNpMgrIpc. A caller must
// supply owner-pinned, current-dispatcher-thread guest boundaries and genuine
// guest identity/authorization. No LoginMgr/account state is implemented here.
using Hooks = NpNativeTransport::Hooks;
using ServerConfig = NpNativeTransport::ServerConfig;
using DataInfo = NpNativeTransport::DataInfo;
using BufferInfo = NpNativeTransport::BufferInfo;
inline constexpr char ServiceName[] = "SceLoginMgrServer";

int Install(Hooks hooks) noexcept;
int Uninstall() noexcept; // Busy retains hooks, registry and owner pin.
int RetryRetirements() noexcept; // Same-owner failed-factory rollback only.
bool Installed() noexcept;
int KYTY_SYSV_ABI CreateServer(void** out, const ServerConfig* config, void* argument, void* storage) noexcept;
int KYTY_SYSV_ABI CreateClient(void** out, const void* config, void* argument, void* storage) noexcept;

} // namespace Libs::LoginMgrNativeTransport
