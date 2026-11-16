#include "../XboxActionBinding.h"
#include <cassert>
#include <iostream>

static XboxActionBinding binding(const wchar_t* text,
    const std::vector<int>& keys, bool expected_valid = true)
{
    bool valid = false;
    XboxActionBinding result = parse_xbox_action(text, keys, &valid);
    assert(valid == expected_valid);
    return result;
}

int main()
{
    using A = XboxActionKind;
    assert(binding(L"screenshot", {}).kind == A::screenshot);
    assert(binding(L"gamebar", {}).kind == A::gamebar);
    assert(binding(L"alt_tab", {}).kind == A::alt_tab);
    assert(binding(L"win_tab", {}).kind == A::win_tab);
    assert(binding(L" WIN_TAB ", {}).kind == A::win_tab);
    assert(binding(L"none", {}).kind == A::none);
    assert(binding(L"open_file", {}).kind == A::open_file);
    assert(binding(L"toggle_mic", {}).kind == A::toggle_mic);
    assert(binding(L"  KEYS  ", {17, 16, 27}).keys == std::vector<int>({17,16,27}));
    assert(binding(L" KeYs\t", {18, 9}).kind == A::keys);
    assert(binding(L"KEYS", {179}).keys == std::vector<int>({179}));
    assert(binding(L"keys", {}, false).kind == A::none);
    assert(binding(L"keys", {0}, false).kind == A::none);
    assert(binding(L"keys", {901}, false).kind == A::none);
    assert(binding(L"keys", {18,9,9}, false).kind == A::none);
    assert(binding(L"keys", {18,9,1}, false).kind == A::none);
    assert(binding(L"keys", {17,16,27,65,66,67,68,69,70}, false).kind == A::none);
    assert(binding(L"typ0", {}, false).kind == A::none);
    assert(binding(L"", {}, false).kind == A::none);
    // Built-in actions must ignore unrelated _click_keys entries.
    assert(binding(L"screenshot", {999}).kind == A::screenshot);
    bool good = false;
    assert(parse_xbox_key_codes(L"18,9", &good) == std::vector<int>({18, 9}) && good);
    assert(parse_xbox_key_codes(L" 17 , 16, 27 ", &good) == std::vector<int>({17, 16, 27}) && good);
    assert(parse_xbox_key_codes(L"179", &good) == std::vector<int>({179}) && good);
    assert(parse_xbox_key_codes(L"18,9junk", &good).empty() && !good);
    assert(parse_xbox_key_codes(L"18,,9", &good).empty() && !good);
    assert(parse_xbox_key_codes(L"18,9,", &good).empty() && !good);
    assert(parse_xbox_key_codes(L"", &good).empty() && !good);
    assert(parse_xbox_key_codes(L"901", &good).empty() && !good);
    assert(parse_xbox_key_codes(L"-18,9", &good).empty() && !good);
    assert(parse_xbox_key_codes(L"999999999999999999999", &good).empty() && !good);
    assert(parse_xbox_key_codes(L"8,9,10,11,12,13,14,15,16", &good).empty() && !good);
    std::cout << "All Xbox configurable-action tests passed (31 scenarios).\n";
}
