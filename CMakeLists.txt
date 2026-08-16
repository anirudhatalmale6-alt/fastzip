cmake_minimum_required(VERSION 3.13)
project(fastzip C)

set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/O2 /W3 /D_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-O2 -Wall)
endif()

# ---- libdeflate (static only, no bundled gzip program) ----------------
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_STATIC_LIB ON  CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP       OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/libdeflate EXCLUDE_FROM_ALL)

# ---- zlib (compiled straight from source, static) ---------------------
set(ZDIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/zlib)
add_library(zlibmini STATIC
  ${ZDIR}/adler32.c ${ZDIR}/compress.c ${ZDIR}/crc32.c  ${ZDIR}/deflate.c
  ${ZDIR}/infback.c ${ZDIR}/inffast.c  ${ZDIR}/inflate.c ${ZDIR}/inftrees.c
  ${ZDIR}/trees.c   ${ZDIR}/uncompr.c  ${ZDIR}/zutil.c)
target_include_directories(zlibmini PUBLIC ${ZDIR})
if(MSVC)
  target_compile_definitions(zlibmini PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- fastzip ----------------------------------------------------------
add_executable(fastzip src/fastzip.c)
target_include_directories(fastzip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libdeflate)
target_link_libraries(fastzip PRIVATE libdeflate_static zlibmini)

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(fastzip PRIVATE Threads::Threads)
endif()

if(MINGW)
  # one self-contained .exe, no runtime DLLs to ship alongside it
  target_link_options(fastzip PRIVATE -static -static-libgcc)
endif()
