#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace reconclave {

using TrustKey = std::array<std::uint8_t, 32>;

bool loadHexTrustKey(const std::string& path, TrustKey& key, std::string& error);
std::string sha256Hex(const std::string& message);
std::string hmacSha256Hex(const TrustKey& key, const std::string& message,
                          std::size_t output_bytes = 32);
bool constantTimeHexEqual(const std::string& left, const std::string& right);

}  // namespace reconclave
