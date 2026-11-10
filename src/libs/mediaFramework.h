#ifndef LIBS_MEDIA_FRAMEWORK_H
#define LIBS_MEDIA_FRAMEWORK_H

#include "libs/mediaFrameworkEsvm.h"

#include <cstdint>
#include <vector>

namespace Libs::MediaFramework {
// Eight-dword HLE replacement for Composite's virtual-canvas command (type 6).
inline constexpr uint32_t CanvasPacket = 0x4b4d4657;
bool                      TraceEnabled();
// Includes retired IDs, so queued commands never reach the native encoder.
bool OwnsCanvas(int meta_id);
bool IsCanvasLive(int meta_id);
// Tagged T# addresses are host media handles, never guest-memory pointers.
bool BuildCanvasTexture(int meta_id, uint32_t (&descriptor)[8]);
int  CanvasFromTextureAddress(uint64_t address);
// Called by the compositor command executor, never by the presentation overlay.
// Produces a snapshot owned by the caller; source deletion cannot invalidate it.
bool CopyCanvasFrame(int meta_id, uint32_t width, uint32_t height, std::vector<uint8_t>& rgba);
} // namespace Libs::MediaFramework

#endif
