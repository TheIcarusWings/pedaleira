# TONE3000 DSP test suite.
# A GTest console app exercising the chain's DSP invariants (oversampler,
# NamEngine phase interleaving, IR base-rate islands) against the real model
# and IR assets in test/files. Build and run locally:
#
#   ./script/test-dsp.sh                     # configure/build/run in one go
#   ./script/test-dsp.sh 'IrConvolutionTest.*'   # gtest filter
#
# or by hand: cmake --build build --target DspTests, then run the binary in
# build/test/DspTests_artefacts (also registered with ctest).

# The root project only enables CXX; the JUCE modules this app pulls in
# (juce_graphics via juce_audio_processors) include a C source.
enable_language(C)

juce_add_console_app(DspTests PRODUCT_NAME "DspTests")

target_sources(DspTests PRIVATE
    src/agent_hub_tests.cpp
    src/audio_measure_tests.cpp
    src/auto_offset_tests.cpp
    src/block_framework_tests.cpp
    src/block_processor_tests.cpp
    src/branch_tests.cpp
    src/capture_tests.cpp
    src/chain_boundary_multi_tests.cpp
    src/clipboard_tests.cpp
    src/command_registry_tests.cpp
    src/command_server_tests.cpp
    src/websocket_transport_tests.cpp
    src/tone_request_tests.cpp
    src/crossover_tests.cpp
    src/dsp_tests.cpp
    src/dual_mono_tests.cpp
    src/engine_commands_tests.cpp
    src/duplicate_tests.cpp
    src/effect_match_tests.cpp
    src/match_recorder_tests.cpp
    src/eq_chain_rate_tests.cpp
    src/eq_post_routing_tests.cpp
    src/factory_preset_tests.cpp
    src/footswitch_tests.cpp
    src/gate_tests.cpp
    src/golden_render_tests.cpp
    src/grid_commands_tests.cpp
    src/grid_hardening_tests.cpp
    src/grid_routing_tests.cpp
    src/grid_split_state_tests.cpp
    src/grid_split_tests.cpp
    src/grid_state_tests.cpp
    src/guitar_profile_tests.cpp
    src/host_program_tests.cpp
    src/ir_block_size_tests.cpp
    src/ir_classification_tests.cpp
    src/ir_temp_file_tests.cpp
    src/library_tests.cpp
    src/listen_commands_tests.cpp
    src/local_load_tests.cpp
    src/midi_map_tests.cpp
    src/multicore_tests.cpp
    src/hosted_plugin_tests.cpp
    src/outgain_tests.cpp
    src/padless_wav_tests.cpp
    src/pitch_shift_tests.cpp
    src/plan_compile_tests.cpp
    src/plan_exec_tests.cpp
    src/plugin_scanner_tests.cpp
    src/preset_importer_tests.cpp
    src/preset_json_tests.cpp
    src/preset_tests.cpp
    src/processor_tests.cpp
    src/scene_render_tests.cpp
    src/scene_tests.cpp
    src/setlist_library_tests.cpp
    src/split_junction_tests.cpp
    src/spread_tests.cpp
    src/state_cache_tests.cpp
    src/swap_fade_tests.cpp
    # Block kinds that only exist in this binary (test.latency).
    src/test_block_kinds.cpp
    src/tone_refresh_tests.cpp
    src/tuner_tests.cpp
    src/wide_io_tests.cpp
    src/worker_pool_tests.cpp
)

# One test file per built-in effect (plus the DSP helper tests), picked up
# by name so adding an effect needs no edit here. The harness they share is
# src/effects/effect_test_helpers.h.
file(GLOB_RECURSE _t3k_effect_tests CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/effects/*_tests.cpp")
target_sources(DspTests PRIVATE ${_t3k_effect_tests})
target_include_directories(DspTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# The real processor (sources, include paths, HEADLESS=1, the NAM whole-archive
# link) comes from the shared recipe the offline renderer in tools/ uses too.
include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/HeadlessProcessor.cmake)
t3k_add_headless_processor(DspTests)

target_compile_definitions(DspTests PRIVATE
    # Absolute path to the test assets, so the binary runs from anywhere.
    T3K_TEST_FILES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/files"
)

# The plugin the hosting tests host (plugins/PedaleiraTestFx.cpp): a tiny VST3
# effect with an exact gain and a real, reported delay. Test build only:
# never installed (no copy step), no manifest step at build time (the scan
# then loads the module, which is the path worth testing), and found by the
# tests in the build tree through the definitions below.
juce_add_plugin(PedaleiraTestFx
    COMPANY_NAME PedaleiraTests
    PLUGIN_NAME "PedaleiraTestFx"
    PRODUCT_NAME "PedaleiraTestFx"
    BUNDLE_ID "com.pedaleira.tests.PedaleiraTestFx"
    PLUGIN_MANUFACTURER_CODE Grdt
    PLUGIN_CODE Gtfx
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_AUTO_MANIFEST FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)
target_sources(PedaleiraTestFx PRIVATE plugins/PedaleiraTestFx.cpp)
target_compile_definitions(PedaleiraTestFx PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_WEBP=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    $<$<PLATFORM_ID:Windows>:NOMINMAX>
)
target_link_libraries(PedaleiraTestFx
    PRIVATE
        juce::juce_audio_processors
    PUBLIC
        juce::juce_recommended_config_flags
)

add_dependencies(DspTests PedaleiraTestFx_VST3 PedaleiraPluginScan)
target_compile_definitions(DspTests PRIVATE
    # PedaleiraTestFx.vst3, in a folder that holds nothing else. (The scan
    # helper needs no path: the headless recipe copies it next to the binary,
    # which is where the scanner looks.)
    # (The property is itself a generator expression, hence the explicit
    # evaluation.)
    T3K_TEST_PLUGIN_FILE="$<TARGET_GENEX_EVAL:PedaleiraTestFx_VST3,$<TARGET_PROPERTY:PedaleiraTestFx_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>"
)

# The headless renderer, which wide_io_tests.cpp runs as a child process (wide
# input files, --out-channels). Its target is defined in tools/, after this
# directory; the generator expression is resolved once both exist.
add_dependencies(DspTests PedaleiraRender)
target_compile_definitions(DspTests PRIVATE
    T3K_RENDER_TOOL_FILE="$<TARGET_FILE:PedaleiraRender>"
)

# The recording half of PedaleiraMatch (tools/MatchRecorder, tools/MatchTake) is
# tested without a sound card: match_recorder_tests.cpp drives its callback.
target_sources(DspTests PRIVATE
    ../tools/MatchRecorder.cpp
    ../tools/MatchTake.cpp
)
target_include_directories(DspTests PRIVATE ../tools)
target_link_libraries(DspTests PRIVATE juce::juce_audio_devices)

# dsp_tests.cpp provides main() (JUCE init + InitGoogleTest).
target_link_libraries(DspTests PRIVATE GTest::gtest)

include(GoogleTest)
gtest_discover_tests(DspTests)
