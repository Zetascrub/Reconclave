#include "framework.h"

#include <Preferences.h>

namespace reconclave {
namespace {
Preferences g_prefs;
constexpr const char* kNamespace = "rc-node";
}  // namespace

void Config::begin() { g_prefs.begin(kNamespace, /*readOnly=*/false); }
uint32_t Config::getUInt(const char* key, uint32_t fallback) { return g_prefs.getUInt(key, fallback); }
void Config::setUInt(const char* key, uint32_t value) { g_prefs.putUInt(key, value); }
String Config::getString(const char* key, const String& fallback) { return g_prefs.getString(key, fallback); }
void Config::setString(const char* key, const String& value) { g_prefs.putString(key, value); }

}  // namespace reconclave
