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
add_subdirectory(unique_codepoint_set)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(preview_block_locator)
add_subdirectory(text_pool)
add_subdirectory(clip_word_store)
add_subdirectory(clipping_preview)
add_subdirectory(dict_layout)
add_subdirectory(reader_progress_save_debouncer)
add_subdirectory(reader_tilt_page_turn)
add_subdirectory(epub_link_return_state)
add_subdirectory(manual_page_turn_queue)
add_subdirectory(page_count_estimator)
add_subdirectory(book_page_estimate)
add_subdirectory(section_page_index)
add_subdirectory(section_persistence)
add_subdirectory(compact_table_layout)
add_subdirectory(quick_lock)
add_subdirectory(frontlight_schedule)
add_subdirectory(pinch_font_resize)
add_subdirectory(font_catalog_index)
add_subdirectory(sd_card_font_intervals)
add_subdirectory(reader_font_sizes)
add_subdirectory(reader_status_bar_tap_target)
add_subdirectory(reader_status_bar_config)
add_subdirectory(date_formatting)
add_subdirectory(two_finger_swipe)
add_subdirectory(edge_slide)
add_subdirectory(option_popup)
add_subdirectory(sleep_wake_policy)
add_subdirectory(sleep_image_deck)
add_subdirectory(firmware_board_tag)
add_subdirectory(ota_staging_guard)
add_subdirectory(font_cache_manager)
add_subdirectory(reader_drawer_model)
add_subdirectory(stable_page_navigation)
add_subdirectory(xtc_navigation)
add_subdirectory(touch_reader_preview)
add_subdirectory(dictionary_word_parts)
add_subdirectory(ligature_guard)
add_subdirectory(chapter_html_slim_parser)
add_subdirectory(korean_line_breaking)
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
add_subdirectory(http_downloader)
add_subdirectory(hal_file_stream)
add_subdirectory(koreader_xpath_resolver)
add_subdirectory(fs_helpers)
add_subdirectory(persistable_store_atomic)
add_subdirectory(book_reading_stats_atomic)
add_subdirectory(opds_parser)
add_subdirectory(library_text)
add_subdirectory(library_format)
add_subdirectory(library_index_file)
add_subdirectory(content_opf_parser)
add_subdirectory(adobe_font_obfuscation)
add_subdirectory(library_builder)
add_subdirectory(library_input_buffer)
add_subdirectory(library_recent_order)
add_subdirectory(scalable_fonts)
add_subdirectory(glyph_bitmap)
add_subdirectory(daily_reading_stats)
add_subdirectory(sync_files)
add_subdirectory(sync_pending)
add_subdirectory(sync_ids)
add_subdirectory(sync_parked)
add_subdirectory(opds_server_store)
add_subdirectory(support_info)
add_subdirectory(log_retention)
add_subdirectory(screen_calibration)
add_subdirectory(keyboard_tips)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME WebDavGetRegression
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/test_webdav_get.py)
add_test(NAME ImageProbeFramebufferRegression
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/test_image_probe_framebuffer.py)
add_subdirectory(asset_cache_storage)
add_subdirectory(language_cache)
add_subdirectory(hyphenation_pack)
add_test(NAME UploadFailureRegression
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/scripts/test_upload_cancellation.py)
add_test(NAME StorageAndBrowsingRegression
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/scripts/test_storage_and_browsing_ports.py)
add_test(NAME OptimizerCacheSeedRegression
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/scripts/test_optimizer_cache_seed.py)
add_test(NAME WifiAutoConnectRegression
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/scripts/test_wifi_auto_connect.py)
add_subdirectory(book_loading_feedback)
add_subdirectory(otf_fonts)

add_subdirectory(asset_catalog)
add_subdirectory(asset_download)
add_subdirectory(asset_companion_index)
add_subdirectory(asset_companion_store)

add_subdirectory(http_header)

add_subdirectory(swipe_delete)
add_subdirectory(touch_action_buttons)

add_test(NAME LanguageAssetPublicationRegression
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/scripts/test_language_asset_publication.py)

add_subdirectory(reading_session_journal)

add_subdirectory(reading_session_checkpoint)

add_subdirectory(protected_paths)
add_subdirectory(plugin_manifest_classification)
add_subdirectory(plugin_catalog_paging)
add_subdirectory(plugin_http_policy)
add_subdirectory(plugin_bundle)

add_subdirectory(plugin_picker_layout)

add_subdirectory(plugin_details)

add_subdirectory(reader_session)
add_subdirectory(reader_plugin_events)

add_subdirectory(hal_power_manager)
add_subdirectory(input_wake)
add_subdirectory(button_navigation)
add_subdirectory(button_capture)

add_test(NAME ReaderRestartStatsRegression
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/test_reader_restart_stats.py)

add_test(NAME ReaderStatsLatencyRegression
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/test_reader_stats_latency.py)

add_test(NAME BookmarkListMemoryRegression
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/scripts/test_bookmark_list_memory.py)

add_subdirectory(bookmark_store)

add_test(NAME UpstreamPortRegressions
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/scripts/test_upstream_ports.py)

add_subdirectory(gif_support)
