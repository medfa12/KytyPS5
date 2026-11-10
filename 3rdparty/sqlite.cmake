# Local AppDb transactions use host SQLite, not guest credentials or guest SQL ABI.
# Official SQLite 3.50.4 amalgamation, public domain; loadable extensions disabled.
include(FetchContent)
FetchContent_Declare(kyty_sqlite_source
    URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
    URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032)
FetchContent_MakeAvailable(kyty_sqlite_source)
add_library(kyty_sqlite STATIC "${kyty_sqlite_source_SOURCE_DIR}/sqlite3.c")
target_include_directories(kyty_sqlite SYSTEM PUBLIC "${kyty_sqlite_source_SOURCE_DIR}")
target_compile_definitions(kyty_sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION=1)
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(kyty_sqlite PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
