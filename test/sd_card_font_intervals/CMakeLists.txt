add_executable(sd_card_font_intervals SdCardFontIntervalsTest.cpp
  ../../lib/EpdFont/SdCardFont.cpp ../../lib/EpdFont/SdCardFontManager.cpp
  ../../lib/EpdFont/EpdFontFamily.cpp ../../lib/EpdFont/EpdFont.cpp ../../lib/Utf8/Utf8.cpp)
target_compile_features(sd_card_font_intervals PRIVATE cxx_std_20)
target_include_directories(sd_card_font_intervals PRIVATE stubs ../../src ../../lib/EpdFont ../../lib/Memory
  ../../lib/MemoryBudget ../../lib/Utf8)
target_compile_options(sd_card_font_intervals PRIVATE -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(sd_card_font_intervals PRIVATE -fsanitize=address,undefined)
add_test(NAME sd_card_font_intervals COMMAND sd_card_font_intervals)
