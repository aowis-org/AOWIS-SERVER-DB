cmake_minimum_required(VERSION 3.21)

project(AOWIS-SERVER-DB VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(Qt6 REQUIRED COMPONENTS Core Sql)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

# System-service builds use /etc for configuration and /var for writable data.
# Normal desktop/embedded builds use Qt's platform-specific AppDataLocation.
option(AOWIS_DB_SERVER_MODE "Use system service configuration paths" OFF)

# Reuse the UUIDv7 implementation already owned by AOWIS-SERVER-MODEL.
# The pinned Git submodule under external/ is the default.
# Parent builds may already provide AOWIS::Model.
if(NOT TARGET AOWIS::Model)
    set(AOWIS_MODEL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/AOWIS-SERVER-MODEL"
        CACHE PATH "Path to AOWIS-SERVER-MODEL")
    if(NOT EXISTS "${AOWIS_MODEL_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Model submodule missing. Run ./git_submodule_init.sh or set AOWIS_MODEL_SOURCE_DIR.")
    endif()
    add_subdirectory("${AOWIS_MODEL_SOURCE_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/aowis-server-model")
endif()

# Reject Model layout changes until the binary codec is consciously evolved.
# Preventing silent field loss takes priority over blindly accepting a new
# submodule revision that the existing decoder cannot represent.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_model_codec.py"
        "${AOWIS_MODEL_SOURCE_DIR}/include/aowis/model"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/model_codec_fields.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/model_codec.cpp"
    RESULT_VARIABLE AOWIS_MODEL_CODEC_CHECK_RESULT
    OUTPUT_VARIABLE AOWIS_MODEL_CODEC_CHECK_OUTPUT
    ERROR_VARIABLE AOWIS_MODEL_CODEC_CHECK_ERROR
)
if(NOT AOWIS_MODEL_CODEC_CHECK_RESULT EQUAL 0)
    message(FATAL_ERROR "Model serialization schema check failed:\n${AOWIS_MODEL_CODEC_CHECK_ERROR}")
endif()
message(STATUS "${AOWIS_MODEL_CODEC_CHECK_OUTPUT}")
# Merge field rules must remain in lockstep with the schema and codec.
execute_process(
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_model_field_merge.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/model_codec_fields.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/model_codec.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/aowis/db/model_persistence.h"
    RESULT_VARIABLE AOWIS_MODEL_FIELD_MERGE_CHECK_RESULT
    ERROR_VARIABLE AOWIS_MODEL_FIELD_MERGE_CHECK_ERROR
)
if(NOT AOWIS_MODEL_FIELD_MERGE_CHECK_RESULT EQUAL 0)
    message(FATAL_ERROR "Field merge coverage check failed:\n${AOWIS_MODEL_FIELD_MERGE_CHECK_ERROR}")
endif()


add_library(aowis-db-core STATIC
    include/aowis/db/configuration.h
    src/configuration.cpp
    include/aowis/db/database.h
    include/aowis/db/identity.h
    src/identity.cpp
    src/sessions.cpp
    src/password_credentials.cpp
    src/session_token_internal.h
    src/security_audit.cpp
    src/security_admin.cpp
    src/security_catalog.cpp
    src/history.cpp
    src/objects.cpp
    src/content_store.h
    src/content_store.cpp
    src/merkle_tree.h
    src/merkle_tree.cpp
    src/revision_root.h
    src/revision_root.cpp
    src/working_storage.cpp
    src/object_groups.cpp
    src/model_codec.cpp
    src/model_persistence.cpp
    src/model_validation.cpp
    include/aowis/db/model_validation.h
    include/aowis/db/model_codec.h
    include/aowis/db/model_persistence.h
    src/database.cpp
    src/storage_backend.h
    src/sqlite_backend.h
    src/sqlite_backend.cpp
    src/postgres_backend.h
    src/postgres_backend.cpp
    src/postgres_schema.cpp
    src/sqlite_schema.cpp
    src/revision_ancestry.cpp
    src/object_merge_preview.cpp
    src/object_merge_state.h
    src/object_merge_commit.cpp
)
add_library(AOWIS::DB ALIAS aowis-db-core)

target_compile_features(aowis-db-core PUBLIC cxx_std_20)
if(AOWIS_DB_SERVER_MODE)
    target_compile_definitions(aowis-db-core PUBLIC AOWIS_DB_SERVER_MODE=1)
