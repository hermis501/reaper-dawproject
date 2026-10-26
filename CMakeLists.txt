# reaper_dawproject: DAWproject import/export for REAPER, plus dawproject2rpp and rpp2dawproject.
cmake_minimum_required(VERSION 3.20)
# macOS: one universal binary for Apple silicon and Intel Macs
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS version")
project(reaper_dawproject VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  # static runtime: the DLL works without a Visual C++ redistributable
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  add_compile_options(/utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Single-configuration generators (Makefiles, Ninja: macOS) also put the binaries in
# build/Release, like Visual Studio does.
get_property(_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _multi_config)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
  endif()
  if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${CMAKE_BUILD_TYPE}")
  endif()
endif()

# The bundled JSFX (stand-ins for DAWproject built-in devices) are compiled into the binaries.
set(JSFX_NAMES dawproject_eq dawproject_compressor dawproject_gate dawproject_limiter)
set(_jsfx_header "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_jsfx_data.h")
set(_jsfx_content "// Generated from jsfx/*.jsfx by CMakeLists.txt - do not edit.
#pragma once
namespace dawp {
static const JsfxFile kBundledJsfx[] = {
")
foreach(_name ${JSFX_NAMES})
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/jsfx/${_name}.jsfx" _text)
  string(APPEND _jsfx_content "  {\"${_name}.jsfx\", R\"JSFX(${_text})JSFX\"},
")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "jsfx/${_name}.jsfx")
endforeach()
string(APPEND _jsfx_content "};
}  // namespace dawp
")
file(WRITE "${_jsfx_header}.tmp" "${_jsfx_content}")
configure_file("${_jsfx_header}.tmp" "${_jsfx_header}" COPYONLY)

# Conversion code used by the extension and the command-line tools
add_library(dawproject_core STATIC
  src/builtin_fx.cpp
  src/converter.cpp
  src/dawproject_container.cpp
  src/exporter.cpp
  src/media_info.cpp
  src/plugin_state.cpp
  src/rpp_reader.cpp
  src/rpp_sink.cpp
  src/tempo_map.cpp
  src/util.cpp
  third_party/pugixml/pugixml.cpp
  third_party/miniz/miniz.c
)
target_include_directories(dawproject_core PUBLIC src third_party/pugixml third_party/miniz "${CMAKE_CURRENT_BINARY_DIR}/generated")

# REAPER loads every reaper_*.dll/.dylib/.so from its UserPlugins folder
set(_suffix "")
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_suffix "-x64")
endif()
add_library(reaper_dawproject SHARED src/reaper_main.cpp src/reaper_ext.cpp)
target_include_directories(reaper_dawproject PRIVATE third_party/reaper-sdk)
target_link_libraries(reaper_dawproject PRIVATE dawproject_core)
set_target_properties(reaper_dawproject PROPERTIES PREFIX "" OUTPUT_NAME "reaper_dawproject${_suffix}")
if(APPLE)
  # REAPER provides the Win32-like API (SWELL) on macOS; swell-modstub.mm looks it up at load time
  enable_language(OBJCXX)
  target_sources(reaper_dawproject PRIVATE third_party/WDL/swell/swell-modstub.mm)
  target_compile_definitions(reaper_dawproject PRIVATE SWELL_PROVIDED_BY_APP)
  target_link_libraries(reaper_dawproject PRIVATE "-framework AppKit")
  set_target_properties(reaper_dawproject PROPERTIES SUFFIX ".dylib")
endif()

foreach(_tool dawproject2rpp rpp2dawproject)
  add_executable(${_tool} cli/${_tool}.cpp)
  target_link_libraries(${_tool} PRIVATE dawproject_core)
endforeach()

if(WIN32)
  set(_userplugins "$ENV{APPDATA}/REAPER/UserPlugins")
elseif(APPLE)
  set(_userplugins "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
else()
  set(_userplugins "$ENV{HOME}/.config/REAPER/UserPlugins")
endif()
set(REAPER_USERPLUGINS_DIR "${_userplugins}" CACHE PATH "REAPER UserPlugins folder used by 'cmake --install'")
install(TARGETS reaper_dawproject RUNTIME DESTINATION "${REAPER_USERPLUGINS_DIR}" LIBRARY DESTINATION "${REAPER_USERPLUGINS_DIR}")
