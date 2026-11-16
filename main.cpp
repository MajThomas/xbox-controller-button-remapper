#include <string>
//#include <format>
#include <chrono>
#include <algorithm>
#include <thread>
#include <iostream>
#include <fstream>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <strsafe.h>
#include <fcntl.h>
#include <stdio.h>
#include <io.h>
#include "INI.hpp"
#include "resource.h"
#include <Xinput.h>
#pragma comment(lib, "xinput.lib")
#include <codecvt>
#include <filesystem>
//#include <Shlwapi.h>
//#pragma comment(lib, "Shlwapi.lib")
//#include <PathCch.h>
//#pragma comment(lib, "PathCch.lib")
//#include <tchar.h>
//#include <shlobj.h>
//#include <algorithm>
//#include <regex>
//#include <atomic>
//#include <thread>
//#include <future>
//#include <mfapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>

#include "common.h"
#include "XboxMultiClick.h"
#include "XboxInputRouting.h"
#include "TaskViewDetector.h"
#include "XboxActionBinding.h"
#include "ScreenCapture.h"

// Need commctrl v6 for LoadIconMetric()
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")

using namespace std;

typedef std::chrono::high_resolution_clock hrc;

struct KeySettings
{
	std::vector<int> key = { 0 };
	int64_t hold_mode = 1;
	std::vector<int> longpress_key = { 0 };
	int64_t longpress_duration = 1000;
	int64_t delay = 0;
	int64_t duration = 1;
};

struct ControllerSettings
{
	KeySettings share;
	KeySettings xbox;
};

struct Controller
{
	SDL_Gamepad* controller{};
	ControllerSettings settings;
	hrc::time_point button_depressed = hrc::now();
	XboxMultiClickTracker xbox_multi_click;
    TaskViewXPressGate task_view_x_press; // One supplemental Delete per physical X press.
};

// Enabled for every controller; click sequences are tracked independently per device.
struct XboxMultiClickSettings
{
    bool enabled = true;
    int64_t down_debounce_ms = 0; // Ignore duplicate DOWN events only when configured.
    int64_t multi_click_interval_ms = 350;
    XboxActionBinding single_click_action{ XboxActionKind::screenshot, {} };
    XboxActionBinding double_click_action{ XboxActionKind::gamebar, {} };
    XboxActionBinding triple_click_action{ XboxActionKind::win_tab, {} };
};

// SDL can expose a third-party Guide tap via either the mapped gamepad
// button or a raw joystick button. Never count both streams at once.
static XboxInputSource xbox_input_source = XboxInputSource::joystick;
static int xbox_raw_joystick_button = 10;
static int input_poll_interval_ms = 2;

// Optional X->Delete in Windows Task View ONLY. Disabled by default.
// Raw X is b2 in the original project mapping; this is independent of
// the Xbox GUIDE button's raw b10 click counter.
static bool task_view_x_enabled = false;
static bool task_view_x_dry_run = true; // diagnostic only until explicitly armed
static int task_view_x_raw_button = 2;


static XboxMultiClickSettings xbox_multi_click_settings;
static std::vector<Controller> controllers;
handy::io::INIFile ini;

std::wstring file_location = L"C:\\Windows\\explorer.exe";
int file_key = 0;
int OpenFileHotKeyID = 3;

int screenshot_key = 0;
int ScreenshotHotKeyID = 1;

bool sdl_initialized = false;
bool input_device_initialized = false;

IMMDeviceEnumerator* pEnumerator = NULL;
IMMDevice* pDevice = NULL;
static IAudioEndpointVolume* g_pEndptVol = NULL;

static std::wstring get_exe_filename()
{
	wchar_t buffer[MAX_PATH];
	GetModuleFileName(NULL, buffer, MAX_PATH);
	return std::wstring(buffer);
}

static std::wstring get_exe_path()
{
	std::wstring f = get_exe_filename();
	return f.substr(0, f.find_last_of(L"\\/"));
}

// Disable only the *controller GUIDE button* shortcut owned by Windows Game Bar.
// This does not disable Win+G, which is intentionally used by double-click.
// A one-time backup of the original HKCU value is kept for exact restoration.
static LONG suppress_windows_gamebar_controller_button()
{
    constexpr wchar_t gamebar_key[] = L"Software\\Microsoft\\GameBar";
    constexpr wchar_t setting_name[] = L"UseNexusForGameBarEnabled";
    constexpr wchar_t backup_key[] =
        L"Software\\XboxControllerButtonRemapper\\GameBarOverride";

    HKEY gamebar = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, gamebar_key, 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &gamebar, nullptr);
    if (result != ERROR_SUCCESS) return result;

    DWORD previous = 0;
    DWORD value_type = 0;
    DWORD value_size = sizeof(previous);
    const LONG read_result = RegQueryValueExW(gamebar, setting_name, nullptr,
        &value_type, reinterpret_cast<LPBYTE>(&previous), &value_size);
    if (read_result == ERROR_SUCCESS &&
        (value_type != REG_DWORD || value_size != sizeof(DWORD)))
    {
        RegCloseKey(gamebar);
        return ERROR_INVALID_DATATYPE; // Do not overwrite an unexpected value type.
    }
    if (read_result != ERROR_SUCCESS && read_result != ERROR_FILE_NOT_FOUND)
    {
        RegCloseKey(gamebar);
        return read_result;
    }
    if (read_result == ERROR_SUCCESS && previous == 0)
    {
        RegCloseKey(gamebar);
        return ERROR_SUCCESS; // Already disabled, no user preference changed.
    }

    HKEY backup = nullptr;
    result = RegCreateKeyExW(HKEY_CURRENT_USER, backup_key, 0, nullptr, 0,
        KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &backup, nullptr);
    if (result != ERROR_SUCCESS)
    {
        RegCloseKey(gamebar);
        return result; // Never change the system setting without a backup.
    }

    DWORD saved = 0;
    DWORD saved_type = 0;
    DWORD saved_size = sizeof(saved);
    const LONG backup_read = RegQueryValueExW(backup, L"OriginalStateSaved", nullptr,
        &saved_type, reinterpret_cast<LPBYTE>(&saved), &saved_size);
    if (backup_read == ERROR_FILE_NOT_FOUND)
    {
        const DWORD existed = read_result == ERROR_SUCCESS ? 1u : 0u;
        result = RegSetValueExW(backup, L"OriginalExists", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&existed), sizeof(existed));
        if (result == ERROR_SUCCESS)
            result = RegSetValueExW(backup, L"OriginalValue", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&previous), sizeof(previous));
        if (result == ERROR_SUCCESS)
        {
            saved = 1;
            result = RegSetValueExW(backup, L"OriginalStateSaved", 0, REG_DWORD,
                reinterpret_cast<const BYTE*>(&saved), sizeof(saved));
        }
    }
    else if (backup_read != ERROR_SUCCESS || saved_type != REG_DWORD ||
        saved_size != sizeof(DWORD) || saved != 1)
    {
        result = ERROR_INVALID_DATA; // Backup is damaged: refuse to overwrite.
    }

    if (result == ERROR_SUCCESS)
    {
        const DWORD disabled = 0;
        result = RegSetValueExW(gamebar, setting_name, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&disabled), sizeof(disabled));
    }
    RegCloseKey(backup);
    RegCloseKey(gamebar);
    return result;
}

