# Vendored PhysX 4.1.2. Keep all upstream project flags
# within this function/subdirectory rather than changing compiler-wide options.
function(darktide_add_physx_runtime)
    set(PHYSX_ROOT_DIR "${DARKTIDE_PHYSX_DIR}/physx")
    set(PXSHARED_PATH "${DARKTIDE_PHYSX_DIR}/pxshared")
    set(CMAKEMODULES_PATH "${DARKTIDE_PHYSX_DIR}/externals/cmakemodules")
    set(CMAKEMODULES_VERSION "1.27")
    set(TARGET_BUILD_PLATFORM windows)
    set(PX_GENERATE_STATIC_LIBRARIES ON CACHE BOOL "Static PhysX libraries" FORCE)
    set(NV_USE_STATIC_WINCRT ON CACHE BOOL "Static PhysX CRT" FORCE)
    # BVH34 triangle-mesh contact/query paths require the Intel SIMD backend.
    # The legacy scalar graph is sufficient for cooking, but not simulation.
    set(PX_SCALAR_MATH OFF CACHE BOOL "Use Intel SIMD physics runtime" FORCE)
    set(PX_OUTPUT_LIB_DIR "${CMAKE_BINARY_DIR}/third_party/physx-sdk" CACHE PATH "PhysX libraries" FORCE)
    set(PX_OUTPUT_BIN_DIR "${CMAKE_BINARY_DIR}/third_party/physx-sdk" CACHE PATH "PhysX outputs" FORCE)
    # New MSVC releases warn about constructs in this historical SDK. Preserve
    # warnings without editing upstream code or treating new warnings as errors.
    set(PHYSX_CXX_FLAGS "/W3 /GF /GR- /Gd /fp:fast /wd4996" CACHE INTERNAL "Pinned SDK compiler flags" FORCE)
    set(CMAKE_CXX_STANDARD 14)
    add_subdirectory("${PHYSX_ROOT_DIR}/source/compiler/cmake"
                     "${CMAKE_BINARY_DIR}/third_party/physx-sdk" EXCLUDE_FROM_ALL)
    add_library(DarktidePhysXCooking INTERFACE)
    target_include_directories(DarktidePhysXCooking INTERFACE
        "${PHYSX_ROOT_DIR}" "${PHYSX_ROOT_DIR}/include" "${PXSHARED_PATH}/include")
    target_compile_definitions(DarktidePhysXCooking INTERFACE PX_PHYSX_STATIC_LIB)
    target_link_libraries(DarktidePhysXCooking INTERFACE PhysXCooking PhysXExtensions)
endfunction()
