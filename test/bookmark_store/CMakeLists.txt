add_executable(BookmarkStoreTest BookmarkStoreTest.cpp
  ${REPO_ROOT}/src/BookmarkStore.cpp ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(BookmarkStoreTest PRIVATE stubs ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/uzlib/src)
target_compile_options(BookmarkStoreTest PRIVATE -UNDEBUG -fno-exceptions)
add_test(NAME BookmarkStoreLowMemoryRegression COMMAND BookmarkStoreTest)
