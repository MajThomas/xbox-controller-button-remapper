#include "../XboxMultiClick.h"
#include <cassert>
#include <iostream>

using Action = XboxClickAction;

int main()
{
    // 1. A single DOWN alone must work, even without any UP.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(100, 350) == Action::none);
        assert(t.on_tick(450, 350) == Action::none);
        assert(t.on_tick(451, 350) == Action::single);
        assert(t.on_tick(1000, 350) == Action::none);
    }
    // 2. Two DOWN events (no UP) resolve to double-click.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(100, 350) == Action::none);
        assert(t.on_press(300, 350) == Action::none);
        assert(t.on_tick(650, 350) == Action::none);
        assert(t.on_tick(651, 350) == Action::double_click);
    }
    // 3. The third DOWN immediately fires and clears the click sequence.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(100, 350) == Action::none);
        assert(t.on_press(220, 350) == Action::none);
        assert(t.on_press(340, 350) == Action::triple);
        assert(t.pending_count() == 0);
        assert(t.on_tick(2000, 350) == Action::none);
    }
    // 4. Fourth DOWN begins another independent sequence.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(0, 350) == Action::none);
        assert(t.on_press(100, 350) == Action::none);
        assert(t.on_press(200, 350) == Action::triple);
        assert(t.on_press(300, 350) == Action::none);
        assert(t.on_tick(651, 350) == Action::single);
    }
    // 5. An expired sequence is emitted on the next DOWN; it also starts a new one.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(0, 350) == Action::none);
        assert(t.on_press(100, 350) == Action::none);
        assert(t.on_press(500, 350) == Action::double_click);
        assert(t.pending_count() == 1);
        assert(t.on_tick(851, 350) == Action::single);
    }
    // 6. A gap exactly equal to the threshold belongs to the same sequence.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(0, 350) == Action::none);
        assert(t.on_tick(350, 350) == Action::none);
        assert(t.on_press(350, 350) == Action::none);
        assert(t.on_press(700, 350) == Action::triple);
    }
    // 7. Two rapid consecutive DOWN events always count if debounce=0.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(0, 350, 0) == Action::none);
        assert(t.on_press(1, 350, 0) == Action::none);
        assert(t.on_press(2, 350, 0) == Action::triple);
    }
    // 8. Configured debounce ignores close duplicate presses even without UP.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(100, 350, 30) == Action::none);
        assert(t.on_press(110, 350, 30) == Action::none);
        assert(t.pending_count() == 1);
        assert(t.on_press(140, 350, 30) == Action::none);
        assert(t.pending_count() == 2);
        assert(t.on_tick(491, 350) == Action::double_click);
    }
    // 9. Debounce doesn't slide last-press timestamp when ignoring a duplicate.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(100, 350, 30) == Action::none);
        assert(t.on_press(110, 350, 30) == Action::none);
        assert(t.on_tick(450, 350) == Action::none);
        assert(t.on_tick(451, 350) == Action::single);
    }
    // 10. Each controller has its own independent click stream.
    {
        XboxMultiClickTracker first, second;
        assert(first.on_press(10, 350) == Action::none);
        assert(second.on_press(20, 350) == Action::none);
        assert(first.on_press(100, 350) == Action::none);
        assert(first.on_press(200, 350) == Action::triple);
        assert(second.on_tick(371, 350) == Action::single);
    }
    // 11. Debounce must not suppress a new click after a triple.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(0, 350, 20) == Action::none);
        assert(t.on_press(80, 350, 20) == Action::none);
        assert(t.on_press(160, 350, 20) == Action::triple);
        assert(t.on_press(170, 350, 20) == Action::none); // duplicate ignored
        assert(t.pending_count() == 0);
        assert(t.on_press(200, 350, 20) == Action::none);
        assert(t.on_tick(551, 350) == Action::single);
    }
    // 12. Clicks following long idle periods form fresh sequences.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(1000, 350) == Action::none);
        assert(t.on_tick(1351, 350) == Action::single);
        assert(t.on_press(10000, 350) == Action::none);
        assert(t.on_press(10100, 350) == Action::none);
        assert(t.on_tick(10451, 350) == Action::double_click);
    }
    // 13. Out-of-order timestamps finalize safely rather than miscounting.
    {
        XboxMultiClickTracker t;
        assert(t.on_press(100, 350) == Action::none);
        assert(t.on_press(90, 350) == Action::single);
        assert(t.on_tick(441, 350) == Action::single);
    }
    std::cout << "All 13 DOWN-only Xbox multi-click tests passed.\n";
    return 0;
}
