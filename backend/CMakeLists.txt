cmake_minimum_required(VERSION 3.10)
project(SteamDeckMotionCues)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(OpenGL_GL_PREFERENCE GLVND)

find_package(glfw3 3.3 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(X11 REQUIRED)

include_directories(${X11_INCLUDE_DIR} ${OPENGL_INCLUDE_DIR})

set(IMGUI_PATH imgui)
file(GLOB IMGUI_SOURCES
    ${IMGUI_PATH}/*.cpp
    ${IMGUI_PATH}/backends/imgui_impl_glfw.cpp
    ${IMGUI_PATH}/backends/imgui_impl_opengl3.cpp
)

add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC ${IMGUI_PATH} ${IMGUI_PATH}/backends)

add_executable(SteamDeckMotionCues main.cpp)
target_link_libraries(SteamDeckMotionCues PRIVATE imgui glfw ${OPENGL_LIBRARIES} ${X11_LIBRARIES} ${CMAKE_DL_LIBS})
