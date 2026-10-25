#
#   Copyright 2016 rm5248
#   updated 2021 alexb@clever.team
#
#   Licensed under the Apache License, Version 2.0 (the "License");
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#
# Built from the main project with -DCSERIAL_BUILD_EXAMPLES=ON, or on its own
# against an installed CSerial (found with pkg-config).
CMAKE_MINIMUM_REQUIRED(VERSION 3.14)

PROJECT(cserial_examples C CXX)

find_package(Threads REQUIRED)

IF( TARGET cserial_static )
    SET( CSERIAL_EXAMPLE_LIB cserial_static )
ELSE()
    find_package(cserial CONFIG QUIET)
ENDIF()

IF( CSERIAL_EXAMPLE_LIB )
ELSEIF( TARGET cserial::cserial_static )
    SET( CSERIAL_EXAMPLE_LIB cserial::cserial_static )
ELSE()
    find_package(PkgConfig)
    IF( PKG_CONFIG_FOUND )
        pkg_search_module( CSERIAL IMPORTED_TARGET cserial )
    ENDIF()
    IF( TARGET PkgConfig::CSERIAL )
        SET( CSERIAL_EXAMPLE_LIB PkgConfig::CSERIAL )
    ELSE()
        # No installed library (e.g. Windows): build it from the parent directory
        SET( CSERIAL_BUILD_TESTS OFF CACHE BOOL "" FORCE )
        SET( CSERIAL_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE )
        add_subdirectory( ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/cserial )
        SET( CSERIAL_EXAMPLE_LIB cserial_static )
    ENDIF()
ENDIF()

SET( CSERIAL_EXAMPLES
    cserial-example:example_full.c
    simpleterm:serial_simple_term.cpp
    list_ports:list_ports.c
    request_response:request_response.c
    cancel_and_close:cancel_and_close.c
    reconnect:reconnect.c
    control_lines:control_lines.c
    settings:settings.c
    loopback_stress:loopback_stress.c )

FOREACH( example ${CSERIAL_EXAMPLES} )
    STRING( REPLACE ":" ";" parts ${example} )
    LIST( GET parts 0 name )
    LIST( GET parts 1 source )
    add_executable( ${name} ${source} )
    target_link_libraries( ${name} PRIVATE ${CSERIAL_EXAMPLE_LIB} Threads::Threads )
    IF( MSVC )
        target_compile_definitions( ${name} PRIVATE _CRT_SECURE_NO_WARNINGS )
    ENDIF()
ENDFOREACH()
