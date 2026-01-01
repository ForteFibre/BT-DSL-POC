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
    parser/test_parser.cpp
    semantic/test_semantic.cpp
    codegen/test_xml_generator.cpp
    cli/test_manifest_converter.cpp
    cli/test_cli_imports.cpp
    lsp/test_lsp_diagnostics.cpp
    lsp/test_lsp_completion.cpp
    lsp/test_lsp_hover_definition.cpp
    lsp/test_lsp_symbols.cpp
    lsp/test_lsp_document_highlight.cpp
    lsp/test_lsp_semantic_tokens.cpp
    ${CMAKE_SOURCE_DIR}/src/cli/manifest_converter.cpp
)

target_include_directories(bt_dsl_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src/cli
)

target_link_libraries(bt_dsl_tests PRIVATE
    bt_dsl_core
    GTest::gtest_main
)

if(TARGET bt_dsl_cli)
    add_dependencies(bt_dsl_tests bt_dsl_cli)
    target_compile_definitions(bt_dsl_tests PRIVATE
        BT_DSL_CLI_PATH="$<TARGET_FILE:bt_dsl_cli>"
    )
endif()

# =============================================================================
# Test discovery
# =============================================================================
include(GoogleTest)
gtest_discover_tests(bt_dsl_tests)

# =============================================================================
# Native LSP server (JSON-RPC over stdio) integration tests
# =============================================================================

if(UNIX AND TARGET bt_dsl_lsp_server)
    add_executable(bt_dsl_lsp_server_tests
        lsp_server/test_lsp_server_jsonrpc.cpp
    )

    add_dependencies(bt_dsl_lsp_server_tests bt_dsl_lsp_server)

    target_link_libraries(bt_dsl_lsp_server_tests PRIVATE
        GTest::gtest_main
        nlohmann_json::nlohmann_json
    )

    target_compile_definitions(bt_dsl_lsp_server_tests PRIVATE
        BT_DSL_LSP_SERVER_PATH="$<TARGET_FILE:bt_dsl_lsp_server>"
        BT_DSL_STDLIB_PATH="${CMAKE_SOURCE_DIR}/stdlib/StandardNodes.bt"
    )

    gtest_discover_tests(bt_dsl_lsp_server_tests)
endif()
