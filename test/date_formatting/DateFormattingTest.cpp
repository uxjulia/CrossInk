#include <DateFormatting.h>
#include <I18n.h>

#include <cassert>
#include <cstring>

int main(int argc, char** argv) {
  assert(argc == 2 || argc == 3);
  if (argc == 3) {
    language_cache::Installed installed;
    assert(I18N.prepare(argv[2], installed) == language_cache::Result::Ok);
    return 0;
  }
  I18N.begin(argv[1]);
  assert(std::strcmp(I18N.getCode(), argv[1]) == 0);
  char date[32];
  if (std::strcmp(argv[1], "EN") == 0) {
    assert(formatDateParts(date, sizeof(date), 2026, 10, 3, 0));
    assert(std::strcmp(date, "Oct 03, 2026") == 0);
  } else if (std::strcmp(argv[1], "DE") == 0) {
    assert(formatDateParts(date, sizeof(date), 2026, 10, 3, 0));
    assert(std::strcmp(date, "Okt. 03, 2026") == 0);
    assert(formatDateParts(date, sizeof(date), 2026, 10, 3, 8));
    assert(std::strcmp(date, "03 Oktober") == 0);
  } else if (std::strcmp(argv[1], "FR") == 0) {
    assert(formatDateParts(date, sizeof(date), 2026, 2, 3, 7));
    assert(std::strcmp(date, "février 03") == 0);
  }
  assert(formatDateParts(date, sizeof(date), 2026, 2, 3, 4, '-'));
  assert(std::strcmp(date, "2026-02-03") == 0);
  assert(formatDateParts(date, sizeof(date), 2026, 2, 3, 3, '.'));
  assert(std::strcmp(date, "03.02.2026") == 0);
  assert(!formatDateParts(date, 4, 2026, 2, 3, 7));
  assert(!formatDateParts(date, sizeof(date), 2026, 0, 3, 0));
  assert(!formatDateParts(date, sizeof(date), 2026, 13, 3, 0));
  for (unsigned month = 1; month <= 12; ++month) {
    for (unsigned format = 0; format <= 8; ++format) {
      assert(formatDateParts(date, sizeof(date), 2026, month, 28, format));
      assert(std::strstr(date, "???") == nullptr);
    }
  }
}
