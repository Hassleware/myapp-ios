cmake_minimum_required(VERSION 3.24)
project(gamelab C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Leave empty for the loader that holds every game. Set to a game's folder name for a standalone release app.
set(SINGLE_GAME "" CACHE STRING "Build a standalone app containing only this game")
set(APP_NAME "Game Lab" CACHE STRING "Name shown under the app icon")
set(APP_BUNDLE_ID "com.starter.gamelab" CACHE STRING "App identifier. Never change it after a game is released")
set(APP_VERSION "1.0" CACHE STRING "Version shown in the App Store")
set(APP_BUILD_NUMBER "1" CACHE STRING "Build number. Must go up with every App Store upload")
set(BUILD_FINGERPRINT "dev" CACHE STRING "Fingerprint of the native code, set by tools/dev.py")
set(MOLTENVK_LIB "" CACHE FILEPATH "iPhone builds: path to libMoltenVK.a")

# ---- libraries, downloaded on the first configure ----
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.20 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3)

# These three are plain source folders with no build files of their own that we want to run.
FetchContent_Declare(lua GIT_REPOSITORY https://github.com/lua/lua.git GIT_TAG v5.4.8 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_Declare(volk GIT_REPOSITORY https://github.com/zeux/volk.git GIT_TAG vulkan-sdk-1.4.363.0 GIT_SHALLOW TRUE SOURCE_SUBDIR _none)
FetchContent_MakeAvailable(lua vulkan_headers volk)

add_library(lua STATIC ${lua_SOURCE_DIR}/onelua.c)
target_compile_definitions(lua PRIVATE MAKE_LIB)
target_include_directories(lua PUBLIC ${lua_SOURCE_DIR})
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  target_compile_definitions(lua PRIVATE LUA_USE_IOS)
endif()

# ---- the app ----
set(ENGINE_SOURCES engine/main.cpp engine/gfx.cpp engine/input.cpp engine/script.cpp)

# Wi-Fi syncing belongs to the loader only. Release builds of a single game never contain it.
set(WITH_SYNC OFF)
if(NOT SINGLE_GAME AND NOT WIN32)
  set(WITH_SYNC ON)
  list(APPEND ENGINE_SOURCES engine/sync.cpp)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  add_executable(gamelab MACOSX_BUNDLE ${ENGINE_SOURCES})
else()
  add_executable(gamelab ${ENGINE_SOURCES})
endif()

target_include_directories(gamelab PRIVATE engine ${vulkan_headers_SOURCE_DIR}/include)
target_link_libraries(gamelab PRIVATE SDL3::SDL3-static lua)
target_compile_definitions(gamelab PRIVATE BUILD_FINGERPRINT="${BUILD_FINGERPRINT}")
if(WITH_SYNC)
  target_compile_definitions(gamelab PRIVATE LOADER_SYNC)
endif()
if(SINGLE_GAME)
  target_compile_definitions(gamelab PRIVATE SINGLE_GAME="${SINGLE_GAME}")
endif()
if(GFX_SCREENSHOT)
  target_compile_definitions(gamelab PRIVATE GFX_SCREENSHOT)
endif()
if(MSVC)
  target_compile_options(gamelab PRIVATE /W3)
else()
  target_compile_options(gamelab PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  # The iPhone has no Vulkan of its own. MoltenVK is linked in and runs Vulkan on Metal.
  if(NOT MOLTENVK_LIB)
    message(FATAL_ERROR "Set MOLTENVK_LIB to the path of libMoltenVK.a")
  endif()
  target_link_libraries(gamelab PRIVATE ${MOLTENVK_LIB}
    "-framework Metal" "-framework QuartzCore" "-framework IOSurface" "-framework Foundation"
    "-framework UIKit" "-framework CoreGraphics" "-lc++")

  # Only the loader talks to the PC and shows its files in the Files app.
  set(IOS_LOADER_KEYS "")
  if(WITH_SYNC)
    set(IOS_LOADER_KEYS "    <key>NSLocalNetworkUsageDescription</key>
    <string>Finds the dev server on your PC to load your games.</string>
    <key>UIFileSharingEnabled</key>
    <true/>
    <key>LSSupportsOpeningDocumentsInPlace</key>
    <true/>")
  endif()
  configure_file(ios/Info.plist.in ${CMAKE_BINARY_DIR}/Info.plist @ONLY)
  set_target_properties(gamelab PROPERTIES
    OUTPUT_NAME "GameLab"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_BINARY_DIR}/Info.plist
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${APP_BUNDLE_ID}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0"
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO")

  # Pack the engine's shaders and the games inside the app.
  add_custom_command(TARGET gamelab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/engine/shaders $<TARGET_BUNDLE_CONTENT_DIR:gamelab>/shaders)
  if(SINGLE_GAME)
    add_custom_command(TARGET gamelab POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/games/${SINGLE_GAME} $<TARGET_BUNDLE_CONTENT_DIR:gamelab>/games/${SINGLE_GAME})
  else()
    add_custom_command(TARGET gamelab POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/games $<TARGET_BUNDLE_CONTENT_DIR:gamelab>/games)
  endif()
else()
  # On the PC, Vulkan is found at run time, and games are read straight from this repo.
  target_sources(gamelab PRIVATE ${volk_SOURCE_DIR}/volk.c)
  target_include_directories(gamelab PRIVATE ${volk_SOURCE_DIR})
  target_compile_definitions(gamelab PRIVATE USE_VOLK VK_NO_PROTOTYPES DEV_REPO_DIR="${CMAKE_SOURCE_DIR}")
endif()
