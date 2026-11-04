#pragma once
#include <string>
class Epub {
 public:
  const std::string& getCachePath() const {
    static const std::string path = "/epub";
    return path;
  }
};
