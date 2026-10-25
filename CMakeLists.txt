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
CMAKE_MINIMUM_REQUIRED(VERSION 3.14)

PROJECT(cserial VERSION 0.4.0 LANGUAGES C)

INCLUDE(CheckIncludeFiles)
INCLUDE(GNUInstallDirs)
INCLUDE(CMakePackageConfigHelpers)

IF( CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND NOT CMAKE_CROSSCOMPILING )
    SET( CSERIAL_TESTS_DEFAULT ON )
ELSE()
    SET( CSERIAL_TESTS_DEFAULT OFF )
ENDIF()

OPTION( CSERIAL_BUILD_TESTS "Build CSerial unit tests (requires C++14 and network access for GoogleTest)" ${CSERIAL_TESTS_DEFAULT} )
OPTION( CSERIAL_BUILD_EXAMPLES "Build the example programs in examples/" OFF )
OPTION( CSERIAL_ENABLE_ASAN "Build with AddressSanitizer" OFF )
OPTION( CSERIAL_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF )
OPTION( CSERIAL_ENABLE_TSAN "Build with ThreadSanitizer" OFF )

#
# CMake variables
#

#see if we are on Linux and have RS485
CHECK_INCLUDE_FILES(linux/serial.h HAVE_LINUX_SERIAL)

# Platform Defines, append to this to set various defines for the code
SET( PLATFORM_DEFINES )
SET( install_targets )

