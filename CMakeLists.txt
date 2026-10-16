cmake_minimum_required(VERSION 3.15)
project(batchssh2 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /utf-8)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    # libssh2 must use the same CRT as us; /MT keeps the exe self-contained.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded" CACHE STRING "MSVC runtime library")
else()
    add_compile_options(-Wall -Wextra)
    # strict -std=c11 hides POSIX decls (clock_gettime, strdup, pthread...)
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# Find libssh2, in order of preference:
#   1. CONFIG package (vcpkg, conan, install-prefix: Libssh2::libssh2)
#   2. pkg-config    (Linux distros: PkgConfig::LIBSSH2_PC)
#   3. manual search (LIBSSH2_ROOT cmake/env variable)
#   4. FetchContent  (download + build 1.11.1 from source, no preinstall)
# ---------------------------------------------------------------------------
set(BS2_SSH2_TARGET "")
set(BS2_SSH2_MANUAL OFF)

find_package(Libssh2 CONFIG QUIET)
if(Libssh2_FOUND AND TARGET Libssh2::libssh2)
    set(BS2_SSH2_TARGET Libssh2::libssh2)
    message(STATUS "libssh2: found via CMake config package")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSSH2_PC IMPORTED_TARGET libssh2)
        if(TARGET PkgConfig::LIBSSH2_PC)
            set(BS2_SSH2_TARGET PkgConfig::LIBSSH2_PC)
            message(STATUS "libssh2: found via pkg-config")
        endif()
    endif()
endif()

if(NOT BS2_SSH2_TARGET)
    find_path(LIBSSH2_INCLUDE_DIR libssh2.h
        HINTS "${LIBSSH2_ROOT}" ENV LIBSSH2_ROOT
        PATH_SUFFIXES include)
    find_library(LIBSSH2_LIBRARY NAMES ssh2 libssh2
        HINTS "${LIBSSH2_ROOT}" ENV LIBSSH2_ROOT
        PATH_SUFFIXES lib)
    if(LIBSSH2_INCLUDE_DIR AND LIBSSH2_LIBRARY)
        set(BS2_SSH2_MANUAL ON)
        message(STATUS "libssh2: found manually at ${LIBSSH2_LIBRARY}")
    endif()
endif()

if(NOT BS2_SSH2_TARGET AND NOT BS2_SSH2_MANUAL)
    message(STATUS "libssh2: no system copy found - fetching and building 1.11.1 from source")
    message(STATUS "libssh2: (set LIBSSH2_ROOT or install libssh2 to use a prebuilt one instead)")

    # libssh2's cmake_minimum_required(3.7) would otherwise ignore
    # CMAKE_MSVC_RUNTIME_LIBRARY (CMP0091) and build with a mismatch CRT.
    set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)

    set(BUILD_STATIC_LIBS     ON  CACHE BOOL   "" FORCE)
    set(BUILD_SHARED_LIBS     OFF CACHE BOOL   "" FORCE)
    set(BUILD_EXAMPLES        OFF CACHE BOOL   "" FORCE)
    set(BUILD_TESTING         OFF CACHE BOOL   "" FORCE)
    set(LIBSSH2_NO_DEPRECATED ON  CACHE BOOL   "" FORCE)
    if(WIN32)
        # WinCNG = zero external crypto deps on Windows
        set(CRYPTO_BACKEND      WinCNG CACHE STRING "" FORCE)
        set(ENABLE_ECDSA_WINCNG ON     CACHE BOOL   "" FORCE)
    else()
        # OpenSSL (requires libssl-dev / openssl-devel)
        set(CRYPTO_BACKEND OpenSSL CACHE STRING "" FORCE)
    endif()

    FetchContent_Declare(libssh2
        URL https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.gz
        URL_HASH SHA256=d9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(libssh2)
    set(BS2_SSH2_TARGET libssh2)
endif()

add_executable(batchssh2
    src/main.c
    src/util.c
    src/output.c
    src/pool.c
    src/ssh_worker.c
)

if(BS2_SSH2_TARGET)
    target_link_libraries(batchssh2 PRIVATE ${BS2_SSH2_TARGET})
else()
    target_include_directories(batchssh2 PRIVATE ${LIBSSH2_INCLUDE_DIR})
    target_link_libraries(batchssh2 PRIVATE ${LIBSSH2_LIBRARY})
endif()

if(WIN32)
    target_link_libraries(batchssh2 PRIVATE ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(batchssh2 PRIVATE Threads::Threads)
endif()
