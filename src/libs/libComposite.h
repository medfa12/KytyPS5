#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBCOMPOSITE_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBCOMPOSITE_H_

#include <cstdint>
#include <string>

namespace Libs::Composite::CompositeHost {

// Allocates a canvas for a scene (Render.<title> : AppScene) and returns its
// handle. Backed by the same table guest GetCanvasHandle uses.
uint64_t CanvasForScene(const std::string& scene);

// RNPS bring-up: dedicated 1280x720x4 host surface (zeroed) + submit it
// through the normal present path. Rasterizers draw into RnSurfaceAddr().
uint64_t RnSurfaceAddr();
void     RnPresent();

} // namespace Libs::Composite::CompositeHost

#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBCOMPOSITE_H_ */
