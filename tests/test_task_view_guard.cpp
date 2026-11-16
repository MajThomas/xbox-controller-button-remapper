#include "../TaskViewXGuard.h"
#include <cassert>
#include <iostream>

int main()
{
    assert(matches_task_view_fingerprint(L"MultitaskingViewFrame", L"explorer.exe", L""));
    assert(matches_task_view_fingerprint(L"Windows.UI.Core.CoreWindow", L"explorer.exe", L"MultitaskingView"));
    // User's 2026-11-16 Task View trace: known-class + explorer + exact thread.
    assert(matches_task_view_fingerprint(L"XamlExplorerHostIslandWindow", L"explorer.exe", L"MultitaskingView"));
    assert(!matches_task_view_fingerprint(L"XamlExplorerHostIslandWindow", L"explorer.exe", L""));
    assert(!matches_task_view_fingerprint(L"XamlExplorerHostIslandWindow", L"explorer.exe", L"StartMenu"));
    assert(!matches_task_view_fingerprint(L"XamlExplorerHostIslandWindow", L"fake-explorer.exe", L"MultitaskingView"));
    assert(!matches_task_view_fingerprint(L"Shell_TrayWnd", L"explorer.exe", L"MultitaskingView"));
    // A long hold, duplicate DOWN events, and release: only one action per press.
    TaskViewXPressGate gate;
    assert(gate.on_down());
    assert(!gate.on_down());
    assert(!gate.on_down());
    gate.on_up();
    assert(gate.on_down());
    gate.on_up();
    assert(!matches_task_view_fingerprint(L"Windows.UI.Core.CoreWindow", L"explorer.exe", L""));
    assert(!matches_task_view_fingerprint(L"Windows.UI.Core.CoreWindow", L"explorer.exe", L"StartMenu"));
    assert(!matches_task_view_fingerprint(L"MultitaskingViewFrame", L"fake-explorer.exe", L"MultitaskingView"));
    assert(!matches_task_view_fingerprint(L"Shell_TrayWnd", L"explorer.exe", L"MultitaskingView"));
    assert(!matches_task_view_fingerprint(L"ApplicationFrameWindow", L"explorer.exe", L"MultitaskingView"));
    assert(!matches_task_view_fingerprint(L"ConsoleWindowClass", L"explorer.exe", L""));
    assert(!matches_task_view_fingerprint(L"MultitaskingViewFrame", L"Xbox.exe", L""));
    assert(!matches_task_view_fingerprint(L"MultitaskingViewFrame", L"", L""));
    assert(!matches_task_view_fingerprint(L"", L"explorer.exe", L"MultitaskingView"));
    std::cout << "Task View conservative fingerprint checks passed.\n";
}
