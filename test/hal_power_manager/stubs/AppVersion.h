#pragma once
struct AppVersion {
  static const char* version() { return "test-picco"; }
  static const char* gitSha() { return "abcdef12"; }
  static const char* gitDirtyFlag() { return "0"; }
};