static void initialize_default_input_device()
{
	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
	hr = pEnumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &pDevice);
	//hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&g_pEndptVol);
	hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL, (void**)&g_pEndptVol);
	//hr = pDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, NULL, reinterpret_cast<void**>(&g_pEndptVol));

	input_device_initialized = true;
}

static void toggle_mute()
{
	if (!input_device_initialized)
	{
		initialize_default_input_device();
	}

	BOOL bMute;

	g_pEndptVol->GetMute(&bMute); // Get current mute state
	g_pEndptVol->SetMute(!bMute, NULL); // Toggle mute
}

static void set_mute(bool mute)
{
	if (!input_device_initialized)
	{
		initialize_default_input_device();
	}

	g_pEndptVol->SetMute(mute, NULL);
}

static void add_controller_mapping(char* guid)
{
	char mapping_string[1024];

	SDL_strlcpy(mapping_string, guid, sizeof(mapping_string));
	SDL_strlcat(mapping_string, ",Xbox Controller,platform:Windows,", sizeof(mapping_string));
	SDL_strlcat(mapping_string, "a:b0,b:b1,x:b2,y:b3,back:b6,guide:b10,start:b7,leftstick:b8,rightstick:b9,leftshoulder:b4,rightshoulder:b5,dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,", sizeof(mapping_string));
	SDL_strlcat(mapping_string, "misc1:b11,", sizeof(mapping_string));

	SDL_AddGamepadMapping(mapping_string);
}

static void load_controller_config(int i)
{
	std::wstring controller = L"controller" + std::to_wstring(i + 1);

	controllers[i].settings.share.key = ini.getIntegers(controller, L"share_key", { 0 });
	controllers[i].settings.share.hold_mode = ini.getInteger(controller, L"share_hold_mode", 2);
	controllers[i].settings.share.longpress_key = ini.getIntegers(controller, L"share_longpress_key", { 0 });
	controllers[i].settings.share.longpress_duration = ini.getInteger(controller, L"share_longpress_duration", 1000);
	controllers[i].settings.share.delay = ini.getInteger(controller, L"share_delay", 0);
	controllers[i].settings.share.duration = ini.getInteger(controller, L"share_duration", 1);
	controllers[i].settings.xbox.key = ini.getIntegers(controller, L"xbox_key", { 0 });
	controllers[i].settings.xbox.hold_mode = ini.getInteger(controller, L"xbox_hold_mode", 2);
	controllers[i].settings.xbox.longpress_key = ini.getIntegers(controller, L"xbox_longpress_key", { 0 });
	controllers[i].settings.xbox.longpress_duration = ini.getInteger(controller, L"xbox_longpress_duration", 1000);
	controllers[i].settings.xbox.delay = ini.getInteger(controller, L"xbox_delay", 0);
	controllers[i].settings.xbox.duration = ini.getInteger(controller, L"xbox_duration", 1);
}

static void add_controllers()
{
	int num_gamepads;
	SDL_JoystickID* gamepads = SDL_GetGamepads(&num_gamepads);

	if (gamepads)
	{
		if (controllers.size() != num_gamepads)
		{
			controllers.resize(num_gamepads);
		}

		for (int i = 0; i < num_gamepads; ++i)
		{
			SDL_JoystickID instance_id = gamepads[i];

			char guid[64];
			SDL_GUIDToString(SDL_GetJoystickGUIDForID(instance_id), guid, sizeof(guid));
			add_controller_mapping(guid);

			if (debug)
			{
				SDL_Log("%s: Mapping: %s\n", get_date_time().c_str(), SDL_GetGamepadMappingForGUID(SDL_GetJoystickGUIDForID(instance_id)));
			}

			SDL_Gamepad* controller = SDL_OpenGamepad(instance_id);
			if (!controller)
			{
				if (debug)
				{
					SDL_Log("%s: Could not open controller: %s\n", get_date_time().c_str(), SDL_GetError());
				}

				continue;
			}

			controllers[i].controller = controller;
			load_controller_config(i);

			if (debug)
			{
				SDL_Log("%s: Opened controller %d: %s\n", get_date_time().c_str(), (i + 1), SDL_GetGamepadName(controller));
			}
		}

		SDL_free(gamepads);
	}
}

static void close_controllers()
{
	for (int i = 0; i < controllers.size(); ++i)
	{
        if (controllers[i].controller) SDL_CloseGamepad(controllers[i].controller);
	}

	controllers.clear();

	if (debug)
	{
		SDL_Log("%s: Closed controllers.\n", get_date_time().c_str());
	}
}

