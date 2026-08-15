#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include <json.hpp>

// Accessors for peer-supplied JSON. nlohmann's at()/get<T>()/value() throw on a
// missing or retyped member, and its arithmetic conversion is a plain
// static_cast — undefined behaviour when a JSON float is outside the integer's
// range ([conv.fpint]). Both the server dispatch and the C++ client read
// untrusted bytes through the helpers here, which do neither.
namespace cyber::net::detail {

// Unsigned field, or `fallback` when the key is absent, is not a non-negative
// integer, or does not fit. Floats are refused rather than cast.
template <typename T>
T readUnsigned(const nlohmann::json& j, const char* key, T fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    const auto it = j.find(key);
    if (it == j.end() || !it->is_number_unsigned()) {
        return fallback;
    }
    const std::uint64_t value = it->template get<std::uint64_t>();
    return value <= static_cast<std::uint64_t>(std::numeric_limits<T>::max())
               ? static_cast<T>(value)
               : fallback;
}

// String field, or `fallback` when the key is absent or is not a string.
inline std::string readString(const nlohmann::json& j, const char* key,
                              const std::string& fallback) {
    if (!j.is_object()) {
        return fallback;
    }
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->template get<std::string>() : fallback;
}

}  // namespace cyber::net::detail
