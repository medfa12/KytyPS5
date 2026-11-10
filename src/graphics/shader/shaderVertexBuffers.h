#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXBUFFERS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXBUFFERS_H_

#include "graphics/shader/shader.h"

namespace Libs::Graphics {

// Groups attribute descriptors into host vertex bindings without changing the descriptors.
// Returns false if resources_num is outside the fixed-size input arrays.
[[nodiscard]] bool ShaderBuildVertexBufferBindings(ShaderVertexInputInfo& info);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERVERTEXBUFFERS_H_ */
