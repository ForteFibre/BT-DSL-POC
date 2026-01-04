cmake_minimum_required(VERSION 3.20)
project(bt-dsl-core VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(BUILD_TESTS "Build tests" ON)
option(BUILD_CLI "Build CLI executable (btc)" ON)
option(BUILD_LSP_SERVER "Build LSP server executable" ON)
option(WERROR "Treat warnings as errors" OFF)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# =============================================================================
# Sanitizers (must be set before any targets)
# =============================================================================
if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(WARNING "Sanitizers are only supported with GCC/Clang")
    endif()
endif()

# =============================================================================
# Dependencies via FetchContent
# =============================================================================
include(FetchContent)

# TinyXML2 (for BT.CPP XML emission)
FetchContent_Declare(
    tinyxml2
    GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
    GIT_TAG 10.0.0
)
FetchContent_MakeAvailable(tinyxml2)

# nlohmann/json (used for serverless LSP JSON APIs)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(nlohmann_json)

# Microsoft GSL (Guideline Support Library)
FetchContent_Declare(
    GSL
    GIT_REPOSITORY https://github.com/microsoft/GSL.git
    GIT_TAG v4.0.0
)
FetchContent_MakeAvailable(GSL)

# yaml-cpp (for btc.yaml project configuration)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)

# GoogleTest (tests)
if(BUILD_TESTS)
    # Keep GoogleTest build lightweight (no install) and deterministic.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    # MSVC: avoid CRT mismatch in downstream consumers.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)
endif()


# =============================================================================
# Core library
# =============================================================================
set(BT_DSL_CORE_SOURCES
    lib/basic/source_manager.cpp
    lib/basic/diagnostic.cpp
    lib/basic/diagnostic_printer.cpp

    # Sema: Resolution (symbols, names, modules)
    lib/sema/resolution/symbol_table.cpp
    lib/sema/resolution/symbol_table_builder.cpp
    lib/sema/resolution/name_resolver.cpp
    lib/sema/resolution/module_resolver.cpp

    # Sema: Types (type system, type checking, constants)
    lib/sema/types/type.cpp
    lib/sema/types/type_utils.cpp
    lib/sema/types/const_evaluator.cpp
    lib/sema/types/type_checker.cpp

    # Sema: Analysis (CFG, data-flow, safety checks)
    lib/sema/analysis/cfg_builder.cpp
    lib/sema/analysis/init_checker.cpp
    lib/sema/analysis/null_checker.cpp
    lib/sema/analysis/tree_recursion_checker.cpp

    # Codegen
    lib/codegen/xml_generator.cpp
    lib/codegen/model_converter.cpp

    # Frontend (Lexer -> recursive descent parser -> AST)
    lib/syntax/lexer.cpp
    lib/syntax/parser.cpp
    lib/syntax/frontend.cpp

    # LSP (serverless language service)
    lib/lsp/completion_context.cpp
    lib/lsp/workspace.cpp

    # Project configuration
    lib/project/project_config.cpp

    # Compiler driver
    lib/driver/compiler.cpp
    lib/driver/stdlib_finder.cpp
)

add_library(bt_dsl_core STATIC ${BT_DSL_CORE_SOURCES})
target_include_directories(bt_dsl_core PUBLIC
    include
)
target_link_libraries(bt_dsl_core PUBLIC tinyxml2)
target_link_libraries(bt_dsl_core PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(bt_dsl_core PUBLIC Microsoft.GSL::GSL)
target_link_libraries(bt_dsl_core PUBLIC yaml-cpp::yaml-cpp)

# Common compiler warnings
set(BT_DSL_COMPILE_OPTIONS
    $<$<CXX_COMPILER_ID:GNU,Clang>:
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Woverloaded-virtual
    -Wconversion -Wsign-conversion
    $<$<BOOL:${WERROR}>:-Werror>
    >
    $<$<CXX_COMPILER_ID:MSVC>:/W4 $<$<BOOL:${WERROR}>:/WX>>
)
set(BT_DSL_TEST_COMPILE_OPTIONS ${BT_DSL_COMPILE_OPTIONS})
list(APPEND BT_DSL_TEST_COMPILE_OPTIONS
    $<$<CXX_COMPILER_ID:GNU,Clang>:-UNDEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
)

target_compile_options(bt_dsl_core PRIVATE ${BT_DSL_COMPILE_OPTIONS})

# =============================================================================
# CLI Executable
# =============================================================================
if(BUILD_CLI)
    add_executable(btc tools/btc/main.cpp)
    target_link_libraries(btc PRIVATE bt_dsl_core)
    target_compile_options(btc PRIVATE ${BT_DSL_COMPILE_OPTIONS})

    # Install btc executable and stdlib
    install(TARGETS btc DESTINATION bin)
    install(DIRECTORY ${CMAKE_SOURCE_DIR}/std/
        DESTINATION share/bt-dsl/std
        FILES_MATCHING PATTERN "*.bt")

    # Define default stdlib path relative to install prefix
    target_compile_definitions(btc PRIVATE
        BT_DSL_STDLIB_INSTALL_PATH="${CMAKE_INSTALL_PREFIX}/share/bt-dsl/std")
endif()

# =============================================================================
# LSP Server Executable (stdio)
# =============================================================================
if(BUILD_LSP_SERVER)
    add_executable(bt_dsl_lsp_server tools/bt_dsl_lsp_server/main.cpp)
    target_link_libraries(bt_dsl_lsp_server PRIVATE bt_dsl_core)
    target_compile_options(bt_dsl_lsp_server PRIVATE ${BT_DSL_COMPILE_OPTIONS})
endif()

# =============================================================================
# Tests
# =============================================================================
if(BUILD_TESTS)
    enable_testing()

    include(GoogleTest)

    # Shared settings for all test executables.
    add_library(bt_dsl_core_test_common INTERFACE)
    target_link_libraries(bt_dsl_core_test_common INTERFACE bt_dsl_core GTest::gtest_main)
    target_compile_options(bt_dsl_core_test_common INTERFACE ${BT_DSL_TEST_COMPILE_OPTIONS})

    # Helper to keep test target definitions consistent.
    function(bt_dsl_add_gtest_exe target)
        add_executable(${target} ${ARGN})
        target_link_libraries(${target} PRIVATE bt_dsl_core_test_common)
        # Discover tests at ctest time so we don't need to execute them at build time.
        gtest_discover_tests(${target} DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30)
    endfunction()

    bt_dsl_add_gtest_exe(bt_dsl_core_tests
        tests/unit/dump/test_dump_types_literals.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_dump_expressions
        tests/unit/dump/test_dump_expressions.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_dump_decls_signatures
        tests/unit/dump/test_dump_decls_signatures.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ast_program_top_level
        tests/unit/ast/test_program_top_level.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ast_docs_crlf
        tests/unit/ast/test_docs_crlf.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ast_statements
        tests/unit/ast/test_statements.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ast_parser_ported_basic
        tests/unit/ast/test_parser_ported_basic.cpp
    )

    # ================================
    # Syntax (lexer/parser) Unit Tests
    # ================================
    bt_dsl_add_gtest_exe(bt_dsl_core_syntax_lexer
        tests/unit/syntax/test_lexer.cpp
    )

    # ================================
    # Sema Tests
    # ================================

    bt_dsl_add_gtest_exe(bt_dsl_core_sema_null_checker
        tests/unit/sema/test_null_checker.cpp
    )

    # ================================
    # New AST Tests
    # ================================
    bt_dsl_add_gtest_exe(bt_dsl_core_ast_literals
        tests/unit/ast/test_literals.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ast_expressions
        tests/unit/ast/test_expressions.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ast_preconditions
        tests/unit/ast/test_preconditions.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ast_children_blocks
        tests/unit/ast/test_children_blocks.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ast_complex_examples
        tests/unit/ast/test_complex_examples.cpp
    )

    # ================================
    # Sema Tests
    # ================================
    bt_dsl_add_gtest_exe(bt_dsl_core_sema_name_resolution
        tests/unit/sema/test_name_resolution.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_sema_type_checker
        tests/unit/sema/test_type_checker.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_sema_tree_recursion
        tests/unit/sema/test_tree_recursion.cpp
    )

    # Additional sema tests (previously present but not wired up in CMake).
    bt_dsl_add_gtest_exe(bt_dsl_core_sema_cfg
        tests/unit/sema/test_cfg.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_sema_const_evaluator
        tests/unit/sema/test_const_evaluator.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_sema_module_resolution
        tests/unit/sema/test_module_resolution.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_sema_init_checker
        tests/unit/sema/test_init_checker.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_sema_null_checker_v2
        tests/unit/sema/test_null_checker_v2.cpp
    )

    # ================================
    # Codegen Tests
    # ================================
    bt_dsl_add_gtest_exe(bt_dsl_core_codegen_xml_generator
        tests/unit/codegen/test_xml_generator.cpp
    )

    # ================================
    # LSP Tests (serverless APIs)
    # ================================
    bt_dsl_add_gtest_exe(bt_dsl_core_lsp_workspace_basic
        tests/unit/lsp/test_workspace_basic.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_lsp_completion_context
        tests/unit/lsp/test_completion_context.cpp
    )

    # ================================
    # Integration (golden) Tests
    # ================================
    bt_dsl_add_gtest_exe(bt_dsl_core_integration_golden
        tests/integration/test_golden_outputs.cpp
    )

    # ================================
    # Reference Compliance Tests
    # ================================
    bt_dsl_add_gtest_exe(bt_dsl_core_ref_lexical_structure
        tests/unit/reference/test_ref_lexical_structure.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ref_syntax
        tests/unit/reference/test_ref_syntax.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ref_type_definitions
        tests/unit/reference/test_ref_type_definitions.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ref_type_inference
        tests/unit/reference/test_ref_type_inference.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ref_type_compatibility
        tests/unit/reference/test_ref_type_compatibility.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ref_expression_typing
        tests/unit/reference/test_ref_expression_typing.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ref_declarations_scopes
        tests/unit/reference/test_ref_declarations_scopes.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ref_static_analysis
        tests/unit/reference/test_ref_static_analysis.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ref_xml_mapping
        tests/unit/reference/test_ref_xml_mapping.cpp
    )

    bt_dsl_add_gtest_exe(bt_dsl_core_ref_imports
        tests/unit/reference/test_ref_imports.cpp
    )
        
    bt_dsl_add_gtest_exe(bt_dsl_core_ref_frontend_recovery
        tests/unit/reference/test_ref_frontend_recovery.cpp
    )
endif()


