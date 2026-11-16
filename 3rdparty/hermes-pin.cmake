# kytyps5 Hermes pin (v0.11.0 + v0.12.0) — fetch is NOT wired by default (KYTY_USE_HERMES
# defaults OFF); 3rdparty/CMakeLists.txt includes this file and calls
# kyty_pin_hermes(), which no-ops unless -DKYTY_USE_HERMES=ON.
#
# Version select: -DKYTY_HERMES_VERSION=0.11 (default, HBC v84) or 0.12
# (HBC v89). The v0.11 path is byte-for-byte intact; 0.12 only adds a tag +
# source-layout + HBC-version expectation branch.
#
# Fetch verdict: VIABLE (tag exists, no tarball downloaded per network budget).
#   git ls-remote --tags https://github.com/facebook/hermes.git
#     204045369cbbfac531867b780d72716422a2443d  refs/tags/v0.11.0   <-- PIN
#     67a8f5502a46881ca3280b0c56ccc910cdc6b18c  refs/tags/hermes-v250829098.0.11
# Tag v0.11.0 = RN-0.70.6-era hermes-0.11 bytecode line. Full clone/tarball was
# deliberately NOT fetched (single ls-remote only, then STOP networking).
# Size note: Hermes source + ICU + dependency build is hundreds of MB once
# materialised; expect a long first host-only build, so keep this OFF by
# default and wire it in only when the VM dispatch turn lands.
#
# v0.12.0 verdict: VIABLE (tag exists, scratch-built 2026-10-06, no download
# per network budget beyond the pinned tag).
#   v0.12.0 tag 7d3e091 "Bump version to 0.12.0"  <-- PIN for 0.12
#   include/hermes/BCGen/HBC/BytecodeVersion.h: BYTECODE_VERSION = 89
#   (decrypted NPXS40024 is raw HBC v89, 23,284 functions, headerless).
#   Scratch _Build/hermes-v89 (src/ = v0.12.0 tag, build/ = configured ninja
#   Release clang-cl, hermesc+hbcdump proven) built libhermes + jsi 2026-10-06:
#   hermes.dll 2932224 B + hermes.lib under build/API/hermes/,
#   jsi.lib static under build/jsi/.
#   NOTE: build.ninja needed the documented /EHsc carve-out appended to the
#   C++ FLAGS (upstream Hermes.cmake strips /EHsc for MSVC/clang-cl and lands
#   -fno-exceptions, which clang-cl ignores while EH stays OFF -> jsi.cpp and
#   API/hermes/hermes.cpp hard-fail on throw/try). The FetchContent path below
#   already applies the same carve-out via target_compile_options.
#
# Usage once wired in (host-executed VM, REAL-service style):
#   hermesc (host compiler) + hermes-vm (host-executed bytecode) link into the
#   emulator; libHermesVm.cpp VmContext.execute dispatches ctx->bytecode via
#   facebook::hermes::vm, stats feed HermesVmGetStats.

include_guard(GLOBAL)

option(KYTY_USE_HERMES "Fetch and link facebook/hermes (host-only hermesc/hvm)" OFF)
set(KYTY_HERMES_VERSION "0.11"
    CACHE STRING "Hermes version pin: 0.11 (HBC v84) or 0.12 (HBC v89)")

