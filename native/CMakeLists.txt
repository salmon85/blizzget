cmake_minimum_required(VERSION 3.16)
project(blizzget_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
pkg_check_modules(JSONGLIB REQUIRED IMPORTED_TARGET json-glib-1.0)

find_library(SYSTEM_CURL_LIBRARY NAMES libcurl.so.4 PATHS /usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH REQUIRED)
find_library(LZ4_LIBRARY NAMES lz4 REQUIRED)
find_library(ZLIB_LIBRARY NAMES z REQUIRED)

add_executable(blizzget
  app.cpp
  cache.cpp
  downloader.cpp
  version_sort.cpp
  ../src/base/checksum.cpp
  ../src/base/common.cpp
  ../src/base/error.cpp
  ../src/base/file.cpp
  ../src/base/http.cpp
  ../src/base/path.cpp
  ../src/base/string.cpp
  ../src/ngdp.cpp
)

target_include_directories(blizzget PRIVATE . ../src)
target_link_libraries(blizzget PRIVATE PkgConfig::GTK3 PkgConfig::JSONGLIB)
target_link_libraries(blizzget PRIVATE ${SYSTEM_CURL_LIBRARY} ${LZ4_LIBRARY} ${ZLIB_LIBRARY})
target_link_options(blizzget PRIVATE "-Wl,-rpath,/usr/lib/x86_64-linux-gnu" "-Wl,-rpath,\$ORIGIN/../lib")

install(TARGETS blizzget RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
  add_executable(index_hash_test
    tests/index_hash_test.cpp
    ../src/base/checksum.cpp
    ../src/base/common.cpp
    ../src/base/error.cpp
    ../src/base/file.cpp
    ../src/base/http.cpp
    ../src/base/path.cpp
    ../src/base/string.cpp
    ../src/ngdp.cpp
  )
  target_include_directories(index_hash_test PRIVATE . ../src)
  target_link_libraries(index_hash_test PRIVATE ${SYSTEM_CURL_LIBRARY} ${LZ4_LIBRARY} ${ZLIB_LIBRARY})
  add_test(NAME index_hash_canonical COMMAND index_hash_test)

  add_executable(index_layout_test
    tests/index_layout_test.cpp
    ../src/base/checksum.cpp
    ../src/base/common.cpp
    ../src/base/error.cpp
    ../src/base/file.cpp
    ../src/base/http.cpp
    ../src/base/path.cpp
    ../src/base/string.cpp
    ../src/ngdp.cpp
  )
  target_include_directories(index_layout_test PRIVATE . ../src)
  target_link_libraries(index_layout_test PRIVATE ${SYSTEM_CURL_LIBRARY} ${LZ4_LIBRARY} ${ZLIB_LIBRARY})
  add_test(NAME index_layout_16_buckets COMMAND index_layout_test)

  add_executable(hashlittle2_test
    tests/hashlittle2_test.cpp
    ../src/base/checksum.cpp
  )
  target_include_directories(hashlittle2_test PRIVATE . ../src)
  add_test(NAME hashlittle2_vectors COMMAND hashlittle2_test)

  add_executable(file_io_test
    tests/file_io_test.cpp
    ../src/base/checksum.cpp
    ../src/base/common.cpp
    ../src/base/error.cpp
    ../src/base/file.cpp
    ../src/base/path.cpp
    ../src/base/string.cpp
  )
  target_include_directories(file_io_test PRIVATE . ../src)
  target_link_libraries(file_io_test PRIVATE ${ZLIB_LIBRARY})
  add_test(NAME file_io_failures COMMAND file_io_test)

  add_executable(cache_clear_test
    tests/cache_clear_test.cpp
    cache.cpp
  )
  target_include_directories(cache_clear_test PRIVATE .)
  add_test(NAME cache_clear_preserves_unrelated COMMAND cache_clear_test)

  add_executable(version_sort_test
    tests/version_sort_test.cpp
    version_sort.cpp
  )
  target_include_directories(version_sort_test PRIVATE .)
  add_test(NAME versions_newest_first COMMAND version_sort_test)

  add_executable(version_cache_sort_test
    tests/version_cache_sort_test.cpp
    downloader.cpp
    version_sort.cpp
    ../src/base/checksum.cpp
    ../src/base/common.cpp
    ../src/base/error.cpp
    ../src/base/file.cpp
    ../src/base/http.cpp
    ../src/base/path.cpp
    ../src/base/string.cpp
    ../src/ngdp.cpp
  )
  target_include_directories(version_cache_sort_test PRIVATE . ../src)
  target_link_libraries(version_cache_sort_test PRIVATE PkgConfig::GTK3 PkgConfig::JSONGLIB)
  target_link_libraries(version_cache_sort_test PRIVATE ${SYSTEM_CURL_LIBRARY} ${LZ4_LIBRARY} ${ZLIB_LIBRARY})
  add_test(NAME cached_versions_newest_first COMMAND version_cache_sort_test)
endif()
