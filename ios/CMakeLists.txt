# BotW-Native for iOS (iPhone, arm64). Separate project so the macOS build (../CMakeLists.txt) stays
# untouched; it compiles the same generated/, runtime/ and runtime/cemu sources plus ios/src.
#
# Use ios/scripts/build.sh (configures build-ios/<sdk>, builds, signs). By hand:
#   cmake -S ios -B build-ios/iphoneos -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0
#   cmake --build build-ios/iphoneos -j6
cmake_minimum_required(VERSION 3.24)
project(BotWRecompIOS LANGUAGES C CXX OBJC OBJCXX)

if(NOT IOS)
  message(FATAL_ERROR "configure with -DCMAKE_SYSTEM_NAME=iOS")
endif()

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
  set(BOTW_SDK iphonesimulator)
  set(BOTW_PLATFORM iPhoneSimulator)
else()
  set(BOTW_SDK iphoneos)
  set(BOTW_PLATFORM iPhoneOS)
endif()
set(BOTW_BUNDLE_ID "com.example.BotWRecomp" CACHE STRING "CFBundleIdentifier (build.sh passes BOTW_BUNDLE_ID)")
set(BOTW_IOS_DEPS "${ROOT}/ios-deps" CACHE PATH "ios/scripts/build_deps.sh output")
set(DEPS ${BOTW_IOS_DEPS}/prefix-${BOTW_SDK})

# deps live outside the SDK sysroot, which the iOS platform module restricts find_package to
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
find_package(SDL3 REQUIRED CONFIG PATHS ${DEPS}/lib/cmake/SDL3 NO_DEFAULT_PATH)
find_package(fmt REQUIRED CONFIG PATHS ${DEPS}/lib/cmake/fmt NO_DEFAULT_PATH)
find_package(zstd REQUIRED CONFIG PATHS ${DEPS}/lib/cmake/zstd NO_DEFAULT_PATH)

# A14 == M1 ISA (armv8.5-a), so the guest code is compiled for the same features as on macOS.
set(BOTW_CPU -mcpu=apple-a14)

# ---- recompiled guest code (same flags as the macOS build) ----------------------------------
file(GLOB GUEST_SOURCES CONFIGURE_DEPENDS
  ${ROOT}/generated/fn_*.cpp ${ROOT}/generated/big_*.cpp ${ROOT}/generated/huge_*.cpp
  ${ROOT}/generated/table.cpp ${ROOT}/generated/hle_stubs.cpp)
add_library(guest OBJECT ${GUEST_SOURCES})
target_include_directories(guest PRIVATE ${ROOT}/runtime/include ${ROOT}/generated)
target_compile_options(guest PRIVATE -O2 -gline-tables-only -w -fwrapv -fno-strict-aliasing ${BOTW_CPU}
  -ffp-contract=off)
