#pragma once
#include <HalStorage.h>
enum class BmpReaderError { Ok, Invalid };
class Bitmap {
  HalFile& file_;

 public:
  explicit Bitmap(HalFile& file) : file_(file) {}
  BmpReaderError parseHeaders() {
    char first = 0;
    return file_.read(&first, 1) == 1 && first == 'V' ? BmpReaderError::Ok : BmpReaderError::Invalid;
  }
};
