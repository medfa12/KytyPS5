#include "libs/libcSort.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Libs::LibcInternal {

void __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI
Qsort(void* base, size_t count, size_t size, SortComparator compare) {
	if (count < 2 || size == 0 || base == nullptr || compare == nullptr ||
	    count > std::numeric_limits<size_t>::max() / size) {
		return;
	}
	auto* bytes = static_cast<uint8_t*>(base);
	const auto element = [&](size_t index) { return bytes + index * size; };
	const auto swap = [&](size_t left, size_t right) {
		std::swap_ranges(element(left), element(left) + size, element(right));
	};
	// In-place heapsort avoids a host-ABI comparator thunk and global callback
	// state. Comparators may recursively sort another array. Every argument
	// points to an actual array element, including for large/unaligned records.
	const auto sift = [&](size_t root, size_t end) {
		while (root < end / 2) {
			size_t child = root * 2 + 1;
			if (child + 1 < end && compare(element(child), element(child + 1)) < 0) {
				++child;
			}
			if (compare(element(root), element(child)) >= 0) {
				break;
			}
			swap(root, child);
			root = child;
		}
	};
	for (size_t root = count / 2; root != 0;) {
		sift(--root, count);
	}
	for (size_t end = count; end > 1;) {
		swap(0, --end);
		sift(0, end);
	}
}

} // namespace Libs::LibcInternal
