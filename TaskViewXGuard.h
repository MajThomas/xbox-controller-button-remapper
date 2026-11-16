#pragma once
// Conservative pure-logic gate. Neither a hotkey history nor a background
// Explorer window is sufficient proof that the Task View is active.
#include <string_view>

inline bool matches_task_view_fingerprint(std::wstring_view window_class,
                                           std::wstring_view executable_name,
                                           std::wstring_view thread_description)
{
    if (executable_name != L"explorer.exe") return false;
    // The historical (Win10) Task View HWND has a dedicated window class.
    if (window_class == L"MultitaskingViewFrame") return true;
    // Modern immersive windows use generic classes, so require an
    // additional, much more specific shell thread identity.
    if ((window_class == L"Windows.UI.Core.CoreWindow" ||
         window_class == L"XamlExplorerHostIslandWindow") &&
        thread_description == L"MultitaskingView") return true;
    return false; // Unknown OS layout: fail CLOSED.
}

// X is a normal, physical gamepad button. This gate observes its transitions
// without consuming, delaying, or injecting any X events. Exactly one
// supplemental Delete attempt may be made per physical X press.
struct TaskViewXPressGate
{
    bool pressed = false;

    bool on_down() noexcept
    {
        if (pressed) return false;
        pressed = true;
        return true;
    }

    void on_up() noexcept { pressed = false; }
};