static void update_controllers()
{
    int num_gamepads = 0;
    SDL_JoystickID* available = SDL_GetGamepads(&num_gamepads);
    // A null array means enumeration failed; keep existing handles and retry.
    if (!available) return;

    // A transient SDL_OpenGamepad failure leaves an empty slot, but the
    // *count* may stay unchanged. Also detect a replaced/reordered instance.
    bool reopen = num_gamepads != static_cast<int>(controllers.size());
    if (!reopen && available)
    {
        for (int i = 0; i < num_gamepads; ++i)
        {
            SDL_Gamepad* opened = controllers[i].controller;
            if (!opened || !SDL_GamepadConnected(opened) ||
                SDL_GetGamepadID(opened) != available[i])
            {
                reopen = true;
                break;
            }
        }
    }
    SDL_free(available); // SDL_GetGamepads returns a caller-owned array.

    if (reopen)
    {
        if (debug) SDL_Log("%s: Controller binding changed or incomplete; rescan.\n",
            get_date_time().c_str());
        close_controllers();
        add_controllers();
    }
}

static int find_controller(SDL_JoystickID controller_id)
{
	for (int i = 0; i < controllers.size(); ++i)
	{
        if (controllers[i].controller &&
            controller_id == SDL_GetGamepadID(controllers[i].controller))
		{
			return i;
		}
	}
	return -1;
}

// All low-frequency button events are useful when diagnosing unusual
// controllers. Analog axes, trigger motion and sensors are never logged.
static void debug_log_gamepad_button(const SDL_Event& event, const char* direction)
{
    if (!debug) return;
    const int index = find_controller(event.gbutton.which);
    const char* button_name = SDL_GetGamepadStringForButton(
        static_cast<SDL_GamepadButton>(event.gbutton.button));
    SDL_Log("%s: Gamepad %d (id=%u) button %s (%d) %s.\n",
        get_date_time().c_str(), index + 1,
        static_cast<unsigned int>(event.gbutton.which),
        button_name ? button_name : "unknown",
        static_cast<int>(event.gbutton.button), direction);
}

static void initialize_sdl()
{
	if (sdl_initialized)
	{
		// Reinitialize SDL
		close_controllers();
		SDL_Quit();
		sdl_initialized = false;
	}

	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

	sdl_initialized = SDL_Init(SDL_INIT_GAMEPAD);

	if (!sdl_initialized)
	{
		//SDL_SetError("Test error.");
		const char* error_msg = SDL_GetError();
		const size_t size = strlen(error_msg) + 1;
		wchar_t error_msg2[] = L"";
		mbstowcs(error_msg2, error_msg, size);
		fatal_error((L"Could not initialize SDL: " + std::wstring(error_msg2)).c_str());
	}
}

static void key_down(int code)
{
	if (code == 901 || code == 903 || code == 904)
	{
		// Take screenshot / open file / toggle mute/unmute on button release only
		return;
	}
	else if (code == 905)
	{
		set_mute(false);
		return;
	}

	// Source: https://stackoverflow.com/a/76579368
	//UINT key = MapVirtualKey(code, MAPVK_VK_TO_VSC_EX); // Convert virtual key code to scan code (with extended support)
	WORD scanCode = LOWORD(MapVirtualKey(code, MAPVK_VK_TO_VSC_EX)); // Convert virtual key code to scan code (with extended support)

	INPUT inp{};
	inp.type = INPUT_KEYBOARD;

	inp.ki.wVk = 0; // virtual key code, doing scan codes instead
	inp.ki.wScan = LOBYTE(scanCode); // hardware scan code for key
	inp.ki.dwFlags = KEYEVENTF_SCANCODE;

	if ((HIBYTE(scanCode) & 0xE0) != 0) { // Check whether it is an extended key
		inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
	}

	inp.ki.time = 0;
	inp.ki.dwExtraInfo = 0;
	
	SendInput(1, &inp, sizeof(INPUT));
}

static void key_up(int code)
{
	if (code == 901)
	{
		take_screenshot();
		return;
	}
	else if (code == 903)
	{
		ShellExecute(NULL, L"open", file_location.c_str(), NULL, NULL, SW_SHOWNORMAL);
		return;
	}
	else if (code == 904)
	{
		toggle_mute();
		return;
	}
	else if (code == 905)
	{
		set_mute(true);
		return;
	}

	// Source: https://stackoverflow.com/a/76579368
	//UINT key = MapVirtualKey(code, MAPVK_VK_TO_VSC_EX); // Convert virtual key code to scan code (with extended support)
	WORD scanCode = LOWORD(MapVirtualKey(code, MAPVK_VK_TO_VSC_EX)); // Convert virtual key code to scan code (with extended support)

	INPUT inp{};
	inp.type = INPUT_KEYBOARD;

	inp.ki.wVk = 0; // virtual key code, doing scan codes instead
	inp.ki.wScan = LOBYTE(scanCode); // hardware scan code for key
	inp.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;

	if ((HIBYTE(scanCode) & 0xE0) != 0) { // Check whether it is an extended key
		inp.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
	}

	inp.ki.time = 0;
	inp.ki.dwExtraInfo = 0;
	
	SendInput(1, &inp, sizeof(INPUT));
}

static void key_tap(std::vector<int> code, int64_t delay, int64_t duration)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(delay));

	for (std::size_t i = 0; i < code.size(); ++i)
	{
		key_down(code[i]);
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(duration));

	for (std::size_t i = 0; i < code.size(); ++i)
	{
		key_up(code[i]);
	}
}

// Send a true modifier+key chord; release in reverse order so Windows does
// not interpret a detached Windows/Alt key press.
static void key_chord(int modifier, int key)
{
    key_down(modifier);
    key_down(key);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    key_up(key);
    key_up(modifier);
}

