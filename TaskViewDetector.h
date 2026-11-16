#pragma once
// Windows-specific foreground task-view probe. Do not accept a remembered
// Win+Tab keystroke, an Explorer process alone, or an arbitrary title.
#include <windows.h>
#include <string>
#include <cwchar>
#include "TaskViewXGuard.h"

struct TaskViewProbe
{
    HWND foreground = nullptr;
    std::wstring window_class;
    std::wstring executable;
    std::wstring thread_description;
    bool confirmed = false;
};

inline TaskViewProbe probe_task_view_foreground()
{
    TaskViewProbe probe;
    HWND hwnd = GetForegroundWindow();
    if (!hwnd || !IsWindowVisible(hwnd)) return probe;
    probe.foreground = hwnd;

    wchar_t cls[256] = {};
    if (!GetClassNameW(hwnd, cls, static_cast<int>(sizeof(cls) / sizeof(cls[0])))) return probe;
    probe.window_class = cls;

    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(hwnd, &process_id);
    if (!thread_id || !process_id) return probe;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (!process) return probe;
    wchar_t image[MAX_PATH * 2] = {};
    DWORD image_size = static_cast<DWORD>(sizeof(image) / sizeof(image[0]));
    const BOOL found = QueryFullProcessImageNameW(process, 0, image, &image_size);
    CloseHandle(process);
    if (!found) return probe;
    const wchar_t* tail = wcsrchr(image, L'\\');
    probe.executable = tail ? tail + 1 : image;

    // Insist on Windows' own Explorer executable, not a lookalike filename.
    wchar_t windows_directory[MAX_PATH] = {};
    const UINT win_length = GetWindowsDirectoryW(windows_directory, MAX_PATH);
    if (!win_length || win_length >= MAX_PATH) return probe;
    const std::wstring trusted_path = std::wstring(windows_directory) + L"\\explorer.exe";
    if (_wcsicmp(image, trusted_path.c_str()) != 0) return probe;

    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
    if (thread)
    {
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description)
        {
            probe.thread_description = description;
            LocalFree(description);
        }
        CloseHandle(thread);
    }

    probe.confirmed = matches_task_view_fingerprint(
        probe.window_class, probe.executable, probe.thread_description);
    return probe;
}
