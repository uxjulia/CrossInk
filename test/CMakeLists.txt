cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/MemoryBudget
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(preview_block_locator)
add_subdirectory(text_pool)
add_subdirectory(clip_word_store)
add_subdirectory(clipping_preview)
add_subdirectory(dict_layout)
add_subdirectory(reader_progress_save_debouncer)
add_subdirectory(manual_page_turn_queue)
add_subdirectory(page_count_estimator)
add_subdirectory(section_page_index)
add_subdirectory(section_persistence)
add_subdirectory(compact_table_layout)
add_subdirectory(quick_lock)
add_subdirectory(frontlight_schedule)
add_subdirectory(pinch_font_resize)
add_subdirectory(font_catalog_index)
add_subdirectory(reader_status_bar_tap_target)
add_subdirectory(two_finger_swipe)
add_subdirectory(option_popup)
add_subdirectory(sleep_wake_policy)
add_subdirectory(firmware_board_tag)
add_subdirectory(ota_staging_guard)
add_subdirectory(font_cache_manager)
add_subdirectory(reader_drawer_model)
add_subdirectory(stable_page_navigation)
add_subdirectory(touch_reader_preview)
add_subdirectory(dictionary_word_parts)
add_subdirectory(ligature_guard)
add_subdirectory(chapter_html_slim_parser)
add_subdirectory(input_release_guard)
add_subdirectory(release_suppression)
add_subdirectory(bitmap_resample)
add_subdirectory(pxc_v2)
add_subdirectory(memory_policy)
add_subdirectory(epub_grayscale)
add_subdirectory(ui_symbol_fallback)
add_subdirectory(absolute_grayscale)
add_subdirectory(buffered_file)
add_subdirectory(ota_boot_switch)
add_subdirectory(http_redirect_policy)
add_subdirectory(hal_file_stream)
add_subdirectory(koreader_xpath_resolver)
add_subdirectory(fs_helpers)
add_subdirectory(persistable_store_atomic)
add_subdirectory(book_reading_stats_atomic)