// Read a named action or an ordered virtual-key chord from config.ini.
// A typo must not make the application crash or trigger an unintended shortcut.
static XboxActionBinding load_xbox_click_action(const wchar_t* action_name,
    const wchar_t* key_name, const wchar_t* default_action)
{
    const std::wstring raw_action = ini.getString(L"XboxMultiClick", action_name, default_action);
    std::vector<int> keys;
    bool keys_read_ok = true;
    if (normalize_xbox_action(raw_action) == L"keys")
    {
        // INI.getString cannot retrieve a lone numeric INI value, so handle
        // both integer (e.g. 65) and string (e.g. 17,16,27) cases explicitly.
        std::wstring raw_keys = ini.getString(L"XboxMultiClick", key_name, L"");
        if (raw_keys.empty())
        {
            bool was_integer = false;
            const auto single_key = ini.getInteger(L"XboxMultiClick", key_name, 0, &was_integer);
            if (was_integer) raw_keys = std::to_wstring(single_key);
        }
        keys = parse_xbox_key_codes(raw_keys, &keys_read_ok);
    }
    bool valid = false;
    XboxActionBinding binding = parse_xbox_action(raw_action, keys, &valid);
    if (!valid || !keys_read_ok)
    {
        // Invalid configuration is disabled instead of silently executing a default.
        SDL_Log("Invalid XboxMultiClick action/keycodes for %ls; action disabled.\n", action_name);
        return {};
    }
    return binding;
}

// Uses the original key_down/key_up scan-code implementation, including
// correct reverse-order modifier release (e.g. Ctrl+Shift+Esc).
static void key_combo(const std::vector<int>& keys)
{
    for (int key : keys) key_down(key);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) key_up(*it);
}

static void execute_xbox_multi_click_action(XboxClickAction action)
{
    const XboxActionBinding* binding = nullptr;
    const char* label = "";
    switch (action)
    {
    case XboxClickAction::single:
        binding = &xbox_multi_click_settings.single_click_action;
        label = "single";
        break;
    case XboxClickAction::double_click:
        binding = &xbox_multi_click_settings.double_click_action;
        label = "double";
        break;
    case XboxClickAction::triple:
        binding = &xbox_multi_click_settings.triple_click_action;
        label = "triple";
        break;
    default:
        return;
    }
    if (binding->kind == XboxActionKind::none) return;
    if (debug) SDL_Log("%s: Xbox %s-click action.\n", get_date_time().c_str(), label);

    switch (binding->kind)
    {
    case XboxActionKind::screenshot:
        take_screenshot(); // Native screenshot, same as original 901 action.
        break;
    case XboxActionKind::gamebar:
        key_chord(VK_LWIN, 'G');
        break;
    case XboxActionKind::alt_tab:
        key_chord(VK_MENU, VK_TAB);
        break;
    case XboxActionKind::win_tab:
        key_chord(VK_LWIN, VK_TAB);
        break;
    case XboxActionKind::open_file:
        ShellExecute(NULL, L"open", file_location.c_str(), NULL, NULL, SW_SHOWNORMAL);
        break;
    case XboxActionKind::toggle_mic:
        toggle_mute();
        break;
    case XboxActionKind::keys:
        key_combo(binding->keys);
        break;
    case XboxActionKind::none:
        break;
    }
}

// Send one down/up pair together: avoids leaving Delete held after Task View
// disappears. An OS focus transition is still possible between probing and
// input delivery, so the feature is OPT-IN and fail-closed on uncertainty.
static bool send_task_view_delete()
{
    INPUT keys[2]{};
    keys[0].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = VK_DELETE;
    keys[1] = keys[0];
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput(2, keys, sizeof(INPUT)) == 2;
}

static void handle_task_view_x_down(int controller_index)
{
    if (!task_view_x_enabled) return;

    // Only a currently visible, foreground, known Task View shell window may
    // activate the shortcut. Re-check immediately before injection.
    const TaskViewProbe first = probe_task_view_foreground();
    if (!first.confirmed)
    {
        if (debug) SDL_Log("%s: TaskViewX BLOCKED outside verified Task View (class=%ls, process=%ls, thread=%ls).\n",
            get_date_time().c_str(), first.window_class.c_str(),
            first.executable.c_str(), first.thread_description.c_str());
        return;
    }

    const TaskViewProbe second = probe_task_view_foreground();
    if (!second.confirmed || second.foreground != first.foreground)
    {
        if (debug) SDL_Log("%s: TaskViewX BLOCKED: Task View focus changed during verification.\n",
            get_date_time().c_str());
        return;
    }

    if (task_view_x_dry_run)
    {
        if (debug) SDL_Log("%s: TaskViewX DRY RUN: X would send Delete in verified Task View (controller=%d).\n",
            get_date_time().c_str(), controller_index + 1);
        return;
    }

    const bool sent = send_task_view_delete();
    if (debug) SDL_Log("%s: TaskViewX %s: verified Task View, controller=%d.\n",
        get_date_time().c_str(), sent ? "Delete sent" : "SendInput failed",
        controller_index + 1);
}

// Console I/O in a Windows GUI App
// Maximum number of lines the output console should have
static const WORD MAX_CONSOLE_LINES = 500;

