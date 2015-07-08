project(vlc-vr)

cmake_minimum_required(VERSION 2.8)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++0x")

set(OVR_DESC "Root path of compiled Oculus Rift SDK 0.4.4")
if(DEFINED ENV{OVR_ROOT})
    set(OVR_ROOT $ENV{OVR_ROOT} CACHE PATH ${OVR_DESC})
else()
    set(OVR_ROOT "OVR_ROOT-NOTSET" CACHE PATH ${OVR_DESC})
endif()
include_directories(${OVR_ROOT}/LibOVR/Include ${OVR_ROOT}/LibOVR/Src)

INCLUDE(FindPkgConfig)
PKG_SEARCH_MODULE(SDL2 REQUIRED sdl2)
PKG_SEARCH_MODULE(VLC REQUIRED libvlc)
PKG_SEARCH_MODULE(GLEW REQUIRED glew)

subdirs(shaders)
include_directories(${CMAKE_BINARY_DIR})

add_executable(vlc-vr vlc-vr.cpp)
target_link_libraries(vlc-vr 
    ${SDL2_LIBS} -L/usr/lib64 -lSDL2 -lpthread
    ${VLC_LIBS} -lvlc
    ${GLEW_LIBS} -lGLEW -lGLU -lGL
    -L${OVR_ROOT}/LibOVR/Lib/Linux/Release/x86_64 -lovr -lpthread -lXrandr -lXinerama -lX11 -lrt
)
add_dependencies(vlc-vr shaders) # shaders converted to C++ header files

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING
    "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
    FORCE)
endif()

