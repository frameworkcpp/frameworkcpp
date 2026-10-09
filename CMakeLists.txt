# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.23)

project(frameworkcpp
    VERSION 1.0.0
    DESCRIPTION "A C++ framework"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FRAMEWORKCPP_BUILD_TESTING "Build tests" ON)

# core framework library (header-only). Sub-modules such as `env` live under
# include/frameworkcpp/<module>/ and share the same target.
add_library(frameworkcpp INTERFACE)
add_library(frameworkcpp::frameworkcpp ALIAS frameworkcpp)

target_include_directories(frameworkcpp INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_definitions(frameworkcpp INTERFACE
    FRAMEWORKCPP_VERSION_STRING="${PROJECT_VERSION}"
)

# --- Install -----------------------------------------------------------------
include(GNUInstallDirs)

install(DIRECTORY include/frameworkcpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/frameworkcpp)

install(TARGETS frameworkcpp EXPORT frameworkcppTargets)
install(EXPORT frameworkcppTargets
    FILE frameworkcppTargets.cmake
    NAMESPACE frameworkcpp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/frameworkcpp
)

include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/frameworkcppConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/frameworkcppConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/frameworkcpp
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/frameworkcppConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/frameworkcppConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/frameworkcppConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/frameworkcpp
)

# --- Tests -------------------------------------------------------------------
if(FRAMEWORKCPP_BUILD_TESTING)
    enable_testing()

    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(frameworkcpp_version_test tests/version_test.cpp)
    target_link_libraries(frameworkcpp_version_test PRIVATE frameworkcpp GTest::gtest_main)
    target_compile_features(frameworkcpp_version_test PRIVATE cxx_std_20)

    include(GoogleTest)
    gtest_discover_tests(frameworkcpp_version_test)
endif()

# --- CPack -------------------------------------------------------------------
# Package architecture mapping so output is reproducible and multi-arch across
# build host machines (amd64 x86_64, arm64 aarch64).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(_frameworkcpp_deb_arch "amd64")
    set(_frameworkcpp_rpm_arch "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
    set(_frameworkcpp_deb_arch "arm64")
    set(_frameworkcpp_rpm_arch "aarch64")
else()
    set(_frameworkcpp_deb_arch "${CMAKE_SYSTEM_PROCESSOR}")
    set(_frameworkcpp_rpm_arch "${CMAKE_SYSTEM_PROCESSOR}")
endif()

# Generators that need no extra system tooling (tar/zip/shell).
set(CPACK_GENERATOR "STGZ;TGZ;TZ;ZIP;7Z")

# DEB needs dpkg-deb, RPM needs rpmbuild: only enable when present so the build
# works out of the box and CI installs the missing tool to get full coverage.
find_program(_frameworkcpp_dpkg_deb NAMES dpkg-deb)
find_program(_frameworkcpp_rpmbuild NAMES rpmbuild)
find_program(_frameworkcpp_pkg_create NAMES pkg-create)
if(_frameworkcpp_dpkg_deb)
    list(APPEND CPACK_GENERATOR "DEB")
endif()
if(_frameworkcpp_rpmbuild)
    list(APPEND CPACK_GENERATOR "RPM")
endif()
if(_frameworkcpp_pkg_create)
    list(APPEND CPACK_GENERATOR "FreeBSD")
endif()

set(CPACK_PACKAGE_NAME "${PROJECT_NAME}")
set(CPACK_PACKAGE_VENDOR "Ian Torres")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A C++ framework")
set(CPACK_PACKAGE_DESCRIPTION "frameworkcpp is a multipurpose C++ framework; sub-modules such as env live under include/frameworkcpp/<module>.")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_LICENSE "AGPL-3.0-only")
set(CPACK_PACKAGE_CONTACT "Ian Torres")
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${_frameworkcpp_rpm_arch}")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Ian Torres")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_frameworkcpp_deb_arch}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)

set(CPACK_RPM_PACKAGE_LICENSE "AGPL-3.0-only")
set(CPACK_RPM_PACKAGE_GROUP "Development/Libraries")
set(CPACK_RPM_PACKAGE_ARCHITECTURE "${_frameworkcpp_rpm_arch}")

set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

include(CPack)