static void RedirectIOToConsole()
{
	int hConHandle;
	long lStdHandle;
	CONSOLE_SCREEN_BUFFER_INFO coninfo;
	FILE* fp;

	// allocate a console for this app
	AllocConsole();

	SetConsoleTitle(szProgramName);

	// set the screen buffer to be big enough to let us scroll text
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &coninfo);
	coninfo.dwSize.Y = MAX_CONSOLE_LINES;
	SetConsoleScreenBufferSize(GetStdHandle(STD_OUTPUT_HANDLE), coninfo.dwSize);

	// redirect unbuffered STDOUT to the console
	lStdHandle = (int64_t)GetStdHandle(STD_OUTPUT_HANDLE);
	hConHandle = _open_osfhandle(lStdHandle, _O_TEXT);
	fp = _fdopen(hConHandle, "w");
	*stdout = *fp;
	setvbuf(stdout, NULL, _IONBF, 0);

	// redirect unbuffered STDIN to the console
	lStdHandle = (int64_t)GetStdHandle(STD_INPUT_HANDLE);
	hConHandle = _open_osfhandle(lStdHandle, _O_TEXT);
	fp = _fdopen(hConHandle, "r");
	*stdin = *fp;
	setvbuf(stdin, NULL, _IONBF, 0);

	// redirect unbuffered STDERR to the console
	lStdHandle = (int64_t)GetStdHandle(STD_ERROR_HANDLE);
	hConHandle = _open_osfhandle(lStdHandle, _O_TEXT);
	fp = _fdopen(hConHandle, "w");
	*stderr = *fp;
	setvbuf(stderr, NULL, _IONBF, 0);

	// make cout, wcout, cin, wcin, wcerr, cerr, wclog, and clog point to console as well
	ios::sync_with_stdio();
}

static void loop()
{
	SDL_Event event;
	int i; // Controller ID
	KeySettings settings;
	//const Uint8* keyboard = SDL_GetKeyboardState(NULL);

	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
			case SDL_EVENT_GAMEPAD_ADDED:
			case SDL_EVENT_GAMEPAD_REMOVED:
				update_controllers();

				break;

			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                debug_log_gamepad_button(event, "DOWN");
				if (event.gbutton.button == SDL_GAMEPAD_BUTTON_MISC1 || event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)
                    {
                        i = find_controller(event.gbutton.which);
                        if (i < 0) break; // A queued event may belong to a removed gamepad.

                        const bool xbox = event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE;
                        if (xbox && xbox_multi_click_settings.enabled &&
                            !count_gamepad_guide_down(xbox_input_source))
                        {
                            // In raw-joystick mode, counting this mapped event as well
                            // would produce duplicate clicks for one physical press.
                            break;
                        }

                        if (xbox && xbox_multi_click_settings.enabled)
                        {
                            // Count a physical/virtual Xbox click at DOWN time only.
                            // Some third-party controllers emit a tap command but do not
                            // produce an observable UP event for every invocation.
                            const XboxClickAction click_action = controllers[i].xbox_multi_click.on_press(
                                static_cast<int64_t>(event.gbutton.timestamp / 1000000ULL),
                                xbox_multi_click_settings.multi_click_interval_ms,
                                xbox_multi_click_settings.down_debounce_ms);
                            if (debug)
                            {
                                SDL_Log("%s: Controller %d Xbox DOWN, pending clicks=%d.\n",
                                    get_date_time().c_str(), i + 1,
                                    controllers[i].xbox_multi_click.pending_count());
                            }
                            execute_xbox_multi_click_action(click_action);
                            break;
                        }

                        settings = xbox ? controllers[i].settings.xbox : controllers[i].settings.share;
                        if (settings.hold_mode == 1)
                        {
                            for (int key : settings.key) key_down(key);
                        }
                        else if (settings.hold_mode == 2)
                        {
                            controllers[i].button_depressed = hrc::now();
                        }
                    }

				break;

			case SDL_EVENT_GAMEPAD_BUTTON_UP:
                debug_log_gamepad_button(event, "UP");
				if (event.gbutton.button == SDL_GAMEPAD_BUTTON_MISC1 || event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE)
                    {
                        i = find_controller(event.gbutton.which);
                        if (i < 0) break;

                        const bool xbox = event.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE;
                        if (xbox && xbox_multi_click_settings.enabled)
                        {
                            // Multi-click deliberately ignores ALL UP events.
                            // A release must never be required to count a click.
                            break;
                        }

                        settings = xbox ? controllers[i].settings.xbox : controllers[i].settings.share;
                        if (settings.hold_mode == 1)
                        {
                            for (int key : settings.key) key_up(key);
                        }
                        else if (settings.hold_mode == 2)
                        {
                            int64_t ms = (std::chrono::duration_cast<std::chrono::milliseconds>(hrc::now() - controllers[i].button_depressed)).count();
                            if (ms >= settings.longpress_duration)
                            {
                                if (!settings.longpress_key.empty())
                                {
                                    key_tap(settings.longpress_key, settings.delay, settings.duration);
                                }
                            }
                            else
                            {
                                if (!settings.key.empty())
                                {
                                    key_tap(settings.key, settings.delay, settings.duration);
                                }
                            }
                        }
                    }

				break;

			/*case SDL_EVENT_KEY_DOWN:
				if (debug)
				{
					SDL_Log("%s: Keyboard key pressed.\n", get_date_time().c_str());
				}

				break;

			case SDL_EVENT_KEY_UP:
				if (debug)
				{
					SDL_Log("%s: Keyboard key released.\n", get_date_time().c_str());
				}

				break;*/

            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
            {
                const bool is_down = event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN;
                const int index = find_controller(event.jbutton.which);
                if (debug)
                {
                    SDL_Log("%s: Joystick %d (id=%u) raw button %d %s.\n",
                        get_date_time().c_str(), index + 1,
                        static_cast<unsigned int>(event.jbutton.which),
                        static_cast<int>(event.jbutton.button), is_down ? "DOWN" : "UP");
                }
                if (index >= 0 && task_view_x_enabled &&
                    static_cast<int>(event.jbutton.button) == task_view_x_raw_button)
                {
                    // SDL receives its own copy of the event; the physical X
                    // input is NOT forwarded, blocked, delayed or re-injected.
                    // This is a supplementary one-shot Delete on the first
                    // physical DOWN only; UP re-arms the next press.
                    TaskViewXPressGate& x_gate = controllers[index].task_view_x_press;
                    if (!is_down)
                        x_gate.on_up();
                    else if (x_gate.on_down())
                        handle_task_view_x_down(index);
                }
                if (is_down && index >= 0 && xbox_multi_click_settings.enabled &&
                    count_joystick_xbox_down(xbox_input_source,
                        static_cast<int>(event.jbutton.button), xbox_raw_joystick_button))
                {
                    // Same state machine as the mapped GUIDE path: DOWN only.
                    const XboxClickAction click_action = controllers[index].xbox_multi_click.on_press(
                        static_cast<int64_t>(event.jbutton.timestamp / 1000000ULL),
                        xbox_multi_click_settings.multi_click_interval_ms,
                        xbox_multi_click_settings.down_debounce_ms);
                    if (debug)
                    {
                        SDL_Log("%s: Controller %d Xbox RAW button %d DOWN, pending clicks=%d.\n",
                            get_date_time().c_str(), index + 1, xbox_raw_joystick_button,
                            controllers[index].xbox_multi_click.pending_count());
                    }
                    execute_xbox_multi_click_action(click_action);
                }
                break;
            }

			case SDL_EVENT_GAMEPAD_REMAPPED:
				if (debug)
				{
					SDL_Log("%s: Mapping was updated for Controller %d.\n", get_date_time().c_str(), find_controller(event.gdevice.which) + 1);
				}

				break;
			
			case SDL_EVENT_JOYSTICK_ADDED:
				if (debug)
				{
					SDL_Log("%s: Controller connected.\n", get_date_time().c_str());
				}

				break;

			case SDL_EVENT_JOYSTICK_REMOVED:
				if (debug)
				{
					SDL_Log("%s: Controller disconnected.\n", get_date_time().c_str());
				}

				break;

			default:
				// Never dump unhandled SDL events in debug mode.
				// Analog stick/trigger axis motion and sensors generate huge volumes
				// of events; they are still polled, just not printed.
				break;
		}
	}

    // SDL event timestamps and SDL_GetTicksNS share the same monotonic time base.
    // Resolve single/double clicks from the last DOWN timestamp without relying
    // on button releases. Do not block the event loop waiting for more clicks.
    if (xbox_multi_click_settings.enabled)
    {
        const int64_t now_ms = static_cast<int64_t>(SDL_GetTicksNS() / 1000000ULL);
        for (Controller& gamepad : controllers)
        {
            execute_xbox_multi_click_action(gamepad.xbox_multi_click.on_tick(
                now_ms, xbox_multi_click_settings.multi_click_interval_ms));
        }
    }
}