#
# library version
#
SET(CSERIAL_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
SET(CSERIAL_VERSION_STRING ${PROJECT_VERSION})

ADD_DEFINITIONS( "-DCSERIAL_VERSION=${CSERIAL_VERSION_STRING}" )

#
# Sanitizers (GCC/Clang only)
#
IF( CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" )
    SET( CSERIAL_SANITIZE_FLAGS )
    IF( CSERIAL_ENABLE_ASAN )
        LIST( APPEND CSERIAL_SANITIZE_FLAGS -fsanitize=address )
    ENDIF()
    IF( CSERIAL_ENABLE_UBSAN )
        LIST( APPEND CSERIAL_SANITIZE_FLAGS -fsanitize=undefined -fno-sanitize-recover=undefined )
    ENDIF()
    IF( CSERIAL_ENABLE_TSAN )
        LIST( APPEND CSERIAL_SANITIZE_FLAGS -fsanitize=thread )
    ENDIF()
    IF( CSERIAL_SANITIZE_FLAGS )
        ADD_COMPILE_OPTIONS( ${CSERIAL_SANITIZE_FLAGS} -fno-omit-frame-pointer -g )
        ADD_LINK_OPTIONS( ${CSERIAL_SANITIZE_FLAGS} )
    ENDIF()
ELSEIF( MSVC AND CSERIAL_ENABLE_ASAN )
    ADD_COMPILE_OPTIONS( /fsanitize=address )
ENDIF()

#
# Enable all warnings
#
if(MSVC)
  if(CMAKE_C_FLAGS MATCHES "/W[0-4]")
    string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /W4")
  endif()
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-long-long -Wno-unused-parameter -pedantic")
endif()

#
# Sources for the library
#
SET( C_SERIAL_SOURCES
	src/c_serial.c )
IF( WIN32 )
    LIST( APPEND C_SERIAL_SOURCES src/c_serial_win.c )
ELSE()
    LIST( APPEND C_SERIAL_SOURCES src/c_serial_posix.c )
    IF( APPLE )
        LIST( APPEND C_SERIAL_SOURCES src/c_serial_darwin.c )
    ELSEIF( CMAKE_SYSTEM_NAME MATCHES "Linux|Android" )
        LIST( APPEND C_SERIAL_SOURCES src/c_serial_linux.c )
    ELSE()
        LIST( APPEND C_SERIAL_SOURCES src/c_serial_unix.c )
    ENDIF()
ENDIF()
SET( C_SERIAL_HEADERS 
	include/cserial/c_serial.h )

SET( C_SERIAL_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src )
INCLUDE_DIRECTORIES( ${C_SERIAL_INCLUDE_DIRECTORIES} ${CMAKE_CURRENT_BINARY_DIR} )

CONFIGURE_FILE(config.h.in config.h)

IF( UNIX )
    SET(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)
ENDIF( UNIX )

#
# Library information
#
ADD_LIBRARY( cserial SHARED ${C_SERIAL_SOURCES} )
ADD_LIBRARY( cserial_static STATIC ${C_SERIAL_SOURCES} )
FOREACH( tgt cserial cserial_static )
    TARGET_INCLUDE_DIRECTORIES( ${tgt} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}> )
ENDFOREACH()
# Same names as after find_package( cserial ), for use as a subproject
ADD_LIBRARY( cserial::cserial ALIAS cserial )
ADD_LIBRARY( cserial::cserial_static ALIAS cserial_static )

# Android packaging (Gradle/AGP) only picks up unversioned libcserial.so
IF( NOT ANDROID )
    set_target_properties(cserial PROPERTIES VERSION ${CSERIAL_VERSION_STRING} SOVERSION ${CSERIAL_VERSION_MAJOR} )
ENDIF()

#
# Install Information
#
SET(install_targets ${install_targets} cserial cserial_static )
INSTALL( TARGETS ${install_targets}
         EXPORT cserialTargets
         RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
         LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
         ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" )
INSTALL( FILES ${C_SERIAL_HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cserial" )

#
# CMake package: find_package( cserial ) provides cserial::cserial and
# cserial::cserial_static
#
SET( CSERIAL_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/cserial" )
INSTALL( EXPORT cserialTargets NAMESPACE cserial:: DESTINATION "${CSERIAL_CMAKE_DIR}" )
CONFIGURE_PACKAGE_CONFIG_FILE( cserialConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/cserialConfig.cmake"
    INSTALL_DESTINATION "${CSERIAL_CMAKE_DIR}" )
WRITE_BASIC_PACKAGE_VERSION_FILE( "${CMAKE_CURRENT_BINARY_DIR}/cserialConfigVersion.cmake"
    COMPATIBILITY SameMinorVersion )
INSTALL( FILES "${CMAKE_CURRENT_BINARY_DIR}/cserialConfig.cmake"
               "${CMAKE_CURRENT_BINARY_DIR}/cserialConfigVersion.cmake"
         DESTINATION "${CSERIAL_CMAKE_DIR}" )

IF( UNIX )
    #
    # pkg-config information
    #
    SET(PKG_CONFIG_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
    SET(PKG_CONFIG_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
    SET(PKG_CONFIG_LIBS "-L\${libdir} -lcserial")
    SET(PKG_CONFIG_LIBS_PRIVATE "-pthread")
    SET(PKG_CONFIG_CFLAGS "-I\${includedir}")

    CONFIGURE_FILE(
        "${CMAKE_CURRENT_SOURCE_DIR}/cserial.pc.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pc"
        @ONLY
    )
    INSTALL( FILES "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pc"
             DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig" )
ENDIF( UNIX )

#
# platform-specific defines
#
IF( WIN32 )
	ADD_DEFINITIONS( "-DCSERIAL_LIB" )
	target_compile_definitions( cserial_static PUBLIC CSERIAL_STATIC )
ENDIF( WIN32 )

#
# pthread dependency
#
IF( UNIX )
    target_link_libraries( cserial_static PUBLIC Threads::Threads )
    target_link_libraries( cserial PUBLIC Threads::Threads )
ENDIF( UNIX )

#
# Tests
#
IF( CSERIAL_BUILD_TESTS )
    enable_testing()
    add_subdirectory( tests )
ENDIF()

IF( CSERIAL_BUILD_EXAMPLES )
    add_subdirectory( examples )
ENDIF()
