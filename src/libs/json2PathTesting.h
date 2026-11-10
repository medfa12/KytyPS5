#pragma once

#if defined(KYTY_JSON_RESOURCE_LOOKUP_TESTS)
#include "common/abi.h"
#include <cstddef>
#include <cstdint>

namespace Libs::LibJson2::Testing {

// Calling-thread fixture seams only; absent from the emulator target. Read/close
// replacements must obey the kernel ABI and close must not throw. These are not
// guest initializer/allocator/reader callbacks. Empty hooks use production I/O.
struct PathParserHooks {
	int64_t (KYTY_SYSV_ABI *read)(int, void*, size_t) = nullptr;
	int (KYTY_SYSV_ABI *close)(int) = nullptr;
	void (*before_materialize)() = nullptr;
};
void SetPathParserHooks(const PathParserHooks& hooks);

struct ValueOwnership {
	const void* parent;
	const void* rootparam;
};
// Observe actual HLE fields on values produced through the registered ABI.
ValueOwnership GetValueOwnership(const void* value);
// Seed an opaque, non-dereferenced token solely to test retention on commit.
// This neither initializes nor implements native root/allocator callbacks.
void SetValueRootParamForTest(void* value, void* token);

} // namespace Libs::LibJson2::Testing
#endif
