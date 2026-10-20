# The simulator's build of DOOM's C side: doomgeneric and Chocolate Doom's
# OPL library (third_party/, patches marked "crosspad:") and the CrossPad port
# (port/), one static library. Included by crosspad-pc's CMakeLists.txt; the app's C++ in src/ is
# globbed there like every app's.
file(GLOB _cpdoom_sources
    "${CMAKE_CURRENT_LIST_DIR}/third_party/doomgeneric/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/opl/*.c"
    "${CMAKE_CURRENT_LIST_DIR}/port/*.c")
add_library(crosspad_doomgeneric STATIC ${_cpdoom_sources})
target_include_directories(crosspad_doomgeneric PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/third_party/doomgeneric"
    "${CMAKE_CURRENT_LIST_DIR}/third_party/opl"
    "${CMAKE_CURRENT_LIST_DIR}/port")
target_compile_definitions(crosspad_doomgeneric PRIVATE
    DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200 CMAP256 FEATURE_SOUND)
set_target_properties(crosspad_doomgeneric PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
if(MSVC)
    target_compile_options(crosspad_doomgeneric PRIVATE /w)
else()
    target_compile_options(crosspad_doomgeneric PRIVATE -w)
endif()
list(APPEND CROSSPAD_PC_APP_LIBS crosspad_doomgeneric)