function(kyty_pin_hermes)
  if(NOT KYTY_USE_HERMES)
    message(STATUS "kyty_pin_hermes: KYTY_USE_HERMES=OFF, skipping Hermes fetch")
    return()
  endif()
  # Version dispatch (v0.11 path intact): tag + emitted HBC version + source
  # layouts to probe next to a prebuilt dir.
  if("${KYTY_HERMES_VERSION}" STREQUAL "0.12")
    set(_kyty_hermes_tag "v0.12.0")
    set(_kyty_hermes_hbc_version 89)
    # v0.12 scratch layout (_Build/hermes-v89: build/ + src/ siblings) first,
    # then the v0.11 FetchContent layout (_deps/hermes-build + _deps/hermes-src).
    set(_kyty_hermes_src_layouts "src" "hermes-src")
  elseif("${KYTY_HERMES_VERSION}" STREQUAL "0.11")
    set(_kyty_hermes_tag "v0.11.0")
    set(_kyty_hermes_hbc_version 84)
    set(_kyty_hermes_src_layouts "hermes-src")
  else()
    message(FATAL_ERROR "kyty_pin_hermes: unsupported KYTY_HERMES_VERSION=${KYTY_HERMES_VERSION} (want 0.11 or 0.12)")
  endif()
  # ps5-vsh: prebuilt-first. A proven scratch build
  # (_Build/hermes-only: libhermes+jsi libs, hermesc/hbcdump tools) can feed
  # the main build WITHOUT re-running Hermes' own configure (whose ICU
  # fallback misfires under clang-cl here: "Unable to find ICU" despite
  # WIN10 fallback ON). Pass
  # -DKYTY_HERMES_PREBUILT_DIR=<scratch>/_deps/hermes-build to use it.
  set(KYTY_HERMES_PREBUILT_DIR ""
      CACHE PATH "Proven Hermes build dir (API/hermes/hermes.lib + jsi/jsi.lib + hermes.dll)")
  if(NOT "${KYTY_HERMES_PREBUILT_DIR}" STREQUAL "")
    set(_kyty_hermes_lib "${KYTY_HERMES_PREBUILT_DIR}/API/hermes/hermes.lib")
    set(_kyty_jsi_lib "${KYTY_HERMES_PREBUILT_DIR}/jsi/jsi.lib")
    set(_kyty_hermes_dll "${KYTY_HERMES_PREBUILT_DIR}/API/hermes/hermes.dll")
    if(EXISTS "${_kyty_hermes_lib}" AND EXISTS "${_kyty_jsi_lib}" AND
       EXISTS "${_kyty_hermes_dll}")
      message(STATUS "kyty_pin_hermes: using prebuilt Hermes ${_kyty_hermes_tag} "
                     "(HBC v${_kyty_hermes_hbc_version}) at ${KYTY_HERMES_PREBUILT_DIR}")
      add_library(libhermes SHARED IMPORTED GLOBAL)
      set_target_properties(libhermes PROPERTIES
        IMPORTED_LOCATION "${_kyty_hermes_dll}"
        IMPORTED_IMPLIB "${_kyty_hermes_lib}")
      add_library(jsi STATIC IMPORTED GLOBAL)
      set_target_properties(jsi PROPERTIES IMPORTED_LOCATION "${_kyty_jsi_lib}")
      # Sibling source dir: v0.11 FetchContent layout keeps sources at
      # <prebuilt>/../hermes-src, the v0.12 scratch layout at <prebuilt>/../src.
      # Probe the version's layouts in order; fall back to
      # KYTY_HERMES_SOURCE_DIR cache.
      get_filename_component(_kyty_deps_dir "${KYTY_HERMES_PREBUILT_DIR}" DIRECTORY)
      foreach(_kyty_src_layout ${_kyty_hermes_src_layouts})
        if(EXISTS "${_kyty_deps_dir}/${_kyty_src_layout}/API/hermes/hermes.h")
          set(KYTY_HERMES_SOURCE_DIR "${_kyty_deps_dir}/${_kyty_src_layout}"
              CACHE PATH "facebook/hermes ${_kyty_hermes_tag} source dir" FORCE)
          break()
        endif()
      endforeach()
      set(KYTY_HERMES_BINARY_DIR "${KYTY_HERMES_PREBUILT_DIR}"
          CACHE PATH "facebook/hermes build dir (generated headers)" FORCE)
      if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # Consuming jsi.lib needs no flags (it was built with /EHsc already);
        # the /EHsc carve-out below targets only FetchContent-built sources.
        # (INTERFACE-only allowed on IMPORTED targets; nothing needed here.)
      endif()
      return()
    else()
      message(WARNING "kyty_pin_hermes: KYTY_HERMES_PREBUILT_DIR set but libs missing; falling back to FetchContent")
    endif()
  endif()
  include(FetchContent)
  set(HERMES_ENABLE_TEST_SUITE OFF CACHE BOOL "" FORCE)
  set(HERMES_BUILD_APPLE_FRAMEWORK OFF CACHE BOOL "" FORCE)
  # Host toolchain: hermesc + hbcdump (+ hvm) only compile when TOOLS is ON
  # (upstream default ON; tools/CMakeLists.txt lists hermesc, hbcdump, hvm).
  set(HERMES_ENABLE_TOOLS ON CACHE BOOL "" FORCE)
  # No ICU needed: HERMES_ENABLE_INTL defaults OFF and external/ ships only
  # icu_decls (no ICU bundle). Pin it explicitly so a future default flip
  # cannot pull an ICU dependency into the scratch/host build.
  set(HERMES_ENABLE_INTL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    hermes
    GIT_REPOSITORY https://github.com/facebook/hermes.git
    GIT_TAG ${_kyty_hermes_tag}
    GIT_SHALLOW TRUE
    GIT_PROGRESS TRUE
  )
  FetchContent_MakeAvailable(hermes)
  # FetchContent leaves hermes_SOURCE_DIR/_BINARY_DIR in this function's scope;
  # persist them for kyty_target_link_hermes (include dirs below).
  set(KYTY_HERMES_SOURCE_DIR "${hermes_SOURCE_DIR}"
      CACHE PATH "facebook/hermes ${_kyty_hermes_tag} source dir" FORCE)
  set(KYTY_HERMES_BINARY_DIR "${hermes_BINARY_DIR}"
      CACHE PATH "facebook/hermes build dir (generated headers)" FORCE)
  if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # clang-cl reports CMAKE_CXX_COMPILER_ID=Clang, so upstream
    # API/jsi/jsi/CMakeLists.txt skips its MSVC-only "/EHsc" carve-out while
    # Hermes.cmake strips /EHsc from the global flags. MSVC tolerates
    # `throw`/`try` with EH off, but clang-cl hard-rejects it ("cannot use
    # 'throw' with exceptions disabled"). Upstream targets that actually
    # throw (jsi, libhermes API) get upstream's MSVC intent restored.
    # Verified 2026-10-05 on v0.11.0 (required to build the `libhermes`
    # target); re-verified 2026-10-06 on v0.12.0 (scratch _Build/hermes-v89:
    # jsi.cpp + API/hermes/hermes.cpp hard-fail on throw/try without it).
    foreach(_kyty_hermes_eh_target jsi libhermes hermesapi hermesapiLean)
      if(TARGET ${_kyty_hermes_eh_target})
        target_compile_options(${_kyty_hermes_eh_target} PRIVATE /EHsc)
      endif()
    endforeach()
  endif()
