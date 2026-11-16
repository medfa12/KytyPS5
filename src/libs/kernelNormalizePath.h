#pragma once

#include "common/abi.h"

#include <cstdint>

namespace Loader { class SymbolDatabase; }

namespace Libs::KernelNormalizePath {
// libkernel KB7Vc2+5S54: lexical guest path normalization, NOT realpath and
// never a host-path translation, directory creation or credential operation.
int KYTY_SYSV_ABI Normalize(const char* source, uint64_t source_bytes, char* destination, uint64_t destination_bytes);
void InitKernelNormalizePath_1(Loader::SymbolDatabase* symbols);
} // namespace Libs::KernelNormalizePath