// Variables
HANDLE hMutex;
HINSTANCE hCurrentInstance = NULL;
HWND hWnd;
MSG msg; // Messages to the application are saved here
NOTIFYICONDATA notifyIconData;
const UINT WM_NOTIFYICON = WM_APP + 1;
HMENU hMenu;

// Forward declarations of functions
// Procedures
LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);

static void AddNotificationIcon(HWND hWnd)
{
	//NOTIFYICONDATA notifyIconData = { sizeof(notifyIconData) };
	//notifyIconData.cbSize = sizeof(notifyIconData);
	notifyIconData.cbSize = sizeof(NOTIFYICONDATA);
	notifyIconData.hWnd = hWnd;
	// Add the icon, setting the icon, tooltip, and callback message
	// The icon will be identified with the uID
	notifyIconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
	notifyIconData.uID = ID_TRAY_APP_ICON;
	notifyIconData.uCallbackMessage = WM_NOTIFYICON;
	LoadIconMetric(hCurrentInstance, MAKEINTRESOURCE(IDI_ICON), LIM_SMALL, &notifyIconData.hIcon);
	// This text will be shown as the icon's tooltip
	//LoadString(hCurrentInstance, szProgramName, notifyIconData.szTip, ARRAYSIZE(notifyIconData.szTip));
	//wcsncpy(notifyIconData.szTip, szProgramName, sizeof(szProgramName));
	StringCchCopy(notifyIconData.szTip, ARRAYSIZE(notifyIconData.szTip), szProgramName);
	Shell_NotifyIcon(NIM_ADD, &notifyIconData);

	// NOTIFYICON_VERSION_4 is preferred
	notifyIconData.uVersion = NOTIFYICON_VERSION_4;
	Shell_NotifyIcon(NIM_SETVERSION, &notifyIconData);

	/*hMenu = CreatePopupMenu();
	AppendMenu(hMenu, MF_STRING, ID_TRAY_EXIT, L"Exit");*/
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ PWSTR pCmdLine, _In_ int nCmdShow)
{
	//HRESULT hr = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
	winrt::init_apartment(); // Init COM

	//MFStartup(MF_VERSION);

	// Try to create the mutex
	hMutex = CreateMutex(NULL, FALSE, szProgramName);

	if (hMutex == NULL)
	{
		// Can not create application mutex
		fatal_error(L"Error starting the program.");
	}
	else if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		// The mutex exists so this is the second instance so return
		message(L"Program is already running.");
		return 0;
	}

	// This is the handle for the window
	hCurrentInstance = hInstance;

	WNDCLASSEX wcex = { sizeof(wcex) };
	//wcex.style = CS_HREDRAW | CS_VREDRAW;
	wcex.lpfnWndProc = WindowProc;
	wcex.hInstance = hInstance;
	//wcex.hIcon = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_NOTIFICATIONICON));
	//wcex.hCursor = LoadCursor(NULL, IDC_ARROW);
	//wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	//wcex.lpszMenuName = pszMenuName;
	wcex.lpszClassName = szProgramName;
	RegisterClassEx(&wcex);

	HWND hWnd = CreateWindow(szProgramName, L"", 0, 0, 0, 0, 0, NULL, NULL, hCurrentInstance, NULL);

	if (!hWnd)
	{
		return 0;
	}

	// Main

	// Initialize SDL
	initialize_sdl();

	std::wstring path = get_exe_path();

	if (!ini.loadFile(path + L"\\config.ini"))
	{
		error(L"Could not load config.ini. Using defaults.");
	}

    input_poll_interval_ms = static_cast<int>(std::clamp<int64_t>(
        ini.getInteger(L"settings", L"poll_interval_ms", 2), 1, 50));
    const std::wstring input_source = normalize_xbox_action(ini.getString(
        L"XboxMultiClick", L"input_source", L"joystick"));
    xbox_input_source = input_source == L"joystick"
        ? XboxInputSource::joystick : XboxInputSource::gamepad;
    xbox_raw_joystick_button = static_cast<int>(std::clamp<int64_t>(
        ini.getInteger(L"XboxMultiClick", L"joystick_button", 10), 0, 255));
    xbox_multi_click_settings.enabled = ini.getInteger(L"XboxMultiClick", L"enabled", 1) == 1;
    xbox_multi_click_settings.multi_click_interval_ms = std::clamp<int64_t>(
        ini.getInteger(L"XboxMultiClick", L"multi_click_interval_ms", 350), 50, 5000);
    xbox_multi_click_settings.down_debounce_ms = std::clamp<int64_t>(
        ini.getInteger(L"XboxMultiClick", L"down_debounce_ms", 0), 0, 250);
    xbox_multi_click_settings.single_click_action = load_xbox_click_action(
        L"single_click_action", L"single_click_keys", L"screenshot");
    xbox_multi_click_settings.double_click_action = load_xbox_click_action(
        L"double_click_action", L"double_click_keys", L"gamebar");
    xbox_multi_click_settings.triple_click_action = load_xbox_click_action(
        L"triple_click_action", L"triple_click_keys", L"win_tab");

