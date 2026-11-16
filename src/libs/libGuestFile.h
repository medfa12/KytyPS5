#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBGUESTFILE_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBGUESTFILE_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace Libs::GuestFile {

// Opaque guest FILE* backed by a host FILE* (for the _Fiopen/fread/
// fclose family, e.g. JSCd reading //common/cert/CA_LIST.cer). Guest code
// must only pass the pointer back to these helpers. Non-shim pointers
// (Sony FILE structs from managed runtimes) fail soft with 0/NULL.

// Opens a guest path for reading/writing; never truncates unless the
// caller passes TRUNC. Returns nullptr on failure (logs the reason).
void* Open(const char* guest_path, int flags);

// Returns items read (0 on any failure / non-shim stream).
size_t Read(void* stream, void* dst, size_t size, size_t count);
// Returns 0 on success; registered cross-family streams delegate to their
// supplied owner close function (no wrapper-layout guessing).
int Close(void* stream);
// Returns 0 on success (like fseek).
int Seek(void* stream, long offset, int whence);
// Returns file position, -1L on failure.
long Tell(void* stream);
// Returns byte or -1 (EOF) on failure.
int Getc(void* stream);

// Cross-family shims: the guest mixes fopen from one family with fread from
// another (JSCd RecordFile opens via GuestFopen, reads via CFread). Foreign
// shims register their (guest-pointer -> host FILE*) mapping here; lookups
// are exact-pointer (never probed), so a miss safely means "not ours". The
// optional close function unregisters and destroys the owning family's shim.
using ForeignClose = int (*)(void*);
void RegisterForeignShim(const void* shim, FILE* host, ForeignClose close = nullptr);
void UnregisterForeignShim(const void* shim);
FILE* ForeignShimHost(const void* shim);
// Exact registered-owner lookup in both direct-title and VSH chain boots;
// never accepts raw host FILE pointers or probes guest FILE memory.
FILE* RegisteredForeignShimHost(const void* shim);
// Our own registered shims, for the reverse direction (libC readers).
FILE* OwnShimHost(const void* shim);
// Diagnostic only: guest path recorded by Open() for one of our shims
// (nullptr for foreign/Sony streams). Never probes foreign memory.
const char* ShimPath(const void* shim);

} // namespace Libs::GuestFile

#endif
