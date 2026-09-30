cmake_minimum_required(VERSION 3.24)
project(DarktideGLBCompiler LANGUAGES C CXX)

# Build vendored dependencies as static libraries. POSIX image decoding also
# links the platform's PNG and JPEG packages, whose linkage is toolchain-specific.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build dependencies as static libraries" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
    # The full PhysX SDK and the consuming compiler must share the static CRT.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

function(apply_project_compiler_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# Active third-party sources are vendored locally. Configuration never fetches
# dependencies from the network.
set(DARKTIDE_VENDOR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

set(DARKTIDE_CGLTF_DIR "${DARKTIDE_VENDOR_ROOT}/cgltf")
set(DARKTIDE_MESHOPTIMIZER_DIR "${DARKTIDE_VENDOR_ROOT}/meshoptimizer")
set(DARKTIDE_MIKKTSPACE_DIR "${DARKTIDE_VENDOR_ROOT}/mikktspace")
set(DARKTIDE_DIRECTXTEX_DIR "${DARKTIDE_VENDOR_ROOT}/DirectXTex")
set(DARKTIDE_BASISU_DIR "${DARKTIDE_VENDOR_ROOT}/basis_universal")
set(DARKTIDE_DRACO_DIR "${DARKTIDE_VENDOR_ROOT}/draco")
set(DARKTIDE_LIBWEBP_DIR "${DARKTIDE_VENDOR_ROOT}/libwebp")
set(DARKTIDE_PHYSX_DIR "${DARKTIDE_VENDOR_ROOT}/physx")

set(_darktide_vendor_sentinels
    "${DARKTIDE_CGLTF_DIR}/cgltf.h"
    "${DARKTIDE_MESHOPTIMIZER_DIR}/src/meshoptimizer.h"
    "${DARKTIDE_MIKKTSPACE_DIR}/mikktspace.h"
    "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTex.h"
    "${DARKTIDE_BASISU_DIR}/transcoder/basisu_transcoder.h"
    "${DARKTIDE_DRACO_DIR}/src/draco/compression/decode.h"
    "${DARKTIDE_LIBWEBP_DIR}/src/webp/decode.h"
    "${DARKTIDE_PHYSX_DIR}/physx/include/cooking/PxCooking.h"
    "${DARKTIDE_PHYSX_DIR}/physx/source/physxcooking/src/Cooking.cpp"
    "${DARKTIDE_PHYSX_DIR}/pxshared/include/foundation/Px.h"
)
foreach(_vendor_file IN LISTS _darktide_vendor_sentinels)
    if(NOT EXISTS "${_vendor_file}")
        message(FATAL_ERROR "Required vendored source is missing: ${_vendor_file}.")
    endif()
endforeach()
unset(_darktide_vendor_sentinels)
unset(_vendor_file)

if(NOT WIN32)
    find_package(PNG REQUIRED)
    find_package(JPEG REQUIRED)
endif()

# Phase 1 compressed-accessor decoders. Keep unrelated tools/install targets disabled.
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${DARKTIDE_MESHOPTIMIZER_DIR}" "${CMAKE_BINARY_DIR}/third_party/meshoptimizer" EXCLUDE_FROM_ALL)

set(DRACO_INSTALL OFF CACHE BOOL "" FORCE)
set(DRACO_TESTS OFF CACHE BOOL "" FORCE)
set(DRACO_GLTF_BITSTREAM ON CACHE BOOL "" FORCE)
# Draco 1.5.7 relies on transitive standard-library includes in a few translation
# units. Newer libc++ versions intentionally stopped providing them. Supply
# the missing standard headers from the consuming build.
if(MINGW AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:CXX>:-includealgorithm>"
        "$<$<COMPILE_LANGUAGE:CXX>:-includeiterator>"
    )
endif()
add_subdirectory("${DARKTIDE_DRACO_DIR}" "${CMAKE_BINARY_DIR}/third_party/draco" EXCLUDE_FROM_ALL)

# The compiler only needs the in-memory WebP decoder API. Keep all WebP
# command-line tools, muxing, and extras out of the build.
set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_CWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_DWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_GIF2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_IMG2WEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_VWEBP OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPINFO OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(WEBP_BUILD_WEBP_JS OFF CACHE BOOL "" FORCE)
set(WEBP_LINK_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory("${DARKTIDE_LIBWEBP_DIR}" "${CMAKE_BINARY_DIR}/third_party/libwebp" EXCLUDE_FROM_ALL)

# MSVC uses one complete SDK graph for cooking, simulation and serialization.
# Other toolchains retain the established cooking-only backend until the full
# upstream runtime has been verified with them.
if(MSVC)
    include(cmake/PhysXRuntime.cmake)
    darktide_add_physx_runtime()
else()
set(_px_physx "${DARKTIDE_PHYSX_DIR}/physx")
set(_px_shared "${DARKTIDE_PHYSX_DIR}/pxshared")
file(GLOB _px_foundation_common CONFIGURE_DEPENDS
    "${_px_physx}/source/foundation/src/*.cpp")
file(GLOB _px_common CONFIGURE_DEPENDS
    "${_px_physx}/source/common/src/*.cpp")
file(GLOB_RECURSE _px_geomutils CONFIGURE_DEPENDS
    "${_px_physx}/source/geomutils/src/*.cpp")
file(GLOB _px_cooking_common CONFIGURE_DEPENDS
    "${_px_physx}/source/physxcooking/src/*.cpp")
file(GLOB _px_cooking_mesh CONFIGURE_DEPENDS
    "${_px_physx}/source/physxcooking/src/mesh/*.cpp")
file(GLOB _px_cooking_convex CONFIGURE_DEPENDS
    "${_px_physx}/source/physxcooking/src/convex/*.cpp")
if(WIN32)
    file(GLOB _px_foundation_platform CONFIGURE_DEPENDS
        "${_px_physx}/source/foundation/src/windows/*.cpp")
    list(FILTER _px_foundation_platform EXCLUDE REGEX "PsUWPThread\\.cpp$")
    set(_px_platform_include "${_px_physx}/source/foundation/include/windows")
else()
    file(GLOB _px_foundation_platform CONFIGURE_DEPENDS
        "${_px_physx}/source/foundation/src/unix/*.cpp")
    set(_px_platform_include "${_px_physx}/source/foundation/include/unix")
endif()

set(_px_include_dirs
    "${_px_physx}"
    "${_px_physx}/include"
    "${_px_shared}/include"
    "${_px_physx}/source/foundation/include"
    "${_px_platform_include}"
    "${_px_physx}/source/common/include"
    "${_px_physx}/source/common/src"
    "${_px_physx}/source/geomutils/include"
    "${_px_physx}/source/geomutils/src"
    "${_px_physx}/source/geomutils/src/contact"
    "${_px_physx}/source/geomutils/src/common"
    "${_px_physx}/source/geomutils/src/convex"
    "${_px_physx}/source/geomutils/src/distance"
    "${_px_physx}/source/geomutils/src/sweep"
    "${_px_physx}/source/geomutils/src/gjk"
    "${_px_physx}/source/geomutils/src/intersection"
    "${_px_physx}/source/geomutils/src/mesh"
    "${_px_physx}/source/geomutils/src/hf"
    "${_px_physx}/source/geomutils/src/pcm"
    "${_px_physx}/source/geomutils/src/ccd"
    "${_px_physx}/source/physxcooking/src"
    "${_px_physx}/source/physxcooking/src/mesh"
    "${_px_physx}/source/physxcooking/src/convex")
set(_px_definitions
    PX_PHYSX_STATIC_LIB
    PX_COOKING
    PX_SIMD_DISABLED
    PX_NVTX=0
    PX_SUPPORT_PVD=0
    _CRT_SECURE_NO_DEPRECATE
    _CRT_NONSTDC_NO_DEPRECATE
    _WINSOCK_DEPRECATED_NO_WARNINGS)

add_library(DarktidePhysXFoundation STATIC
    ${_px_foundation_common} ${_px_foundation_platform})
add_library(DarktidePhysXCommon STATIC ${_px_common} ${_px_geomutils})
add_library(DarktidePhysXCooking STATIC
    ${_px_cooking_common} ${_px_cooking_mesh} ${_px_cooking_convex})
foreach(_px_target DarktidePhysXFoundation DarktidePhysXCommon DarktidePhysXCooking)
    target_include_directories(${_px_target} PRIVATE ${_px_include_dirs})
    target_compile_definitions(${_px_target} PRIVATE ${_px_definitions})
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${_px_target} PRIVATE -w)
    endif()
endforeach()
target_include_directories(DarktidePhysXCooking PUBLIC
    "${_px_physx}"
    "${_px_physx}/include"
    "${_px_shared}/include")
target_compile_definitions(DarktidePhysXCooking PUBLIC PX_PHYSX_STATIC_LIB)
target_link_libraries(DarktidePhysXCommon PUBLIC DarktidePhysXFoundation)
target_link_libraries(DarktidePhysXCooking PUBLIC DarktidePhysXCommon DarktidePhysXFoundation)
if(WIN32)
    target_link_libraries(DarktidePhysXFoundation PUBLIC ws2_32)
else()
    target_link_libraries(DarktidePhysXFoundation PUBLIC pthread dl)
endif()
endif()

# KHR_texture_basisu / KTX2 source decoder.  Use the transcoder-only path and
# Basis Universal's bundled single-file Zstd decoder; no encoder or tools.
set(DARKTIDE_BASISU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/basis_universal")
add_library(DarktideBasisTranscoder STATIC
    "${DARKTIDE_BASISU_DIR}/transcoder/basisu_transcoder.cpp"
    "${DARKTIDE_BASISU_DIR}/zstd/zstddeclib.c")
target_include_directories(DarktideBasisTranscoder PUBLIC
    "${DARKTIDE_BASISU_DIR}"
    "${DARKTIDE_BASISU_DIR}/transcoder")
target_compile_definitions(DarktideBasisTranscoder PUBLIC
    BASISD_SUPPORT_KTX2=1
    BASISD_SUPPORT_KTX2_ZSTD=1
    BASISU_SUPPORT_SSE=0)

if(WIN32)
    # DirectXTex's upstream project also configures tools, D3D, WIC, and
    # optional package dependencies.  The compiler only needs the portable
    # software BC compressor, so keep a private static target for its core
    # library sources instead of changing the vendored project.
    set(DARKTIDE_DIRECTXTEX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/DirectXTex")
    set(DARKTIDE_DIRECTXMATH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/DirectXMath")
    add_library(DarktideDirectXTex STATIC
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/BC.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/BC4BC5.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/BC6HBC7.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexCompress.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexConvert.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexDDS.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexHDR.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexImage.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexMipmaps.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexMisc.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexNormalMaps.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexPMAlpha.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexResize.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexTGA.cpp"
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex/DirectXTexUtil.cpp")
    target_include_directories(DarktideDirectXTex PUBLIC
        "${DARKTIDE_DIRECTXTEX_DIR}/DirectXTex"
        "${DARKTIDE_DIRECTXMATH_DIR}/Inc")
    target_compile_definitions(DarktideDirectXTex PRIVATE
        DIRECTX_TEX_STATIC
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00)
endif()

set(DARKTIDE_COMPILER_CORE_SOURCES
    src/compiler/resource_key.cpp
    src/compiler/resource_graph.cpp
    src/compiler/resource_output.cpp
    src/compiler/capability_registry.cpp
    src/compiler/source_queries.cpp
    src/compiler/material_builder.cpp
    src/compiler/resource_builder.cpp
    src/compiler/asset_definition_resolver.cpp
    src/compiler/compilation_plan.cpp
    src/compiler/rigid_animation.cpp
    src/compiler/compilation_context.cpp
    src/compiler/target_profile.cpp
    src/app/compiler.cpp
    src/app/asset_settings.cpp
    src/app/source_inspection.cpp
    src/gltf/glb_loader.cpp
    src/gltf/gltf_images.cpp
    src/gltf/gltf_meshes.cpp
    src/gltf/animation_sampler.cpp
    src/processing/tangent_generator.cpp
    src/processing/scene_scaler.cpp
    src/processing/material_canonicalizer.cpp
    src/processing/uv1_material_canonicalizer.cpp
    src/processing/attribute_canonicalizer.cpp
    src/processing/topology_canonicalizer.cpp
    src/processing/scene_feature_canonicalizer.cpp
    src/processing/native_profile_canonicalizer.cpp
    src/processing/animation_canonicalizer.cpp
    src/processing/texture_processor.cpp
    src/processing/image_sampler.cpp
    src/stingray/murmur_hash.cpp
    src/stingray/binary_writer.cpp
    src/stingray/cooked_resource.cpp
    src/stingray/physics/physx_cooking.cpp
    src/stingray/physics/authored_physics.cpp
    src/stingray/unit/unit_v115.cpp
    src/stingray/unit/unit_identity.cpp
    src/stingray/bones/bones_writer.cpp
    src/stingray/bones/bone_identity.cpp
    src/stingray/material/material_v61.cpp
    src/stingray/material/emissive_profile.cpp
    src/stingray/material/pbr_emissive_profile.cpp
    src/stingray/material/mask_profile.cpp
    src/stingray/material/transparent_profile.cpp
    src/stingray/texture/texture_writer.cpp
    src/stingray/texture/ktx2_decoder.cpp
    src/stingray/animation/animation_writer.cpp
    src/stingray/animation/animation_fitter.cpp
    src/stingray/animation/animation_evaluator.cpp
    src/stingray/state_machine/state_machine_writer.cpp
    src/validation/unit_validator.cpp
    third_party/mikktspace/mikktspace.c
)

add_library(DarktideCompilerCore STATIC ${DARKTIDE_COMPILER_CORE_SOURCES})
if(MSVC)
    target_sources(DarktideCompilerCore PRIVATE
        src/stingray/physics/physics_collection.cpp
        src/stingray/physics/authored_scene.cpp
        src/stingray/physics/physics_scene.cpp)
    target_compile_definitions(DarktideCompilerCore PUBLIC DTGLB_HAS_PHYSICS_COLLECTIONS=1)
endif()
target_include_directories(DarktideCompilerCore
    PUBLIC src
    PRIVATE
    third_party/mikktspace
    "${DARKTIDE_CGLTF_DIR}"
    "${DARKTIDE_DRACO_DIR}/src"
    "${DARKTIDE_LIBWEBP_DIR}/src"
    "${CMAKE_BINARY_DIR}"
)
apply_project_compiler_options(DarktideCompilerCore)

# Keep the command-line shell deliberately thin: consumers can link the core
# without pulling in argument parsing or defining a second main().
add_executable(DarktideGLBCompiler
    src/main.cpp
)
target_include_directories(DarktideGLBCompiler PRIVATE src)
apply_project_compiler_options(DarktideGLBCompiler)

# Embed the complete license texts so the three-file release can report them
# without shipping a separate licenses directory.
file(GLOB DARKTIDE_LICENSE_FILES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/licenses/*.txt")
list(APPEND DARKTIDE_LICENSE_FILES
    "${DARKTIDE_BASISU_DIR}/zstd/LICENSE")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DARKTIDE_LICENSE_FILES})
set(_darktide_license_header "${CMAKE_CURRENT_BINARY_DIR}/generated/third_party_licenses.h")
get_filename_component(_darktide_license_header_dir "${_darktide_license_header}" DIRECTORY)
file(MAKE_DIRECTORY "${_darktide_license_header_dir}")
set(_darktide_license_header_content
    "#pragma once\n\ninline constexpr char darktide_third_party_licenses[] =\n")
set(_darktide_license_index 0)
foreach(_license_file IN LISTS DARKTIDE_LICENSE_FILES)
    get_filename_component(_license_name "${_license_file}" NAME)
    file(READ "${_license_file}" _license_text)
    string(APPEND _darktide_license_header_content
        "R\"DTGLB_LICENSE_${_darktide_license_index}(===== ${_license_name} =====\n${_license_text})DTGLB_LICENSE_${_darktide_license_index}\"\n")
    math(EXPR _darktide_license_index "${_darktide_license_index} + 1")
endforeach()
string(APPEND _darktide_license_header_content ";\n")
file(WRITE "${_darktide_license_header}" "${_darktide_license_header_content}")
target_include_directories(DarktideGLBCompiler PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# LLVM-MinGW otherwise leaves libc++, unwind, and threading runtime DLLs beside
# the toolchain rather than the compiler executable.  Keep the delivered CLI
# self-contained; Windows system DLLs remain normal imports.
if(MINGW)
    target_link_options(DarktideGLBCompiler PRIVATE -static)
endif()

# Native GLB image decoding used by the Darktide texture cooker.
target_link_libraries(DarktideCompilerCore PRIVATE
    meshoptimizer
    draco::draco
    webpdecoder
    DarktideBasisTranscoder
    DarktidePhysXCooking)
if(WIN32)
    target_compile_definitions(DarktideCompilerCore PRIVATE DTGLB_USE_DIRECTXTEX)
    target_link_libraries(DarktideCompilerCore PRIVATE DarktideDirectXTex windowscodecs ole32)
else()
    target_link_libraries(DarktideCompilerCore PRIVATE PNG::PNG JPEG::JPEG)
endif()

target_link_libraries(DarktideGLBCompiler PRIVATE DarktideCompilerCore)
if(WIN32)
    target_link_libraries(DarktideGLBCompiler PRIVATE shell32)
endif()

# Build the Blender addon archive from its Python sources so it is available to
# both `cmake --install` and CPack without checking generated files into source.
file(GLOB DARKTIDE_BLENDER_ADDON_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/blender/darktide_assets/*.py")
set(_darktide_blender_addon_entries)
foreach(_addon_source IN LISTS DARKTIDE_BLENDER_ADDON_SOURCES)
    file(RELATIVE_PATH _addon_entry
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/blender" "${_addon_source}")
    list(APPEND _darktide_blender_addon_entries "${_addon_entry}")
endforeach()
set(DARKTIDE_BLENDER_ADDON_ZIP "${CMAKE_CURRENT_BINARY_DIR}/darktide_assets_blender.zip")
add_custom_command(
    OUTPUT "${DARKTIDE_BLENDER_ADDON_ZIP}"
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${DARKTIDE_BLENDER_ADDON_ZIP}"
        --format=zip -- ${_darktide_blender_addon_entries}
    DEPENDS ${DARKTIDE_BLENDER_ADDON_SOURCES}
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tools/blender"
    COMMENT "Packaging the Darktide Blender addon"
    VERBATIM)
add_custom_target(DarktideBlenderAddonZip ALL DEPENDS "${DARKTIDE_BLENDER_ADDON_ZIP}")

# The Wine download uses the same x64 Windows compiler and addon. Keep a
# separately named archive so Linux users can select the supported runner.
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_darktide_wine_stage "${CMAKE_CURRENT_BINARY_DIR}/linux-wine-stage")
    file(MAKE_DIRECTORY "${_darktide_wine_stage}")
    set(DARKTIDE_LINUX_WINE_ZIP "${CMAKE_CURRENT_BINARY_DIR}/DarktideGLBCompiler-linux-wine-x64.zip")
    add_custom_target(DarktideLinuxWineBundle ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_darktide_wine_stage}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:DarktideGLBCompiler>" "${_darktide_wine_stage}/DarktideGLBCompiler.exe"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${_darktide_wine_stage}/README.md"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${DARKTIDE_BLENDER_ADDON_ZIP}" "${_darktide_wine_stage}/darktide_assets_blender.zip"
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${DARKTIDE_LINUX_WINE_ZIP}"
            --format=zip -- README.md DarktideGLBCompiler.exe darktide_assets_blender.zip
        DEPENDS DarktideGLBCompiler DarktideBlenderAddonZip
        WORKING_DIRECTORY "${_darktide_wine_stage}"
        COMMENT "Packaging the Linux Wine SDK"
        VERBATIM)
endif()

# The release archive intentionally contains only these three root-level files.
install(TARGETS DarktideGLBCompiler RUNTIME DESTINATION ".")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md" DESTINATION ".")
install(FILES "${DARKTIDE_BLENDER_ADDON_ZIP}" DESTINATION ".")

if(WIN32)
    set(_darktide_package_platform windows)
elseif(APPLE)
    set(_darktide_package_platform macos)
else()
    set(_darktide_package_platform linux)
endif()
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _darktide_package_arch)
if(_darktide_package_arch MATCHES "^(x86_64|amd64)$")
    set(_darktide_package_arch x64)
elseif(_darktide_package_arch MATCHES "^(aarch64|arm64)$")
    set(_darktide_package_arch arm64)
elseif(_darktide_package_arch MATCHES "^i[3-6]86$")
    set(_darktide_package_arch x86)
endif()
set(CPACK_GENERATOR ZIP)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_PACKAGE_FILE_NAME "DarktideGLBCompiler-${_darktide_package_platform}-${_darktide_package_arch}")
include(CPack)
