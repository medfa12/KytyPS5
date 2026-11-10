#pragma once

#include "common/abi.h"
#include <cstdint>

namespace Libs::ShellHits::Kernel {
// Shared by early and late providers so exact and NID-only resolution retain
// the same descriptor-aware ioctl dispatch and existing socket operations.
int KYTY_SYSV_ABI Ioctl(uint64_t fd, uint64_t request, uint64_t argument,
                        uint64_t a3, uint64_t a4, uint64_t a5);
}
