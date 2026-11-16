#ifndef KYTY_LIBS_LIBCSORT_H_
#define KYTY_LIBS_LIBCSORT_H_

#include "common/abi.h"
#include <cstddef>

namespace Libs::LibcInternal {

using SortComparator = int (KYTY_SYSV_ABI *)(const void*, const void*);
void KYTY_SYSV_ABI Qsort(void* base, size_t count, size_t size, SortComparator compare);

} // namespace Libs::LibcInternal
#endif
