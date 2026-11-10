#pragma once

#if defined(KYTY_JSON_RESOURCE_LOOKUP_TESTS)
#include <cstddef>

namespace Libs::LibJson2::Testing {

struct IteratorAllocations {
	std::size_t allocated {};
	std::size_t released {};
	std::size_t pairs_allocated {};
	std::size_t pairs_released {};
};

// Calling-thread snapshots of actual JsonObjectIteratorState new/delete calls.
// No resets, guest exports, or production-emulator instrumentation.
IteratorAllocations GetIteratorAllocations();

} // namespace Libs::LibJson2::Testing
#endif
