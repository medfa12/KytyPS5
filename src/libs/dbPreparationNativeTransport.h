#pragma once
#include "libs/npNativeTransport.h"
namespace Libs::DbPreparationNativeTransport {
using Hooks = NpNativeTransport::Hooks;
using ServerConfig = NpNativeTransport::ServerConfig;
using DataInfo = NpNativeTransport::DataInfo;
using BufferInfo = NpNativeTransport::BufferInfo;
inline constexpr char ServiceName[] = "SceDbPreparationServer";
int Install(Hooks hooks) noexcept;
int Uninstall() noexcept;
int RetryRetirements() noexcept;
bool Installed() noexcept;
int KYTY_SYSV_ABI CreateServer(void**, const ServerConfig*, void*, void*) noexcept;
int KYTY_SYSV_ABI CreateClient(void**, const void*, void*, void*) noexcept;
}
