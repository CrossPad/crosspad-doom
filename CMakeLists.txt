# DOOM -- ESP-IDF component.
#
# The one app of the doom firmware kit (crosspad-app.json "kits"): built into
# that image only. In any other kit it is a component with no sources, so the
# full firmware carries no DOOM code -- every kB of an image is a kB of PSRAM
# (it is copied there at boot). The requirements stay the same either way:
# ESP-IDF reads them in an early pass that does not know the kit.
#
# The PC simulator does not read this file: it globs src/*.cpp and includes
# crosspad_pc.cmake for the C side.

set(_doom_srcs "")
set(_doom_kit "full")
if(DEFINED CROSSPAD_FW_KIT)
    set(_doom_kit "${CROSSPAD_FW_KIT}")
endif()
file(READ "${CMAKE_CURRENT_LIST_DIR}/crosspad-app.json" _doom_app_json)
string(JSON _doom_kit_count LENGTH "${_doom_app_json}" kits)
math(EXPR _doom_last "${_doom_kit_count} - 1")
foreach(_i RANGE ${_doom_last})
    string(JSON _k GET "${_doom_app_json}" kits ${_i})
    if(_k STREQUAL _doom_kit)
        file(GLOB _doom_srcs
            "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp"
            "${CMAKE_CURRENT_LIST_DIR}/port/*.c"
            "${CMAKE_CURRENT_LIST_DIR}/third_party/doomgeneric/*.c"
            "${CMAKE_CURRENT_LIST_DIR}/third_party/opl/*.c")
    endif()
endforeach()

# linker.lf: DOOM's zero-initialised globals (the renderer's visplanes,
# openings, drawsegs -- ~260 kB) in PSRAM; in internal RAM they overflowed
# dram0 by 200 kB. Nothing in them is touched with the cache off.
set(_doom_ldfragments "")
if(_doom_srcs)
    set(_doom_ldfragments "linker.lf")
endif()

idf_component_register(
    SRCS ${_doom_srcs}
    LDFRAGMENTS ${_doom_ldfragments}
    PRIV_INCLUDE_DIRS
        "third_party/doomgeneric"
        "third_party/opl"
        "port"
    REQUIRES
        crosspad-core
        crosspad-gui
        lvgl
)

if(_doom_srcs)
    # DOOM's own C: its defines, and no warnings from code that is not ours.
    file(GLOB _doom_c "${CMAKE_CURRENT_LIST_DIR}/third_party/*/*.c" "${CMAKE_CURRENT_LIST_DIR}/port/*.c")
    set_source_files_properties(${_doom_c} PROPERTIES COMPILE_OPTIONS "-w;-std=gnu11")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200 CMAP256 FEATURE_SOUND)
    # WHOLE_ARCHIVE keeps the REGISTER_APP-generated _register_Doom_app()
    # alive through linking -- main calls it via AppRegistry_InitAll().
    set_property(TARGET ${COMPONENT_LIB} PROPERTY WHOLE_ARCHIVE ON)
endif()
