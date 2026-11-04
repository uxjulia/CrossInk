#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>

// Matches order of PARAGRAPH_ALIGNMENT in CrossPointSettings
enum class CssTextAlign : uint8_t { Justify = 0, Left = 1, Center = 2, Right = 3, None = 4 };
enum class CssUnit : uint8_t { Pixels = 0, Em = 1, Rem = 2, Points = 3, Percent = 4 };
enum class CssTextDirection : uint8_t { Ltr = 0, Rtl = 1 };

// Represents a CSS length value with its unit, allowing deferred resolution to pixels
struct CssLength {
  float value = 0.0f;
  CssUnit unit = CssUnit::Pixels;

  CssLength() = default;
  CssLength(const float v, const CssUnit u) : value(v), unit(u) {}

  // Convenience constructor for pixel values (most common case)
  explicit CssLength(const float pixels) : value(pixels) {}

  // Returns true if this length can be resolved to pixels with the given context.
  // Percentage units require a non-zero containerWidth to resolve.
  [[nodiscard]] bool isResolvable(const float containerWidth = 0) const {
    return unit != CssUnit::Percent || containerWidth > 0;
  }

  // Resolve to pixels given the current em size (font line height)
  // containerWidth is needed for percentage units (e.g. viewport width)
  [[nodiscard]] float toPixels(const float emSize, const float containerWidth = 0) const {
    switch (unit) {
      case CssUnit::Em:
      case CssUnit::Rem:
        return value * emSize;
      case CssUnit::Points:
        return value * 1.33f;  // Approximate pt to px conversion
      case CssUnit::Percent:
        return value * containerWidth / 100.0f;
      default:
        return value;
    }
  }

  // Resolve to int16_t pixels (for BlockStyle fields)
  [[nodiscard]] int16_t toPixelsInt16(const float emSize, const float containerWidth = 0) const {
    return static_cast<int16_t>(toPixels(emSize, containerWidth));
  }
};

// Font style options matching CSS font-style property
enum class CssFontStyle : uint8_t { Normal = 0, Italic = 1 };

// Font weight options - CSS supports 100-900, we simplify to normal/bold
enum class CssFontWeight : uint8_t { Normal = 0, Bold = 1 };

// Font variant caps options matching the small subset CrossInk renders.
enum class CssFontVariantCaps : uint8_t { Normal = 0, SmallCaps = 1 };

// Text decoration options. Values are bit flags so CSS can combine multiple line decorations.
enum class CssTextDecoration : uint8_t { None = 0, Underline = 1, LineThrough = 2 };

