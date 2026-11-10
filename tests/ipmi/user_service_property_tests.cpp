#include "libs/userServiceProperty.h"
#include "common/abi.h"
#include <cstdlib>
#include <iostream>

#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)
static int error = 0, calls = 0;
static uint64_t flags = 0;
static int KYTY_SYSV_ABI Getter(int user, uint32_t property, uint64_t* output) {
	CHECK(user == -1 && property == 6);
	++calls;
	*output = flags;
	return error;
}
int main() {
	using namespace Libs::UserService;
	NativeBackendTable table;
	table.functions[2] = reinterpret_cast<uint64_t>(&Getter);
	auto invoke = [](uint64_t address, const uint64_t* args, size_t count) {
		CHECK(count == 3);
		using Function = int (KYTY_SYSV_ABI *)(int, uint32_t, uint64_t*);
		return reinterpret_cast<Function>(address)(static_cast<int>(args[0]), static_cast<uint32_t>(args[1]), reinterpret_cast<uint64_t*>(args[2]));
	};
	struct { int before = 17, value = 23, after = 31; } output;
	CHECK(ReadNativeProperty6(0, -1, &output.value, invoke) == static_cast<int>(0x80960002u));
	CHECK(ReadNativeProperty6(table.functions[2], -1, nullptr, invoke) == static_cast<int>(0x80960005u));
	CHECK(calls == 0 && output.value == 23);
	flags = 0x1234567880000021ULL;
	CHECK(ReadNativeProperty6(table.functions[2], -1, &output.value, invoke) == 0);
	CHECK(static_cast<uint32_t>(output.value) == 0x80000021u && output.before == 17 && output.after == 31);
	for (int e: {static_cast<int>(0x80960002u), static_cast<int>(0x80960004u), -7}) {
		error = e; flags = 0;
		CHECK(ReadNativeProperty6(table.functions[2], -1, &output.value, invoke) == e);
		CHECK(static_cast<uint32_t>(output.value) == 0x80000021u);
	}
	error = 0; flags = 0;
	CHECK(ReadNativeProperty6(table.functions[2], -1, &output.value, invoke) == 0 && output.value == 0);
	CHECK(calls == 5 && output.before == 17 && output.after == 31);
	std::cout << "UserService property6: native ABI, 64-bit staging, exact output and error propagation passed\n";
}
