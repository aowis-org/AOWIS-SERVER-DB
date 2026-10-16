cmake_minimum_required(VERSION 3.21)

project(AOWIS-SERVER-DB VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
find_package(Qt6 REQUIRED COMPONENTS Core Sql)

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

add_library(aowis-db-core STATIC
    include/aowis/db/database.h
    include/aowis/db/identity.h
    src/identity.cpp
    src/history.cpp
    src/objects.cpp
    src/database.cpp
)
add_library(AOWIS::DB ALIAS aowis-db-core)

target_compile_features(aowis-db-core PUBLIC cxx_std_20)
target_include_directories(aowis-db-core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(aowis-db-core PUBLIC Qt6::Core Qt6::Sql AOWIS::Model)
set_target_properties(aowis-db-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(BUILD_TESTING)
    add_executable(aowis-db-sqlite-tests tests/sqlite_test.cpp)
    target_link_libraries(aowis-db-sqlite-tests PRIVATE aowis-db-core)
    add_test(NAME aowis-db-sqlite COMMAND aowis-db-sqlite-tests)
    add_executable(aowis-db-migration-tests tests/sqlite_migration_test.cpp)
    target_link_libraries(aowis-db-migration-tests PRIVATE aowis-db-core)
    add_test(NAME aowis-db-migration COMMAND aowis-db-migration-tests)
    add_executable(aowis-db-history-tests tests/history_test.cpp)
    target_link_libraries(aowis-db-history-tests PRIVATE aowis-db-core)
    add_test(NAME aowis-db-history COMMAND aowis-db-history-tests)
    add_executable(aowis-db-objects-tests tests/objects_test.cpp)
    target_link_libraries(aowis-db-objects-tests PRIVATE aowis-db-core)
    add_test(NAME aowis-db-objects COMMAND aowis-db-objects-tests)
    set_tests_properties(aowis-db-sqlite aowis-db-migration aowis-db-history aowis-db-objects PROPERTIES LABELS "aowis;database")
endif()
