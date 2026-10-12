# Copies the plugin scan helper into a macOS bundle and keeps the bundle's
# signature valid. Run as a POST_BUILD step (see plugin/CMakeLists.txt):
#
#   cmake -Dhelper=<PedaleiraPluginScan> -Dbundle=<X.app|X.vst3|X.component>
#         -Dsign_check=<JUCE's checkBundleSigning.cmake> -P EmbedScanHelper.cmake
#
# Contents/Helpers is one of the places codesign treats as nested code, so
# the helper is sealed into the bundle's signature like the main binary. A
# local build is signed ad hoc by JUCE before this step runs; adding a file
# breaks that seal, so the helper gets its own ad hoc signature (nested code
# must be signed before the bundle that holds it) and JUCE's check then
# re-signs the bundle. A release signs everything again with a real identity,
# inside out, and must list the helper before the bundle.
set(dest "${bundle}/Contents/Helpers")
file(MAKE_DIRECTORY "${dest}")
get_filename_component(helper_name "${helper}" NAME)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${helper}" "${dest}/${helper_name}"
    RESULT_VARIABLE copy_result)
if(copy_result)
    message(FATAL_ERROR "Could not copy ${helper} into ${dest}")
endif()

find_program(xcrun_program xcrun)
if(xcrun_program)
    # Keep an existing signature (a linker-signed arm64 binary, or a previous
    # run of this step); sign only what has none.
    execute_process(
        COMMAND "${xcrun_program}" codesign --verify "${dest}/${helper_name}"
        RESULT_VARIABLE helper_unsigned
        OUTPUT_QUIET ERROR_QUIET)
    if(helper_unsigned)
        execute_process(COMMAND "${xcrun_program}" codesign -f -s - "${dest}/${helper_name}")
    endif()
endif()

set(src "${bundle}")
include("${sign_check}")
