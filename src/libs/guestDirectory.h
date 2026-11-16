#pragma once

namespace Libs::GuestDirectory {
// Opaque libc DIR streams over the guest filesystem, not host CRT DIR objects.
void* Open(const char* path);
void* Read(void* stream);
int Close(void* stream);
} // namespace Libs::GuestDirectory
