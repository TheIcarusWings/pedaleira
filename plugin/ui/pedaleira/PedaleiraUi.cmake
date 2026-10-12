# The Pedaleira UI: the shell, pages, widgets and tokens of
# the signed-off design. pedaleira_add_ui(<target>) compiles it into a target
# that also builds the engine (the standalone app, the UI testbed).

set(PEDALEIRA_UI_DIR "${CMAKE_CURRENT_LIST_DIR}")

# The two typefaces of the design, both SIL OFL 1.1 (assets/README.md).
if (NOT TARGET PedaleiraUiData)
    juce_add_binary_data(PedaleiraUiData
        NAMESPACE PedaleiraUiData
        HEADER_NAME PedaleiraUiData.h
        SOURCES
            "${PEDALEIRA_UI_DIR}/assets/HankenGrotesk-400.ttf"
            "${PEDALEIRA_UI_DIR}/assets/HankenGrotesk-500.ttf"
            "${PEDALEIRA_UI_DIR}/assets/HankenGrotesk-600.ttf"
            "${PEDALEIRA_UI_DIR}/assets/HankenGrotesk-700.ttf"
            "${PEDALEIRA_UI_DIR}/assets/HankenGrotesk-500Italic.ttf"
            "${PEDALEIRA_UI_DIR}/assets/JetBrainsMono-400.ttf"
            "${PEDALEIRA_UI_DIR}/assets/JetBrainsMono-500.ttf"
    )
    set_target_properties(PedaleiraUiData PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

file(GLOB_RECURSE PEDALEIRA_UI_SOURCES CONFIGURE_DEPENDS
    "${PEDALEIRA_UI_DIR}/*.cpp" "${PEDALEIRA_UI_DIR}/*.h")
# The testbed is its own target; standalone/ reaches the application's audio
# device (the JUCE holder), which only the standalone build has.
list(FILTER PEDALEIRA_UI_SOURCES EXCLUDE REGEX "/(testbed|standalone)/")
file(GLOB PEDALEIRA_UI_STANDALONE_SOURCES CONFIGURE_DEPENDS
    "${PEDALEIRA_UI_DIR}/standalone/*.cpp" "${PEDALEIRA_UI_DIR}/standalone/*.h")

function(pedaleira_add_ui target)
    target_sources(${target} PRIVATE ${PEDALEIRA_UI_SOURCES})
    target_include_directories(${target} PRIVATE "${PEDALEIRA_UI_DIR}")
    target_link_libraries(${target} PRIVATE PedaleiraUiData juce::juce_gui_extra)
endfunction()

# The part that needs the standalone holder: the application's target only.
function(pedaleira_add_ui_standalone target)
    target_sources(${target} PRIVATE ${PEDALEIRA_UI_STANDALONE_SOURCES})
    target_compile_definitions(${target} PRIVATE PEDALEIRA_STANDALONE_DEVICE=1)
endfunction()
