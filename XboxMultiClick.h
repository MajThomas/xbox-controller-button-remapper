#pragma once
// Guide-button multi-click state machine. No button-up/release event is required.
// The elapsed window is measured between successive SDL_GAMEPAD_BUTTON_DOWN events.
#include <cstdint>

enum class XboxClickAction { none, single, double_click, triple };

class XboxMultiClickTracker
{
public:
    // Every DOWN event is a click, including when no matching UP event arrives.
    // A configurable debounce is only for devices that emit duplicate DOWN events.
    // A new press after the previous sequence has expired finalizes that sequence
    // and also starts a NEW one; no press is lost.
    XboxClickAction on_press(int64_t now_ms, int64_t interval_ms, int64_t debounce_ms = 0)
    {
        if (has_last_press_)
        {
            const int64_t elapsed_ms = now_ms - last_press_ms_;
            // Ignore out-of-order or spurious duplicate events only if requested.
            if (debounce_ms > 0 && elapsed_ms >= 0 && elapsed_ms < debounce_ms)
                return XboxClickAction::none;
        }

        XboxClickAction prior_action = XboxClickAction::none;
        if (click_count_ != 0 && (now_ms < last_press_ms_ ||
            now_ms - last_press_ms_ > interval_ms))
        {
            prior_action = finish();
        }

        has_last_press_ = true;
        last_press_ms_ = now_ms;
        ++click_count_;
        if (click_count_ == 3)
        {
            click_count_ = 0;
            return XboxClickAction::triple;
        }
        return prior_action;
    }

    XboxClickAction on_tick(int64_t now_ms, int64_t interval_ms)
    {
        if (click_count_ != 0 && now_ms >= last_press_ms_ &&
            now_ms - last_press_ms_ > interval_ms)
        {
            return finish();
        }
        return XboxClickAction::none;
    }

    int pending_count() const { return click_count_; }

private:
    XboxClickAction finish()
    {
        XboxClickAction action = XboxClickAction::none;
        if (click_count_ == 1) action = XboxClickAction::single;
        else if (click_count_ == 2) action = XboxClickAction::double_click;
        click_count_ = 0;
        return action;
    }

    int click_count_ = 0;
    int64_t last_press_ms_ = 0;
    bool has_last_press_ = false;
};
