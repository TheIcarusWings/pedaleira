# Maintainer utilities, never shipped.
#
# PresetTool is not built by default:
#   cmake --build build --target PresetTool
#   build/tools/PresetTool_artefacts/Release/PresetTool migrate resources/factory-presets
#
# PedaleiraRender is part of the default build (see below):
#   build/tools/PedaleiraRender_artefacts/Release/PedaleiraRender --preset p.t3kpreset --in in.wav --out out.wav
# The JUCE modules the headless tools pull in (juce_graphics via
# juce_audio_processors) include a C source, and the root project only enables
# CXX.
enable_language(C)

# PresetTool: maintainer tool for preset files. Its --import-foreign mode runs
# the importer (plugin/src/PresetImporter.cpp), which reads the block kind
# registry, so it is built from the same headless recipe as the renderer.
juce_add_console_app(PresetTool PRODUCT_NAME "PresetTool")
set_target_properties(PresetTool PROPERTIES EXCLUDE_FROM_ALL TRUE)

target_sources(PresetTool PRIVATE
    preset_tool.cpp
)
target_compile_definitions(PresetTool PRIVATE
    JUCE_STANDALONE_APPLICATION=1
)
target_link_libraries(PresetTool PRIVATE
    juce::juce_core
    juce::juce_data_structures
    juce::juce_recommended_warning_flags
)
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HeadlessProcessor.cmake)
t3k_add_headless_processor(PresetTool)

# Headless renderer: runs a preset over a WAV through the real processor, no
# window and no audio device. It is the safety net for engine refactors (the
# golden render tests in test/ use the same driving sequence), so it is built
# with everything else on all three platforms.
juce_add_console_app(PedaleiraRender PRODUCT_NAME "PedaleiraRender")

target_sources(PedaleiraRender PRIVATE
    render_tool.cpp
)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HeadlessProcessor.cmake)
t3k_add_headless_processor(PedaleiraRender)

# Headless host: the engine with its command server and no window or audio
# device (see headless_host.cpp). The command server tests and the MCP
# server's tests run against it, so it is built by default like the renderer.
juce_add_console_app(PedaleiraHeadless PRODUCT_NAME "PedaleiraHeadless")

target_sources(PedaleiraHeadless PRIVATE
    headless_host.cpp
)

t3k_add_headless_processor(PedaleiraHeadless)

# Effect-matching kit: generates the test signal, analyses recordings of it and
# compares two analyses (docs/matching.md). Pure audio analysis, so it links
# only the two sources it needs. The record and session commands add the audio
# device layer (match_record.cpp, MatchRecorder.cpp). The plugin reference mode
# (plugin-params, plugin-render: MatchPlugin.cpp) hosts a plugin with the app's
# own hosting code, so it gets the headless processor recipe like the renderer
# (which also brings AudioMeasure.cpp and EffectMatch.cpp).
juce_add_console_app(PedaleiraMatch PRODUCT_NAME "PedaleiraMatch")

target_sources(PedaleiraMatch PRIVATE
    match_tool.cpp
    match_record.cpp
    MatchRecorder.cpp
    MatchTake.cpp
    MatchPlugin.cpp
)
t3k_add_headless_processor(PedaleiraMatch)
target_include_directories(PedaleiraMatch PRIVATE ../plugin/include)
target_compile_definitions(PedaleiraMatch PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
)
target_link_libraries(PedaleiraMatch PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_dsp
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)

# The Pedaleira UI testbed: renders the UI's
# pages over a real headless engine to PNG files. Off by default:
#   cmake -B build -DPEDALEIRA_BUILD_UI_TESTBED=ON
#   cmake --build build --target PedaleiraUiTestbed
option(PEDALEIRA_BUILD_UI_TESTBED "Build the Pedaleira UI testbed" OFF)
if (PEDALEIRA_BUILD_UI_TESTBED)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../plugin/ui/pedaleira/PedaleiraUi.cmake)
    juce_add_console_app(PedaleiraUiTestbed PRODUCT_NAME "PedaleiraUiTestbed")
    target_sources(PedaleiraUiTestbed PRIVATE
        ../plugin/ui/pedaleira/testbed/Main.cpp
        ../plugin/ui/pedaleira/testbed/CaptureScenes.cpp
        ../plugin/ui/pedaleira/testbed/CaptureScenes.h
        ../plugin/ui/pedaleira/testbed/Gallery.cpp
        ../plugin/ui/pedaleira/testbed/Gallery.h
    )
    target_include_directories(PedaleiraUiTestbed PRIVATE ../plugin/ui/pedaleira/testbed)
    target_compile_definitions(PedaleiraUiTestbed PRIVATE PEDALEIRA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    t3k_add_headless_processor(PedaleiraUiTestbed)
    pedaleira_add_ui(PedaleiraUiTestbed)
endif()