constexpr CssTextDecoration operator|(const CssTextDecoration a, const CssTextDecoration b) {
  return static_cast<CssTextDecoration>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

constexpr CssTextDecoration operator&(const CssTextDecoration a, const CssTextDecoration b) {
  return static_cast<CssTextDecoration>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

constexpr uint8_t CSS_TEXT_DECORATION_MASK =
    static_cast<uint8_t>(CssTextDecoration::Underline) | static_cast<uint8_t>(CssTextDecoration::LineThrough);

// Keep inline distinct so replaced elements can participate in text layout.
enum class CssDisplay : uint8_t { Block = 0, None = 1, Inline = 2 };

// Vertical alignment options for inline elements (e.g. superscript/subscript)
enum class CssVerticalAlign : uint8_t { Baseline = 0, Super = 1, Sub = 2 };

// List markers supported by the EPUB renderer.
enum class CssListStyleType : uint8_t { Disc = 0, None = 1 };

enum class CssBorderStyle : uint8_t { None, Solid, Double, Dotted, Dashed };
struct CssBorderSide {
  uint8_t width = 3;
  CssBorderStyle style = CssBorderStyle::None;
  bool visible() const { return width && style != CssBorderStyle::None; }
};

// Bitmask for tracking which properties have been explicitly set
struct CssPropertyFlags {
  uint32_t textAlign : 1;
  uint32_t fontStyle : 1;
  uint32_t fontWeight : 1;
  uint32_t textDecoration : 1;
  uint32_t textIndent : 1;
  uint32_t marginTop : 1;
  uint32_t marginBottom : 1;
  uint32_t marginLeft : 1;
  uint32_t marginRight : 1;
  uint32_t paddingTop : 1;
  uint32_t paddingBottom : 1;
  uint32_t paddingLeft : 1;
  uint32_t paddingRight : 1;
  uint32_t imageHeight : 1;
  uint32_t imageWidth : 1;
  uint32_t display : 1;
  uint32_t backgroundBlack : 1;
  uint32_t verticalAlign : 1;
  uint32_t direction : 1;
  uint32_t pageBreakBefore : 1;
  uint32_t pageBreakAfter : 1;
  uint32_t fontVariantCaps : 1;
  uint32_t listStyleType : 1;
  uint32_t fontSize : 1;
  uint32_t border : 1;
  uint32_t whiteSpace : 1;
  uint32_t shaded : 1;
  uint32_t floatLeft : 1;
  uint32_t initialLetter : 1;
  uint32_t fontFamily : 1;

  CssPropertyFlags()
      : textAlign(0),
        fontStyle(0),
        fontWeight(0),
        textDecoration(0),
        textIndent(0),
        marginTop(0),
        marginBottom(0),
        marginLeft(0),
        marginRight(0),
        paddingTop(0),
        paddingBottom(0),
        paddingLeft(0),
        paddingRight(0),
        imageHeight(0),
        imageWidth(0),
        display(0),
        backgroundBlack(0),
        verticalAlign(0),
        direction(0),
        pageBreakBefore(0),
        pageBreakAfter(0),
        fontVariantCaps(0),
        listStyleType(0),
        fontSize(0),
        border(0),
        whiteSpace(0),
        shaded(0),
        floatLeft(0),
        initialLetter(0),
        fontFamily(0) {}

  [[nodiscard]] bool anySet() const {
    return fontFamily || shaded || floatLeft || initialLetter || whiteSpace || border || fontSize || textAlign ||
           fontStyle || fontWeight || textDecoration || textIndent || marginTop || marginBottom || marginLeft ||
           marginRight || paddingTop || paddingBottom || paddingLeft || paddingRight || imageHeight || imageWidth ||
           display || backgroundBlack || verticalAlign || direction || pageBreakBefore || pageBreakAfter ||
           fontVariantCaps || listStyleType;
  }

  void clearAll() {
    textAlign = fontStyle = fontWeight = textDecoration = textIndent = 0;
    marginTop = marginBottom = marginLeft = marginRight = 0;
    paddingTop = paddingBottom = paddingLeft = paddingRight = 0;
    imageHeight = imageWidth = display = backgroundBlack = verticalAlign = direction = 0;
    pageBreakBefore = pageBreakAfter = fontVariantCaps = listStyleType = fontSize = border = whiteSpace = shaded =
        floatLeft = initialLetter = fontFamily = 0;
  }
};

static_assert(sizeof(CssPropertyFlags) <= sizeof(uint32_t),
              "CssPropertyFlags exceeds 32 bits; update cache read/write in CssParser.cpp");

// Represents a collection of CSS style properties
// Only stores properties relevant to e-ink text rendering
// Length values are stored as CssLength (value + unit) for deferred resolution
struct CssStyle {
  uint32_t fontFamily = 0;
  CssTextAlign textAlign = CssTextAlign::Left;
  CssFontStyle fontStyle = CssFontStyle::Normal;
  CssFontWeight fontWeight = CssFontWeight::Normal;
  CssTextDecoration textDecoration = CssTextDecoration::None;
  CssTextDirection direction = CssTextDirection::Ltr;
  CssFontVariantCaps fontVariantCaps = CssFontVariantCaps::Normal;

  CssLength fontSize;       // Resolved against parent/root font, not the box width
  CssLength textIndent;     // First-line indent (deferred resolution)
  CssLength marginTop;      // Vertical spacing before block
  CssLength marginBottom;   // Vertical spacing after block
  CssLength marginLeft;     // Horizontal spacing left of block
  CssLength marginRight;    // Horizontal spacing right of block
  CssLength paddingTop;     // Padding before
  CssLength paddingBottom;  // Padding after
  CssLength paddingLeft;    // Padding left
  CssLength paddingRight;   // Padding right
  CssLength imageHeight;    // Height for img (e.g. 2em) – width derived from aspect ratio when only height set
  CssLength imageWidth;     // Width for img when both or only width set
  CssDisplay display = CssDisplay::Block;
  CssBorderSide borders[4];  // top, right, bottom, left; shares the existing defined edge masks
  bool shaded = false;
  bool floatLeft = false;
  uint8_t initialLetter = 0;
  bool hasVisibleBorder() const {
    return std::any_of(std::begin(borders), std::end(borders),
                       [](const CssBorderSide& side) { return side.visible(); });
  }
  bool preserveWhitespace = false;
  bool backgroundBlack = false;                                 // Simple black inline/block background support
  CssVerticalAlign verticalAlign = CssVerticalAlign::Baseline;  // vertical-align (super/sub positioning)
  bool pageBreakBefore = false;
  bool pageBreakAfter = false;
  CssListStyleType listStyleType = CssListStyleType::Disc;

  // Four edge bits (top/right/bottom/left). Track style and width separately so
  // later declarations can restore one without losing suppression by the other.
  uint8_t borderStyleSuppressed = 0;
  uint8_t borderWidthSuppressed = 0;
  uint8_t borderStyleDefined = 0;
  uint8_t borderWidthDefined = 0;

  [[nodiscard]] bool suppressesHorizontalRule() const {
    return (borderStyleSuppressed | borderWidthSuppressed) == 0x0F;
  }

  CssPropertyFlags defined;  // Tracks which properties were explicitly set

  // Apply properties from another style, only overwriting if the other style
  // has that property explicitly defined
  void applyOver(const CssStyle& base) {
    if (base.defined.fontFamily) {
      fontFamily = base.fontFamily;
      defined.fontFamily = 1;
    }
    if (base.defined.shaded) {
      shaded = base.shaded;
      defined.shaded = 1;
    }
    if (base.defined.floatLeft) {
      floatLeft = base.floatLeft;
      defined.floatLeft = 1;
    }
    if (base.defined.initialLetter) {
      initialLetter = base.initialLetter;
      defined.initialLetter = 1;
    }
    if (base.hasWhiteSpace()) {
      preserveWhitespace = base.preserveWhitespace;
      defined.whiteSpace = 1;
    }
    if (base.defined.border) {
      for (size_t edge = 0; edge < 4; ++edge) {
        if (base.borderStyleDefined & (1u << edge)) borders[edge].style = base.borders[edge].style;
        if (base.borderWidthDefined & (1u << edge)) borders[edge].width = base.borders[edge].width;
      }
      borderStyleSuppressed = (borderStyleSuppressed & ~base.borderStyleDefined) | base.borderStyleSuppressed;
      borderWidthSuppressed = (borderWidthSuppressed & ~base.borderWidthDefined) | base.borderWidthSuppressed;
      borderStyleDefined |= base.borderStyleDefined;
      borderWidthDefined |= base.borderWidthDefined;
      defined.border = 1;
    }
    if (base.hasFontSize()) {
      fontSize = base.fontSize;
      defined.fontSize = 1;
    }
    if (base.hasTextAlign()) {
      textAlign = base.textAlign;
      defined.textAlign = 1;
    }
    if (base.hasFontStyle()) {
      fontStyle = base.fontStyle;
      defined.fontStyle = 1;
    }
    if (base.hasFontWeight()) {
      fontWeight = base.fontWeight;
      defined.fontWeight = 1;
    }
    if (base.hasTextDecoration()) {
      textDecoration = base.textDecoration;
      defined.textDecoration = 1;
    }
    if (base.hasTextIndent()) {
      textIndent = base.textIndent;
      defined.textIndent = 1;
    }
    if (base.hasMarginTop()) {
      marginTop = base.marginTop;
      defined.marginTop = 1;
    }
    if (base.hasMarginBottom()) {
      marginBottom = base.marginBottom;
      defined.marginBottom = 1;
    }
    if (base.hasMarginLeft()) {
      marginLeft = base.marginLeft;
      defined.marginLeft = 1;
    }
    if (base.hasMarginRight()) {
      marginRight = base.marginRight;
      defined.marginRight = 1;
    }
    if (base.hasPaddingTop()) {
      paddingTop = base.paddingTop;
      defined.paddingTop = 1;
    }
    if (base.hasPaddingBottom()) {
      paddingBottom = base.paddingBottom;
      defined.paddingBottom = 1;
    }
    if (base.hasPaddingLeft()) {
      paddingLeft = base.paddingLeft;
      defined.paddingLeft = 1;
    }
    if (base.hasPaddingRight()) {
      paddingRight = base.paddingRight;
      defined.paddingRight = 1;
    }
    if (base.hasImageHeight()) {
      imageHeight = base.imageHeight;
      defined.imageHeight = 1;
    }
    if (base.hasImageWidth()) {
      imageWidth = base.imageWidth;
      defined.imageWidth = 1;
    }
    if (base.hasDisplay()) {
      display = base.display;
      defined.display = 1;
    }
    if (base.hasBackgroundBlack()) {
      backgroundBlack = base.backgroundBlack;
      defined.backgroundBlack = 1;
    }
    if (base.hasDirection()) {
      direction = base.direction;
      defined.direction = 1;
    }
    if (base.hasVerticalAlign()) {
      verticalAlign = base.verticalAlign;
      defined.verticalAlign = 1;
    }
    if (base.hasPageBreakBefore()) {
      pageBreakBefore = base.pageBreakBefore;
      defined.pageBreakBefore = 1;
    }
    if (base.hasPageBreakAfter()) {
      pageBreakAfter = base.pageBreakAfter;
      defined.pageBreakAfter = 1;
    }
    if (base.hasFontVariantCaps()) {
      fontVariantCaps = base.fontVariantCaps;
      defined.fontVariantCaps = 1;
    }
    if (base.hasListStyleType()) {
      listStyleType = base.listStyleType;
      defined.listStyleType = 1;
    }
  }

  [[nodiscard]] bool hasWhiteSpace() const { return defined.whiteSpace; }
  [[nodiscard]] bool hasFontSize() const { return defined.fontSize; }
  [[nodiscard]] bool hasTextAlign() const { return defined.textAlign; }
  [[nodiscard]] bool hasFontStyle() const { return defined.fontStyle; }
  [[nodiscard]] bool hasFontWeight() const { return defined.fontWeight; }
  [[nodiscard]] bool hasTextDecoration() const { return defined.textDecoration; }
  [[nodiscard]] bool hasTextIndent() const { return defined.textIndent; }
  [[nodiscard]] bool hasMarginTop() const { return defined.marginTop; }
  [[nodiscard]] bool hasMarginBottom() const { return defined.marginBottom; }
  [[nodiscard]] bool hasMarginLeft() const { return defined.marginLeft; }
  [[nodiscard]] bool hasMarginRight() const { return defined.marginRight; }
  [[nodiscard]] bool hasPaddingTop() const { return defined.paddingTop; }
  [[nodiscard]] bool hasPaddingBottom() const { return defined.paddingBottom; }
  [[nodiscard]] bool hasPaddingLeft() const { return defined.paddingLeft; }
  [[nodiscard]] bool hasPaddingRight() const { return defined.paddingRight; }
  [[nodiscard]] bool hasImageHeight() const { return defined.imageHeight; }
  [[nodiscard]] bool hasImageWidth() const { return defined.imageWidth; }
  [[nodiscard]] bool hasDisplay() const { return defined.display; }
  [[nodiscard]] bool hasBackgroundBlack() const { return defined.backgroundBlack; }
  [[nodiscard]] bool hasVerticalAlign() const { return defined.verticalAlign; }
  [[nodiscard]] bool hasDirection() const { return defined.direction; }
  [[nodiscard]] bool hasPageBreakBefore() const { return defined.pageBreakBefore; }
  [[nodiscard]] bool hasPageBreakAfter() const { return defined.pageBreakAfter; }
  [[nodiscard]] bool hasFontVariantCaps() const { return defined.fontVariantCaps; }
  [[nodiscard]] bool hasListStyleType() const { return defined.listStyleType; }

  void reset() { *this = CssStyle{}; }
};
