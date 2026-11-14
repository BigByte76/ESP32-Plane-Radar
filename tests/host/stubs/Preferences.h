// Host stub for the ESP32 Preferences (NVS) library: an in-memory store keyed
// by namespace, shared by every Preferences instance so values survive across
// begin()/end() exactly like flash does.
#pragma once

#include <Arduino.h>

#include <cmath>
#include <map>
#include <string>

namespace fakenvs {
using Namespace = std::map<std::string, std::string>;  // key -> raw bytes
extern std::map<std::string, Namespace> g_store;
/** When true, every begin() fails (simulates a corrupt/missing NVS partition). */
extern bool g_fail_begin;
inline void reset() {
  g_store.clear();
  g_fail_begin = false;
}
}  // namespace fakenvs

class Preferences {
 public:
  bool begin(const char* name, bool read_only = false) {
    if (fakenvs::g_fail_begin) return false;
    ns_ = &fakenvs::g_store[name];
    read_only_ = read_only;
    return true;
  }
  void end() { ns_ = nullptr; }

  bool isKey(const char* key) { return ns_ && ns_->count(key) != 0; }
  bool remove(const char* key) {
    if (!writable()) return false;
    return ns_->erase(key) != 0;
  }
  bool clear() {
    if (!writable()) return false;
    ns_->clear();
    return true;
  }

  size_t putUChar(const char* k, uint8_t v) { return putRaw(k, v); }
  size_t putUShort(const char* k, uint16_t v) { return putRaw(k, v); }
  size_t putBool(const char* k, bool v) { return putRaw(k, static_cast<uint8_t>(v)); }
  size_t putDouble(const char* k, double v) { return putRaw(k, v); }
  size_t putString(const char* k, const char* v) {
    if (!writable()) return 0;
    (*ns_)[k] = std::string(v);
    return strlen(v);
  }

  uint8_t getUChar(const char* k, uint8_t d = 0) { return getRaw(k, d); }
  uint16_t getUShort(const char* k, uint16_t d = 0) { return getRaw(k, d); }
  bool getBool(const char* k, bool d = false) {
    return getRaw<uint8_t>(k, d ? 1 : 0) != 0;
  }
  double getDouble(const char* k, double d = NAN) { return getRaw(k, d); }
  size_t getString(const char* k, char* out, size_t max_len) {
    if (!isKey(k) || max_len == 0) return 0;
    const std::string& s = (*ns_)[k];
    if (s.size() + 1 > max_len) return 0;  // matches ESP32: too small -> fail
    memcpy(out, s.c_str(), s.size() + 1);
    return s.size() + 1;
  }

 private:
  bool writable() const { return ns_ != nullptr && !read_only_; }

  template <typename T>
  size_t putRaw(const char* k, T v) {
    if (!writable()) return 0;
    (*ns_)[k] = std::string(reinterpret_cast<const char*>(&v), sizeof(T));
    return sizeof(T);
  }
  template <typename T>
  T getRaw(const char* k, T d) {
    if (!isKey(k)) return d;
    const std::string& s = (*ns_)[k];
    if (s.size() != sizeof(T)) return d;
    T v;
    memcpy(&v, s.data(), sizeof(T));
    return v;
  }

  fakenvs::Namespace* ns_ = nullptr;
  bool read_only_ = false;
};
