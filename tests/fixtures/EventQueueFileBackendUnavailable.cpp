#include "kernel/fileSystem.h"

#include <cstdlib>

// Lightweight queue/layer fixtures never register descriptor-backed writes.
// Abort if that assumption changes; the full-runtime kernel_write_events test
// links and exercises the real retained-file backend.
namespace Libs::LibKernel::FileSystem {
int PinWriteEventFile(int, size_t, std::shared_ptr<File>*) { std::abort(); }
int PollWriteEventFile(const std::shared_ptr<File>&) { std::abort(); }
} // namespace Libs::LibKernel::FileSystem
