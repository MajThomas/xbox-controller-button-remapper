#pragma once
// Windows-independent action configuration parser for Xbox single/double/triple clicks.
// Each binding can use a built-in action or a user-provided virtual-key combination.
#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

enum class XboxActionKind
{
    none, screenshot, gamebar, alt_tab, win_tab, open_file, toggle_mic, keys
};

struct XboxActionBinding
{
    XboxActionKind kind = XboxActionKind::none;
    std::vector<int> keys;
};

inline std::wstring normalize_xbox_action(std::wstring value)
{
    const auto non_space = [](wchar_t c) { return !std::iswspace(c); };
    const auto begin = std::find_if(value.begin(), value.end(), non_space);
    if (begin == value.end()) return L"";
    const auto end = std::find_if(value.rbegin(), value.rend(), non_space).base();
    std::wstring normalized(begin, end);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return normalized;
}

// Strictly parse comma-separated decimal virtual-key codes, rejecting missing
// entries, extra text, non-ASCII digits and out-of-range values. This avoids
// the legacy INI.getIntegers behavior on malformed or string-typed entries.
inline std::vector<int> parse_xbox_key_codes(const std::wstring& csv, bool* valid = nullptr)
{
    if (valid) *valid = false;
    std::vector<int> result;
    if (csv.empty()) return {};
    std::size_t position = 0;
    for (;;)
    {
        const auto end = csv.find(L',', position);
        const auto last = end == std::wstring::npos ? csv.size() : end;
        std::wstring token = csv.substr(position, last - position);
        const auto first_digit = std::find_if_not(token.begin(), token.end(),
            [](wchar_t c) { return std::iswspace(c); });
        const auto right = std::find_if_not(token.rbegin(), token.rend(),
            [](wchar_t c) { return std::iswspace(c); }).base();
        if (first_digit == token.end() || first_digit >= right) return {};
        int code = 0;
        for (auto it = first_digit; it != right; ++it)
        {
            if (*it < L'0' || *it > L'9') return {};
            code = code * 10 + (*it - L'0');
            if (code > 254) return {};
        }
        if (code < 8) return {};
        result.push_back(code);
        if (result.size() > 8) return {};
        if (end == std::wstring::npos) break;
        position = end + 1;
        if (position >= csv.size()) return {};
    }
    if (valid) *valid = true;
    return result;
}

// Returns an empty binding for invalid entries, and sets *valid=false.
// Do not allow synthetic special action codes (901/903/904/905) in key chords.
// Use their named action equivalents instead.
inline XboxActionBinding parse_xbox_action(const std::wstring& value,
    const std::vector<int>& key_codes = {}, bool* valid = nullptr)
{
    if (valid) *valid = true;
    const std::wstring action = normalize_xbox_action(value);
    if (action == L"none") return {};
    if (action == L"screenshot") return { XboxActionKind::screenshot, {} };
    if (action == L"gamebar") return { XboxActionKind::gamebar, {} };
    if (action == L"alt_tab") return { XboxActionKind::alt_tab, {} };
    if (action == L"win_tab") return { XboxActionKind::win_tab, {} };
    if (action == L"open_file") return { XboxActionKind::open_file, {} };
    if (action == L"toggle_mic") return { XboxActionKind::toggle_mic, {} };
    if (action == L"keys")
    {
        // Match the project's documented Win32 virtual-key numbering.
        // A chord is pressed left to right and released right to left.
        if (!key_codes.empty() && key_codes.size() <= 8 &&
            std::all_of(key_codes.begin(), key_codes.end(),
                [](int vk) { return vk >= 8 && vk <= 254; }))
        {
            std::vector<int> sorted = key_codes;
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end())
            {
                return { XboxActionKind::keys, key_codes };
            }
        }
    }
    if (valid) *valid = false;
    return {};
}
