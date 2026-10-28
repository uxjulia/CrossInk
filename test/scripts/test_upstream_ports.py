#!/usr/bin/env python3
"""Run production masking, URL/connection and SD advance paths with bounded fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]

def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

keyboard = (ROOT / 'src/activities/util/KeyboardEntryActivity.cpp').read_text()
http = (ROOT / 'src/util/PluginHttp.cpp').read_text()
gfx = (ROOT / 'lib/GfxRenderer/GfxRenderer.cpp').read_text()
source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <Utf8.h>
#include <EpdFontData.h>
#define LOG_ERR(...) ((void)0)
class KeyboardEntryActivity {
 public:
  enum class InputType { Text, Password, Url };
  std::string text;
  InputType inputType = InputType::Password;
  bool passwordVisible = false, cursorMode = false;
  size_t cursorPos = 0;
  static size_t utf8Prev(const std::string&, size_t);
  std::string displayTextForCurrentState() const;
};
'''
source += function(keyboard, 'size_t KeyboardEntryActivity::utf8Prev(')
source += function(keyboard, 'std::string KeyboardEntryActivity::displayTextForCurrentState(')
source += '\nnamespace pluginhttp {\n'
source += 'using Headers = std::vector<std::pair<std::string, std::string>>;\nbool validHeaders(const Headers&) { return true; }\n'
source += function(http, 'std::string urlEncodeQuery(')
source += function(http, 'std::string escapeUrl(')
source += r'''
}
namespace freeink {
struct SecureHttpClient {
  std::vector<std::string> calls;
  bool connected = false, beginOk = true, connectOk = true;
  void setUserAgent(const char*) {} void setInsecure() {} void setTimeout(uint32_t) {}
  bool begin(const std::string&) { calls.push_back("begin"); return beginOk; }
  bool connect(const std::function<bool()>& abort) {
    calls.push_back("connect");
    connected = connectOk && !(abort && abort());
    return connected;
  }
  void addHeader(const std::string&, const std::string&) { assert(connected); calls.push_back("header"); }
};
}
'''
source += function(http, 'freeink::SecureHttpClient* openClient(')
source += r'''
namespace BidiUtils { bool isTransparentMark(uint32_t cp) { return cp == 0x05b0; } }
struct EpdFontFamily {
  enum Style { REGULAR=0, SMALL_CAPS=4, SUP=8, SUB=16 };
  int kern = 0;
  int getKerning(uint32_t, uint32_t, Style) const { return kern; }
};
struct SdCardFont {
  int advance = 17;
  bool hasAdvanceTable() const { return true; }
};
uint8_t resolveSdCardStyle(const SdCardFont&, EpdFontFamily::Style) { return 0; }
int32_t resolveSdCardAdvanceFP(const SdCardFont& font, const EpdFontFamily&, uint32_t, EpdFontFamily::Style, uint8_t) {
  return font.advance;
}
bool isSmallCapsLowercase(EpdFontFamily::Style style, uint32_t cp) { return (style & EpdFontFamily::SMALL_CAPS) && cp >= 'a' && cp <= 'z'; }
uint32_t smallCapsUppercaseCodepoint(uint32_t cp) { return cp - 'a' + 'A'; }
'''
for signature in ['bool characterSpacingBoundary(', 'int characterSpacingPixels(', 'int32_t halfAdvanceFP(', 'int32_t smallCapsAdvanceFP(']:
    source += function(gfx, signature)
start = gfx.index('  // Advance table fast-path for SD card fonts during layout.', gfx.index('int GfxRenderer::getTextAdvanceX('))
end = gfx.index('\n  const auto fontIt = fontMap.find(resolvedFontId);', start + 300)
source += r'''
int measure(const char* text, int advance = 17, EpdFontFamily::Style style = EpdFontFamily::REGULAR,
            int8_t characterSpacing = 0, uint32_t followingCp = 0, int kern = 0) {
  const int resolvedFontId = 1;
  SdCardFont sd{advance};
  std::map<int, SdCardFont*> sdCardFonts_{{1, &sd}};
  std::map<int, EpdFontFamily> fontMap{{1, EpdFontFamily{kern}}};
'''+gfx[start:end]+r'''
  assert(false); return 0;
}
int main() {
  KeyboardEntryActivity k;
  for (const std::string text : {"", "a", "aú", "é中😀", "ab"}) {
    k.text = text;
    for (size_t cursor = 0; cursor <= text.size(); ++cursor) {
      if (cursor < text.size() && (static_cast<unsigned char>(text[cursor]) & 0xc0) == 0x80) continue;
      k.cursorPos = cursor;
      k.inputType = KeyboardEntryActivity::InputType::Password;
      k.passwordVisible = false;
      k.cursorMode = true;
      assert(k.displayTextForCurrentState() == std::string(text.size(), '*'));
      k.cursorMode = false;
      const size_t prev = k.utf8Prev(text, cursor);
      assert(k.displayTextForCurrentState() == std::string(prev, '*') + text.substr(prev, cursor-prev) + std::string(text.size()-cursor, '*'));
      k.passwordVisible = true; assert(k.displayTextForCurrentState() == text);
      k.passwordVisible = false; k.inputType = KeyboardEntryActivity::InputType::Url;
      assert(k.displayTextForCurrentState() == text);
    }
  }
  assert(pluginhttp::urlEncodeQuery("a &/é?#%+") == "a%20%26%2F%C3%A9%3F%23%25%2B");
  assert(pluginhttp::escapeUrl("https://x/a é?q=%20&b=+/#") == "https://x/a%20%C3%A9?q=%20&b=+/#");
  assert(pluginhttp::escapeUrl("x\r\ny\t") == "x%0D%0Ay%09");
  freeink::SecureHttpClient temporary, session;
  const pluginhttp::Headers headers{{"Cookie", std::string(2000, 'a')}};
  assert(openClient(&session, temporary, "https://x", headers, 42, [] {return false;}) == &session);
  assert((session.calls == std::vector<std::string>{"begin", "connect", "header"}));
  session.calls.clear(); session.connectOk = false;
  assert(openClient(&session, temporary, "https://x", headers, 42, [] {return false;}) == nullptr);
  assert(session.calls.size() == 2);
  session.calls.clear(); session.connectOk = true;
  assert(openClient(&session, temporary, "https://x", headers, 42, [] {return true;}) == nullptr);
  assert(session.calls.size() == 2);
  assert(measure("aaaaaaaaaa") == 10); // Previously rounds 170/16 once, giving 11.
  assert(measure("a\u0301\ufe0fb\u05b0c") == 3);
  assert(measure("abc", 23, EpdFontFamily::SUP) == 3);
  assert(measure("abc", 23, EpdFontFamily::SMALL_CAPS) == 3);
  for (int level = -5; level <= 5; ++level) {
    assert(measure("abcd", 17, EpdFontFamily::REGULAR, level) == 4 + fp4::toPixel(3*level*8));
    assert(measure("abcd", 17, EpdFontFamily::REGULAR, level, 'e', 7) == 3 + fp4::toPixel(24) + fp4::toPixel(4*level*8));
  }
  assert(measure("") == 0);
}
'''
with tempfile.TemporaryDirectory(prefix='crossink-upstream-ports-') as directory:
    cpp = Path(directory)/'test.cpp'
    exe = Path(directory)/'test'
    cpp.write_text(source)
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I'+str(ROOT/'lib/Utf8'), '-I'+str(ROOT/'lib/EpdFont'),
                    str(cpp), str(ROOT/'lib/Utf8/Utf8.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print('Production-path regression checks passed: UTF-8 masking, URL encoding, connection ordering/cancellation and SD advance rounding.')
