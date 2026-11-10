cmake_minimum_required(VERSION 3.19...4.3)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
	set(CMAKE_POLICY_VERSION_MINIMUM 3.10 CACHE STRING "Minimum policy version for bundled dependencies")
endif()

get_property(KYTY_GENERATOR_IS_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT KYTY_GENERATOR_IS_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build" FORCE)
	set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

project(Kyty)

find_program(SCCACHE_PROGRAM sccache)
find_program(CCACHE_PROGRAM ccache)

foreach(lang IN ITEMS C CXX)
    # An explicitly empty launcher disables caching for this language.
    if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER)
        if(SCCACHE_PROGRAM)
            set(CMAKE_${lang}_COMPILER_LAUNCHER "${SCCACHE_PROGRAM}")
            message(STATUS "sccache enabled for ${lang}: ${SCCACHE_PROGRAM}")
        elseif(CCACHE_PROGRAM)
            set(CMAKE_${lang}_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
            message(STATUS "ccache enabled for ${lang}: ${CCACHE_PROGRAM}")
        endif()
    endif()
endforeach()

set(KYTY_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(KYTY_TESTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests")
set(KYTY_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty")

if(CMAKE_SYSTEM_NAME MATCHES ".*Linux")
	set(LINUX TRUE)
endif()

if (NOT (WIN32 OR LINUX OR APPLE))
    message(FATAL_ERROR "only Windows, Linux, and macOS builds are supported")
endif()

if(WIN32)
	# Prevent Windows headers from defining min/max macros in all targets.
	add_compile_definitions(NOMINMAX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include("${KYTY_SOURCE_DIR}/utils.cmake")
include(CTest)

option(KYTY_ENABLE_CLANG_TIDY "Run clang-tidy checks during builds" OFF)

set(KYTY_BUILD_ORIGIN "Source" CACHE STRING "Build provenance shown in diagnostics")
set_property(CACHE KYTY_BUILD_ORIGIN PROPERTY STRINGS Source Official Fork)
if(NOT KYTY_BUILD_ORIGIN STREQUAL "Source" AND NOT KYTY_BUILD_ORIGIN STREQUAL "Official" AND
   NOT KYTY_BUILD_ORIGIN STREQUAL "Fork")
	message(FATAL_ERROR "KYTY_BUILD_ORIGIN must be Source, Official, or Fork")
endif()

set(KYTY_RELEASE_TAG "" CACHE STRING "Release tag embedded in official builds")
set(KYTY_BUILD_REPOSITORY "" CACHE STRING "GitHub repository embedded in fork builds")
set(KYTY_OFFICIAL_BUILD OFF)
set(KYTY_FORK_BUILD OFF)
if(KYTY_BUILD_ORIGIN STREQUAL "Official")
	if(KYTY_RELEASE_TAG STREQUAL "")
		message(FATAL_ERROR "Official builds require KYTY_RELEASE_TAG")
	endif()
	set(KYTY_OFFICIAL_BUILD ON)
elseif(KYTY_BUILD_ORIGIN STREQUAL "Fork")
	if(KYTY_BUILD_REPOSITORY STREQUAL "")
		message(FATAL_ERROR "Fork builds require KYTY_BUILD_REPOSITORY")
	endif()
	set(KYTY_FORK_BUILD ON)
endif()

set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

include(TestBigEndian)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "only 64-bit builds are supported")
endif()

test_big_endian(BIG_ENDIAN)
if(BIG_ENDIAN)
	set(KYTY_ENDIAN KYTY_ENDIAN_BIG)
else()
	set(KYTY_ENDIAN KYTY_ENDIAN_LITTLE)
endif()

if(CMAKE_BUILD_TYPE MATCHES Debug)
	set(KYTY_BUILD KYTY_BUILD_DEBUG)
else()
	set(KYTY_BUILD KYTY_BUILD_RELEASE)
endif()

if(LINUX OR APPLE)
	# macOS rides the POSIX/Linux code paths until it gets a dedicated platform
	set(KYTY_PLATFORM KYTY_PLATFORM_LINUX)
else()
	set(KYTY_PLATFORM KYTY_PLATFORM_WINDOWS)
endif()

set(CLANG FALSE)
set(GCC FALSE)
set(KYTY_CLANG_CL FALSE)
get_filename_component(KYTY_CXX_COMPILER_NAME "${CMAKE_CXX_COMPILER}" NAME_WE)
string(TOLOWER "${KYTY_CXX_COMPILER_NAME}" KYTY_CXX_COMPILER_NAME)

if(CMAKE_CXX_COMPILER_ID MATCHES "^(Apple)?Clang$")
	set(CLANG TRUE)
	set(KYTY_COMPILER CLANG)
	if(WIN32 AND KYTY_CXX_COMPILER_NAME STREQUAL "clang-cl")
		set(KYTY_CLANG_CL TRUE)
		set(KYTY_LINKER LLD_LINK)
	elseif(APPLE)
		set(KYTY_LINKER LD64) # Apple's default linker; lld flags don't apply
	else()
		set(KYTY_LINKER LLD)
	endif()
	if ((CMAKE_AR MATCHES "llvm") AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 12.0.0) AND (CMAKE_CXX_COMPILER MATCHES "winlibs"))
		string(REPLACE "llvm-ar.exe" "ar.exe" CMAKE_AR ${CMAKE_AR}) # llvm-ar.exe 11.0.0 from winlibs is broken
	endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
	set(GCC TRUE)
	set(KYTY_COMPILER GCC)
	set(KYTY_LINKER LD)
else()
	message(FATAL_ERROR "only Clang and GCC are supported; found ${CMAKE_CXX_COMPILER_ID}")
endif()

set(KYTY_PROJECT_NAME "Emulator" CACHE STRING "Project name suffix")

string(TOLOWER ${KYTY_COMPILER} KYTY_COMPILER_ID)
string(TOLOWER ${KYTY_LINKER} KYTY_LINKER_ID)

if (CLANG AND (KYTY_LINKER STREQUAL LLD))
	set(KYTY_LD_OPTIONS "-fuse-ld=lld")
endif()

if (KYTY_LINKER STREQUAL LD AND NOT LINUX)
	set(KYTY_LD_OPTIONS "-Wl,--image-base=0x100000000000")
endif()

project(Kyty${KYTY_PROJECT_NAME}${CMAKE_BUILD_TYPE}${KYTY_COMPILER} VERSION 0.3.0)

include_directories(
	${KYTY_THIRD_PARTY_DIR}/gtest/include
	${KYTY_THIRD_PARTY_DIR}/gtest
	${KYTY_THIRD_PARTY_DIR}/fmt/include
	${KYTY_THIRD_PARTY_DIR}/magic_enum/include/magic_enum
	${PROJECT_BINARY_DIR}
	${KYTY_SOURCE_DIR}
)

set(KYTY_VERSION "${PROJECT_VERSION}")

configure_file(
  ${KYTY_SOURCE_DIR}/cmake_config.h.in
  ${PROJECT_BINARY_DIR}/cmake_config.h
  )

find_package(Git)

add_custom_target(KytyGitVersion
	COMMAND ${CMAKE_COMMAND}
		-D INPUT_FILE=${KYTY_SOURCE_DIR}/kytyGitVersion.h.in
		-D OUTPUT_FILE=${PROJECT_BINARY_DIR}/kytyGitVersion.h
		-D GIT_EXECUTABLE=${GIT_EXECUTABLE}
		-D GIT_WORKING_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}
		-P ${KYTY_SOURCE_DIR}/generate_version.cmake
	BYPRODUCTS ${PROJECT_BINARY_DIR}/kytyGitVersion.h
	COMMENT "Generate kytyGitVersion.h"
)

add_library(kyty_git_version INTERFACE)
add_dependencies(kyty_git_version KytyGitVersion)

if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 12.0.0)
	list(APPEND KYTY_IWYU
		kyty_emulator
		common
		#launcher
	)
	list(APPEND KYTY_CLANG_TIDY
		kyty_emulator
		#common
		launcher
	)
endif()

option(KYTY_BUILD_LAUNCHER "Build Qt launcher" ON)

config_compiler_and_linker()

add_subdirectory("${KYTY_THIRD_PARTY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/3rdparty")
add_subdirectory("${KYTY_SOURCE_DIR}/common" "${CMAKE_CURRENT_BINARY_DIR}/common")

file(GLOB kyty_emulator_src CONFIGURE_DEPENDS
	"${KYTY_SOURCE_DIR}/libs/*.cpp"
	"${KYTY_SOURCE_DIR}/libs/*.h"
	"${KYTY_SOURCE_DIR}/graphics/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/*.h"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/*.h"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/command_processor/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/command_processor/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/cache/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/cache/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/image/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/image/*.h"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/pipeline/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/renderer/pipeline/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/cfg/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/cfg/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/decode/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/decode/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/translate/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/frontend/translate/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/backend/spirv/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/backend/spirv/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/opcodes/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/opcodes/*.h"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/opcodes/*.inc"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/*.h"
	"${KYTY_SOURCE_DIR}/graphics/presentation/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/presentation/*.h"
	"${KYTY_SOURCE_DIR}/graphics/presentation/window/*.cpp"
	"${KYTY_SOURCE_DIR}/graphics/presentation/window/*.h"
	"${KYTY_SOURCE_DIR}/kernel/*.cpp"
	"${KYTY_SOURCE_DIR}/kernel/*.h"
	"${KYTY_SOURCE_DIR}/loader/*.cpp"
	"${KYTY_SOURCE_DIR}/loader/*.h"
)

find_program(KYTY_GLSLANG_VALIDATOR NAMES glslangValidator glslang REQUIRED)
set(gpu_tiler_shader_dir "${KYTY_SOURCE_DIR}/graphics/host_gpu/shaders")
set(gpu_tiler_generated_dir "${PROJECT_BINARY_DIR}/gpu_tiler_shaders")
set(gpu_tiler_shader_names
	standard256
	standard4
	standard4_3d
	standard64
	standard64_3d
	prt
	prt_3d
	render_target
	depth
	promote_d16
	demote_d16
	color_transform
	color_clear
)
file(GLOB gpu_tiler_shader_includes CONFIGURE_DEPENDS "${gpu_tiler_shader_dir}/gpu_tiler_*.inc")
foreach(shader_name IN LISTS gpu_tiler_shader_names)
	set(shader_source "${gpu_tiler_shader_dir}/gpu_tiler_${shader_name}.comp")
	set(shader_spv "${gpu_tiler_generated_dir}/gpu_tiler_${shader_name}.spv")
	set(shader_header "${gpu_tiler_generated_dir}/gpu_tiler_${shader_name}_spv.h")
	string(TOUPPER "GPU_TILER_${shader_name}_SPV" shader_symbol)
	add_custom_command(
		OUTPUT "${shader_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_tiler_generated_dir}"
		COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
		        "-I${gpu_tiler_shader_dir}" -o "${shader_spv}" "${shader_source}"
		COMMAND ${CMAKE_COMMAND} -DINPUT=${shader_spv} -DOUTPUT=${shader_header}
		        -DSYMBOL=${shader_symbol} -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
		DEPENDS "${shader_source}" ${gpu_tiler_shader_includes}
		VERBATIM
	)
	list(APPEND gpu_tiler_shader_headers "${shader_header}")
endforeach()

set(fault_buffer_shader_source "${gpu_tiler_shader_dir}/fault_buffer_process.comp")
set(fault_buffer_shader_spv "${gpu_tiler_generated_dir}/fault_buffer_process.spv")
set(fault_buffer_shader_header "${gpu_tiler_generated_dir}/fault_buffer_process_spv.h")
add_custom_command(
	OUTPUT "${fault_buffer_shader_header}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_tiler_generated_dir}"
	COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
	        -DCACHING_PAGEBITS=14 -DMAX_PAGE_FAULTS=1024
	        -o "${fault_buffer_shader_spv}" "${fault_buffer_shader_source}"
	COMMAND ${CMAKE_COMMAND} -DINPUT=${fault_buffer_shader_spv} -DOUTPUT=${fault_buffer_shader_header}
	        -DSYMBOL=FAULT_BUFFER_PROCESS_SPV -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
	DEPENDS "${fault_buffer_shader_source}"
	VERBATIM
)
list(APPEND kyty_emulator_src "${fault_buffer_shader_header}")
list(APPEND kyty_emulator_src ${gpu_tiler_shader_headers})

set(gpu_blit_generated_dir "${PROJECT_BINARY_DIR}/gpu_blit_shaders")
function(add_gpu_blit_shader shader_name shader_file)
	set(shader_source "${gpu_tiler_shader_dir}/${shader_file}")
	set(shader_spv "${gpu_blit_generated_dir}/${shader_name}.spv")
	set(shader_header "${gpu_blit_generated_dir}/${shader_name}_spv.h")
	string(TOUPPER "${shader_name}_SPV" shader_symbol)
	add_custom_command(
		OUTPUT "${shader_header}"
		COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_blit_generated_dir}"
		COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os ${ARGN}
		        "-I${gpu_tiler_shader_dir}" -o "${shader_spv}" "${shader_source}"
		COMMAND ${CMAKE_COMMAND} -DINPUT=${shader_spv} -DOUTPUT=${shader_header}
		        -DSYMBOL=${shader_symbol} -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
		DEPENDS "${shader_source}" "${gpu_tiler_shader_dir}/gpu_video_out_hdr10_to_sdr.inc"
		VERBATIM
	)
	set(gpu_blit_shader_headers ${gpu_blit_shader_headers} "${shader_header}" PARENT_SCOPE)
endfunction()
add_gpu_blit_shader(gpu_blit_fs_triangle gpu_blit_fs_triangle.vert)
add_gpu_blit_shader(gpu_blit_color_to_ms_depth gpu_blit_color_to_ms_depth.frag)
add_gpu_blit_shader(gpu_video_out_overlay gpu_video_out_overlay.frag)
add_gpu_blit_shader(gpu_imgui_hdr gpu_video_out_overlay.frag -DIMGUI_HDR=1)
add_gpu_blit_shader(gpu_pssr_linear gpu_pssr_linear.comp)
# At SPIR-V 1.6 glslang declares a task shader's workgroup size with LocalSizeId, which needs maintenance4.
add_gpu_blit_shader(gpu_mesh_indirect gpu_mesh_indirect.task --target-env spirv1.5)
list(APPEND kyty_emulator_src ${gpu_blit_shader_headers})

list(APPEND kyty_emulator_src
	"${KYTY_SOURCE_DIR}/emulator.h"
	"${KYTY_SOURCE_DIR}/emulator.cpp"
)
list(REMOVE_DUPLICATES kyty_emulator_src)

if (KYTY_CLANG_CL)
	set_source_files_properties(${kyty_emulator_src} PROPERTIES COMPILE_FLAGS "-Wno-pragma-pack -Wno-deprecated-declarations -D_TIMESPEC_DEFINED")
	# ps5-vsh chainSUI148: guest calls arrive with misaligned stacks on some
	# JIT->jemspace chains; MSVC-ABI prologues spill callee-saved xmm6-15
	# with movaps and fault. Reserving those regs in the guest-printf TU
	# removes frame movaps there (no ABI change; entry points take no xmm
	# args). Verified via objdump (no %xmm6-15, no frame movaps).
	set_source_files_properties("${KYTY_SOURCE_DIR}/libs/guestPrintf.cpp" PROPERTIES COMPILE_FLAGS
		"-ffixed-xmm6 -ffixed-xmm7 -ffixed-xmm8 -ffixed-xmm9 -ffixed-xmm10 -ffixed-xmm11 -ffixed-xmm12 -ffixed-xmm13 -ffixed-xmm14 -ffixed-xmm15")
endif()

set(kyty_emulator_link_libraries common Vulkan::Headers spirv-tools-opt spirv-tools SDL3::SDL3-static imgui xxhash FFmpeg::ffmpeg fmt::fmt nlohmann_json::nlohmann_json LibAtrac9 kyty_git_version Zydis::Zydis xbyak::xbyak opus OpenXR::openxr_loader libzstd_static)
list(APPEND kyty_emulator_link_libraries kyty_sqlite)

add_executable(local_appdb_tests EXCLUDE_FROM_ALL
    "${KYTY_TESTS_DIR}/LocalAppDbTests.cpp" "${KYTY_SOURCE_DIR}/loader/runtimeLocalAppDb.cpp"
    "${KYTY_SOURCE_DIR}/loader/runtimeLocalSystemCatalog.cpp"
    "${KYTY_TESTS_DIR}/LocalHomeQueryTests.cpp" "${KYTY_SOURCE_DIR}/loader/runtimeLocalHomeQuery.cpp"
    "${KYTY_TESTS_DIR}/LocalSystemCatalogCommitTests.cpp"
    "${KYTY_TESTS_DIR}/LocalDbPreparationTests.cpp" "${KYTY_TESTS_DIR}/LocalNativeCatalogTests.cpp"
    "${KYTY_TESTS_DIR}/LocalCoreAppInfoTests.cpp" "${KYTY_SOURCE_DIR}/loader/runtimeLocalCoreAppInfo.cpp"
    "${KYTY_SOURCE_DIR}/loader/runtimeLocalDbPreparation.cpp" "${KYTY_SOURCE_DIR}/loader/runtimeLocalNativeCatalog.cpp"
    "${KYTY_TESTS_DIR}/LocalDbRecoveryTests.cpp" "${KYTY_SOURCE_DIR}/loader/runtimeLocalDbRecovery.cpp")
target_include_directories(local_appdb_tests PRIVATE "${KYTY_SOURCE_DIR}")
target_link_libraries(local_appdb_tests PRIVATE kyty_sqlite)
add_test(NAME local_appdb_tests COMMAND local_appdb_tests)

# SHM prepares allocation-prone metadata before host mutation and translates
# allocation exceptions into ENOMEM in both production and test compilations.
if(KYTY_CLANG_CL)
	set(kyty_shm_exception_option /EHsc)
else()
	set(kyty_shm_exception_option -fexceptions)
endif()
set_property(SOURCE "${KYTY_SOURCE_DIR}/kernel/memory.cpp"
	"${KYTY_SOURCE_DIR}/kernel/fileSystem.cpp" APPEND PROPERTY
	COMPILE_OPTIONS ${kyty_shm_exception_option})

if(WIN32)
	list(APPEND kyty_emulator_link_libraries bcrypt)
endif()

# Linux system libraries.
if(LINUX)
	find_package(Threads REQUIRED)
	list(APPEND kyty_emulator_link_libraries m ${CMAKE_DL_LIBS} Threads::Threads)
endif()

set(inc_headers
	${KYTY_SOURCE_DIR}
	${KYTY_THIRD_PARTY_DIR}/VulkanMemoryAllocator/include
	${KYTY_THIRD_PARTY_DIR}/SPIRV-Tools/include
	${KYTY_THIRD_PARTY_DIR}/SPIRV-Headers/include
	${KYTY_THIRD_PARTY_DIR}/fmt/include
	${KYTY_THIRD_PARTY_DIR}/xxHash
	${KYTY_THIRD_PARTY_DIR}/cpuinfo/include
	${KYTY_THIRD_PARTY_DIR}/renderdoc
	${KYTY_THIRD_PARTY_DIR}/stb
)

if (KYTY_CLANG_CL)
	list(APPEND kyty_emulator_link_libraries winpthread)
	list(APPEND inc_headers
		${KYTY_THIRD_PARTY_DIR}/winpthread/include
	)
endif()

list(APPEND check_headers
	${KYTY_SOURCE_DIR}
)

function(add_kyty_full_emulator_test target source)
	add_executable(${target} EXCLUDE_FROM_ALL ${source} ${kyty_emulator_src})
	target_link_libraries(${target} ${kyty_emulator_link_libraries})
	target_include_directories(${target} PRIVATE ${inc_headers})
	if (WIN32)
		target_link_libraries(${target} iphlpapi winhttp)
	endif()
	if (KYTY_CLANG_CL)
		target_link_libraries(${target} onecore)
		add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" $<TARGET_FILE_DIR:${target}>/libwinpthread-1.dll)
	endif()
	# The macOS x86_64 guest address space needs its .zerofill segments anchored
	# by linker flags, or the kernel kills the binary on load (posix_spawn EIO).
	configure_macos_guest_address_space(${target})
endfunction()

if(WIN32)
	add_executable(host_http_tests EXCLUDE_FROM_ALL
		"${KYTY_TESTS_DIR}/HostHttpTests.cpp" "${KYTY_SOURCE_DIR}/libs/hostHttp.cpp")
	target_include_directories(host_http_tests PRIVATE "${KYTY_SOURCE_DIR}")
	target_link_libraries(host_http_tests winhttp)
	add_executable(rnps_curl_tests EXCLUDE_FROM_ALL
		"${KYTY_TESTS_DIR}/RnpsCurlTests.cpp" "${KYTY_SOURCE_DIR}/libs/rnpsCurl.cpp"
		"${KYTY_SOURCE_DIR}/libs/hostHttp.cpp")
	target_include_directories(rnps_curl_tests PRIVATE "${KYTY_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
	target_link_libraries(rnps_curl_tests winhttp)
endif()

function(configure_macos_guest_address_space target)
	if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR
			(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")))
		target_sources(${target} PRIVATE "${KYTY_SOURCE_DIR}/kernel/macosGuestAddressSpace.cpp")
		target_compile_definitions(${target} PRIVATE KYTY_LINKED_GUEST_ADDRESS_SPACE=1)
		target_link_options(${target} PRIVATE
			-Wl,-ld_classic,-no_pie,-no_fixup_chains,-no_huge,-pagezero_size,0x40000,-segaddr,SYSTEM_MANAGED,0x40000,-segaddr,SYSTEM_RESERVED,0x7ffffc000,-segaddr,USER_AREA,0x7000000000,-image_base,0x700000000000)
	endif()
endfunction()

add_kyty_full_emulator_test(shader_cfg_tests "${KYTY_TESTS_DIR}/shaderCfgTests.cpp")
add_kyty_full_emulator_test(lnc_app_status_tests "${KYTY_TESTS_DIR}/LncAppStatusTests.cpp")

add_executable(scalar_provenance_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ScalarProvenanceTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ReadLaneElimination.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
)
target_link_libraries(scalar_provenance_tests fmt::fmt xbyak::xbyak)
target_include_directories(scalar_provenance_tests PRIVATE ${inc_headers})

add_executable(build_flags_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/BuildFlagsTests.cpp")

add_executable(page_manager_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PageManagerTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
)
target_link_libraries(page_manager_tests common)
target_include_directories(page_manager_tests PRIVATE ${inc_headers})

add_executable(archive_file_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ArchiveFileTests.cpp"
)
target_link_libraries(archive_file_tests common ZArchive::zarchive)
target_include_directories(archive_file_tests PRIVATE ${inc_headers})

add_executable(trophy_system_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/TrophySystemTests.cpp"
)
target_link_libraries(trophy_system_tests common)
target_include_directories(trophy_system_tests PRIVATE ${inc_headers})

add_executable(avplayer_file_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AvPlayerFileTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(avplayer_file_tests common FFmpeg::ffmpeg ZArchive::zarchive)
target_include_directories(avplayer_file_tests PRIVATE ${inc_headers})

add_executable(media_framework_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/MediaFrameworkTests.cpp")
target_link_libraries(media_framework_tests common FFmpeg::ffmpeg ZArchive::zarchive)
target_include_directories(media_framework_tests PRIVATE ${inc_headers})
add_executable(media_framework_esvm_decoder_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/MediaFrameworkEsvmDecoderTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/mediaFrameworkEsvmDecoder.cpp"
)
target_link_libraries(media_framework_esvm_decoder_tests FFmpeg::ffmpeg)
target_include_directories(media_framework_esvm_decoder_tests PRIVATE ${inc_headers})
add_executable(native_video_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/NativeVideoTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/presentation/nativeVideo.cpp"
)
target_include_directories(native_video_tests PRIVATE ${inc_headers})
add_executable(media_framework_esvm_certificate_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/MediaFrameworkEsvmCertificateTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/mediaFrameworkEsvmCertificate.cpp"
)
target_compile_definitions(media_framework_esvm_certificate_tests PRIVATE KYTY_ESVM_CERTIFICATE_TESTS)
target_include_directories(media_framework_esvm_certificate_tests PRIVATE ${inc_headers})
if (WIN32)
	target_link_libraries(media_framework_esvm_certificate_tests bcrypt)
endif()
add_executable(vdecsw_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/VdecswTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/videoDec2Decoder.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(vdecsw_tests common FFmpeg::ffmpeg)
target_include_directories(vdecsw_tests PRIVATE ${inc_headers})

add_executable(bit_array_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/BitArrayTests.cpp"
)
target_include_directories(bit_array_tests PRIVATE ${inc_headers})

add_executable(lru_cache_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/LruCacheTests.cpp"
)
target_include_directories(lru_cache_tests PRIVATE ${inc_headers})

add_executable(symbol_database_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SymbolDatabaseTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/symbolDatabase.cpp"
)
target_link_libraries(symbol_database_tests common fmt::fmt ZArchive::zarchive)
target_include_directories(symbol_database_tests PRIVATE ${inc_headers})

add_executable(static_executable_layout_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/StaticExecutableLayoutTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/staticExecutableLayout.cpp"
)
target_include_directories(static_executable_layout_tests PRIVATE ${inc_headers})

add_executable(agc_queue_identity_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AgcQueueIdentityTests.cpp"
)
target_include_directories(agc_queue_identity_tests PRIVATE ${inc_headers})
add_executable(slot_vector_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SlotVectorTests.cpp"
)
target_include_directories(slot_vector_tests PRIVATE ${inc_headers})

add_executable(ime_dialog_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ImeDialogTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/imeCommon.cpp"
	"${KYTY_SOURCE_DIR}/libs/imeDialog.cpp"
)
target_include_directories(ime_dialog_tests PRIVATE ${inc_headers})

add_executable(memory_tracker_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/MemoryTrackerTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/pageManager.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/memoryTracker.cpp"
)
target_link_libraries(memory_tracker_tests fmt::fmt common)
target_include_directories(memory_tracker_tests PRIVATE ${inc_headers})

add_executable(graphics_observe_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/GraphicsObserveTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/graphicsObserve.cpp"
)
target_include_directories(graphics_observe_tests PRIVATE ${inc_headers})

add_executable(libc_sort_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/LibcSortTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/libcSort.cpp"
)
target_include_directories(libc_sort_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(libc_scan_tests "${KYTY_TESTS_DIR}/LibcScanTests.cpp")

add_executable(ipmi_observe_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/IpmiObserveTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/ipmiObserve.cpp"
)
target_include_directories(ipmi_observe_tests PRIVATE ${inc_headers})

add_executable(shader_vertex_metadata_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ShaderVertexMetadataTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/shaderVertexMetadata.cpp"
)
target_include_directories(shader_vertex_metadata_tests PRIVATE ${inc_headers})

add_executable(resource_materialization_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ResourceMaterializationTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
)
target_link_libraries(resource_materialization_tests fmt::fmt xbyak::xbyak)
target_include_directories(resource_materialization_tests PRIVATE ${inc_headers})

add_executable(resource_tracking_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ResourceTrackingTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/guest_gpu/gpu_format.cpp"
	"${KYTY_SOURCE_DIR}/graphics/host_gpu/hostMemory.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/shaderPixelParameter.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/SrtWalker.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceTracking.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ResourceMaterialization.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/recompiler/ir/passes/ShaderInfoCollection.cpp"
)
target_link_libraries(resource_tracking_tests fmt::fmt xbyak::xbyak)
target_include_directories(resource_tracking_tests PRIVATE ${inc_headers})

add_executable(audio_out2_port_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AudioOut2PortTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/libAudio2.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(audio_out2_port_tests common fmt::fmt)
target_include_directories(audio_out2_port_tests PRIVATE ${inc_headers})

add_executable(audio_out_timing_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/AudioOutTimingTests.cpp")
target_link_libraries(audio_out_timing_tests common fmt::fmt SDL3::SDL3-static)
target_include_directories(audio_out_timing_tests PRIVATE ${inc_headers})

add_executable(ngs2_sampler_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/Ngs2SamplerTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(ngs2_sampler_tests common fmt::fmt LibAtrac9)
target_include_directories(ngs2_sampler_tests PRIVATE ${inc_headers})

add_executable(audiodec_atrac9_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AudiodecAtrac9Tests.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(audiodec_atrac9_tests common fmt::fmt LibAtrac9 FFmpeg::ffmpeg)
target_include_directories(audiodec_atrac9_tests PRIVATE ${inc_headers})
add_executable(ajm_resample_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AjmResampleTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(ajm_resample_tests common fmt::fmt FFmpeg::ffmpeg LibAtrac9 opus)
target_include_directories(ajm_resample_tests PRIVATE ${inc_headers})

add_executable(pad_haptics_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PadHapticsTests.cpp"
)
target_link_libraries(pad_haptics_tests common fmt::fmt SDL3::SDL3-static opus)
target_include_directories(pad_haptics_tests PRIVATE ${inc_headers})

add_executable(controller_settings_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ControllerSettingsTests.cpp"
)
target_link_libraries(controller_settings_tests common fmt::fmt SDL3::SDL3-static)
target_include_directories(controller_settings_tests PRIVATE ${inc_headers})

add_executable(video_out_status_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/VideoOutStatusTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/presentation/videoOutStatus.cpp"
)
target_link_libraries(video_out_status_tests common fmt::fmt)
target_include_directories(video_out_status_tests PRIVATE ${inc_headers})

add_executable(shader_vertex_buffer_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ShaderVertexBufferTests.cpp"
	"${KYTY_SOURCE_DIR}/graphics/shader/shaderVertexBuffers.cpp"
)
target_include_directories(shader_vertex_buffer_tests PRIVATE ${inc_headers})

add_executable(host_input_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/HostInputTests.cpp")
target_link_libraries(host_input_tests common fmt::fmt SDL3::SDL3-static)
target_include_directories(host_input_tests PRIVATE ${inc_headers})

add_executable(save_data_memory_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SaveDataMemoryTests.cpp"
)
target_link_libraries(save_data_memory_tests common fmt::fmt)
target_include_directories(save_data_memory_tests PRIVATE ${inc_headers})

add_executable(png_enc_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/PngEncTests.cpp")
target_link_libraries(png_enc_tests common fmt::fmt)
target_include_directories(png_enc_tests PRIVATE ${inc_headers})

add_executable(jpeg_dec_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/JpegDecTests.cpp")
target_link_libraries(jpeg_dec_tests common fmt::fmt)
target_include_directories(jpeg_dec_tests PRIVATE ${inc_headers})

add_executable(jpeg_enc_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/JpegEncTests.cpp")
target_link_libraries(jpeg_enc_tests common fmt::fmt)
target_include_directories(jpeg_enc_tests PRIVATE ${inc_headers})

add_executable(occlusion_queries_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/OcclusionQueriesTests.cpp")
target_link_libraries(occlusion_queries_tests common fmt::fmt Vulkan::Headers)
target_include_directories(occlusion_queries_tests PRIVATE ${inc_headers})

add_executable(ces_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/CesTests.cpp")
target_link_libraries(ces_tests common fmt::fmt SDL3::SDL3-static)
target_include_directories(ces_tests PRIVATE ${inc_headers})

add_executable(save_data_dialog_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SaveDataDialogTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/dialog.cpp"
)
target_link_libraries(save_data_dialog_tests common fmt::fmt)
target_include_directories(save_data_dialog_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(http_uri_parse_tests
	"${KYTY_TESTS_DIR}/HttpUriParseTests.cpp")

if(WIN32)
	add_kyty_full_emulator_test(network_http_tests "${KYTY_TESTS_DIR}/NetworkHttpTests.cpp")
endif()

add_kyty_full_emulator_test(network_unix_transport_tests
	"${KYTY_TESTS_DIR}/NetworkUnixTransportTests.cpp")

add_executable(event_queue_lifetime_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/EventQueueLifetimeTests.cpp"
	"${KYTY_TESTS_DIR}/fixtures/EventQueueFileBackendUnavailable.cpp"
	"${KYTY_SOURCE_DIR}/kernel/eventQueue.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp"
)
target_link_libraries(event_queue_lifetime_tests common fmt::fmt)
target_include_directories(event_queue_lifetime_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(kernel_file_system_tests
	"${KYTY_TESTS_DIR}/KernelFileSystemTests.cpp")
target_link_libraries(kernel_file_system_tests ZArchive::zarchive)
target_sources(kernel_file_system_tests PRIVATE "${KYTY_TESTS_DIR}/JsonResourceLookupTests.cpp")
target_sources(kernel_file_system_tests PRIVATE "${KYTY_TESTS_DIR}/JsonPathParserTests.cpp")
target_compile_definitions(kernel_file_system_tests PRIVATE KYTY_JSON_RESOURCE_LOOKUP_TESTS=1)
if(WIN32)
	target_compile_definitions(kernel_file_system_tests PRIVATE KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS=1)
	target_sources(kernel_file_system_tests PRIVATE "${KYTY_TESTS_DIR}/SocketpairTests.cpp")
	target_sources(kernel_file_system_tests PRIVATE "${KYTY_TESTS_DIR}/KernelSemaphoreProvenanceTests.cpp")
	target_sources(kernel_file_system_tests PRIVATE "${KYTY_TESTS_DIR}/KernelSemaphoreTests.cpp" "${KYTY_TESTS_DIR}/KernelSemaphoreGuestTests.cpp")
	target_sources(kernel_file_system_tests PRIVATE "${KYTY_TESTS_DIR}/KernelEventFlagLifetimeTests.cpp" "${KYTY_TESTS_DIR}/KernelEventFlagGuestTests.cpp")
	target_sources(kernel_file_system_tests PRIVATE "${KYTY_TESTS_DIR}/NetworkP2pOwnershipTests.cpp")
endif()

add_executable(sync_on_address_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/SyncOnAddressTests.cpp"
	"${KYTY_SOURCE_DIR}/kernel/syncOnAddress.cpp"
)
target_link_libraries(sync_on_address_tests common)
target_include_directories(sync_on_address_tests PRIVATE ${inc_headers})

add_executable(deferred_import_thunk_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/DeferredImportThunkTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/deferredImportThunk.cpp"
	"${KYTY_SOURCE_DIR}/loader/guestCallBoundary.cpp"
)
target_link_libraries(deferred_import_thunk_tests PRIVATE common xbyak::xbyak)
target_include_directories(deferred_import_thunk_tests PRIVATE ${inc_headers})

add_executable(guest_call_boundary_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/GuestCallBoundaryTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/guestCallBoundary.cpp"
)
target_include_directories(guest_call_boundary_tests PRIVATE ${inc_headers})

add_executable(host_call_return_thunk_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/HostCallReturnThunkTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/hostCallReturnThunk.cpp"
	"${KYTY_SOURCE_DIR}/loader/deferredImportThunk.cpp"
	"${KYTY_SOURCE_DIR}/loader/guestCallBoundary.cpp"
)
target_link_libraries(host_call_return_thunk_tests PRIVATE common xbyak::xbyak)
target_include_directories(host_call_return_thunk_tests PRIVATE ${inc_headers})

add_executable(image_page_table_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/ImagePageTableTests.cpp"
)
target_link_libraries(image_page_table_tests common fmt::fmt)
target_include_directories(image_page_table_tests PRIVATE ${inc_headers})

add_kyty_full_emulator_test(shader_recompiler_compute_tests "${KYTY_TESTS_DIR}/ShaderRecompilerComputeTests.cpp")

set(gpu_test_generated_dir "${PROJECT_BINARY_DIR}/gpu_test_shaders")
set(gpu_test_ms_depth_source
	"${gpu_tiler_shader_dir}/gpu_test_ms_depth.comp")
set(gpu_test_ms_depth_spv
	"${gpu_test_generated_dir}/gpu_test_ms_depth.spv")
set(gpu_test_ms_depth_header
	"${gpu_test_generated_dir}/gpu_test_ms_depth_spv.h")
add_custom_command(
	OUTPUT "${gpu_test_ms_depth_header}"
	COMMAND ${CMAKE_COMMAND} -E make_directory "${gpu_test_generated_dir}"
	COMMAND "${KYTY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -Os
	        -o "${gpu_test_ms_depth_spv}" "${gpu_test_ms_depth_source}"
	COMMAND ${CMAKE_COMMAND} -DINPUT=${gpu_test_ms_depth_spv}
	        -DOUTPUT=${gpu_test_ms_depth_header} -DSYMBOL=GPU_TEST_MS_DEPTH_SPV
	        -P "${KYTY_SOURCE_DIR}/embed_spirv.cmake"
	DEPENDS "${gpu_test_ms_depth_source}"
	VERBATIM
)
target_sources(shader_recompiler_compute_tests PRIVATE
	"${gpu_test_ms_depth_header}")

add_kyty_full_emulator_test(virtual_memory_allocation_tests "${KYTY_TESTS_DIR}/VirtualMemoryAllocationTests.cpp")
target_link_libraries(virtual_memory_allocation_tests ZArchive::zarchive)
# Compile the logger with this target's test hooks; the common archive's logger
# object is then unused. Production keeps the hook-free common implementation.
target_sources(virtual_memory_allocation_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/common/logging/log.cpp")
target_link_libraries(virtual_memory_allocation_tests spdlog::spdlog)
add_library(kyty_test_gtest STATIC EXCLUDE_FROM_ALL
	"${KYTY_THIRD_PARTY_DIR}/fmt/test/gtest/gmock-gtest-all.cc")
target_include_directories(kyty_test_gtest PUBLIC "${KYTY_THIRD_PARTY_DIR}/fmt/test/gtest")
target_sources(virtual_memory_allocation_tests PRIVATE "${KYTY_TESTS_DIR}/TrinityTests.cpp")
target_sources(virtual_memory_allocation_tests PRIVATE "${KYTY_TESTS_DIR}/SystemContentApplicationTests.cpp")
target_sources(virtual_memory_allocation_tests PRIVATE "${KYTY_TESTS_DIR}/LocalUserStorageTests.cpp")
if(WIN32)
	target_sources(virtual_memory_allocation_tests PRIVATE "${KYTY_TESTS_DIR}/subprocess/composed_frames_tests.cpp")
endif()
target_sources(virtual_memory_allocation_tests PRIVATE "${KYTY_TESTS_DIR}/SyncOnAddressTests.cpp")
target_compile_definitions(virtual_memory_allocation_tests PRIVATE KYTY_SYNC_ON_ADDRESS_GUEST_TESTS=1)
target_link_libraries(virtual_memory_allocation_tests kyty_test_gtest)

add_kyty_full_emulator_test(runtime_integer_callback_tests "${KYTY_TESTS_DIR}/RuntimeIntegerCallbackTests.cpp")
target_link_libraries(runtime_integer_callback_tests ZArchive::zarchive spdlog::spdlog)

# Opt-in firmware integration executable; never registered as an automatic test.
# Requires a generated importer, local firmware root and synthetic E-AC-3 input.
add_kyty_full_emulator_test(production_audiodec_tests "${KYTY_TESTS_DIR}/ProductionAudiodecTests.cpp")
target_link_libraries(production_audiodec_tests ZArchive::zarchive spdlog::spdlog)

add_kyty_full_emulator_test(np_account_abi_tests "${KYTY_TESTS_DIR}/NpAccountAbiTests.cpp")
target_link_libraries(np_account_abi_tests ZArchive::zarchive spdlog::spdlog)

add_kyty_full_emulator_test(net_resolver_tests "${KYTY_TESTS_DIR}/NetResolverTests.cpp")
target_link_libraries(net_resolver_tests kyty_test_gtest)

add_executable(psml_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/PsmlTests.cpp"
	"${KYTY_SOURCE_DIR}/libs/libPsml.cpp"
	"${KYTY_SOURCE_DIR}/loader/symbolDatabase.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp")
target_link_libraries(psml_tests common kyty_test_gtest)
target_include_directories(psml_tests PRIVATE ${inc_headers})
target_compile_definitions(virtual_memory_allocation_tests PRIVATE
	KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS=1
	KYTY_ESVM_KEYS_TESTS=1)

add_executable(gpu_blit_shader_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/GpuBlitShaderTests.cpp"
	"${gpu_blit_generated_dir}/gpu_mesh_indirect_spv.h")
target_link_libraries(gpu_blit_shader_tests kyty_test_gtest)
target_include_directories(gpu_blit_shader_tests PRIVATE ${inc_headers})

add_executable(scanout_layers_tests EXCLUDE_FROM_ALL "${KYTY_TESTS_DIR}/ScanoutLayersTests.cpp")
add_executable(runtime_application_plan_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/RuntimeApplicationPlanTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/runtimeApplicationPlan.cpp")
target_include_directories(runtime_application_plan_tests PRIVATE "${KYTY_SOURCE_DIR}")
target_link_libraries(runtime_application_plan_tests PRIVATE nlohmann_json::nlohmann_json)
add_executable(app_local_libraries_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/AppLocalLibrariesTests.cpp"
	"${KYTY_SOURCE_DIR}/loader/appLocalLibraries.cpp")
target_include_directories(app_local_libraries_tests PRIVATE "${KYTY_SOURCE_DIR}")
target_link_libraries(scanout_layers_tests kyty_test_gtest)
target_include_directories(scanout_layers_tests PRIVATE ${inc_headers})

add_executable(video_out_system_events_tests EXCLUDE_FROM_ALL
	"${KYTY_TESTS_DIR}/VideoOutSystemEventsTests.cpp"
	"${KYTY_TESTS_DIR}/fixtures/EventQueueFileBackendUnavailable.cpp"
	"${KYTY_SOURCE_DIR}/graphics/presentation/videoOutSystemEvents.cpp"
	"${KYTY_SOURCE_DIR}/kernel/eventQueue.cpp"
	"${KYTY_SOURCE_DIR}/loader/timer.cpp")
target_link_libraries(video_out_system_events_tests common fmt::fmt)
target_include_directories(video_out_system_events_tests PRIVATE ${inc_headers})

# These tests use exceptions.
if(NOT KYTY_CLANG_CL)
	foreach(kyty_exception_test scalar_provenance_tests resource_tracking_tests
			virtual_memory_allocation_tests graphics_observe_tests ipmi_observe_tests app_local_libraries_tests)
		target_compile_options(${kyty_exception_test} PRIVATE -fexceptions)
	endforeach()
endif()

if(BUILD_TESTING)
	if(WIN32)
		add_subdirectory("${KYTY_TESTS_DIR}/subprocess" "${CMAKE_CURRENT_BINARY_DIR}/subprocess-tests")
		add_kyty_full_emulator_test(vsh_subprocess_lnc_tests "${KYTY_TESTS_DIR}/subprocess/lnc_process_tests.cpp")
		target_link_libraries(vsh_subprocess_lnc_tests ZArchive::zarchive spdlog::spdlog)
		add_test(NAME vsh_subprocess_lnc_tokens COMMAND $<TARGET_FILE:vsh_subprocess_lnc_tests>)
		add_test(NAME runtime_child_status COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-child-status-only)
		add_test(NAME runtime_child_composed_pixels COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --composed-frames-only)
		set_tests_properties(runtime_child_composed_pixels PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
		set_tests_properties(runtime_child_status PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
		foreach(ending eof ack-failure window-loss)
			add_test(NAME runtime_child_status_${ending} COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-child-status-only ${ending})
			set_tests_properties(runtime_child_status_${ending} PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
		endforeach()
		set_tests_properties(vsh_subprocess_lnc_tokens PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
		foreach(t controller_settings_tests audio_out_timing_tests pad_haptics_tests)
			target_link_libraries(${t} vsh_subprocess_component)
		endforeach()
	endif()
	add_test(NAME lnc_app_status COMMAND $<TARGET_FILE:lnc_app_status_tests>)
	set_tests_properties(lnc_app_status PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
	add_test(NAME scanout_layers COMMAND $<TARGET_FILE:scanout_layers_tests>)
	add_test(NAME video_out_system_events COMMAND $<TARGET_FILE:video_out_system_events_tests>)
	add_test(NAME runtime_application_plan COMMAND $<TARGET_FILE:runtime_application_plan_tests>)
	add_test(NAME app_local_libraries COMMAND $<TARGET_FILE:app_local_libraries_tests>)
	add_test(NAME app_local_programs COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --app-local-programs-only)
	set_tests_properties(app_local_libraries app_local_programs PROPERTIES TIMEOUT 20)
	add_test(NAME application_symbol_scope COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --application-symbol-scope-only)
	add_test(NAME kernel_text_location COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --kernel-text-location-only)
	add_test(NAME videodec2_resources COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --videodec2-resources-only)
	set_tests_properties(videodec2_resources PROPERTIES TIMEOUT 60)
	add_test(NAME runtime_guest_execution COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-guest-execution-only)
	set_tests_properties(runtime_guest_execution PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_integer_callback COMMAND $<TARGET_FILE:runtime_integer_callback_tests>)
	set_tests_properties(runtime_integer_callback PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME np_account_abi COMMAND $<TARGET_FILE:np_account_abi_tests>)
	set_tests_properties(np_account_abi PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
	add_test(NAME runtime_guest_threads COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-guest-threads-only)
	set_tests_properties(runtime_guest_threads PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_guest_waits COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-guest-waits-only)
	set_tests_properties(runtime_guest_waits PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_guest_stop COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-guest-stop-only)
	set_tests_properties(runtime_guest_stop PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_native_target_pins COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-native-target-pins-only)
	set_tests_properties(runtime_native_target_pins PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_guest_mutex_stop COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-guest-mutex-stop-only)
	set_tests_properties(runtime_guest_mutex_stop PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_native_drain COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-native-drain-only)
	set_tests_properties(runtime_native_drain PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_user_service_owners COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-user-service-owners-only)
	add_test(NAME local_user_creation COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --local-user-creation-only)
	add_test(NAME local_user_storage COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --local-user-storage-only)
	set_tests_properties(local_user_storage PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME local_user_storage_upgrade COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --local-user-storage-upgrade-only)
	add_test(NAME local_user_storage_invalid_store COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --local-user-storage-invalid-store-only)
	set_tests_properties(local_user_storage_upgrade local_user_storage_invalid_store PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	set_tests_properties(local_user_creation PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_user_service_controllers COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-user-service-controllers-only)
	set_tests_properties(runtime_user_service_owners runtime_user_service_controllers PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_executable_memory_policy COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-executable-memory-policy-only)
	set_tests_properties(runtime_executable_memory_policy PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_application_service_events COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-application-service-events-only)
	set_tests_properties(runtime_application_service_events PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME application_import_bindings COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --application-import-bindings-only)
	add_test(NAME runtime_application_import_data COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-application-import-data-only)
	set_tests_properties(runtime_application_import_data PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME vsh_ipv6_connectivity COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --vsh-ipv6-connectivity-only)
	set_tests_properties(vsh_ipv6_connectivity PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
	add_test(NAME np_manager_client COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --np-manager-client-only)
	set_tests_properties(np_manager_client PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
	add_test(NAME np_manager_service COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --np-manager-service-only)
	set_tests_properties(np_manager_service PROPERTIES TIMEOUT 20 RUN_SERIAL TRUE)
	add_test(NAME runtime_sanitizer_owner COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-sanitizer-owner-only)
	set_tests_properties(runtime_sanitizer_owner PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_allocator_bootstrap COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-allocator-bootstrap-only)
	set_tests_properties(runtime_allocator_bootstrap PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_crt_sync COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-crt-sync-only)
	set_tests_properties(runtime_crt_sync PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_crt_condition_wrappers COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-crt-condition-wrappers-only)
	set_tests_properties(runtime_crt_condition_wrappers PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME runtime_rwlock_lifecycle COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-rwlock-lifecycle-only)
	set_tests_properties(runtime_rwlock_lifecycle PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	set_tests_properties(application_import_bindings PROPERTIES TIMEOUT 90 RUN_SERIAL TRUE)
	add_test(NAME sync_on_address_guest COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --sync-on-address-guest-only)
	set_tests_properties(kernel_text_location sync_on_address_guest PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
	add_test(NAME runtime_program_file_reuse COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-program-file-reuse-only)
	add_test(NAME sequence_event_idle COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --sequence-event-idle-only)
	add_test(NAME usb_storage_enumeration COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --usb-storage-enumeration-only)
	add_test(NAME floor_rtti_kind COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --floor-rtti-kind-only)
	add_test(NAME sysmodule_native COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --sysmodule-native-only)
	set_tests_properties(sysmodule_native PROPERTIES TIMEOUT 20)
	add_test(NAME guest_vsnprintf COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --guest-vsnprintf-only)
	add_test(NAME media_framework_esvm COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --media-framework-esvm-only)
	set_tests_properties(media_framework_esvm PROPERTIES TIMEOUT 30)
	add_test(NAME media_framework_esvm_keys COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --media-framework-esvm-keys-only)
	set_tests_properties(media_framework_esvm_keys PROPERTIES TIMEOUT 30)
	add_test(NAME media_framework_esvm_source COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --media-framework-esvm-source-only)
	set_tests_properties(media_framework_esvm_source PROPERTIES TIMEOUT 45)
	add_test(NAME composite_memory_pools COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --composite-memory-pools-only)
	set_tests_properties(composite_memory_pools PROPERTIES TIMEOUT 30)
	add_test(NAME media_framework_esvm_decoder COMMAND $<TARGET_FILE:media_framework_esvm_decoder_tests>)
	set_tests_properties(media_framework_esvm_decoder PROPERTIES TIMEOUT 30)
	add_test(NAME native_video COMMAND $<TARGET_FILE:native_video_tests>)
	set_tests_properties(native_video PROPERTIES TIMEOUT 20)
	add_test(NAME media_framework_esvm_certificate COMMAND $<TARGET_FILE:media_framework_esvm_certificate_tests>)
	set_tests_properties(media_framework_esvm_certificate PROPERTIES TIMEOUT 30)
	add_test(NAME kernel_normalize_path COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --kernel-normalize-path-only)
	set_tests_properties(kernel_normalize_path PROPERTIES TIMEOUT 20)
	add_test(NAME pthread_cond_deadline COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --pthread-cond-deadline-only)
	set_tests_properties(pthread_cond_deadline PROPERTIES TIMEOUT 20)
	add_test(NAME application_thread_exit COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --application-thread-exit-only)
	add_test(NAME runtime_process_identity COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --runtime-process-identity-only)
	add_test(NAME appdb_boot_routing COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --appdb-boot-routing-only)
	set_tests_properties(appdb_boot_routing PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
	add_test(NAME launch_uri_messaging COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --launch-uri-messaging-only)
	add_test(NAME vsh_input_focus COMMAND $<TARGET_FILE:kernel_file_system_tests> --vsh-input-focus-only)
	add_test(NAME network_peer_name COMMAND $<TARGET_FILE:kernel_file_system_tests> --network-peer-name-only)
	set_tests_properties(network_peer_name PROPERTIES TIMEOUT 20)
	add_test(NAME network_nonblocking_connect COMMAND $<TARGET_FILE:kernel_file_system_tests> --network-nonblocking-connect-only)
	set_tests_properties(network_nonblocking_connect PROPERTIES TIMEOUT 20)
	add_test(NAME posix_open COMMAND $<TARGET_FILE:kernel_file_system_tests> --posix-open-only)
	add_test(NAME profile_cache_filesystem COMMAND $<TARGET_FILE:kernel_file_system_tests> --profile-cache-filesystem-only)
	add_test(NAME sshot_device COMMAND $<TARGET_FILE:kernel_file_system_tests> --sshot-device-only)
	add_test(NAME metadbg_device COMMAND $<TARGET_FILE:kernel_file_system_tests> --metadbg-device-only)
	add_test(NAME devctl_device COMMAND $<TARGET_FILE:kernel_file_system_tests> --devctl-device-only)
	add_test(NAME retail_m2_info COMMAND $<TARGET_FILE:kernel_file_system_tests> --retail-m2-info-only)
	add_test(NAME retail_coelf_context COMMAND $<TARGET_FILE:kernel_file_system_tests> --retail-coelf-context-only)
	add_test(NAME retail_system_state COMMAND $<TARGET_FILE:kernel_file_system_tests> --retail-system-state-only)
	add_test(NAME retail_mbus_pad_event COMMAND $<TARGET_FILE:kernel_file_system_tests> --retail-mbus-pad-event-only)
	add_test(NAME kernel_write_events COMMAND $<TARGET_FILE:kernel_file_system_tests> --kernel-write-events-only)
	set_tests_properties(kernel_write_events PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
	add_test(NAME certification_scope COMMAND $<TARGET_FILE:kernel_file_system_tests> --certification-scope-only)
	add_test(NAME net_resolver_ex COMMAND $<TARGET_FILE:kernel_file_system_tests> --net-resolver-ex-only)
	set_tests_properties(net_resolver_ex PROPERTIES TIMEOUT 30)
	add_test(NAME net_resolver_ntoa6 COMMAND $<TARGET_FILE:kernel_file_system_tests> --net-resolver-ntoa6-only)
	set_tests_properties(net_resolver_ntoa6 PROPERTIES TIMEOUT 30)
	add_test(NAME socket_fstat COMMAND $<TARGET_FILE:kernel_file_system_tests> --socket-fstat-only)
	set_tests_properties(socket_fstat PROPERTIES TIMEOUT 20)
	add_test(NAME system_content_application COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --system-content-application-only)
	add_test(NAME emulator_user_name_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --user-name 1234567890123456)
	add_test(NAME emulator_user_name_too_long
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --user-name 12345678901234567)
	set_tests_properties(emulator_user_name_too_long PROPERTIES WILL_FAIL TRUE)
	add_test(NAME emulator_present_mode_cli
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --present-mode Immediate)
	add_test(NAME emulator_present_mode_invalid
	         COMMAND $<TARGET_FILE:kyty_emulator> --help --present-mode Invalid)
	set_tests_properties(emulator_present_mode_invalid PROPERTIES WILL_FAIL TRUE)
	add_test(NAME ime_dialog COMMAND $<TARGET_FILE:ime_dialog_tests>)
	add_test(NAME shader_cfg COMMAND $<TARGET_FILE:shader_cfg_tests>)
	add_test(NAME scalar_provenance COMMAND $<TARGET_FILE:scalar_provenance_tests>)
	add_test(NAME image_page_table COMMAND $<TARGET_FILE:image_page_table_tests>)
	add_test(NAME memory_tracker COMMAND $<TARGET_FILE:memory_tracker_tests>)
	add_test(NAME static_executable_layout COMMAND $<TARGET_FILE:static_executable_layout_tests>)
	add_test(NAME fixed_program_allocation COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --fixed-program-allocation-only)
	add_test(NAME static_executable_image COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --static-executable-image-only)
	add_test(NAME guest_host_bridge COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --guest-host-bridge-only)
	add_test(NAME graphics_observe COMMAND $<TARGET_FILE:graphics_observe_tests>)
	add_test(NAME libc_sort COMMAND $<TARGET_FILE:libc_sort_tests>)
	add_test(NAME libc_scan COMMAND $<TARGET_FILE:libc_scan_tests>)
	add_test(NAME ipmi_observe COMMAND $<TARGET_FILE:ipmi_observe_tests>)
	add_test(NAME np_mutex COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --np-mutex-only)
	add_test(NAME np_allocator COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --np-allocator-only)
	add_test(NAME np_manager_ipc_memory COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --np-manager-ipc-memory-only)
	add_test(NAME np_native_worker_http COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --np-native-worker-http-only)
	set_tests_properties(np_manager_ipc_memory np_native_worker_http PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
	add_test(NAME np_heap COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --np-heap-only)
	add_test(NAME np_global COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --np-global-only)
	add_test(NAME np_atomic COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --np-atomic-only)
	add_test(NAME process_sdk COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --process-sdk-only)
	add_test(NAME rng_sysctl COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --rng-sysctl-only)
	add_test(NAME numeric_sysctl COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --numeric-sysctl-only)
	add_test(NAME guest_fatal_trap COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --guest-fatal-trap-only)
	add_test(NAME native_libc_startup COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --native-libc-startup-only)
	add_test(NAME kernel_backtrace COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --kernel-backtrace-only)
	add_test(NAME standard_write COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --standard-write-only)
	add_test(NAME guest_file COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --guest-file-only)
	add_test(NAME native_ssl_smoke COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --native-ssl-smoke-only)
	add_test(NAME native_ssl_production COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --native-ssl-production-only)
	add_test(NAME native_ssl_http COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --native-ssl-http-only)
	add_test(NAME np_drm COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --np-drm-only)
	add_test(NAME fs_sandbox_mounts COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --fs-sandbox-mounts-only)
	add_test(NAME net_pool_config COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --net-pool-config-only)
	add_test(NAME authority_metadata COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --authority-metadata-only)
	add_test(NAME app_id COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --app-id-only)
	add_test(NAME page_manager COMMAND $<TARGET_FILE:page_manager_tests>)
	add_test(NAME build_flags COMMAND $<TARGET_FILE:build_flags_tests>)
	add_test(NAME archive_file COMMAND $<TARGET_FILE:archive_file_tests>)
	add_test(NAME trophy_system COMMAND $<TARGET_FILE:trophy_system_tests>)
	add_test(NAME avplayer_file COMMAND $<TARGET_FILE:avplayer_file_tests>)
	add_test(NAME media_framework COMMAND $<TARGET_FILE:media_framework_tests>)
	add_test(NAME vdecsw COMMAND $<TARGET_FILE:vdecsw_tests>)
	add_test(NAME bit_array COMMAND $<TARGET_FILE:bit_array_tests>)
	add_test(NAME lru_cache COMMAND $<TARGET_FILE:lru_cache_tests>)
	add_test(NAME symbol_database COMMAND $<TARGET_FILE:symbol_database_tests>)
	add_test(NAME agc_queue_identity COMMAND $<TARGET_FILE:agc_queue_identity_tests>)
	add_test(NAME shader_vertex_metadata COMMAND $<TARGET_FILE:shader_vertex_metadata_tests>)
	add_test(NAME resource_materialization COMMAND $<TARGET_FILE:resource_materialization_tests>)
	add_test(NAME resource_tracking COMMAND $<TARGET_FILE:resource_tracking_tests>)
	add_test(NAME event_queue_lifetime COMMAND $<TARGET_FILE:event_queue_lifetime_tests>)
	add_test(NAME kernel_file_system COMMAND $<TARGET_FILE:kernel_file_system_tests>)
	add_test(NAME filesystem_identity COMMAND $<TARGET_FILE:kernel_file_system_tests> --filesystem-identity-only)
	add_test(NAME filesystem_sync COMMAND $<TARGET_FILE:kernel_file_system_tests> --filesystem-sync-only)
	add_test(NAME guest_fs_context COMMAND $<TARGET_FILE:kernel_file_system_tests> --guest-fs-context-only)
	add_test(NAME json_resource_lookup COMMAND $<TARGET_FILE:kernel_file_system_tests> --json-resource-lookup-only)
	add_test(NAME json_path_parser COMMAND $<TARGET_FILE:kernel_file_system_tests> --json-path-parser-only)
	if(WIN32)
		add_test(NAME socketpair_network COMMAND $<TARGET_FILE:kernel_file_system_tests> --socketpair-network-only)
		add_test(NAME network_p2p_ownership COMMAND $<TARGET_FILE:kernel_file_system_tests> --network-p2p-only)
		set_tests_properties(network_p2p_ownership PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
		add_test(NAME semaphore_provenance_enabled COMMAND $<TARGET_FILE:kernel_file_system_tests> --semaphore-provenance-enabled)
		add_test(NAME semaphore_provenance_disabled COMMAND $<TARGET_FILE:kernel_file_system_tests> --semaphore-provenance-disabled)
		set_tests_properties(semaphore_provenance_enabled semaphore_provenance_disabled PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)
		set_tests_properties(semaphore_provenance_enabled PROPERTIES ENVIRONMENT "KYTY_SEMAPHORE_PROVENANCE=1")
		set_tests_properties(semaphore_provenance_disabled PROPERTIES ENVIRONMENT "KYTY_SEMAPHORE_PROVENANCE=0")
		add_test(NAME kernel_semaphores COMMAND $<TARGET_FILE:kernel_file_system_tests> --kernel-semaphores)
		add_test(NAME kernel_semaphore_guest_threads COMMAND $<TARGET_FILE:kernel_file_system_tests> --kernel-semaphore-guest-threads)
		set_tests_properties(kernel_semaphores kernel_semaphore_guest_threads PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE ENVIRONMENT "KYTY_SEMAPHORE_PROVENANCE=0")
		add_test(NAME kernel_event_flag_lifetime COMMAND $<TARGET_FILE:kernel_file_system_tests> --kernel-event-flag-lifetime)
		add_test(NAME kernel_event_flag_guest_threads COMMAND $<TARGET_FILE:kernel_file_system_tests> --kernel-event-flag-guest-threads)
		set_tests_properties(kernel_event_flag_lifetime kernel_event_flag_guest_threads PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE ENVIRONMENT "KYTY_SEMAPHORE_PROVENANCE=0")
		add_test(NAME socketpair_filesystem COMMAND $<TARGET_FILE:kernel_file_system_tests> --socketpair-filesystem-only)
		set_tests_properties(socketpair_network socketpair_filesystem PROPERTIES TIMEOUT 150 RUN_SERIAL TRUE)
		add_test(NAME anonymous_shm COMMAND $<TARGET_FILE:kernel_file_system_tests> --anonymous-shm-only)
		add_test(NAME anonymous_shm_shutdown_descriptors COMMAND $<TARGET_FILE:kernel_file_system_tests> --anonymous-shm-shutdown-descriptors)
		add_test(NAME anonymous_shm_shutdown_memory COMMAND $<TARGET_FILE:kernel_file_system_tests> --anonymous-shm-shutdown-memory)
		set_tests_properties(anonymous_shm anonymous_shm_shutdown_descriptors anonymous_shm_shutdown_memory
			PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
		foreach(shm_fatal_mode unmap protect)
			add_test(NAME anonymous_shm_fatal_${shm_fatal_mode}
				COMMAND ${CMAKE_COMMAND} "-DTEST_EXECUTABLE=$<TARGET_FILE:kernel_file_system_tests>"
					"-DMODE=${shm_fatal_mode}" -P "${KYTY_TESTS_DIR}/AnonymousShmFatalTest.cmake")
			set_tests_properties(anonymous_shm_fatal_${shm_fatal_mode} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
		endforeach()
	endif()
	add_test(NAME sync_on_address COMMAND $<TARGET_FILE:sync_on_address_tests>)
	add_test(NAME deferred_import_thunk COMMAND $<TARGET_FILE:deferred_import_thunk_tests>)
	set_tests_properties(deferred_import_thunk PROPERTIES TIMEOUT 90)
	add_test(NAME guest_call_boundary COMMAND $<TARGET_FILE:guest_call_boundary_tests>)
	set_tests_properties(guest_call_boundary PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
	add_test(NAME host_call_return_thunk COMMAND $<TARGET_FILE:host_call_return_thunk_tests>)
	set_tests_properties(host_call_return_thunk PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
	add_test(NAME posix_vector_time COMMAND $<TARGET_FILE:kernel_file_system_tests> --posix-vector-time-only)
	set_tests_properties(posix_vector_time PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
	add_test(NAME audio_out2_port COMMAND $<TARGET_FILE:audio_out2_port_tests>)
	add_test(NAME audio_out_timing COMMAND $<TARGET_FILE:audio_out_timing_tests>)
	add_test(NAME ngs2_sampler COMMAND $<TARGET_FILE:ngs2_sampler_tests>)
	add_test(NAME audiodec_atrac9 COMMAND $<TARGET_FILE:audiodec_atrac9_tests>)
	add_test(NAME ajm_resample COMMAND $<TARGET_FILE:ajm_resample_tests>)
	add_test(NAME pad_haptics COMMAND $<TARGET_FILE:pad_haptics_tests>)
	add_test(NAME controller_settings COMMAND $<TARGET_FILE:controller_settings_tests>)
	add_test(NAME video_out_status COMMAND $<TARGET_FILE:video_out_status_tests>)
	add_test(NAME shader_vertex_buffer COMMAND $<TARGET_FILE:shader_vertex_buffer_tests>)
	add_test(NAME host_input COMMAND $<TARGET_FILE:host_input_tests>)
	add_test(NAME save_data_memory COMMAND $<TARGET_FILE:save_data_memory_tests>)
	add_test(NAME png_enc COMMAND $<TARGET_FILE:png_enc_tests>)
	add_test(NAME jpeg_dec COMMAND $<TARGET_FILE:jpeg_dec_tests>)
	add_test(NAME jpeg_enc COMMAND $<TARGET_FILE:jpeg_enc_tests>)
	add_test(NAME occlusion_queries COMMAND $<TARGET_FILE:occlusion_queries_tests>)
	add_test(NAME ces COMMAND $<TARGET_FILE:ces_tests>)
	add_test(NAME save_data_dialog COMMAND $<TARGET_FILE:save_data_dialog_tests>)
	add_test(NAME http_uri_parse COMMAND $<TARGET_FILE:http_uri_parse_tests>)
	add_test(NAME network_unix_transport COMMAND $<TARGET_FILE:network_unix_transport_tests>)
	add_test(NAME shader_recompiler_compute COMMAND $<TARGET_FILE:shader_recompiler_compute_tests>)
	add_test(NAME shader_recompiler_tessellation
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --tessellation-only)
	add_test(NAME shader_recompiler_float_min_max
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --float-min-max-only)
	add_test(NAME shader_recompiler_vop3_conversions
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --vop3-conversions-only)
	add_test(NAME shader_recompiler_quadmask
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --quadmask-only)
	add_test(NAME shader_recompiler_integer16_ternary
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --integer16-ternary-only)
	add_test(NAME shader_recompiler_mad_u16
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --mad-u16-only)
	add_test(NAME shader_recompiler_mul_hi24
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --mul-hi24-only)
	add_test(NAME shader_recompiler_alignbyte
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --alignbyte-only)
	add_test(NAME graphics_pipeline_rasterization
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --polygon-mode-only)
	add_test(NAME graphics_draw_offsets
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --draw-offset-only)
	add_test(NAME virtual_memory_allocation
		COMMAND $<TARGET_FILE:virtual_memory_allocation_tests>)
	add_test(NAME trinity_mode
		COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --trinity-only)
	add_test(NAME psml COMMAND $<TARGET_FILE:psml_tests>)
	add_test(NAME pssr COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --pssr-only)
	add_test(NAME guest_red_zone_patcher
		COMMAND $<TARGET_FILE:virtual_memory_allocation_tests> --red-zone-patcher-only)
	add_test(NAME command_scheduler_timeline
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --scheduler-only)
	add_test(NAME stream_buffer_ring
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --stream-buffer-only)
	add_test(NAME gpu_command_lane
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-command-lane-only)
	add_test(NAME pm4_flip
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --flip-only)
	add_test(NAME video_out_hdr10_to_sdr
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --video-out-only)
	add_test(NAME depth_bits
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --depth-bits-only)
	add_test(NAME msaa_storage
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --msaa-storage-only)
	add_test(NAME pm4_context_state
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --context-state-only)
	add_test(NAME gpu_tiler
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --gpu-tiler-only)
	add_test(NAME image_transfer_layout
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-layout-only)

	add_test(NAME texture_cache_layered_image
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --layered-image-only)
	add_test(NAME host_image_allocation
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --host-image-allocation-only)
	add_test(NAME texture_cache_image_views
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-view-cache-only)
	add_test(NAME texture_cache_storage_sampled
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --storage-sampled-only)
	add_test(NAME texture_cache_depth_readback
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --depth-readback-only)
	add_test(NAME compute_meta_clear_classification
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --compute-meta-clear-only)
	add_test(NAME buffer_cache_dirty_gc
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-gc-only)
	add_test(NAME color_render_target_1d
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --rt1d-only)

	add_test(NAME texture_cache_image_overlap
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --image-overlap-only)
	add_test(NAME texture_cache_htile_clear
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --htile-clear-only)
	add_test(NAME buffer_cache_ranges
		COMMAND $<TARGET_FILE:shader_recompiler_compute_tests> --buffer-cache-range-only)

	add_custom_target(kyty_tests DEPENDS
		scanout_layers_tests
		app_local_libraries_tests
		video_out_system_events_tests
		ime_dialog_tests
		shader_cfg_tests
		scalar_provenance_tests
		image_page_table_tests
		memory_tracker_tests
		page_manager_tests
		build_flags_tests
		archive_file_tests
		trophy_system_tests
		avplayer_file_tests
		media_framework_tests
		media_framework_esvm_decoder_tests
		native_video_tests
		media_framework_esvm_certificate_tests
		vdecsw_tests
		bit_array_tests
		lru_cache_tests
		shader_vertex_metadata_tests
		resource_materialization_tests
		resource_tracking_tests
		event_queue_lifetime_tests
		kernel_file_system_tests
		sync_on_address_tests
		deferred_import_thunk_tests
		guest_call_boundary_tests
		host_call_return_thunk_tests
		shader_recompiler_compute_tests
		virtual_memory_allocation_tests
		audio_out2_port_tests
		audio_out_timing_tests
		ngs2_sampler_tests
		audiodec_atrac9_tests
		ajm_resample_tests
		pad_haptics_tests
		controller_settings_tests
		host_input_tests
		save_data_memory_tests
		png_enc_tests
		jpeg_dec_tests
		jpeg_enc_tests
		occlusion_queries_tests
		ces_tests
		save_data_dialog_tests
		http_uri_parse_tests
	)
	if(WIN32)
		find_package(Python3 COMPONENTS Interpreter QUIET)
		if(Python3_Interpreter_FOUND)
			foreach(target host_http_tests network_http_tests rnps_curl_tests)
				add_test(NAME ${target} COMMAND ${Python3_EXECUTABLE}
					"${CMAKE_SOURCE_DIR}/tools/test_host_http.py" $<TARGET_FILE:${target}>)
				set_tests_properties(${target} PROPERTIES TIMEOUT 30)
				add_dependencies(kyty_tests ${target})
			endforeach()
		endif()
	endif()
endif()

# Test Darwin signal-context restoration for SSE4a instructions.
if(APPLE AND (CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64" OR
		(NOT CMAKE_OSX_ARCHITECTURES AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")))
	add_executable(macos_sse4a_tests EXCLUDE_FROM_ALL
		"${KYTY_TESTS_DIR}/macosSse4aTests.cpp"
		"${KYTY_SOURCE_DIR}/loader/x64InstructionEmulator.cpp"
		"${KYTY_SOURCE_DIR}/loader/guestInstructionPatcher.cpp"
	)
	target_link_libraries(macos_sse4a_tests common fmt::fmt Zydis::Zydis xbyak::xbyak)
	target_include_directories(macos_sse4a_tests PRIVATE ${inc_headers})
	if(BUILD_TESTING)
		add_test(NAME macos_sse4a COMMAND $<TARGET_FILE:macos_sse4a_tests>)
		set_tests_properties(macos_sse4a PROPERTIES TIMEOUT 15)
		add_dependencies(kyty_tests macos_sse4a_tests)
	endif()
endif()

add_executable(kyty_emulator "${KYTY_SOURCE_DIR}/main.cpp" ${kyty_emulator_src})
configure_macos_guest_address_space(kyty_emulator)

set(KYTY_TROPHY_UNLOCK_SOUND "${CMAKE_CURRENT_SOURCE_DIR}/assets/sounds/trophy-unlock.wav")
add_custom_target(kyty_trophy_sound
	COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:kyty_emulator>/assets/sounds"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_TROPHY_UNLOCK_SOUND}"
		"$<TARGET_FILE_DIR:kyty_emulator>/assets/sounds/trophy-unlock.wav"
	VERBATIM)
add_dependencies(kyty_emulator kyty_trophy_sound)
install(FILES "${KYTY_TROPHY_UNLOCK_SOUND}" DESTINATION assets/sounds)

add_custom_target(kyty_touch_cursors
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_CURRENT_SOURCE_DIR}/assets/cursors"
		"$<TARGET_FILE_DIR:kyty_emulator>/assets/cursors"
	VERBATIM)
add_dependencies(kyty_emulator kyty_touch_cursors)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/assets/cursors" DESTINATION assets)

set(KYTY_NOTO_SANS_FONTS
	"${KYTY_NOTO_SANS_DIR}/NotoSans-Regular.ttf"
	"${KYTY_NOTO_SANS_DIR}/NotoSans-Bold.ttf"
	"${KYTY_NOTO_SANS_DIR}/NotoSans-Italic.ttf"
	"${KYTY_NOTO_SANS_DIR}/NotoSans-BoldItalic.ttf")
add_custom_target(kyty_noto_sans_fonts
	COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:kyty_emulator>/assets/fonts"
	COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KYTY_NOTO_SANS_FONTS}
		"$<TARGET_FILE_DIR:kyty_emulator>/assets/fonts"
	VERBATIM)
add_dependencies(kyty_emulator kyty_noto_sans_fonts)
install(FILES ${KYTY_NOTO_SANS_FONTS} DESTINATION assets/fonts)

target_link_libraries(kyty_emulator ${kyty_emulator_link_libraries})
# ps5-vsh: Hermes VM (facebook/hermes v0.11.0 pin). No-op unless
# -DKYTY_USE_HERMES=ON; links libhermes+jsi, defines KYTY_USE_HERMES=1 for the
# real makeHermesRuntime dispatch in src/libs/libHermesVm.cpp (proven link in
# 3rdparty/hermes-pin.cmake: kyty_hermes_e2e_ok).
kyty_target_link_hermes(kyty_emulator)
if (WIN32)
	target_link_libraries(kyty_emulator opengl32)
	target_link_libraries(kyty_emulator iphlpapi winhttp)
	target_link_libraries(kyty_emulator setupapi)
	target_link_libraries(kyty_emulator ws2_32)
endif()
if (KYTY_CLANG_CL)
	target_link_libraries(kyty_emulator onecore)
endif()
if (CLANG AND NOT KYTY_CLANG_CL)
	target_link_libraries(kyty_emulator pthread)
endif()
# dlopen/dlsym/dladdr for RenderDoc.
target_link_libraries(kyty_emulator ${CMAKE_DL_LIBS})
target_include_directories(kyty_emulator PRIVATE ${inc_headers})

clang_tidy_check(kyty_emulator "" "${check_headers}" "${inc_headers}")

include_what_you_use(kyty_emulator "${inc_headers}")

set(KYTY_EMULATOR_MAP_NAME "kyty_emulator_${KYTY_COMPILER_ID}_${KYTY_LINKER_ID}.map")

set(KYTY_EMULATOR_MAP_LINK_PATH "${CMAKE_CURRENT_BINARY_DIR}/${KYTY_EMULATOR_MAP_NAME}")
set(KYTY_EMULATOR_PDB_LINK_PATH "${CMAKE_CURRENT_BINARY_DIR}/kyty_emulator.pdb")

if(KYTY_CLANG_CL)
	target_link_options(kyty_emulator PRIVATE
		"/DYNAMICBASE:NO"
		"/DEBUG:FULL"
		"/PDB:${KYTY_EMULATOR_PDB_LINK_PATH}"
		"/lldmap:${KYTY_EMULATOR_MAP_LINK_PATH}"
	)
	add_custom_command(TARGET kyty_emulator POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${KYTY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" $<TARGET_FILE_DIR:kyty_emulator>/libwinpthread-1.dll)
elseif(WIN32 OR LINUX)
	set_target_properties(kyty_emulator PROPERTIES LINK_FLAGS "${KYTY_LD_OPTIONS}")
	# LINK_FLAGS is a space-delimited string: a -Map path containing spaces would
	# be split into bogus arguments, so pass it as a discrete link option instead.
	target_link_options(kyty_emulator PRIVATE "-Wl,-Map=${KYTY_EMULATOR_MAP_LINK_PATH}")
endif()

if(LINUX)
    target_link_options(kyty_emulator PRIVATE
        $<$<CONFIG:Release>:-Wl,--strip-all>
    )
endif()

if(APPLE)
	# The emulator writes x86-64 trampolines/PLT stubs into memory and executes them, and
	# runs guest code under Rosetta. That requires the JIT / unsigned-executable-memory
	# entitlements, so re-sign the binary after every link (an unsigned/relinked binary
	# reverts to the hardened defaults and aborts when it first executes written code).
	add_custom_command(TARGET kyty_emulator POST_BUILD
		COMMAND codesign -s - --force --options runtime
			--entitlements "${KYTY_SOURCE_DIR}/macos_jit.entitlements"
			$<TARGET_FILE:kyty_emulator>
		COMMENT "Codesign kyty_emulator with JIT entitlements (macOS)")
endif()

install(TARGETS kyty_emulator DESTINATION .)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallRetailSetup.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/InstallThirdPartyNotices.cmake")
if(KYTY_BUILD_LAUNCHER)
	add_subdirectory("${KYTY_SOURCE_DIR}/launcher" "${CMAKE_CURRENT_BINARY_DIR}/launcher")

	if(LINUX)
		target_link_options(launcher PRIVATE $<$<CONFIG:Release>:-Wl,--strip-all>)
		target_link_options(kyty_updater PRIVATE $<$<CONFIG:Release>:-Wl,--strip-all>)
	endif()
endif()
if(KYTY_CLANG_CL)
	install(FILES "${KYTY_THIRD_PARTY_DIR}/winpthread/bin/libwinpthread-1.dll" DESTINATION .)
endif()