// Safety opt-in: must explicitly enable this new feature, and then
// explicitly switch out of dry-run after checking the debug logs.
task_view_x_enabled = ini.getInteger(L"TaskViewX", L"enabled", 0) == 1;
task_view_x_dry_run = ini.getInteger(L"TaskViewX", L"dry_run", 1) == 1;
task_view_x_raw_button = static_cast<int>(std::clamp<int64_t>(
    ini.getInteger(L"TaskViewX", L"joystick_button", 2), 0, 255));
// A shared raw button would make X actions ambiguous: fail closed.
if (task_view_x_enabled && xbox_multi_click_settings.enabled &&
    xbox_input_source == XboxInputSource::joystick &&
    task_view_x_raw_button == xbox_raw_joystick_button)
{
    SDL_Log("WARNING: TaskViewX shares the Xbox raw button; TaskViewX disabled.\n");
    task_view_x_enabled = false;
}

	debug = ini.getInteger(L"settings", L"debug", 0) == 1;

	if (debug)
	{
		RedirectIOToConsole();
        SDL_Log("%s: Reading configuration from %ls\n", get_date_time().c_str(),
            (path + L"\\config.ini").c_str());
		SDL_Log("%s: Debug mode enabled. Poll=%d ms, Xbox source=%s, raw button=%d.\n",
            get_date_time().c_str(), input_poll_interval_ms,
            xbox_input_source == XboxInputSource::gamepad ? "gamepad" : "joystick",
            xbox_raw_joystick_button);
SDL_Log("%s: TaskViewX: enabled=%d, dry_run=%d, joystick X button=%d; strict foreground detection.\n",
    get_date_time().c_str(), task_view_x_enabled ? 1 : 0,
    task_view_x_dry_run ? 1 : 0, task_view_x_raw_button);
        SDL_Log("%s: Screenshot location: %ls\n", get_date_time().c_str(),
            ini.getString(L"screen_capture", L"location", L"C:\\Screenshots").c_str());
	}

    // Opt-in by default for multi-click mode. This preserves keyboard Win+G.
    // To restore the previous OS preference, first set this INI key to 0,
    // then run the bundled restoration script (and restart/sign out of Windows).
    const bool disable_system_gamebar_button = ini.getInteger(
        L"XboxMultiClick", L"disable_system_gamebar_button", 1) == 1;
    if (xbox_multi_click_settings.enabled && disable_system_gamebar_button)
    {
        const LONG status = suppress_windows_gamebar_controller_button();
        if (status != ERROR_SUCCESS)
            SDL_Log("%s: WARNING: Could not disable the native Xbox->Game Bar shortcut (Win32 error %ld).\n",
                get_date_time().c_str(), status);
        else if (debug)
            SDL_Log("%s: Native Xbox->Game Bar controller shortcut disabled (Win+G remains available).\n",
                get_date_time().c_str());
    }

	file_location = ini.getString(L"settings", L"file_location", L"C:\\Windows\\explorer.exe");

	file_key = ini.getInteger(L"settings", L"file_key", 0);

	if (file_key != 0)
	{
		// Convert virtual key code to scan code (with extended support) and back to ensure correct mapping
		UINT file_key_vsc = MapVirtualKey((unsigned int) file_key, MAPVK_VK_TO_VSC_EX);
		UINT file_key_vk = MapVirtualKey(file_key_vsc, MAPVK_VSC_TO_VK_EX);

		RegisterHotKey(hWnd, OpenFileHotKeyID, MOD_NOREPEAT, file_key_vk);
	}

	captures_location = ini.getString(L"screen_capture", L"location", L"C:\\Screenshots");

	screenshot_key = ini.getInteger(L"screen_capture", L"screenshot_key", 0);

	if (screenshot_key != 0)
	{
		// Convert virtual key to scan code (with extended support) and back to ensure correct mapping
		UINT screenshot_key_vsc = MapVirtualKey((unsigned int) screenshot_key, MAPVK_VK_TO_VSC_EX);
		UINT screenshot_key_vk = MapVirtualKey(screenshot_key_vsc, MAPVK_VSC_TO_VK_EX);

		RegisterHotKey(hWnd, ScreenshotHotKeyID, MOD_NOREPEAT, screenshot_key_vk);
	}

	capture_border = ini.getInteger(L"screen_capture", L"border", 1) == 1;
	capture_cursor = ini.getInteger(L"screen_capture", L"cursor", 0) == 1;

	while (true)
	{
		// Main message loop
		// Will get messages until queue is clear
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);

			if (msg.message == WM_QUIT)
			{
				return 0;
			}
			else if (msg.message == WM_HOTKEY)
			{
				if (msg.wParam == ScreenshotHotKeyID)
				{
					if (debug)
					{
						SDL_Log("%s: Screenshot HotKey pressed.\n", get_date_time().c_str());
					}

					take_screenshot();
				}
				else if (msg.wParam == OpenFileHotKeyID)
				{
					if (debug)
					{
						SDL_Log("%s: Open file HotKey pressed.\n", get_date_time().c_str());
					}

					ShellExecute(NULL, L"open", file_location.c_str(), NULL, NULL, SW_SHOWNORMAL);
				}
			}
		}

		// Do a tick of any intensive stuff here such as graphics processing
		loop();

        // Preserve the original XInput reconnect probe, but run it only once
        // per second instead of blocking every 2 ms input polling cycle.
        static Uint64 last_xinput_probe_ns = 0;
        const Uint64 now_ns = SDL_GetTicksNS();
        if (now_ns - last_xinput_probe_ns >= 1000000000ULL)
        {
            // Also retry SDL handles after startup/temporary open failures.
            // This is intentionally low-frequency to preserve input latency.
            update_controllers();
            XINPUT_CAPABILITIES capabilities{};
            XInputGetCapabilities(0, XINPUT_FLAG_GAMEPAD, &capabilities);
            last_xinput_probe_ns = now_ns;
        }

		/*if (!Shell_NotifyIcon(NIM_MODIFY, &notifyIconData))
		{
			AddNotificationIcon(hWnd);
		}*/

        // Synthetic GUIDE taps can last less than the old 50 ms polling
        // interval. SDL_DelayPrecise gives short pulses a much better chance
        // of being sampled; neither event stream requires a button release.
        SDL_DelayPrecise(static_cast<Uint64>(input_poll_interval_ms) * 1000000ULL);
	}

	return msg.wParam;
}

