cmake_minimum_required(VERSION 3.20)

project(dra2_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)
# Debug info for the runtime only, not the 20,000 generated functions: lets
# the sampling profiler name the runtime's static functions.
if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /Zi)
endif()

# CONFIGURE_DEPENDS: a plain GLOB would miss files a re-run of the recompiler adds.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
)

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dxguid
    winmm
    dbghelp     # crash reports name the guest function
)

# The game icon, only when res/dra2.ico is present: it is the game's own
# artwork, so it stays in private copies. rc runs directly because CMake's RC
# wrapper fails (RC1107) on a non-English MSVC.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/res/dra2.ico)
    set(DRA2_RES ${CMAKE_CURRENT_BINARY_DIR}/dra2.res)
    add_custom_command(OUTPUT ${DRA2_RES}
        COMMAND rc /nologo /fo ${DRA2_RES} ${CMAKE_CURRENT_SOURCE_DIR}/res/dra2.rc
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/res
        DEPENDS res/dra2.rc res/dra2.ico)
    set_source_files_properties(${DRA2_RES} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    target_sources(${PROJECT_NAME} PRIVATE ${DRA2_RES})
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND_EXPAND_LISTS)

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # A PDB with public symbols only, without a /Zi rebuild.
        /DEBUG /OPT:REF /OPT:ICF
    )
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()
