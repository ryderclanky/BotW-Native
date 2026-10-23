cmake_minimum_required(VERSION 3.24)

# APPLE is not set until project(), so pick languages from the host.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(CMAKE_OSX_ARCHITECTURES arm64)
  set(CMAKE_OSX_DEPLOYMENT_TARGET 13.3)
endif()

# Windows and Linux use Vulkan. Metal stays the Apple backend from upstream BotW Recompiled.
project(BotWRecomp
  DESCRIPTION "Breath of the Wild recompiled for Windows and Linux (Vulkan)"
  LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJC)
  enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Metal stays the default on Apple. Vulkan is the Windows/Linux backend and can be turned on
# beside Metal (MoltenVK) with -DBOTW_ENABLE_VULKAN=ON.
if(APPLE)
  set(_botw_metal_default ON)
  set(_botw_vulkan_default OFF)
else()
  set(_botw_metal_default OFF)
  set(_botw_vulkan_default ON)
endif()
option(BOTW_ENABLE_METAL "Build the Metal renderer (Apple only)" ${_botw_metal_default})
option(BOTW_ENABLE_VULKAN "Build the Vulkan renderer" ${_botw_vulkan_default})
if(NOT APPLE AND BOTW_ENABLE_METAL)
  message(STATUS "Metal is Apple-only; disabling BOTW_ENABLE_METAL")
  set(BOTW_ENABLE_METAL OFF CACHE BOOL "Build the Metal renderer (Apple only)" FORCE)
endif()
if(NOT APPLE AND NOT BOTW_ENABLE_VULKAN)
  message(FATAL_ERROR "Windows and Linux builds require -DBOTW_ENABLE_VULKAN=ON")
endif()
if(NOT BOTW_ENABLE_METAL AND NOT BOTW_ENABLE_VULKAN)
  message(FATAL_ERROR "Enable BOTW_ENABLE_METAL, BOTW_ENABLE_VULKAN, or both")
endif()

if(APPLE)
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
  if(EXISTS /opt/homebrew/opt/openssl@3)
    set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
  endif()
endif()

include(FetchContent)
find_package(SDL3 CONFIG QUIET)
if(NOT SDL3_FOUND)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.10
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(SDL)
endif()

