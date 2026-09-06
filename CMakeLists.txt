cmake_minimum_required(VERSION 3.20)
project(MineDLLXRay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(MineXRay SHARED
    src/xray_dll.cpp
)
target_include_directories(MineXRay PRIVATE include)
target_compile_definitions(MineXRay PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(MineXRay PROPERTIES OUTPUT_NAME "MineXRay")

add_executable(MineXRayInjector
    injector/main.cpp
)
target_compile_definitions(MineXRayInjector PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(MineXRayInjector PROPERTIES OUTPUT_NAME "MineXRayInjector")

if(MSVC)
    target_compile_options(MineXRay PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(MineXRayInjector PRIVATE /W4 /permissive- /EHsc)
    target_link_options(MineXRay PRIVATE /DEBUG)
    target_link_options(MineXRayInjector PRIVATE /DEBUG)
endif()
