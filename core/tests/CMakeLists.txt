# =============================================================================
# Google Test via FetchContent
# =============================================================================
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
# Prevent overriding parent project's compiler/linker settings on Windows
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# =============================================================================
# Test executable
# =============================================================================
add_executable(bt_dsl_tests
    test_parser.cpp
    test_semantic.cpp
    test_xml_generator.cpp
    test_manifest_converter.cpp
    test_lsp_diagnostics.cpp
    test_lsp_completion.cpp
    test_lsp_hover_definition.cpp
    test_lsp_symbols.cpp
    test_lsp_document_highlight.cpp
    test_lsp_semantic_tokens.cpp
)

target_link_libraries(bt_dsl_tests PRIVATE
    bt_dsl_core
    GTest::gtest_main
)

# =============================================================================
# Test discovery
# =============================================================================
include(GoogleTest)
gtest_discover_tests(bt_dsl_tests)
