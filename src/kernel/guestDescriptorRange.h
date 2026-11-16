#pragma once

namespace Libs::GuestDescriptors {
// HLE allocation policy, not native descriptor-number fidelity. Files may grow
// beyond SocketEnd; this reserves the already-existing Net table without races.
inline constexpr int SocketBegin = 128;
inline constexpr int SocketEnd = 1024;
constexpr bool IsSocketRange(int fd) { return fd >= SocketBegin && fd < SocketEnd; }
} // namespace Libs::GuestDescriptors
