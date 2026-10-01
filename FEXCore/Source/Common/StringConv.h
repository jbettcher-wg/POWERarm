// SPDX-License-Identifier: MIT
#pragma once
#include <FEXCore/Config/Config.h>
#include <FEXCore/fextl/string.h>

#include <concepts>
#include <string_view>

namespace FEXCore::StrConv {
// bool satisfies std::integral, so without this overload every bool config
// option was converted by the strtoull template below: "on" became 0, i.e.
// OFF, and the conversion reported success. FEXCore::Config::ParseBool holds
// the accepted spellings; anything else fails here so the caller can say which
// option was given what rather than silently pick a value.
inline bool Conv(std::string_view Value, bool* Result) {
  const auto Parsed = FEXCore::Config::ParseBool(Value);
  if (!Parsed.has_value()) {
    return false;
  }
  *Result = *Parsed;
  return true;
}

template<std::integral T>
bool Conv(std::string_view Value, T* Result) {
  if constexpr (std::is_signed_v<T>) {
    *Result = static_cast<T>(std::strtoll(Value.data(), nullptr, 0));
  } else {
    *Result = static_cast<T>(std::strtoull(Value.data(), nullptr, 0));
  }
  return true;
}

template<typename T, typename = std::enable_if_t<std::is_enum_v<T>, T>>
bool Conv(std::string_view Value, T* Result) {
  *Result = static_cast<T>(std::strtoull(Value.data(), nullptr, 0));
  return true;
}

inline bool Conv(std::string_view Value, fextl::string* Result) {
  *Result = Value;
  return true;
}
} // namespace FEXCore::StrConv