find_package(fmt REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_library(ZSTD_LIBRARY zstd REQUIRED)
if(BOTW_ENABLE_VULKAN)
  find_package(Vulkan REQUIRED)
  find_package(glslang CONFIG QUIET)
  if(NOT TARGET glslang::glslang AND NOT TARGET glslang)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glslang
      GIT_REPOSITORY https://github.com/KhronosGroup/glslang.git
      GIT_TAG 15.1.0
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(glslang)
  endif()
endif()

if(UNIX AND NOT APPLE)
  find_package(X11 REQUIRED)
  # Wayland headers are only needed when the renderer is built with HAS_WAYLAND.
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(WAYLAND QUIET wayland-client)
  endif()
endif()

# Recompiled guest code (tools/recompile.py writes generated/). Without it, a stub links so the
# Vulkan device can still be brought up.
file(GLOB GUEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/generated/fn_*.cpp
  ${CMAKE_SOURCE_DIR}/generated/big_*.cpp
  ${CMAKE_SOURCE_DIR}/generated/huge_*.cpp
  ${CMAKE_SOURCE_DIR}/generated/table.cpp
  ${CMAKE_SOURCE_DIR}/generated/hle_stubs.cpp)
if(NOT GUEST_SOURCES)
  set(GUEST_SOURCES ${CMAKE_SOURCE_DIR}/runtime/src/guest_stub.cpp)
  message(STATUS "generated/ is empty: linking a guest stub (graphics init only; the game will not run)")
endif()
add_library(guest OBJECT ${GUEST_SOURCES})
target_include_directories(guest PRIVATE runtime/include generated)
if(APPLE)
  target_compile_options(guest PRIVATE -O2 -gline-tables-only -w -fwrapv -fno-strict-aliasing -mcpu=apple-m1
    -ffp-contract=off)
else()
  target_compile_options(guest PRIVATE -O2 -g1 -w -fwrapv -fno-strict-aliasing -ffp-contract=off)
endif()
file(GLOB BIG_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/big_*.cpp)
file(GLOB HUGE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/huge_*.cpp)
if(BIG_SOURCES)
  set_source_files_properties(${BIG_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1")
endif()
if(HUGE_SOURCES)
  set_source_files_properties(${HUGE_SOURCES} PROPERTIES COMPILE_OPTIONS "-O0")
endif()

# Vendored Cemu Latte GPU layer (runtime/cemu, MPL-2.0) + shims.
file(GLOB_RECURSE CEMU_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/*.mm
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Common/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu/util/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cemu/FileCache/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/imgui/*.mm
  ${CMAKE_SOURCE_DIR}/runtime/cemu_shims/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu_shims/*.mm)
list(FILTER CEMU_SOURCES EXCLUDE REGEX "LatteOverlay\\.cpp$|/dependencies/")
if(NOT WIN32)
  list(FILTER CEMU_SOURCES EXCLUDE REGEX "SystemInfoWin\\.cpp$|/Common/windows/")
endif()
if(WIN32)
  # FileStream_unix, unix/platform.cpp (GetTickCount) and SystemInfoUnix (sys/times.h) are POSIX-only.
  list(FILTER CEMU_SOURCES EXCLUDE REGEX "/Common/unix/|SystemInfoUnix\\.cpp$")
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(FILTER CEMU_SOURCES EXCLUDE REGEX "SystemInfoLinux\\.cpp$")
endif()
if(NOT APPLE)
  list(FILTER CEMU_SOURCES EXCLUDE REGEX "SystemInfoMac\\.cpp$|\\.mm$")
endif()
if(NOT BOTW_ENABLE_METAL)
  list(FILTER CEMU_SOURCES EXCLUDE REGEX "/Renderer/Metal/|MetalView\\.mm$|LatteDecompilerEmitMSL\\.cpp$|imgui_impl_metal")
endif()
if(NOT BOTW_ENABLE_VULKAN)
  list(FILTER CEMU_SOURCES EXCLUDE REGEX "/Renderer/Vulkan/")
endif()
list(APPEND CEMU_SOURCES
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_draw.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_tables.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_widgets.cpp)
if(BOTW_ENABLE_VULKAN)
  list(APPEND CEMU_SOURCES ${CMAKE_SOURCE_DIR}/runtime/cemu/imgui/imgui_impl_vulkan.cpp)
endif()

add_library(cemu_gpu OBJECT ${CEMU_SOURCES})
target_include_directories(cemu_gpu PRIVATE runtime/cemu_shims runtime/cemu runtime/cemu/Cafe
  runtime/cemu/dependencies/imgui runtime/include)
if(BOTW_ENABLE_METAL)
  target_include_directories(cemu_gpu PRIVATE runtime/cemu/dependencies/metal-cpp)
endif()
if(APPLE)
  target_include_directories(cemu_gpu PRIVATE /opt/homebrew/include)
endif()
if(WAYLAND_FOUND)
  target_include_directories(cemu_gpu PRIVATE ${WAYLAND_INCLUDE_DIRS})
endif()

target_compile_definitions(cemu_gpu PRIVATE BOOST_NOWIDE_NO_LIB BOTWRECOMP
  EMULATOR_VERSION_MAJOR=2 EMULATOR_VERSION_MINOR=6 EMULATOR_VERSION_PATCH=0 EMULATOR_HASH=a53cf0e)
if(BOTW_ENABLE_METAL)
  target_compile_definitions(cemu_gpu PRIVATE ENABLE_METAL IMGUI_IMPL_METAL_CPP)
endif()
if(BOTW_ENABLE_VULKAN)
  target_compile_definitions(cemu_gpu PUBLIC ENABLE_VULKAN VK_NO_PROTOTYPES)
  if(WIN32)
    target_compile_definitions(cemu_gpu PUBLIC VK_USE_PLATFORM_WIN32_KHR)
  elseif(APPLE)
    target_compile_definitions(cemu_gpu PUBLIC VK_USE_PLATFORM_METAL_EXT VK_USE_PLATFORM_MACOS_MVK)
  else()
    target_compile_definitions(cemu_gpu PUBLIC VK_USE_PLATFORM_XLIB_KHR VK_USE_PLATFORM_XCB_KHR)
    if(WAYLAND_FOUND)
      target_compile_definitions(cemu_gpu PUBLIC HAS_WAYLAND VK_USE_PLATFORM_WAYLAND_KHR)
    endif()
  endif()
endif()
target_compile_options(cemu_gpu PRIVATE -w -O2 -g1 "SHELL:-include Common/precompiled.h")

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/runtime/src/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/src/*.mm)
list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "/guest_stub\\.cpp$")
if(NOT APPLE)
  list(FILTER RUNTIME_SOURCES EXCLUDE REGEX "\\.mm$")
endif()

add_executable(BotWRecomp ${RUNTIME_SOURCES} $<TARGET_OBJECTS:guest> $<TARGET_OBJECTS:cemu_gpu>)
target_include_directories(BotWRecomp PRIVATE runtime/include runtime/src generated)
if(APPLE)
  target_compile_options(BotWRecomp PRIVATE -Wall -Wno-unused-function -Wno-unused-variable -fwrapv -mcpu=apple-m1
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
else()
  target_compile_options(BotWRecomp PRIVATE -Wall -Wno-unused-function -Wno-unused-variable -fwrapv)
endif()
if(BOTW_ENABLE_VULKAN)
  target_compile_definitions(BotWRecomp PRIVATE ENABLE_VULKAN)
endif()
if(BOTW_ENABLE_METAL)
  target_compile_definitions(BotWRecomp PRIVATE ENABLE_METAL)
endif()

# Community SDL game controller database, copied next to the executable.
# SDL_AddGamepadMappingsFromFile reads it at startup. The compile path is the
# fallback when the process is not started from the build directory.
set(_botw_controller_db "${CMAKE_SOURCE_DIR}/runtime/gamecontrollerdb.txt")
file(TO_CMAKE_PATH "${_botw_controller_db}" _botw_controller_db_fwd)
target_compile_definitions(BotWRecomp PRIVATE "BOTW_GAMECONTROLLER_DB=\"${_botw_controller_db_fwd}\"")
add_custom_command(TARGET BotWRecomp POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_botw_controller_db}" "$<TARGET_FILE_DIR:BotWRecomp>/gamecontrollerdb.txt"
  VERBATIM)

set(_botw_glslang "")
if(BOTW_ENABLE_VULKAN)
  if(TARGET glslang::glslang)
    list(APPEND _botw_glslang glslang::glslang)
  elseif(TARGET glslang)
    list(APPEND _botw_glslang glslang)
  else()
    message(FATAL_ERROR "glslang target not found (need glslang 15.x)")
  endif()
  if(TARGET glslang::SPIRV)
    list(APPEND _botw_glslang glslang::SPIRV)
  elseif(TARGET SPIRV)
    list(APPEND _botw_glslang SPIRV)
  else()
    message(FATAL_ERROR "glslang SPIRV target not found")
  endif()
endif()

# cemu_gpu is an OBJECT library, so usage requirements (glslang's include path) have to be
# attached here. $<TARGET_OBJECTS> does not carry them onto the executable.
if(_botw_glslang)
  target_link_libraries(cemu_gpu PUBLIC ${_botw_glslang})
endif()
# Shims under cemu_gpu include the host headers (SDL events for the touch overlay).
target_link_libraries(cemu_gpu PUBLIC SDL3::SDL3)
# The glslang build tree exposes <glslang/Public/...> but Cemu includes <glslang/SPIRV/...>,
# which is how an installed glslang lays the headers out.
if(BOTW_ENABLE_VULKAN AND DEFINED glslang_SOURCE_DIR)
  set(_glslang_inc ${CMAKE_BINARY_DIR}/glslang-include)
  file(MAKE_DIRECTORY ${_glslang_inc}/glslang)
  foreach(_sub Public SPIRV Include)
    if(NOT EXISTS ${_glslang_inc}/glslang/${_sub})
      if(_sub STREQUAL "SPIRV")
        set(_src ${glslang_SOURCE_DIR}/SPIRV)
      else()
        set(_src ${glslang_SOURCE_DIR}/glslang/${_sub})
      endif()
      file(CREATE_LINK ${_src} ${_glslang_inc}/glslang/${_sub} SYMBOLIC)
    endif()
  endforeach()
  target_include_directories(cemu_gpu PUBLIC ${_glslang_inc})
endif()

target_link_libraries(BotWRecomp PRIVATE
  SDL3::SDL3 fmt::fmt OpenSSL::Crypto PNG::PNG ZLIB::ZLIB ${ZSTD_LIBRARY} Threads::Threads
  ${_botw_glslang})
if(BOTW_ENABLE_VULKAN)
  target_link_libraries(BotWRecomp PRIVATE Vulkan::Vulkan)
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(BotWRecomp PRIVATE ${CMAKE_DL_LIBS} X11::X11)
endif()
if(WIN32)
  # psapi: GetProcessMemoryInfo (shader-cache log).
  # ntdll: NtQuerySystemInformation in SystemInfoWin.cpp. That file's
  # `#pragma comment(lib, "ntdll.lib")` is MSVC-only; gcc ignores it, so this
  # link is what resolves the symbol. The Windows CI job links SystemInfoWin.cpp
  # into this executable and fails if ntdll is dropped.
  target_link_libraries(BotWRecomp PRIVATE psapi ntdll)
endif()
if(APPLE)
  target_link_libraries(BotWRecomp PRIVATE
    "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit" "-framework MetalFX"
    "-framework CoreGraphics" "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework VideoToolbox"
    "-framework CoreMedia" "-framework CoreVideo")
endif()

message(STATUS "BotWRecomp backends: Metal=${BOTW_ENABLE_METAL} Vulkan=${BOTW_ENABLE_VULKAN}")

# Native Metal GPU path (docs/native-renderer.md): option BOTW_NATIVE_GPU, default OFF (gpu_native.cmake).
include(${CMAKE_SOURCE_DIR}/runtime/src/gpu_native/gpu_native.cmake OPTIONAL)
