# LightweightVK
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

cmake_minimum_required(VERSION 3.16)

project(LVKVulkan CXX C)

file(GLOB SRC_FILES LIST_DIRECTORIES false RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.cpp *.c)
file(GLOB HEADER_FILES LIST_DIRECTORIES false RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} *.h)

add_library(LVKVulkan ${SRC_FILES} ${HEADER_FILES})

lvk_setup_target(LVKVulkan)
lvk_set_folder(LVKVulkan "LVK")

lvk_setup_groups("${SRC_FILES}")
lvk_setup_groups("${HEADER_FILES}")

# glslang
# cmake-format: off
set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "")
set(ENABLE_HLSL             OFF CACHE BOOL "")
set(ENABLE_CTEST            OFF CACHE BOOL "")
set(ENABLE_OPT              OFF CACHE BOOL "")
set(ENABLE_SPVREMAPPER      OFF CACHE BOOL "")
set(SKIP_GLSLANG_INSTALL    ON  CACHE BOOL "")
add_subdirectory(${LVK_ROOT_DIR}/third-party/deps/src/glslang "glslang")
lvk_set_folder(GenericCodeGen     "third-party/glslang")
lvk_set_folder(glslang            "third-party/glslang")
lvk_set_folder(MachineIndependent "third-party/glslang")
lvk_set_folder(OGLCompiler        "third-party/glslang")
lvk_set_folder(OSDependent        "third-party/glslang")
lvk_set_folder(SPIRV              "third-party/glslang")
lvk_set_folder(glslang-default-resource-limits "third-party/glslang")
# cmake-format: on

find_package(Vulkan REQUIRED)

target_link_libraries(LVKVulkan PRIVATE LVKLibrary)
target_link_libraries(LVKVulkan PRIVATE glslang SPIRV glslang-default-resource-limits)
target_link_libraries(LVKVulkan PUBLIC Vulkan::Vulkan)

target_include_directories(LVKVulkan PUBLIC "${LVK_ROOT_DIR}/third-party/deps/src/volk")
target_include_directories(LVKVulkan PUBLIC "${LVK_ROOT_DIR}/third-party/deps/src/vma/include")

if(WIN32)
  add_definitions("-DVK_USE_PLATFORM_WIN32_KHR=1")
  add_definitions("-DNOMINMAX")
elseif(UNIX)
  add_definitions("-DVK_USE_PLATFORM_XLIB_KHR=1")
endif()
