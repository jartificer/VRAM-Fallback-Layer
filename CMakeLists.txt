cmake_minimum_required(VERSION 3.16)
project(vram_fallback_layer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Vulkan REQUIRED)

add_library(VkLayer_vram_fallback SHARED src/vram_fallback.cpp)
target_include_directories(VkLayer_vram_fallback PRIVATE ${Vulkan_INCLUDE_DIRS})
target_compile_options(VkLayer_vram_fallback PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_options(VkLayer_vram_fallback PRIVATE -Wl,--no-undefined)
find_package(Threads REQUIRED)
target_link_libraries(VkLayer_vram_fallback PRIVATE Threads::Threads)

# Manifests next to the library so VK_ADD_LAYER_PATH=<build dir> works.
configure_file(layer/VkLayer_vram_fallback.json ${CMAKE_BINARY_DIR}/VkLayer_vram_fallback.json COPYONLY)
configure_file(layer/VkLayer_vram_fallback_implicit.json ${CMAKE_BINARY_DIR}/implicit/VkLayer_vram_fallback.json COPYONLY)

option(VRAM_FALLBACK_TESTS "Build the simulator layer and test app" ON)
if(VRAM_FALLBACK_TESTS)
  add_library(VkLayer_test_nvsim SHARED test/nvsim_layer.cpp)
  target_include_directories(VkLayer_test_nvsim PRIVATE ${Vulkan_INCLUDE_DIRS})
  configure_file(test/VkLayer_test_nvsim.json ${CMAKE_BINARY_DIR}/VkLayer_test_nvsim.json COPYONLY)
  add_executable(vram_test test/vram_test.cpp)
  target_link_libraries(vram_test PRIVATE Vulkan::Vulkan)
endif()

include(GNUInstallDirs)
install(TARGETS VkLayer_vram_fallback LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
