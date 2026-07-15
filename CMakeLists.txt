cmake_minimum_required(VERSION 3.21)

project(
    AOWIS-SERVER-DB
    VERSION 0.1.0
    LANGUAGES CXX
)

find_package(Qt6 REQUIRED COMPONENTS Core)

qt_standard_project_setup()

if(PROJECT_IS_TOP_LEVEL AND NOT EMSCRIPTEN)
    set(AOWIS_SERVER_DB_BUILD_TESTS_DEFAULT ON)
else()
    set(AOWIS_SERVER_DB_BUILD_TESTS_DEFAULT OFF)
endif()

option(AOWIS_SERVER_DB_BUILD_API "Build the server API database adapter" ON)
option(AOWIS_SERVER_DB_BUILD_TESTS "Build database adapter test executables" ${AOWIS_SERVER_DB_BUILD_TESTS_DEFAULT})

set(AOWIS_SERVER_DB_COMMON_SOURCES
    src/models/database_configuration.h
    src/database.cpp
    src/database.h
)

qt_add_library(AOWIS-SERVER-DB-GUI STATIC
    ${AOWIS_SERVER_DB_COMMON_SOURCES}
    src/database_gui.cpp
    src/database_gui.h
)

target_include_directories(AOWIS-SERVER-DB-GUI
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(AOWIS-SERVER-DB-GUI
    PUBLIC
        Qt6::Core
)

target_compile_features(AOWIS-SERVER-DB-GUI PUBLIC cxx_std_17)

target_compile_definitions(AOWIS-SERVER-DB-GUI
    PUBLIC
        AOWIS_SERVER_DB_GUI
)

if(EMSCRIPTEN)
    target_compile_options(AOWIS-SERVER-DB-GUI
        PRIVATE
            "-sUSE_SQLITE3=1"
    )

    target_link_options(AOWIS-SERVER-DB-GUI
        INTERFACE
            "-sUSE_SQLITE3=1"
            "-sFORCE_FILESYSTEM=1"
            "-lidbfs.js"
    )

    target_compile_definitions(AOWIS-SERVER-DB-GUI
        PUBLIC
            AOWIS_SERVER_DB_WASM
    )
else()
    find_package(SQLite3 REQUIRED)

    target_link_libraries(AOWIS-SERVER-DB-GUI
        PUBLIC
            SQLite::SQLite3
    )
endif()

add_library(AOWIS::ServerDBGUI ALIAS AOWIS-SERVER-DB-GUI)

if(AOWIS_SERVER_DB_BUILD_API AND NOT EMSCRIPTEN)
    find_package(Qt6 REQUIRED COMPONENTS Sql)

    qt_add_library(AOWIS-SERVER-DB-API STATIC
        ${AOWIS_SERVER_DB_COMMON_SOURCES}
        src/database_api.cpp
        src/database_api.h
    )

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
endif()

if(AOWIS_SERVER_DB_BUILD_TESTS)
    qt_add_executable(AOWIS-SERVER-DB-TEST-GUI
        tests/main.cpp
    )

    target_link_libraries(AOWIS-SERVER-DB-TEST-GUI
        PRIVATE
            AOWIS::ServerDBGUI
    )

    if(TARGET AOWIS-SERVER-DB-API)
        qt_add_executable(AOWIS-SERVER-DB-TEST-API
            tests/main.cpp
        )

        target_link_libraries(AOWIS-SERVER-DB-TEST-API
            PRIVATE
                AOWIS::ServerDBAPI
        )
    endif()
endif()
