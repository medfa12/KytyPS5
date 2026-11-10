#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_GUESTHEAP_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_GUESTHEAP_H_

#include <cstddef>

// Guest-visible heap for libc/C++ runtime HLE allocations.
//
// Firmware packs heap pointers into narrow fields (13.60 AgcCompositor's
// hash_table encodes object pointers into 40 bits and asserts otherwise).
// The host CRT heap can live above 2^40, so guest allocations come from
// guest-range arenas instead. Host-side containers keep using the CRT.
namespace Libs::GuestHeap {

// Returns nullptr on exhaustion. Alignment must be a power of two.
void* Allocate(size_t size, size_t alignment = 16);
// True when ptr is the start of a live guest-heap block (and frees it).
bool Release(void* ptr);
// Size of a live block, 0 when ptr is not a guest-heap block.
size_t BlockSize(const void* ptr);
bool   Owns(const void* ptr);

// Guest free/realloc entry points: guest-heap blocks are recycled here; any other
// pointer (CRT blocks handed out before or outside this heap) goes to the CRT.
void  Free(void* ptr);
void* Reallocate(void* ptr, size_t size);
void* AllocateZeroed(size_t count, size_t size);

} // namespace Libs::GuestHeap

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_GUESTHEAP_H_ */