// This function is called by DispatchMessage()
LRESULT CALLBACK WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	static UINT s_uTaskbarRestart;

	// Handle the messages
	switch (uMsg)
	{
		case WM_CREATE:
		{
			s_uTaskbarRestart = RegisterWindowMessage(L"TaskbarCreated");
			AddNotificationIcon(hWnd);
		}
		break;

		case WM_COMMAND:
		{
			int const wmId = LOWORD(wParam);
			// Parse the menu selections:
			switch (wmId)
			{
				case IDM_EXIT:
				{
					DestroyWindow(hWnd);
					break;
				}

				default:
				{
					return DefWindowProc(hWnd, uMsg, wParam, lParam);
				}
			}
		}
		break;

		case WM_NOTIFYICON:
		{
			/*if (lParam == WM_RBUTTONDOWN)
			{
				// Get current mouse cursor position
				POINT curPoint;
				GetCursorPos(&curPoint);
				SetForegroundWindow(hWnd);

				// TrackPopupMenu blocks the app until TrackPopupMenu returns
				UINT clicked = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, curPoint.x, curPoint.y, 0, hWnd, NULL);

				if (clicked == ID_TRAY_EXIT)
				{
					// Quit the application
					Shell_NotifyIcon(NIM_DELETE, &notifyIconData);
					PostQuitMessage(0);
				}
			}*/

			switch (LOWORD(lParam))
			{
				case WM_CONTEXTMENU:
					POINT const pt = { LOWORD(wParam), HIWORD(wParam) };
					HMENU hMenu = LoadMenu(hCurrentInstance, MAKEINTRESOURCE(IDR_CONTEXTMENU));

					if (hMenu)
					{
						HMENU hSubMenu = GetSubMenu(hMenu, 0);

						if (hSubMenu)
						{
							// The window must be foreground before calling TrackPopupMenu or the menu will not disappear when the user clicks away
							SetForegroundWindow(hWnd);

							// Respect menu drop alignment
							UINT uFlags = TPM_RIGHTBUTTON;
							if (GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0)
							{
								uFlags |= TPM_RIGHTALIGN;
							}
							else
							{
								uFlags |= TPM_LEFTALIGN;
							}

							//TrackPopupMenu(hSubMenu, uFlags, pt.x, pt.y, 0, hWnd, NULL);
							TrackPopupMenuEx(hSubMenu, uFlags, pt.x, pt.y, hWnd, NULL);
						}

						DestroyMenu(hMenu);
					}

					break;
			}
		}
		break;

		case WM_DESTROY:
		{
			// Quit the application

			CoUninitialize();

			//MFShutdown();

			if (file_key != 0)
			{
				UnregisterHotKey(hWnd, OpenFileHotKeyID);
			}

			if (screenshot_key != 0)
			{
				UnregisterHotKey(hWnd, ScreenshotHotKeyID);
			}

			if (screen_capture_initialized)
			{
				device = NULL;
				toneMapper = NULL;

				screen_capture_initialized = false;
			}

			if (input_device_initialized)
			{
				pEnumerator->Release();
				pDevice->Release();
				g_pEndptVol->Release();

				input_device_initialized = false;
			}

			close_controllers();

			//SDL_Quit();
			atexit(SDL_Quit);
			sdl_initialized = false;

			// Remove the tray icon

			/*NOTIFYICONDATA notifyIconData2;

			notifyIconData2.cbSize = sizeof(NOTIFYICONDATA);
			notifyIconData2.hWnd = hWnd;
			notifyIconData2.uID = ID_TRAY_APP_ICON;*/

			Shell_NotifyIcon(NIM_DELETE, &notifyIconData);

			PostQuitMessage(0);

			if (hMutex != NULL)
			{
				// The application is closing so release and close the mutex
				ReleaseMutex(hMutex);
				CloseHandle(hMutex);
			}
		}
		break;

		default:
		{
			if (uMsg == s_uTaskbarRestart)
			{
				AddNotificationIcon(hWnd);
			}

			return DefWindowProc(hWnd, uMsg, wParam, lParam);
		}
	}

	return 0;
}
