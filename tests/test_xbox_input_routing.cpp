#include "../XboxInputRouting.h"
#include "../XboxMultiClick.h"
#include <cassert>
#include <iostream>

int main()
{
    constexpr auto raw = XboxInputSource::joystick;
    constexpr auto mapped = XboxInputSource::gamepad;
    static_assert(count_gamepad_guide_down(raw) == false, "");
    static_assert(count_gamepad_guide_down(mapped) == true, "");
    static_assert(count_joystick_xbox_down(raw, 10, 10) == true, "");
    static_assert(count_joystick_xbox_down(raw, 5, 10) == false, "");
    static_assert(count_joystick_xbox_down(mapped, 10, 10) == false, "");

    // Event stream seen in the user's log: the mapped GUIDE remains down;
    // each raw button 10 pulse has its own DOWN and UP. Only DOWN is counted.
    XboxMultiClickTracker counter;
    const auto press = [&](int64_t ms, int button) {
        return count_joystick_xbox_down(raw, button, 10)
            ? counter.on_press(ms, 350, 0) : XboxClickAction::none;
    };
    // SDL Gamepad GUIDE DOWN at 0ms: ignored in joystick mode.
    assert(!count_gamepad_guide_down(raw));
    assert(press(1, 10) == XboxClickAction::none);
    assert(press(100, 5) == XboxClickAction::none); // another button
    assert(press(121, 10) == XboxClickAction::none);
    assert(counter.pending_count() == 2);
    assert(press(241, 10) == XboxClickAction::triple);
    assert(counter.pending_count() == 0);
    // GUIDE UP at 500ms and joystick UPs: not counted.
    assert(counter.on_tick(600, 350) == XboxClickAction::none);

    XboxMultiClickTracker double_click;
    assert(double_click.on_press(1000, 350) == XboxClickAction::none);
    assert(double_click.on_press(1150, 350) == XboxClickAction::none);
    assert(double_click.on_tick(1501, 350) == XboxClickAction::double_click);

    XboxMultiClickTracker single;
    assert(single.on_press(2000, 350) == XboxClickAction::none);
    assert(single.on_tick(2351, 350) == XboxClickAction::single);
    std::cout << "Raw button 10 vs mapped GUIDE replay test passed.\n";
}
