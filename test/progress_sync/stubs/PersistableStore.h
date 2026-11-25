#pragma once
template <class T>
class PersistableStore {
 public:
  static T& getInstance() {
    static T instance;
    return instance;
  }
  void ensureLoaded() const {}
};
