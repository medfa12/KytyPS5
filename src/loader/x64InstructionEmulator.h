#ifndef KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_
#define KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_

#include <cstddef>
#include <cstdint>
#include <span>

namespace Loader::X64InstructionEmulator {

[[nodiscard]] bool TryEmulate(void* native_context);
// Exact absolute qword MOV/XOR FS:[0x28] forms used by guest stack guards.
// Caller verifies guest code ownership and supplies that main's current TCB.
[[nodiscard]] bool IsFsTcbRead(std::span<const uint8_t> instruction);
[[nodiscard]] bool TryEmulateFsTcbRead(void* native_context, std::span<const uint8_t> instruction,
                                     std::span<const uint8_t> tcb);
// Completes a faulting load into EAX as a zero read and resumes after it.
void SkipLoad(void* native_context, size_t length);

// ps5-vsh: skip a single-byte guest `int3` (Sony `log; ud2; int3` padding).
// Caller must verify RIP belongs to guest code first (host asserts use
// int3 too). Returns false when the byte is not 0xCC or the runaway cap
// (100k) is hit, so real trap loops still die loudly.
[[nodiscard]] bool TrySkipInt3(void* native_context);

} // namespace Loader::X64InstructionEmulator

#endif /* KYTY_LOADER_X64_INSTRUCTION_EMULATOR_H_ */