endif()
target_include_directories(aowis-db-core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(aowis-db-core PUBLIC Qt6::Core Qt6::Sql AOWIS::Model PRIVATE OpenSSL::Crypto)
set_target_properties(aowis-db-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Trusted local database operator CLI. No network dependency.
add_executable(aowis-db-admin src/db_admin_main.cpp)
target_link_libraries(aowis-db-admin PRIVATE aowis-db-core)

# The embedded core remains usable without Qt HttpServer/Qt Network.
# The HTTP library is optional because many headless/embedded builds install
# only Qt Core and SQL. No HTTP port is opened unless the executable is run.
option(AOWIS_DB_BUILD_HTTP "Build optional REST transport and local development server" ON)
if(AOWIS_DB_BUILD_HTTP)
    find_package(Qt6 QUIET COMPONENTS Network HttpServer)
    if(TARGET Qt6::Network AND TARGET Qt6::HttpServer)
        add_library(aowis-db-http STATIC
            include/aowis/db/http_routes.h
            include/aowis/db/http_hardening.h
            include/aowis/db/login_throttle.h
            src/login_throttle.cpp
            src/http_routes.cpp
            src/http_hardening.cpp
        )
        add_library(AOWIS::DBHttp ALIAS aowis-db-http)
        target_compile_features(aowis-db-http PUBLIC cxx_std_20)
        target_include_directories(aowis-db-http PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        )
        target_link_libraries(aowis-db-http PUBLIC aowis-db-core Qt6::HttpServer Qt6::Network)
        add_executable(aowis-server-db src/http_server_main.cpp)
        target_link_libraries(aowis-server-db PRIVATE aowis-db-http)
        message(STATUS "AOWIS optional HTTP server enabled")
    else()
        message(STATUS "AOWIS HTTP server not built (Qt6 HttpServer/Network unavailable)")
    endif()
endif()

if(BUILD_TESTING)
    # Explicit opt-in performance executable. Deliberately not registered with
    # CTest or the aowis-db-tests aggregate target: its cost scales with N.
    add_executable(aowis-db-network-benchmark benchmarks/network_persistence.cpp)
    target_link_libraries(aowis-db-network-benchmark PRIVATE aowis-db-core)

    # Register each DB test in one place: the aggregate build target automatically
    # includes new tests, so CTest never finds an executable the script omitted.
    function(aowis_add_db_test target test_name source_file)
        add_executable(${target} ${source_file})
        target_link_libraries(${target} PRIVATE aowis-db-core)
        add_test(NAME ${test_name} COMMAND ${target})
        set_tests_properties(${test_name} PROPERTIES LABELS "aowis;database")
        set_property(GLOBAL APPEND PROPERTY AOWIS_DB_TEST_EXECUTABLES ${target})
    endfunction()

    aowis_add_db_test(aowis-db-configuration-tests aowis-db-configuration tests/configuration_test.cpp)
    add_executable(aowis-db-admin-cli-tests tests/admin_cli_test.cpp)
    target_link_libraries(aowis-db-admin-cli-tests PRIVATE aowis-db-core)
    add_dependencies(aowis-db-admin-cli-tests aowis-db-admin)
    add_test(NAME aowis-db-admin-cli COMMAND aowis-db-admin-cli-tests $<TARGET_FILE:aowis-db-admin>)
    set_tests_properties(aowis-db-admin-cli PROPERTIES LABELS "aowis;database")
    set_property(GLOBAL APPEND PROPERTY AOWIS_DB_TEST_EXECUTABLES aowis-db-admin-cli-tests)
    aowis_add_db_test(aowis-db-sqlite-tests aowis-db-sqlite tests/sqlite_test.cpp)
    aowis_add_db_test(aowis-db-backend-contract-tests aowis-db-backend-contract tests/backend_contract_test.cpp)
    aowis_add_db_test(aowis-db-sqlite-schema-verification-tests aowis-db-sqlite-schema-verification tests/sqlite_schema_verification_test.cpp)
    aowis_add_db_test(aowis-db-history-tests aowis-db-history tests/history_test.cpp)
    aowis_add_db_test(aowis-db-revision-hash-contract-tests aowis-db-revision-hash-contract tests/revision_hash_contract_test.cpp)
    aowis_add_db_test(aowis-db-revision-ancestry-tests aowis-db-revision-ancestry tests/revision_ancestry_test.cpp)
    aowis_add_db_test(aowis-db-object-merge-preview-tests aowis-db-object-merge-preview tests/object_merge_preview_test.cpp)
    aowis_add_db_test(aowis-db-object-merge-commit-tests aowis-db-object-merge-commit tests/object_merge_commit_test.cpp)
    aowis_add_db_test(aowis-db-object-merge-local-edits-tests aowis-db-object-merge-local-edits tests/object_merge_local_edits_test.cpp)
    aowis_add_db_test(aowis-db-object-merge-fields-tests aowis-db-object-merge-fields tests/object_merge_fields_test.cpp)
    aowis_add_db_test(aowis-db-object-merge-field-resolutions-tests aowis-db-object-merge-field-resolutions tests/object_merge_field_resolutions_test.cpp)
    aowis_add_db_test(aowis-db-object-merge-metadata-tests aowis-db-object-merge-metadata tests/object_merge_metadata_test.cpp)
    aowis_add_db_test(aowis-db-object-merge-tags-tests aowis-db-object-merge-tags tests/object_merge_tags_test.cpp)
    aowis_add_db_test(aowis-db-content-store-tests aowis-db-content-store tests/content_store_test.cpp)
    aowis_add_db_test(aowis-db-merkle-tree-tests aowis-db-merkle-tree tests/merkle_tree_test.cpp)
    aowis_add_db_test(aowis-db-revision-root-tests aowis-db-revision-root tests/revision_root_test.cpp)
    aowis_add_db_test(aowis-db-objects-tests aowis-db-objects tests/objects_test.cpp)
    aowis_add_db_test(aowis-db-model-tests aowis-db-model tests/model_persistence_test.cpp)
    aowis_add_db_test(aowis-db-object-groups-tests aowis-db-object-groups tests/object_groups_test.cpp)
    aowis_add_db_test(aowis-db-model-validation-tests aowis-db-model-validation tests/model_validation_test.cpp)
    aowis_add_db_test(aowis-db-model-object-guard-tests aowis-db-model-object-guard tests/model_object_guard_test.cpp)
    aowis_add_db_test(aowis-db-model-membership-tests aowis-db-model-membership tests/model_membership_index_test.cpp)
    aowis_add_db_test(aowis-db-model-references-tests aowis-db-model-references tests/model_reference_index_test.cpp)
    aowis_add_db_test(aowis-db-model-save-indexed-tests aowis-db-model-save-indexed tests/model_save_indexed_test.cpp)
    aowis_add_db_test(aowis-db-model-reference-deltas-tests aowis-db-model-reference-deltas tests/model_reference_delta_test.cpp)
    aowis_add_db_test(aowis-db-model-incremental-groups-tests aowis-db-model-incremental-groups tests/model_incremental_groups_test.cpp)
    aowis_add_db_test(aowis-db-model-local-structure-tests aowis-db-model-local-structure tests/model_local_structure_test.cpp)
    aowis_add_db_test(aowis-db-model-local-links-tests aowis-db-model-local-links tests/model_local_links_test.cpp)
    aowis_add_db_test(aowis-db-model-local-resources-tests aowis-db-model-local-resources tests/model_local_resources_test.cpp)
    aowis_add_db_test(aowis-db-sessions-tests aowis-db-sessions tests/sessions_test.cpp)
    aowis_add_db_test(aowis-db-session-capacity-tests aowis-db-session-capacity tests/session_capacity_test.cpp)
    aowis_add_db_test(aowis-db-session-redaction-tests aowis-db-session-redaction tests/session_redaction_test.cpp)
    aowis_add_db_test(aowis-db-password-credentials-tests aowis-db-password-credentials tests/password_credentials_test.cpp)
    aowis_add_db_test(aowis-db-password-change-tests aowis-db-password-change tests/password_self_change_test.cpp)
    aowis_add_db_test(aowis-db-password-recovery-tests aowis-db-password-recovery tests/password_recovery_test.cpp)
    aowis_add_db_test(aowis-db-security-audit-tests aowis-db-security-audit tests/security_audit_test.cpp)
    aowis_add_db_test(aowis-db-security-admin-tests aowis-db-security-admin tests/security_admin_test.cpp)
    aowis_add_db_test(aowis-db-security-catalog-tests aowis-db-security-catalog tests/security_catalog_test.cpp)
    if(TARGET aowis-db-http)
        # Exercise INI default and optional SQLite command-line override in
        # the actual HTTP server executable, not only the configuration parser.
        add_executable(aowis-server-db-cli-tests tests/server_cli_test.cpp)
        target_link_libraries(aowis-server-db-cli-tests PRIVATE aowis-db-core Qt6::Network)
        add_dependencies(aowis-server-db-cli-tests aowis-server-db)
        add_test(NAME aowis-server-db-cli COMMAND aowis-server-db-cli-tests $<TARGET_FILE:aowis-server-db>)
        set_tests_properties(aowis-server-db-cli PROPERTIES LABELS "aowis;database")
        set_property(GLOBAL APPEND PROPERTY AOWIS_DB_TEST_EXECUTABLES aowis-server-db-cli-tests)
        aowis_add_db_test(aowis-db-http-tests aowis-db-http tests/http_test.cpp)
        target_link_libraries(aowis-db-http-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-mutations-tests aowis-db-http-mutations tests/http_mutations_test.cpp)
        target_link_libraries(aowis-db-http-mutations-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-branch-create-tests aowis-db-http-branch-create tests/http_branch_create_test.cpp)
        target_link_libraries(aowis-db-http-branch-create-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-merge-preview-tests aowis-db-http-merge-preview tests/http_merge_preview_test.cpp)
        target_link_libraries(aowis-db-http-merge-preview-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-merge-commit-tests aowis-db-http-merge-commit tests/http_merge_commit_test.cpp)
        target_link_libraries(aowis-db-http-merge-commit-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-sessions-tests aowis-db-http-sessions tests/http_sessions_test.cpp)
        target_link_libraries(aowis-db-http-sessions-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-security-catalog-tests aowis-db-http-security-catalog tests/http_security_catalog_test.cpp)
        target_link_libraries(aowis-db-http-security-catalog-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-security-mutations-tests aowis-db-http-security-mutations tests/http_security_mutations_test.cpp)
        target_link_libraries(aowis-db-http-security-mutations-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-hardening-tests aowis-db-http-hardening tests/http_hardening_test.cpp)
        target_link_libraries(aowis-db-http-hardening-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-login-throttle-tests aowis-db-http-login-throttle tests/http_login_throttle_test.cpp)
        target_link_libraries(aowis-db-http-login-throttle-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-self-service-tests aowis-db-http-self-service tests/http_session_self_service_test.cpp)
        aowis_add_db_test(aowis-db-http-password-login-tests aowis-db-http-password-login tests/http_password_login_test.cpp)
        aowis_add_db_test(aowis-db-http-password-change-tests aowis-db-http-password-change tests/http_password_change_test.cpp)
        aowis_add_db_test(aowis-db-http-password-recovery-tests aowis-db-http-password-recovery tests/http_password_recovery_test.cpp)
        aowis_add_db_test(aowis-db-http-own-sessions-tests aowis-db-http-own-sessions tests/http_own_sessions_test.cpp)
        target_link_libraries(aowis-db-http-own-sessions-tests PRIVATE aowis-db-http)
        aowis_add_db_test(aowis-db-http-revoke-others-tests aowis-db-http-revoke-others tests/http_revoke_other_sessions_test.cpp)
        target_link_libraries(aowis-db-http-revoke-others-tests PRIVATE aowis-db-http)
        target_link_libraries(aowis-db-http-password-recovery-tests PRIVATE aowis-db-http)
        target_link_libraries(aowis-db-http-password-change-tests PRIVATE aowis-db-http)
        target_link_libraries(aowis-db-http-password-login-tests PRIVATE aowis-db-http)
        target_link_libraries(aowis-db-http-self-service-tests PRIVATE aowis-db-http)
    endif()

    get_property(aowis_db_test_executables GLOBAL PROPERTY AOWIS_DB_TEST_EXECUTABLES)
    add_custom_target(aowis-db-tests DEPENDS ${aowis_db_test_executables})
    # Build the actual server executable as part of the normal test build too.
    # The target name is part of the public distribution interface.
    if(TARGET aowis-server-db)
        add_dependencies(aowis-db-tests aowis-server-db)
    endif()
endif()
