#pragma once

#include <chrono>
#include <cstdint>

// A small, platform-independent click recognizer for the Xbox/Guide button.
// The device only needs to deliver button-down and button-up events; there is
// intentionally NO hold-duration or long-press detection.
enum class XboxClickAction
{
    None,
    Screenshot,
    GameBar,
    SwitchTasks
};

class XboxMultiClick
{
public:
    using clock = std::chrono::steady_clock;
    using time_point = clock::time_point;

    // A new down event starts the next click. Expire the previous sequence
    // here too, in case the SDL event pump was delayed between polls.
    XboxClickAction onPress(time_point now, std::int64_t interval_ms)
    {
        if (pressed_)
        {
            return XboxClickAction::None; // Ignore duplicate DOWN events.
        }

        const XboxClickAction expired = flushIfExpired(now, interval_ms);
        pressed_ = true;
        return expired;
    }

    // Count a click only after an actual DOWN -> UP pair.
    // A triple click is executed immediately after the third release.
    XboxClickAction onRelease(time_point now)
    {
        if (!pressed_)
        {
            return XboxClickAction::None; // Ignore stray/repeated UP events.
        }

        pressed_ = false;
        last_release_ = now;
        ++click_count_;

        if (click_count_ >= 3)
        {
            reset();
            return XboxClickAction::SwitchTasks;
        }
        return XboxClickAction::None;
    }

    // Poll once per main-loop iteration, including when no SDL events arrive.
    XboxClickAction poll(time_point now, std::int64_t interval_ms)
    {
        return flushIfExpired(now, interval_ms);
    }

    void reset()
    {
        click_count_ = 0;
        pressed_ = false;
        last_release_ = time_point{};
    }

private:
    XboxClickAction flushIfExpired(time_point now, std::int64_t interval_ms)
    {
        // Never finalize a pending sequence while the next press is held.
        if (pressed_ || click_count_ == 0 || now < last_release_)
        {
            return XboxClickAction::None;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_release_).count();
        if (elapsed < interval_ms)
        {
            return XboxClickAction::None;
        }

        const XboxClickAction result = click_count_ == 1
            ? XboxClickAction::Screenshot
            : XboxClickAction::GameBar;
        reset();
        return result;
    }

    int click_count_ = 0;
    bool pressed_ = false;
    time_point last_release_{};
};
