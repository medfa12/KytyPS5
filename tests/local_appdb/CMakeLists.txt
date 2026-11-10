cmake_minimum_required(VERSION 3.24)
project(LocalAppDbValidation LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${ROOT}/3rdparty/sqlite.cmake")
add_executable(local_appdb_tests "${ROOT}/tests/LocalAppDbTests.cpp" "${ROOT}/tests/LocalDbRecoveryTests.cpp" "${ROOT}/tests/LocalHomeQueryTests.cpp"
    "${ROOT}/tests/LocalSystemCatalogCommitTests.cpp"
    "${ROOT}/tests/LocalDbPreparationTests.cpp" "${ROOT}/tests/LocalNativeCatalogTests.cpp"
    "${ROOT}/tests/LocalCoreAppInfoTests.cpp" "${ROOT}/src/loader/runtimeLocalCoreAppInfo.cpp"
    "${ROOT}/src/loader/runtimeLocalDbPreparation.cpp" "${ROOT}/src/loader/runtimeLocalNativeCatalog.cpp"
    "${ROOT}/src/loader/runtimeLocalAppDb.cpp" "${ROOT}/src/loader/runtimeLocalSystemCatalog.cpp"
    "${ROOT}/src/loader/runtimeLocalDbRecovery.cpp" "${ROOT}/src/loader/runtimeLocalHomeQuery.cpp")
target_include_directories(local_appdb_tests PRIVATE "${ROOT}/src")
target_link_libraries(local_appdb_tests PRIVATE kyty_sqlite)
add_executable(local_catalog_comparison "${ROOT}/tests/LocalCatalogComparison.cpp"
    "${ROOT}/src/loader/runtimeLocalSystemCatalog.cpp"
    "${ROOT}/src/loader/runtimeLocalHomeQuery.cpp"
    "${ROOT}/src/loader/runtimeLocalNativeCatalog.cpp")
target_include_directories(local_catalog_comparison PRIVATE "${ROOT}/src")
target_link_libraries(local_catalog_comparison PRIVATE kyty_sqlite)
add_executable(local_storage_recover_probe "${ROOT}/tests/LocalStorageRecoverProbe.cpp"
    "${ROOT}/src/libs/localUserStorage.cpp")
target_include_directories(local_storage_recover_probe PRIVATE "${ROOT}/src")
enable_testing()
add_test(NAME local_appdb_tests COMMAND local_appdb_tests)
