#pragma once
// Select exactly one Xbox-button event stream for click counting.
// Some third-party controllers emit a distinct raw joystick DOWN for every
// tap, but SDL's mapped Gamepad GUIDE button remains held across those taps.
// Do not count both streams, because that would turn one tap into two clicks.
enum class XboxInputSource { gamepad, joystick };

constexpr bool count_gamepad_guide_down(XboxInputSource source)
{
    return source == XboxInputSource::gamepad;
}

constexpr bool count_joystick_xbox_down(XboxInputSource source, int button, int configured_button)
{
    return source == XboxInputSource::joystick && button == configured_button;
}
