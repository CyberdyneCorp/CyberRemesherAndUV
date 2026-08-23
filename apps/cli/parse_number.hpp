#pragma once

// Strict command-line number parsing.
//
// std::from_chars is the whole grammar the CLI wants: no leading whitespace, no
// leading '+', no hex form, no trailing junk, and out-of-range rejected rather
// than saturated. Its FLOATING-POINT overloads, however, are missing from
// libc++ — the standard library of both mobile targets (Apple platforms and the
// Android NDK) — so a build for either fails to link the moment --edge-scale
// (or any other float flag) instantiates it.
//
// So the float path is written twice: std::from_chars where the library has it,
// and the same grammar reimposed over strtof/strtod where it does not. Both are
// compiled everywhere and tests/cli/test_parse_number.cpp holds the fallback to
// the reference's exact verdicts, so the branch nobody's desktop build takes
// cannot rot.

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>

// libc++ implements std::from_chars for integers only.
#if defined(_LIBCPP_VERSION)
#define CYBER_CLI_STRTOD_FLOATS 1
#else
#define CYBER_CLI_STRTOD_FLOATS 0
#endif

namespace cyber::cli {

namespace detail {

template <typename T>
T callStrtod(const char* begin, char** end) {
    if constexpr (std::is_same_v<T, float>) {
        return std::strtof(begin, end);
    } else if constexpr (std::is_same_v<T, long double>) {
        return std::strtold(begin, end);
    } else {
        return std::strtod(begin, end);
    }
}

// Grammar checks std::from_chars applies and strtod does not: it accepts no
// leading whitespace and no leading '+', and its `general` format has no hex
// form, so "0x10" parses as 0 with 'x' left over and is rejected as trailing
// junk. Only the optional '-' is shared.
inline bool hasFromCharsPrefix(const std::string& text) {
    const std::string::size_type digits = text.rfind('-', 0) == 0 ? 1 : 0;
    if (text.size() <= digits) {
        return false;
    }
    const unsigned char lead = static_cast<unsigned char>(text[digits]);
    if (std::isspace(lead) != 0 || lead == '+') {
        return false;
    }
    return !(text.size() > digits + 1 && text[digits] == '0' &&
             (text[digits + 1] == 'x' || text[digits + 1] == 'X'));
}

// std::from_chars' floating-point behaviour over strtof/strtod, for the
// libraries that ship no floating-point from_chars.
template <typename T>
std::optional<T> parseFloatingViaStrtod(const std::string& text) {
    if (!hasFromCharsPrefix(text)) {
        return std::nullopt;
    }
    errno = 0;
    char* end = nullptr;
    const T value = callStrtod<T>(text.c_str(), &end);
    // Whole string consumed, and out-of-range rejected the way from_chars
    // reports result_out_of_range rather than handing back a saturated value.
    if (end != text.c_str() + text.size() || errno == ERANGE) {
        return std::nullopt;
    }
    return value;
}

template <typename T>
std::optional<T> parseFloatingViaFromChars(const std::string& text) {
#if CYBER_CLI_STRTOD_FLOATS
    (void)text;  // no floating-point std::from_chars to call here
    return std::nullopt;
#else
    T value{};
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
#endif
}

}  // namespace detail

// Parses a whole string as T, or returns nullopt. Non-numeric values are never
// silently converted to zero (AutoRemesher turned "abc" into 0 — spec'd away).
template <typename T>
std::optional<T> parseNumber(const std::string& text) {
    if constexpr (std::is_floating_point_v<T>) {
#if CYBER_CLI_STRTOD_FLOATS
        return detail::parseFloatingViaStrtod<T>(text);
#else
        return detail::parseFloatingViaFromChars<T>(text);
#endif
    } else {
        T value{};
        const char* begin = text.data();
        const char* end = begin + text.size();
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        if (ec != std::errc{} || ptr != end) {
            return std::nullopt;
        }
        return value;
    }
}

}  // namespace cyber::cli
