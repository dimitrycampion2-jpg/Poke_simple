cmake_minimum_required(VERSION 3.15)
project(PokeSimple C)

# --- raylib (sources dans raylib/src) ---------------------------------------
set(RL raylib/src)
add_library(raylib_local STATIC
    ${RL}/rcore.c ${RL}/rshapes.c ${RL}/rtextures.c ${RL}/rtext.c
    ${RL}/utils.c ${RL}/rmodels.c ${RL}/raudio.c ${RL}/rglfw.c)
target_compile_definitions(raylib_local PUBLIC PLATFORM_DESKTOP GRAPHICS_API_OPENGL_33)
target_include_directories(raylib_local PUBLIC ${RL} PRIVATE ${RL}/external/glfw/include)
if(WIN32)
    target_link_libraries(raylib_local PUBLIC opengl32 gdi32 winmm shell32 user32 ws2_32)
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(raylib_local PRIVATE _GNU_SOURCE _GLFW_X11)
    target_link_libraries(raylib_local PUBLIC GL m pthread dl rt X11)
endif()

# --- le jeu ------------------------------------------------------------------
add_executable(jeu main.c net.c)
target_link_libraries(jeu PRIVATE raylib_local)
if(MSVC)
    target_compile_options(jeu PRIVATE /utf-8)      # main.c est en UTF-8 (accents)
    target_compile_options(raylib_local PRIVATE /utf-8)
    target_compile_definitions(jeu PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(raylib_local PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# copie l'exe a la racine, a cote de sprites/
add_custom_command(TARGET jeu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:jeu> ${CMAKE_SOURCE_DIR}/)