endfunction()

# Verified 2026-10-05: `git clone --depth 1 --branch v0.11.0` works (51MB,
# 9340 files). Tree has lib/VM, tools/{hermesc,hvm,hbcdump}, API/{hermes,jsi}.
# NOTE: v0.11.0 builds the VM as target `libhermes` (API/hermes/CMakeLists.txt:
# `add_library(libhermes SHARED ...`), NOT `hermes-vm`. Link helper below
# reflects the real name. Tool targets: `hermesc`, `hbcdump`, `hvm`
# (all built 2026-10-05 in scratch _Build/hermes-only).

# kyty_hermes_e2e_ok (2026-10-05, scratch _Build/hermes-only, clang-cl, Release):
#   `cmake --build _Build/hermes-only --target libhermes jsi` -> "ninja: no
#   work to do" (hermes.dll 2566144 B + hermes.lib import lib under
#   _deps/hermes-build/API/hermes/; jsi.lib static under _deps/hermes-build/jsi/).
#   hermes-lib-e2e.cpp (makeHermesRuntime + evaluateJavaScript, same dispatch
#   as libHermesVm.cpp under KYTY_USE_HERMES) compiled /EHsc /MD against
#   hermes.lib + jsi.lib with -I API, API/jsi, public, include,
#   build/include; ran on hello.hbc (408 B, HBC v84, magic C6-1F-BC-03):
#   print "hello hermes 1+2=3", srcEval=3, hbcEval=undefined-ok, LINK_RUN_OK,
#   exit 0. jsi symbols are NOT re-exported by hermes.dll, so both libs are
#   required. Helper below mirrors that exact link. Still OFF by default.
#
# kyty_hermes_e2e_v89 (2026-10-06, scratch _Build/hermes-v89, clang-cl, Release):
#   `cmake --build _Build/hermes-v89/build --target libhermes jsi` ->
#   hermes.dll 2932224 B + hermes.lib import lib under build/API/hermes/;
#   jsi.lib static under build/jsi/ (BYTECODE_VERSION=89 confirmed in
#   include/hermes/BCGen/HBC/BytecodeVersion.h). Same /EHsc carve-out as v0.11
#   was required (applied to build.ninja C++ FLAGS; FetchContent path above
#   applies it via target_compile_options). Prebuilt use:
#   -DKYTY_USE_HERMES=ON -DKYTY_HERMES_VERSION=0.12
#   -DKYTY_HERMES_PREBUILT_DIR=<repo>/_Build/hermes-v89/build
#   (sibling src/ auto-detected for headers).
function(kyty_target_link_hermes target)
  if(NOT KYTY_USE_HERMES)
    return()
  endif()
  if(NOT TARGET libhermes OR NOT TARGET jsi)
    message(WARNING "kyty_target_link_hermes: libhermes/jsi targets missing (pin not fetched yet)")
    return()
  endif()
  # Proven link line (see kyty_hermes_e2e_ok above): hermes.lib is the
  # API/hermes import lib (hermes.dll); jsi.lib is the static API/jsi/jsi.lib.
  # Plain signature: kyty_emulator's own link calls are plain
  # (CMakeLists.txt:728), and all uses must agree.
  target_link_libraries(${target} libhermes jsi)
  # Upstream PUBLIC includes cover API/ (hermes/hermes.h) and API/jsi/
  # (jsi/jsi.h) only; hermes.h also pulls <hermes/Public/RuntimeConfig.h>,
  # which lives under public/ — hence the explicit proven -I set (plus
  # include/ and the generated build include dir).
  target_include_directories(${target} PRIVATE
    "${KYTY_HERMES_SOURCE_DIR}/API"
    "${KYTY_HERMES_SOURCE_DIR}/API/jsi"
    "${KYTY_HERMES_SOURCE_DIR}/public"
    "${KYTY_HERMES_SOURCE_DIR}/include"
    "${KYTY_HERMES_BINARY_DIR}/include"
  )
  target_compile_definitions(${target} PRIVATE KYTY_USE_HERMES=1)
  if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # libHermesVm.cpp throws/catches facebook::jsi::JSError; clang-cl
    # hard-rejects try/throw with EH off (same reason as the /EHsc carve-out
    # in kyty_pin_hermes for jsi/libhermes/hermesapi/hermesapiLean).
    # NOTE: Linux CLANG builds add -fno-exceptions in src/utils.cmake; that
    # platform needs its own -fexceptions TU carve-out when it wires the VM
    # (not proven here — this pin is validated on clang-cl/Windows only).
    target_compile_options(${target} PRIVATE /EHsc)
  endif()
  # libhermes is SHARED: stage hermes.dll next to the emulator so it starts
  # with KYTY_USE_HERMES=ON (same pattern as the libwinpthread-1.dll staging).
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:libhermes>" "$<TARGET_FILE_DIR:${target}>"
    COMMENT "Staging hermes.dll next to ${target}"
  )
endfunction()

# --- ORCHESTRATOR: the pin (include + kyty_pin_hermes()) is already called in
# 3rdparty/CMakeLists.txt. Call the link helper from the TOP-LEVEL
# CMakeLists.txt AFTER add_executable(kyty_emulator) (3rdparty/ is processed
# before that target exists, so calling it there fails configure):
#   kyty_target_link_hermes(kyty_emulator)
