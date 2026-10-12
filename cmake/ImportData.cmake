# The built-in import mapping tables (resources/import/*.json, docs/import.md)
# as binary data linked into the target, so the importer finds them without
# any file next to the binary. One static library for the whole build:
#
#   include(cmake/ImportData.cmake)
#   t3k_link_import_data(MyTarget)
#
# The generated header is PedaleiraImportData.h (namespace
# PedaleiraImportData); PresetImporter.cpp reads it. Adding a table is adding
# a file to resources/import and re-running CMake.
function(t3k_link_import_data target)
    if (NOT TARGET PedaleiraImportData)
        file(GLOB import_tables CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../resources/import/*.json")
        juce_add_binary_data(PedaleiraImportData
            HEADER_NAME "PedaleiraImportData.h"
            NAMESPACE PedaleiraImportData
            SOURCES ${import_tables})
        # Linked into shared objects (the plugin formats) as well.
        set_target_properties(PedaleiraImportData PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endif()
    target_link_libraries(${target} PRIVATE PedaleiraImportData)
endfunction()
