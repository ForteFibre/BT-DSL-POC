cmake_minimum_required(VERSION 3.20)
project(bt-dsl-core VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(BUILD_TESTS "Build tests" ON)
option(BUILD_CLI "Build CLI executable (btc)" ON)
option(BUILD_LSP_SERVER "Build LSP server executable" ON)
option(BUILD_WASM "Build WebAssembly bindings" OFF)
option(WERROR "Treat warnings as errors" OFF)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# Minimal build mode (syntax + formatter/JSON only). Useful for WebAssembly.
option(BT_DSL_MINIMAL_CORE "Build only syntax/basic components" OFF)

if(EMSCRIPTEN)
    # When targeting WASM, default to a minimal core build.
    set(BT_DSL_MINIMAL_CORE ON CACHE BOOL "" FORCE)
    set(BUILD_CLI OFF CACHE BOOL "" FORCE)
    set(BUILD_LSP_SERVER OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif()

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
if(NOT BT_DSL_MINIMAL_CORE)
    FetchContent_Declare(
        tinyxml2
        GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
        GIT_TAG 10.0.0
    )
    FetchContent_MakeAvailable(tinyxml2)
endif()

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

# fmt (fast formatting library)
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.1.4
)
FetchContent_MakeAvailable(fmt)

# rang (header-only terminal colors library)
FetchContent_Declare(
    rang
    GIT_REPOSITORY https://github.com/agauniyal/rang.git
    GIT_TAG v3.2
)
FetchContent_MakeAvailable(rang)

# yaml-cpp (for btc.yaml project configuration)
if(NOT BT_DSL_MINIMAL_CORE)
    FetchContent_Declare(
        yaml-cpp
        GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
        GIT_TAG 0.8.0
    )
    set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(yaml-cpp)
endif()

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
if(BT_DSL_MINIMAL_CORE)
    set(BT_DSL_CORE_SOURCES
        lib/basic/source_manager.cpp
        lib/basic/diagnostic.cpp

        # Frontend (Lexer -> recursive descent parser -> AST)
        lib/syntax/lexer.cpp
        lib/syntax/parser.cpp
        lib/syntax/frontend.cpp

        # AST utilities
        lib/ast/json_visitor.cpp
    )
else()
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

        # AST utilities
        lib/ast/json_visitor.cpp

        # LSP (serverless language service)
        lib/lsp/completion_context.cpp
        lib/lsp/workspace.cpp

        # Project configuration
        lib/project/project_config.cpp

        # Compiler driver
        lib/driver/compiler.cpp
        lib/driver/stdlib_finder.cpp
    )
endif()

add_library(bt_dsl_core STATIC ${BT_DSL_CORE_SOURCES})
target_include_directories(bt_dsl_core PUBLIC
    include
)
if(NOT BT_DSL_MINIMAL_CORE)
    target_link_libraries(bt_dsl_core PUBLIC tinyxml2)
endif()
target_link_libraries(bt_dsl_core PUBLIC nlohmann_json::nlohmann_json)
target_link_libraries(bt_dsl_core PUBLIC Microsoft.GSL::GSL)
if(NOT BT_DSL_MINIMAL_CORE)
    target_link_libraries(bt_dsl_core PUBLIC yaml-cpp::yaml-cpp)
endif()
target_link_libraries(bt_dsl_core PUBLIC fmt::fmt)
target_include_directories(bt_dsl_core PUBLIC ${rang_SOURCE_DIR}/include)

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
# WebAssembly bindings (Emscripten) - for Prettier formatter
# =============================================================================
if(BUILD_WASM)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "BUILD_WASM requires Emscripten toolchain")
    endif()

    add_executable(formatter_wasm tools/formatter_wasm/main.cpp)
    target_link_libraries(formatter_wasm PRIVATE bt_dsl_core)

    # Produce an ES module factory (MODULARIZE) for Node/Esm usage.
    target_link_options(formatter_wasm PRIVATE
        "--bind"
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sENVIRONMENT=node"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sASSERTIONS=0"
    )
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

    # Auto-discover all test files
    file(GLOB_RECURSE TEST_SOURCES
        tests/unit/*.cpp
        tests/integration/*.cpp
    )

    foreach(TEST_SRC ${TEST_SOURCES})
        get_filename_component(TEST_NAME ${TEST_SRC} NAME_WE)
        bt_dsl_add_gtest_exe(${TEST_NAME} ${TEST_SRC})
    endforeach()
endif()


