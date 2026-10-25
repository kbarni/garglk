# Cross toolchain for Kindle firmware 5.16.3 and later (hard float).
# Expects koxtoolchain's kindlehf toolchain with kindle-sdk installed on top of it.

set(KINDLE_TC "arm-kindlehf-linux-gnueabihf")

if(NOT KINDLE_TC_DIR)
  if(DEFINED ENV{KINDLE_TC_DIR})
    set(KINDLE_TC_DIR "$ENV{KINDLE_TC_DIR}")
  else()
    set(KINDLE_TC_DIR "$ENV{HOME}/x-tools/${KINDLE_TC}")
  endif()
endif()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER "${KINDLE_TC_DIR}/bin/${KINDLE_TC}-gcc")
set(CMAKE_CXX_COMPILER "${KINDLE_TC_DIR}/bin/${KINDLE_TC}-g++")
set(CMAKE_SYSROOT "${KINDLE_TC_DIR}/${KINDLE_TC}/sysroot")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}" "${KINDLE_DEPS_PREFIX}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "${CMAKE_SYSROOT}/usr/lib/pkgconfig")

set(PNG_PNG_INCLUDE_DIR "${CMAKE_SYSROOT}/usr/include/libpng16" CACHE PATH "")