file(GLOB BIG_SOURCES CONFIGURE_DEPENDS ${ROOT}/generated/big_*.cpp)
file(GLOB HUGE_SOURCES CONFIGURE_DEPENDS ${ROOT}/generated/huge_*.cpp)
set_source_files_properties(${BIG_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1")
set_source_files_properties(${HUGE_SOURCES} PROPERTIES COMPILE_OPTIONS "-O0")

# ---- vendored Cemu Latte GPU + shims ---------------------------------------------------------
file(GLOB_RECURSE CEMU_SOURCES CONFIGURE_DEPENDS
  ${ROOT}/runtime/cemu/Cafe/*.cpp ${ROOT}/runtime/cemu/Cafe/*.mm
  ${ROOT}/runtime/cemu/Common/*.cpp ${ROOT}/runtime/cemu/util/*.cpp
  ${ROOT}/runtime/cemu/Cemu/FileCache/*.cpp
  ${ROOT}/runtime/cemu/imgui/*.mm
  ${ROOT}/runtime/cemu_shims/*.cpp ${ROOT}/runtime/cemu_shims/*.mm)
list(FILTER CEMU_SOURCES EXCLUDE REGEX "LatteOverlay\\.cpp$|/dependencies/|SystemInfo(Win|Linux|BSD)\\.cpp$")
# The desktop Vulkan backend is not part of the iOS Metal build.
list(FILTER CEMU_SOURCES EXCLUDE REGEX "/Renderer/Vulkan/|imgui_impl_vulkan|SystemInfoWin\\.cpp$|/Common/windows/")
# AppKit view/layer creation: replaced by ios/src/MetalLayer_ios.mm
list(FILTER CEMU_SOURCES EXCLUDE REGEX "Renderer/MetalView\\.mm$|Renderer/Metal/MetalLayer\\.mm$")
list(APPEND CEMU_SOURCES
  ${ROOT}/runtime/cemu/dependencies/imgui/imgui.cpp
  ${ROOT}/runtime/cemu/dependencies/imgui/imgui_draw.cpp
  ${ROOT}/runtime/cemu/dependencies/imgui/imgui_tables.cpp
  ${ROOT}/runtime/cemu/dependencies/imgui/imgui_widgets.cpp)
add_library(cemu_gpu OBJECT ${CEMU_SOURCES})
# ios/compat first: CommonCrypto-backed <openssl/sha.h>/<openssl/evp.h>, empty <png.h>.
# boost/glm are header-only; take them from their Homebrew kegs (no other Homebrew headers).
target_include_directories(cemu_gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat
  ${ROOT}/runtime/cemu_shims ${ROOT}/runtime/cemu ${ROOT}/runtime/cemu/Cafe
  ${ROOT}/runtime/cemu/dependencies/metal-cpp ${ROOT}/runtime/cemu/dependencies/imgui
  /opt/homebrew/opt/boost/include /opt/homebrew/opt/glm/include)
target_link_libraries(cemu_gpu PRIVATE fmt::fmt zstd::libzstd_static)
target_compile_definitions(cemu_gpu PRIVATE BOOST_NOWIDE_NO_LIB ENABLE_METAL BOTWRECOMP IMGUI_IMPL_METAL_CPP
  EMULATOR_VERSION_MAJOR=2 EMULATOR_VERSION_MINOR=6 EMULATOR_VERSION_PATCH=0 EMULATOR_HASH=a53cf0e)
target_compile_options(cemu_gpu PRIVATE -w -O2 -g1
  "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/compat/botw_ios_platform.h" "SHELL:-include Common/precompiled.h")

# ---- runtime + iOS host layer -----------------------------------------------------------------
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${ROOT}/runtime/src/*.cpp ${ROOT}/runtime/src/*.mm)
file(GLOB IOS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.mm ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
# main.cpp's main() becomes botw_main(); ios/src/main_ios.mm owns main() (UIApplicationMain via SDL)
set_source_files_properties(${ROOT}/runtime/src/main.cpp PROPERTIES COMPILE_DEFINITIONS "main=botw_main")
# CEMU_SOURCES with the AppKit view excluded need a CreateMetalLayer: ios/src/MetalLayer_ios.mm
add_executable(BotWRecomp MACOSX_BUNDLE ${RUNTIME_SOURCES} ${IOS_SOURCES}
  $<TARGET_OBJECTS:guest> $<TARGET_OBJECTS:cemu_gpu>)
target_include_directories(BotWRecomp PRIVATE ${ROOT}/runtime/include ${ROOT}/runtime/src ${ROOT}/generated
)
target_compile_options(BotWRecomp PRIVATE -Wall -Wno-unused-function -Wno-unused-variable -fwrapv ${BOTW_CPU}
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(BotWRecomp PRIVATE z SDL3::SDL3-static fmt::fmt zstd::libzstd_static
  "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework UIKit"
  "-framework CoreGraphics" "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework VideoToolbox"
  "-framework CoreMedia" "-framework CoreVideo" "-framework GameController" "-framework AVFoundation"
  "-framework AudioToolbox")

if(BOTW_SDK STREQUAL "iphoneos")
  # MetalFX (upscaling) exists on devices only, not in the simulator SDK
  target_link_libraries(BotWRecomp PRIVATE "-framework MetalFX")
endif()

set(BOTW_MIN_IOS ${CMAKE_OSX_DEPLOYMENT_TARGET})
set_target_properties(BotWRecomp PROPERTIES
  OUTPUT_NAME BotWRecomp
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
  MACOSX_BUNDLE_GUI_IDENTIFIER ${BOTW_BUNDLE_ID}
  MACOSX_BUNDLE_BUNDLE_NAME BotWRecomp
  MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1
  MACOSX_BUNDLE_BUNDLE_VERSION 1)
