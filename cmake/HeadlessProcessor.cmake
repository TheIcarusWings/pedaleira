# The real TONE3000Processor, compiled without any GUI, for console apps that
# drive it directly: the DSP test suite (test/) and the offline renderer
# (tools/). One recipe so the two can never drift apart.
#
#   include(cmake/HeadlessProcessor.cmake)
#   t3k_add_headless_processor(MyConsoleApp)
#
# The caller owns juce_add_console_app, its own sources and its own extra
# definitions; it must also have enable_language(C) in effect (the JUCE
# modules this pulls in via juce_graphics include a C source).
function(t3k_add_headless_processor target)
    set(plugin_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../plugin")

    # Console apps have no JucePlugin_VersionString; the command layer reports
    # the version from the same VERSION file the plugin is built from.
    file(READ "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../VERSION" headless_version)
    string(STRIP "${headless_version}" headless_version)

    target_sources(${target} PRIVATE
        # Compiled straight from the plugin tree so callers exercise the real
        # processor, not a copy. GUI sources are excluded; HEADLESS=1 below
        # compiles the processor exactly like the headless plugin build.
        ${plugin_dir}/src/AgentHub.cpp
        ${plugin_dir}/src/AssetTags.cpp
        ${plugin_dir}/src/CaptureChecks.cpp
        ${plugin_dir}/src/CaptureCommands.cpp
        ${plugin_dir}/src/CaptureSession.cpp
        ${plugin_dir}/src/CaptureStudio.cpp
        ${plugin_dir}/src/TrainerRunner.cpp
        ${plugin_dir}/src/ToneRequestCommands.cpp
        ${plugin_dir}/src/AudioMeasure.cpp
        ${plugin_dir}/src/AutoOffset.cpp
        ${plugin_dir}/src/BlockEq.cpp
        ${plugin_dir}/src/BlockKinds.cpp
        ${plugin_dir}/src/BlockProcessor.cpp
        ${plugin_dir}/src/BlockSpectrum.cpp
        ${plugin_dir}/src/CommandRegistry.cpp
        ${plugin_dir}/src/CommandServer.cpp
        ${plugin_dir}/src/EffectMatch.cpp
        ${plugin_dir}/src/WebSocketTransport.cpp
        ${plugin_dir}/src/EngineCommands.cpp
        ${plugin_dir}/src/GainBlockProcessor.cpp
        ${plugin_dir}/src/GuitarCommands.cpp
        ${plugin_dir}/src/NamLibrary.cpp
        ${plugin_dir}/src/NamCommands.cpp
        ${plugin_dir}/src/GuitarProfiles.cpp
        ${plugin_dir}/src/HostedPluginBlockProcessor.cpp
        ${plugin_dir}/src/IrBlockProcessor.cpp
        ${plugin_dir}/src/LegacyParamIds.cpp
        ${plugin_dir}/src/LibraryPaths.cpp
        ${plugin_dir}/src/ListenCommands.cpp
        ${plugin_dir}/src/MidiMapper.cpp
        ${plugin_dir}/src/NamBlockProcessor.cpp
        ${plugin_dir}/src/NamEngine.cpp
        ${plugin_dir}/src/NamModels.cpp
        ${plugin_dir}/src/ParameterisedBlockProcessor.cpp
        ${plugin_dir}/src/PresetFile.cpp
        ${plugin_dir}/src/PresetJson.cpp
        ${plugin_dir}/src/PresetImporter.cpp
        ${plugin_dir}/src/PresetManager.cpp
        ${plugin_dir}/src/ProcessingPlan.cpp
        ${plugin_dir}/src/Processor.cpp
        ${plugin_dir}/src/ProcessorChain.cpp
        ${plugin_dir}/src/ProcessorHistory.cpp
        ${plugin_dir}/src/ProcessorModelLoader.cpp
        ${plugin_dir}/src/ProcessorPlanExec.cpp
        ${plugin_dir}/src/ProcessorGrid.cpp
        ${plugin_dir}/src/GridCommands.cpp
        ${plugin_dir}/src/Looper.cpp
        ${plugin_dir}/src/LooperCommands.cpp
        ${plugin_dir}/src/LibraryCommands.cpp
        ${plugin_dir}/src/ProcessorLibrary.cpp
        ${plugin_dir}/src/ProcessorPlugins.cpp
        ${plugin_dir}/src/ProcessorPresets.cpp
        ${plugin_dir}/src/ProcessorScenes.cpp
        ${plugin_dir}/src/ProcessorState.cpp
        ${plugin_dir}/src/SandboxRig.cpp
        ${plugin_dir}/src/Scenes.cpp
        ${plugin_dir}/src/SetlistLibrary.cpp
        ${plugin_dir}/src/Spread.cpp
        ${plugin_dir}/src/SplitJunction.cpp
        # Processor.cpp's constructor calls StandaloneStateAutosave::install().
        # Always the portable no-op stub here, never the .mm: the real observer
        # is iOS-standalone-only and none of these binaries run there (the stub
        # is also what the plugin itself compiles to everywhere but iOS).
        ${plugin_dir}/src/StandaloneStateAutosave.cpp
        ${plugin_dir}/src/PitchShift.cpp
        ${plugin_dir}/src/PluginScanner.cpp
        ${plugin_dir}/src/StereoOffset.cpp
        ${plugin_dir}/src/TunerDetector.cpp
    )

    # The built-in effects and the generated list of their registration
    # hooks, exactly as the plugin target gets them.
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EffectSources.cmake")
    t3k_effect_sources(effect_sources)
    target_sources(${target} PRIVATE ${effect_sources})

    target_include_directories(${target} PRIVATE
        ${plugin_dir}/include
        # ChainDomain.h pulls in the AudioDSPTools resampling boundary.
        ${plugin_dir}/AudioDSPTools
        ${plugin_dir}/AudioDSPTools/dsp/ResamplingContainer/Dependencies
        # NAM core + deps. Added explicitly (not via the NAM target) because on
        # macOS/Windows NAM is linked as a whole-archive *flag*, which doesn't
        # propagate the target's include directories.
        ${plugin_dir}/NeuralAmpModelerCore
        ${plugin_dir}/NeuralAmpModelerCore/Dependencies/eigen
        ${plugin_dir}/NeuralAmpModelerCore/Dependencies/nlohmann
    )

    target_compile_definitions(${target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_WEBP=0
        JUCE_USE_CURL=0
        # Plugin hosting, as in plugin/CMakeLists.txt: VST3 everywhere, Audio
        # Units on macOS. (None of these binaries is built for iOS.)
        JUCE_PLUGINHOST_VST3=1
        $<$<PLATFORM_ID:Darwin>:JUCE_PLUGINHOST_AU=1>
        # Prevent windows.h min/max macros from breaking std::min/std::max in
        # AudioDSPTools and plugin code (same as plugin/CMakeLists.txt).
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
        # The processor compiles in its headless configuration (no editor /
        # standalone-window code); the DSP under test is identical.
        HEADLESS=1
        # Callers pump the dispatch loop (runDispatchLoopUntil) to deliver the
        # processor's AsyncUpdater work; the plugin itself never needs modal
        # loops.
        JUCE_MODAL_LOOPS_PERMITTED=1
        # Exposes is_a2_shape, which ProcessorModelLoader.cpp uses to gate
        # drop-loaded local NAM files (same as plugin/CMakeLists.txt).
        NAM_ENABLE_A2_FAST
        T3K_VERSION_STRING="${headless_version}"
    )

    target_link_libraries(${target}
        PRIVATE
            juce::juce_dsp
            juce::juce_audio_formats
            juce::juce_audio_processors
            # SHA-256 for the library's asset store (PresetJson.h).
            juce::juce_cryptography
            # JUCE_PLUGINHOST_AU pulls the Audio Unit editor code into
            # juce_audio_processors, and with it AUGenericView. The plugin
            # gets CoreAudioKit through juce_audio_utils; these binaries do
            # not link that module.
            "$<$<PLATFORM_ID:Darwin>:-framework CoreAudioKit>"
            # NAM must be linked whole-archive so its self-registering config
            # parsers (WaveNet/ConvNet/LSTM static initializers) survive the
            # linker (same recipe as plugin/CMakeLists.txt).
            $<$<PLATFORM_ID:Darwin>:-Wl,-force_load,'$<TARGET_FILE:NAM>'>
            $<$<PLATFORM_ID:Linux>:-Wl,--whole-archive>
            $<$<PLATFORM_ID:Linux>:NAM>
            $<$<PLATFORM_ID:Linux>:-Wl,--no-whole-archive>
            $<$<PLATFORM_ID:Windows>:NAM>
            $<$<PLATFORM_ID:Windows>:-WHOLEARCHIVE:$<TARGET_FILE:NAM>>
        PUBLIC
            juce::juce_recommended_config_flags
    )

    add_dependencies(${target} NAM)

    # The built-in import mapping tables (PresetImporter.cpp).
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ImportData.cmake")
    t3k_link_import_data(${target})

    # The shipped amp descriptions and chowdsp_wdf, for the amp effects.
    include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/AmpFramework.cmake")
    t3k_link_amp_framework(${target})

    # The scan helper next to the binary, where PluginScanner looks for it
    # (see plugin/CMakeLists.txt, which defines the target).
    if (TARGET PedaleiraPluginScan)
        add_dependencies(${target} PedaleiraPluginScan)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<TARGET_FILE:PedaleiraPluginScan>" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()
