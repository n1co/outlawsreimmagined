cmake_minimum_required(VERSION 3.16)
project(outlaws_engine C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build type default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Compiler flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wno-unused-parameter
        -ffast-math
    )
    add_compile_options("$<$<CONFIG:Debug>:-g>")
    add_compile_options("$<$<CONFIG:Debug>:-O0>")
    add_compile_options("$<$<CONFIG:Release>:-O3>")
    add_compile_definitions("$<$<CONFIG:Release>:NDEBUG>")
endif()

# -------------------------------------------------------------------------
# Dependencies via pkg-config
# -------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2       REQUIRED sdl2)
pkg_check_modules(SDL2_MIXER REQUIRED SDL2_mixer)
pkg_check_modules(GLEW       REQUIRED glew)
pkg_check_modules(GL         REQUIRED gl)

# -------------------------------------------------------------------------
# Source files
# -------------------------------------------------------------------------
set(ENGINE_SOURCES
    src/main.c
    src/lab.c
    src/pcx.c
    src/lvt.c
    src/wax.c
    src/tdo.c
    src/obt.c
    src/renderer.c
    src/input.c
    src/player.c
    src/audio.c
    src/world.c
    src/collision.c
    src/entity.c
    src/weapon.c
    src/inf.c
    src/msg.c
    src/mission.c
    src/menu.c
    src/laf.c
    src/savegame.c
)

# ImGui (C++ compiled separately)
set(IMGUI_SOURCES
    src/imgui/imgui.cpp
    src/imgui/imgui_draw.cpp
    src/imgui/imgui_tables.cpp
    src/imgui/imgui_widgets.cpp
    src/imgui/backends/imgui_impl_sdl2.cpp
    src/imgui/backends/imgui_impl_opengl3.cpp
    src/debug_ui.cpp
)

# -------------------------------------------------------------------------
# Main executable
# -------------------------------------------------------------------------
add_executable(outlaws ${ENGINE_SOURCES} ${IMGUI_SOURCES})

target_include_directories(outlaws PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_MIXER_INCLUDE_DIRS}
    ${GLEW_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src        # for imgui/ includes
    ${CMAKE_SOURCE_DIR}/src/imgui  # for imgui.h direct includes
)

target_link_libraries(outlaws PRIVATE
    ${SDL2_LIBRARIES}
    ${SDL2_MIXER_LIBRARIES}
    ${GLEW_LIBRARIES}
    ${GL_LIBRARIES}
    m   # math library
)

target_compile_definitions(outlaws PRIVATE
    ${SDL2_CFLAGS_OTHER}
    ${GLEW_CFLAGS_OTHER}
)

# -------------------------------------------------------------------------
# Data directory symlink for development
# -------------------------------------------------------------------------
# Create a 'data' symlink in the build directory pointing to the real game data.
# Override at configure time with: cmake -DGAME_DATA_DIR=/path/to/data ..
set(GAME_DATA_DIR "${CMAKE_SOURCE_DIR}/data"
    CACHE PATH "Path to Outlaws game data directory")

add_custom_command(TARGET outlaws POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "${GAME_DATA_DIR}"
        "$<TARGET_FILE_DIR:outlaws>/data"
    COMMENT "Creating data symlink -> ${GAME_DATA_DIR}"
    VERBATIM
)

# -------------------------------------------------------------------------
# Standalone tools
# -------------------------------------------------------------------------
add_executable(lab_tool tools/lab_tool.c)

# -------------------------------------------------------------------------
# Installation
# -------------------------------------------------------------------------
install(TARGETS outlaws DESTINATION bin)

message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Game data: ${GAME_DATA_DIR}")
message(STATUS "")
message(STATUS "Build with:")
message(STATUS "  cmake -B build -DGAME_DATA_DIR=/mnt/linuxdata/Games/Outlaws")
message(STATUS "  cmake --build build")
message(STATUS "  ./build/outlaws --level CANYON")
