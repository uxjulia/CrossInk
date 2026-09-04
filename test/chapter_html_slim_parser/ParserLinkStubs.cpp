#include <BidiUtils.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/PreviewBlockLocator.h>
#include <Epub/tables/CompactTableLayout.h>
#include <GfxRenderer.h>

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }

namespace BidiUtils {
bool startsWithRtl(const char*, int) { return false; }
int detectParagraphLevel(const char*, int fallbackLevel, int) { return fallbackLevel; }
bool computeVisualWordOrder(const std::vector<std::string>& words, bool, std::vector<uint16_t>& order) {
  order.resize(words.size());
  for (size_t index = 0; index < words.size(); ++index) order[index] = static_cast<uint16_t>(index);
  return true;
}
}  // namespace BidiUtils

TextBlock::TextBlock(const std::vector<std::string>&, const std::vector<int16_t>&,
                     const std::vector<EpdFontFamily::Style>&, const std::vector<uint8_t>&,
                     const std::vector<uint16_t>&, const std::vector<uint16_t>&, const std::vector<uint8_t>&,
                     const std::vector<bool>&, const BlockStyle& blockStyle, std::vector<std::string> rubyTexts)
    : blockStyle(blockStyle), rubyTexts(std::move(rubyTexts)) {}
bool TextBlock::hasRuby() const { return false; }

bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }

PreviewBlockLocator::PreviewBlockLocator(const char*, IsBlockTagFn) {}
PreviewBlockLocator::~PreviewBlockLocator() = default;

CompactTableLayout::CompactTableLayout(GfxRenderer& renderer, int, uint16_t, uint16_t, uint16_t, uint8_t,
                                       BlockStyle tableStyle)
    : renderer_(renderer), tableStyle_(tableStyle) {}
bool CompactTableLayout::beginRow() { return true; }
bool CompactTableLayout::beginCell(bool, uint8_t, uint32_t, const BlockStyle&) { return true; }
bool CompactTableLayout::appendWord(std::string_view, EpdFontFamily::Style, bool, bool, uint8_t) { return true; }
bool CompactTableLayout::endCell(const std::vector<std::pair<int, FootnoteEntry>>&) { return true; }
CompactTableLayout::RowResult CompactTableLayout::finishRow(TableFragmentRow&, std::vector<std::shared_ptr<TextBlock>>&,
                                                            std::vector<FootnoteEntry>&, uint32_t&) {
  return RowResult::Ok;
}

void PageLine::render(GfxRenderer&, int, int, int, bool) {}
bool PageLine::serialize(FsFile&) { return false; }
void PageHorizontalRule::render(GfxRenderer&, int, int, int, bool) {}
bool PageHorizontalRule::serialize(FsFile&) { return false; }
void PageTableFragment::render(GfxRenderer&, int, int, int, bool) {}
bool PageTableFragment::serialize(FsFile&) { return false; }
