enable_language( CXX )

set( CMAKE_CXX_STANDARD 14 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

include( FetchContent )
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set( gtest_force_shared_crt ON CACHE BOOL "" FORCE )
set( INSTALL_GTEST OFF CACHE BOOL "" FORCE )
set( BUILD_GMOCK OFF CACHE BOOL "" FORCE )
FetchContent_MakeAvailable( googletest )

include( GoogleTest )

set( CSERIAL_TEST_SOURCES
    test_api.cpp
    test_pty.cpp
    test_win_loopback.cpp )

add_executable( cserial_tests ${CSERIAL_TEST_SOURCES} )
target_link_libraries( cserial_tests PRIVATE cserial_static GTest::gtest_main )

gtest_discover_tests( cserial_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 30 )
