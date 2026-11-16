#include "libs/libcSort.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {
void Check(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "libc sort: %s\n", message);
		std::exit(1);
	}
}

int KYTY_SYSV_ABI CompareInt(const void* a, const void* b) {
	int x, y;
	std::memcpy(&x, a, sizeof(x));
	std::memcpy(&y, b, sizeof(y));
	return (x > y) - (x < y);
}

int KYTY_SYSV_ABI CompareNested(const void* a, const void* b) {
	int inner[] {7, -1, 4, 4, 0};
	Libs::LibcInternal::Qsort(inner, 5, sizeof(int), CompareInt);
	Check(std::is_sorted(std::begin(inner), std::end(inner)), "nested comparator sort");
	return CompareInt(a, b);
}

int KYTY_SYSV_ABI CompareBytes(const void* a, const void* b) {
	return std::memcmp(a, b, 3);
}
}

int main() {
	using Libs::LibcInternal::Qsort;
	std::mt19937 random(12345);
	for (size_t count: {0u, 1u, 2u, 3u, 16u, 127u, 256u, 1025u}) {
		std::vector<int> values(count);
		for (auto& value: values) { value = static_cast<int>(random() % 101) - 50; }
		auto expected = values;
		std::sort(expected.begin(), expected.end());
		Qsort(values.data(), values.size(), sizeof(int), CompareNested);
		Check(values == expected, "order and multiset with duplicates/negative keys");
		std::reverse(values.begin(), values.end());
		Qsort(values.data(), values.size(), sizeof(int), CompareInt);
		Check(values == expected, "reverse-sorted input");
	}
	std::array<unsigned char, 17> odd {0xa5, 9, 1, 2, 3, 4, 5, 9, 0, 1, 0, 2, 3, 3, 4, 6, 0x5a};
	Qsort(odd.data() + 1, 5, 3, CompareBytes);
	Check(odd.front() == 0xa5 && odd.back() == 0x5a, "unaligned record canaries");
	for (size_t i = 1; i < 5; ++i) {
		Check(std::memcmp(odd.data() + 1 + (i - 1) * 3, odd.data() + 1 + i * 3, 3) <= 0,
		      "odd-sized records");
	}
	struct Record { int key; std::array<unsigned char, 0x444> payload; };
	std::array<Record, 256> records {};
	for (size_t i = 0; i < records.size(); ++i) {
		records[i].key = static_cast<int>(255 - i);
		records[i].payload.fill(static_cast<unsigned char>(255 - i));
	}
	Qsort(records.data(), records.size(), sizeof(Record), CompareInt);
	for (size_t i = 0; i < records.size(); ++i) {
		Check(records[i].key == static_cast<int>(i) &&
		      std::all_of(records[i].payload.begin(), records[i].payload.end(),
		                  [i](unsigned char byte) { return byte == i; }), "large record integrity");
	}
	Qsort(nullptr, 0, sizeof(int), nullptr);
	std::puts("libc sort tests passed");
}
