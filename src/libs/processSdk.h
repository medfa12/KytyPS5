#pragma once

#include "common/abi.h"

#include <cstdint>

namespace Libs::LibKernel {

int32_t KYTY_SYSV_ABI KernelGetCompiledSdkVersion(uint32_t* out);
int32_t KYTY_SYSV_ABI KernelGetProsperoCompiledSdkVersion(uint32_t* out);

} // namespace Libs::LibKernel
