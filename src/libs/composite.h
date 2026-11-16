#ifndef LIBS_COMPOSITE_H
#define LIBS_COMPOSITE_H
#include <cstdint>

namespace Libs::Composite {
// Preserve the native command encoder for canvases not owned by MediaFramework.
void SetNativeVirtualCanvasDraw(uint64_t address);
void SetNativeTexturePatch(uint64_t address);
void SetNativeCanvasScratch(uint64_t address);
} // namespace Libs::Composite
#endif
