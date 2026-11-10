# Retain notices for pinned/bundled dependencies in binary installations.
# Presence of a notice is not a substitute for satisfying its other terms.
set(_kyty_notices
    LibAtrac9/LICENSE SDL3/LICENSE.txt SPIRV-Headers/LICENSE SPIRV-Tools/LICENSE
    Vulkan-Headers/LICENSE.md VulkanMemoryAllocator/LICENSE.txt
    ffmpeg-core/copyright fmt/LICENSE imgui/LICENSE.txt magic_enum/LICENSE
    nlohmann_json/LICENSE.MIT spdlog/LICENSE tracy/LICENSE xxHash/LICENSE
    stb/LICENSE cpuinfo/LICENSE cpuinfo/deps/clog/LICENSE winpthread/COPYING)
foreach(_notice IN LISTS _kyty_notices)
    get_filename_component(_dependency "${_notice}" DIRECTORY)
    install(FILES "${KYTY_THIRD_PARTY_DIR}/${_notice}" DESTINATION "licenses/${_dependency}")
endforeach()
install(FILES "${CMAKE_CURRENT_LIST_DIR}/../LICENSE" DESTINATION licenses/kyty)
