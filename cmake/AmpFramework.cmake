# What the amp framework (plugin/include/amp, plugin/src/effects/amp) and the
# cab model (plugin/include/cab, plugin/src/effects/cab) need on a target that
# compiles the built-in effects:
#
#   include(cmake/AmpFramework.cmake)
#   t3k_link_amp_framework(MyTarget)
#
# 1. The shipped amp descriptions (resources/amps/*.json, docs/amp-framework.md)
#    as binary data linked into the target, so the amps are found without any
#    file next to the binary. The generated header is PedaleiraAmpData.h
#    (namespace PedaleiraAmpData); plugin/src/effects/amp/dsp/BuiltinAmps.h
#    reads it. Adding an amp is adding a file to resources/amps and re-running
#    CMake.
# 2. The shipped speaker descriptions of the cab model (resources/cabs/*.json,
#    docs/cab-framework.md), the same way: PedaleiraCabData.h (namespace
#    PedaleiraCabData), read by plugin/include/effects/cab/BuiltinCabs.h.
# 3. chowdsp_wdf (BSD-3, header only), for the wave digital filter stages. The
#    PedaleiraWdf target is defined once in the top-level CMakeLists.txt, next
#    to the CPM call that fetches the library at a pinned tag.
function(t3k_link_amp_framework target)
    if (NOT TARGET PedaleiraAmpData)
        file(GLOB amp_descriptions CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../resources/amps/*.json")
        juce_add_binary_data(PedaleiraAmpData
            HEADER_NAME "PedaleiraAmpData.h"
            NAMESPACE PedaleiraAmpData
            SOURCES ${amp_descriptions})
        # Linked into shared objects (the plugin formats) as well.
        set_target_properties(PedaleiraAmpData PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    if (NOT TARGET PedaleiraCabData)
        file(GLOB cab_descriptions CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../resources/cabs/*.json")
        juce_add_binary_data(PedaleiraCabData
            HEADER_NAME "PedaleiraCabData.h"
            NAMESPACE PedaleiraCabData
            SOURCES ${cab_descriptions})
        set_target_properties(PedaleiraCabData PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    target_link_libraries(${target} PRIVATE PedaleiraAmpData PedaleiraCabData PedaleiraWdf)
endfunction()
