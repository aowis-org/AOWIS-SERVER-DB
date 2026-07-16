cmake_minimum_required(VERSION 3.21)

project(
    AOWIS-SERVER-DB
    VERSION 0.1.0
    LANGUAGES CXX
)

find_package(Qt6 REQUIRED COMPONENTS Core Sql)

qt_standard_project_setup()

set(AOWIS_SERVER_DB_COMMON_SOURCES
    src/models/database_configuration.h
    src/database_shared.h src/database_shared.cpp
    src/database_helpers.h src/database_helpers.cpp
)

qt_add_library(AOWIS-SERVER-DB-GUI STATIC
    ${AOWIS_SERVER_DB_COMMON_SOURCES}
    src/database_sqlite.cpp
    src/database_sqlite.h
    src/database_gui.h src/database_gui.cpp
)

qt_add_library(AOWIS-SERVER-DB-API STATIC
    ${AOWIS_SERVER_DB_COMMON_SOURCES}
    src/database_api.h src/database_api.cpp
)

target_include_directories(AOWIS-SERVER-DB-GUI
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(AOWIS-SERVER-DB-GUI
    PUBLIC
        Qt6::Core
        Qt6::Sql
        AOWIS::ServerModel
)

if(EMSCRIPTEN)
    target_link_options(AOWIS-SERVER-DB-GUI
        INTERFACE
            "-sFORCE_FILESYSTEM=1"
            "-lidbfs.js"
    )
endif()

target_compile_features(AOWIS-SERVER-DB-GUI PUBLIC cxx_std_17)

target_compile_definitions(AOWIS-SERVER-DB-GUI
    PUBLIC
        AOWIS_SERVER_DB_GUI
)

add_library(AOWIS::ServerDBGUI ALIAS AOWIS-SERVER-DB-GUI)

target_include_directories(AOWIS-SERVER-DB-API
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(AOWIS-SERVER-DB-API
    PUBLIC
        Qt6::Core
        Qt6::Sql
)

target_compile_features(AOWIS-SERVER-DB-API PUBLIC cxx_std_17)

target_compile_definitions(AOWIS-SERVER-DB-API
    PUBLIC
        AOWIS_SERVER_DB_API
)

add_library(AOWIS::ServerDBAPI ALIAS AOWIS-SERVER-DB-API)

qt_add_executable(AOWIS-SERVER-DB-TEST-GUI
    tests/main.cpp
)

target_link_libraries(AOWIS-SERVER-DB-TEST-GUI
    PRIVATE
        AOWIS::ServerDBGUI
)

qt_add_executable(AOWIS-SERVER-DB-TEST-API
    tests/main.cpp
)

target_link_libraries(AOWIS-SERVER-DB-TEST-API
    PRIVATE
        AOWIS::ServerDBAPI
)
