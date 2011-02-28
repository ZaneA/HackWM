//
// HackWM
// Copyright 2010-2011, Zane Ashby, http://github.com/ZaneA/HackWM
//

//
// Includes
//

#define WIN32_LEAN_AND_MEAN
#define WINVER			0x0501
#define _WIN32_WINNT		0x0501
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>


#include "lua/include/lua.h"
#include "lua/include/lauxlib.h"


//
// Defines
//

// Helpers
#define strcmp_end(search, input) !(strlen(input) >= strlen(search) && !strcmp(search, input + strlen(input) - strlen(search)))
#define strcmp_start(search, input) !strncmp(search, input, strlen(search))

// Timers
#define TIMER_CLOSESTATUS	1
#define TIMER_QUIT		2

// Status Window
#define WINDOW_NAME		"HackWM"
#define WINDOW_STYLE		WS_POPUP & ~WS_BORDER
#define WINDOW_STYLE_EX		WS_EX_NOACTIVATE | WS_EX_TOPMOST
#define SetStatus(fmt, args...) SetStatusExt(TRUE, fmt, ## args)
#define SetStatusS(fmt, args...) SetStatusExt(FALSE, fmt, ## args)

// Command Handlers
#define COM_UNHANDLED		0
#define COM_HANDLED		1
#define COM_QUIT		2


//
// Structures
//

typedef struct {
        HWND hwnd;

        void *next;
} window_t;

typedef struct {
        // if cursor falls within these values
        // the window should be put into this group?
        int x, y, width, height;
        int master_count;
        int margin;

        window_t *first_window;

        void *next;
} group_t;

// Status stuff
struct {
        HWND hwnd;

        char text[128];
        char input[48];

        int padding;
        char font[32];
        int font_size;
        BOOL font_bold;

        int activate_key;
} status;


//
// Globals
//

group_t *groups = NULL;

UINT shell_message_id = 0;

HHOOK key_hook = NULL;

lua_State *lvm = NULL;

char tile_function[128];


//
// Functions
//

// Print to Debug Console
void printd(char *fmt, ...)
{
        char line[256];

        // Build new status using variable arguments
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(line, sizeof(line), fmt, ap);
        va_end(ap);

        // Get a console handle
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out) { // if found an open console, then write to it
		SetConsoleTitle("HackWM Debug Console");
                DWORD written;
                WriteConsole(out, line, strlen(line), &written, NULL);
        }
}

// Read from Debug Console
char *readd()
{
	static char line[256];
	int charsRead = 0;

	memset(line, 0, sizeof(line));

	HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
	if (in) { // Found input handle
		ReadConsole(in, &line, sizeof(line), &charsRead, NULL);
	}

	return &line;
}

char *WindowTitle(HWND hwnd)
{
	static char title[256];
	GetWindowText(hwnd, title, sizeof(title));
	return &title;
}

static int Lua_MessageBox(lua_State *lvm)
{
	char *text = lua_tostring(lvm, 1);
	MessageBox(NULL, text, text, MB_OK);
	return 0;
}

static int Lua_printd(lua_State *lvm)
{
	printd("%s\n", lua_tostring(lvm, 1));
	return 0;
}

static int Lua_readd(lua_State *lvm)
{
	lua_pushstring(lvm, readd());
	return 1;
}

static int Lua_SetStatus(lua_State *lvm)
{
	SetStatus("%s", lua_tostring(lvm, 1));
	return 0;
}

static int Lua_SetStatusS(lua_State *lvm)
{
	SetStatusS("%s", lua_tostring(lvm, 1));
	return 0;
}

// Initialize Lua Interpreter
static BOOL CreateLua()
{
        lvm = lua_open();

	lua_register(lvm, "MessageBox", Lua_MessageBox);
	lua_register(lvm, "printd", Lua_printd);
	lua_register(lvm, "readd", Lua_printd);
	lua_register(lvm, "SetStatus", Lua_SetStatus);
	lua_register(lvm, "SetStatusS", Lua_SetStatusS);

        return TRUE;
}

// Load and run a Lua script
static BOOL LuaRunFile(char *path)
{
        if (!lvm) { return FALSE; }

        char real_path[MAX_PATH];
        GetCurrentDirectory(MAX_PATH, real_path);
        strncat(real_path, "\\", sizeof(real_path)-strlen(real_path)-1);
        strncat(real_path, path, sizeof(real_path)-strlen(real_path)-1);

        int s = luaL_dofile(lvm, real_path);
        if (s != 0) {
                printd("Error in lua script: %s\n", lua_tostring(lvm, 1));
                lua_pop(lvm, 1);

                return FALSE;
        }

        return TRUE;
}

static BOOL LuaEval(char *code)
{
	int error = luaL_loadbuffer(lvm, code, strlen(code), "eval") || lua_pcall(lvm, 0, LUA_MULTRET, 0);

	if (error) {
		SetStatus("Error: %s", lua_tostring(lvm, -1));
		lua_pop(lvm, 1);
		return FALSE;
	}

	return TRUE;
}

// Destroy Lua Interpreter
static void DestroyLua()
{
        lua_close(lvm);
}

// Set status text and open the status window
void SetStatusExt(BOOL close, char *text, ...)
{
	// Clear status
	memset(&status.text, 0, sizeof(status.text));

	// Build new status using variable arguments
	va_list ap;
	va_start(ap, text);
	vsnprintf(status.text, sizeof(status.text), text, ap);
	va_end(ap);

	ShowWindow(status.hwnd, SW_SHOW);

	// Invalidate so it repaints with new text
	InvalidateRect(status.hwnd, NULL, TRUE);

	if (close) {
		// Set timer for it to blend out
		SetTimer(status.hwnd, TIMER_CLOSESTATUS, 2000, NULL);
	}
}

static BOOL IsGoodWindow(HWND hwnd)
{
        if (hwnd == status.hwnd) return FALSE; // Don't want to tile the status window

        if (IsWindowVisible(hwnd) && (GetParent(hwnd) == 0)) {
                int exstyle = GetWindowLong(hwnd, GWL_EXSTYLE);
                HWND owner = GetWindow(hwnd, GW_OWNER);

                if ((((exstyle & WS_EX_TOOLWINDOW) == 0)
                       && (owner == 0))
                       || ((exstyle & WS_EX_APPWINDOW)
                            && (owner != 0))) {
                        return TRUE;
                }
        }

        return FALSE;
}

// Insert a window into a group
static BOOL InsertWindow(group_t *groupnode, HWND hwnd)
{
        if (!groupnode || !hwnd) { return FALSE; }

        if (!IsGoodWindow(hwnd)) { return FALSE; }

        window_t *node = (window_t*)calloc(1, sizeof(window_t));

        node->hwnd = hwnd;
        node->next = groupnode->first_window;

        groupnode->first_window = node;

	printd("%i (%s) Inserted into group\n", hwnd, WindowTitle(hwnd));

        return TRUE;
}

// Remove a window from a group
static BOOL RemoveWindow(group_t *groupnode, HWND hwnd)
{
        if (!groupnode || !hwnd) { return FALSE; }

        window_t *last = NULL;
        window_t *node = groupnode->first_window;

        for (; node; last = node, node = node->next) {
                if (node->hwnd == hwnd) {
                        if (last != NULL) {
                                last->next = node->next;
                        } else {
                                groupnode->first_window = node->next;
                        }

                        free(node);

			printd("%i (%s) Removed from group\n", hwnd, WindowTitle(hwnd));

                        return TRUE; // Found and removed
                }
        }

        return FALSE; // Didn't remove anything
}

// Find a window in a group and return a handle to it
static window_t* FindWindowInGroup(group_t *groupnode, HWND hwnd)
{
        if (!groupnode || !hwnd) { return NULL; }

        for (window_t *node = groupnode->first_window; node; node = node->next) {
                if (node->hwnd == hwnd) {
                        return node;
                }
        }

        return NULL;
}

// Find a window across groups
static window_t* FindWindowInAll(HWND hwnd)
{
        // Loop through groups and windows and return group
        for (group_t *groupnode = groups; groupnode; groupnode = groupnode->next) {
                window_t* win = FindWindowInGroup(groupnode, hwnd);
                if (win) return win;
        }

        return NULL;
}

// Clears text from status window and closes it
static void CloseStatus() {
        // Clear status
        memset(&status.text, 0, sizeof(status.text));
        memset(&status.input, 0, sizeof(status.input));

        // Repaint
        InvalidateRect(status.hwnd, NULL, TRUE);

        // Kill blend out timer
        KillTimer(status.hwnd, TIMER_CLOSESTATUS);

        ShowWindow(status.hwnd, SW_HIDE);
}

// Draw status window
static void DrawStatus()
{
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(status.hwnd, &ps);
        HFONT font = CreateFont(status.font_size, 0, 0, 0, status.font_bold ? FW_BOLD : 0, 0, 0, 0, 0, 0, 0, 0, 0, status.font);
        SelectObject(hdc, font);

        RECT rc;
        GetClientRect(status.hwnd, &rc);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));

        DrawText(hdc, status.text, -1, &rc, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOCLIP);

        DeleteObject(font);

        HPEN newPen = CreatePen(PS_SOLID, 3, RGB(0, 0, 0));
        HPEN oldPen = SelectObject(hdc, newPen);
        HBRUSH oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

        Rectangle(hdc, 0, 0, rc.right, rc.bottom);

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(newPen);

        EndPaint(status.hwnd, &ps);
}

static int LuaGetNumber(char *name, int def)
{
        lua_getglobal(lvm, name);
        int number = lua_tonumber(lvm, -1);
        lua_pop(lvm, 1);
        return number;
}

static BOOL LuaGetBool(char *name, BOOL def)
{
        lua_getglobal(lvm, name);
        BOOL boolean = lua_toboolean(lvm, -1);
        lua_pop(lvm, 1);
        return boolean;
}

static void LuaPutNumber(char *name, int num)
{
        lua_pushnumber(lvm, num);
        lua_setglobal(lvm, name);
}

// Load settings from lua file
static void LoadSettings()
{
	LuaRunFile("config.lua");

	status.padding = LuaGetNumber("status_padding", 10);
        status.font_size = LuaGetNumber("status_font_size", 16);
        status.font_bold = LuaGetBool("status_font_bold", TRUE);
        status.activate_key = LuaGetNumber("status_activate_key", VK_CAPITAL);

        // Strings are a bit more involved
        memset(status.font, 0, sizeof(status.font));
        lua_getglobal(lvm, "status_font");
        if (lua_isstring(lvm, -1)) {
                strncpy(status.font, lua_tostring(lvm, -1), sizeof(status.font));
                lua_pop(lvm, 1);
        } else {
                strncpy(status.font, "Calibri", sizeof(status.font));
        }

        memset(tile_function, 0, sizeof(tile_function));
        lua_getglobal(lvm, "tile_function");
        if (lua_isstring(lvm, -1)) {
                strncpy(tile_function, lua_tostring(lvm, -1), sizeof(tile_function));
                lua_pop(lvm, 1);
        } else {
                strncpy(tile_function, "plugins/vertical-stack.lua", sizeof(tile_function));
        }
}

// Get number of windows in group
static int GetWindowCount(group_t *group)
{
        int a = 0; // Window count

        for (window_t *node = group->first_window; node; node = node->next, a++) {
                if (IsIconic(node->hwnd) || IsZoomed(node->hwnd)) { a--; }
        }

        return a;
}

// Tile windows
static BOOL TileWindowsInGroup(group_t *group)
{
        int a = GetWindowCount(group);

        int i = 0;
        for (window_t *node = group->first_window; node; node = node->next, i++) {
                if (IsIconic(node->hwnd) || IsZoomed(node->hwnd)) {
			printd("%i (%s) is Fullscreen or Minimized, skipping tiling..\n", node->hwnd, WindowTitle(node->hwnd));
			i--;
			continue;
		}

                LuaPutNumber("a", a);
                LuaPutNumber("i", i);
                LuaPutNumber("screen_width", group->width);
                LuaPutNumber("screen_height", group->height);
                LuaPutNumber("margin", group->margin);
                LuaPutNumber("master_count", group->master_count + 1);

                LuaRunFile(tile_function);

                int x = LuaGetNumber("x", 0);
                int y = LuaGetNumber("y", 0);
                int width = LuaGetNumber("width", group->width);
                int height = LuaGetNumber("height", group->height);

                printd("%i (%s) tiled at: %i %i %i %i\n", node->hwnd, WindowTitle(node->hwnd), x, y, width, height);

                //ShowWindow(node->hwnd, SW_RESTORE);
                SetWindowPos(node->hwnd, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
                //SetForegroundWindow(node->hwnd);
        }

        return TRUE;
}

// Check input for patterns and perform actions if necessary
static int ScanInput()
{
        if (!strcmp("Q, Q", status.input)) { // Close HackWM
                SetStatus("Bye bye!");
                SetTimer(status.hwnd, TIMER_QUIT, 1000, NULL);
                return COM_QUIT;
        }

        if (!strcmp("Q, R", status.input)) { // Reload Settings
                LoadSettings();
                return COM_HANDLED;
        }

        if (!strcmp("Q, D", status.input)) { // Open/Close Debug Console
                static BOOL debug = FALSE;
                (debug = !debug) ? AllocConsole() : FreeConsole();
                return COM_HANDLED;
        }

	if (!strcmp("Q, E", status.input)) { // Evaluate Lua
		printd("Lua: ");
		LuaEval(readd());
		return COM_HANDLED;
	}

        if (!strcmp("M, H", status.input)) { // Increase Master Area Count
                groups->master_count++;
                TileWindowsInGroup(groups);
                return COM_HANDLED;
        }

        if (!strcmp("M, L", status.input)) { // Decrease Master Area Count
                groups->master_count--;
                if (groups->master_count < 0) groups->master_count = 0;
                TileWindowsInGroup(groups);
                return COM_HANDLED;
        }

        if (!strcmp("M, K", status.input)) { // Increase Margin
                groups->margin -= 20;
                TileWindowsInGroup(groups);
                return COM_HANDLED;
        }

        if (!strcmp("M, J", status.input)) { // Decrease Margin
                groups->margin += 20;
                TileWindowsInGroup(groups);
                return COM_HANDLED;
        }

        return COM_UNHANDLED;
}

// Main Keyhook logic
static int HandleKeyPress(PKBDLLHOOKSTRUCT key)
{
        static BOOL status_open = FALSE;

        if (status_open) {
                if (key->vkCode == VK_ESCAPE || key->vkCode == status.activate_key) {
                        status_open = FALSE;
                        CloseStatus();
                        return 1;
                }

                if ((key->vkCode > 31) && (key->vkCode < 127)) {
                        if (strlen(status.input) > 0) strncat(status.input, ", ", sizeof(status.input)-strlen(status.input)-1);

                        if (GetKeyState(VK_CONTROL) < 0) { strncat(status.input, "C-", sizeof(status.input)-strlen(status.input)-1); }
                        else if (GetKeyState(VK_SHIFT) < 0) { strncat(status.input, "S-", sizeof(status.input)-strlen(status.input)-1); }
                        else if (GetKeyState(VK_LWIN) < 0) { strncat(status.input, "W-", sizeof(status.input)-strlen(status.input)-1); }
                        else if (key->flags & LLKHF_ALTDOWN) { strncat(status.input, "M-", sizeof(status.input)-strlen(status.input)-1); }

                        char keyChar[3] = { key->vkCode, '\0' };
                        strncat(status.input, keyChar, sizeof(status.input)-strlen(status.input)-1);

                        if (GetKeyState(VK_CONTROL) < 0 && key->vkCode == 'G') {
                                status_open = FALSE;
                                CloseStatus();
                                return 1;
                        }

                        int ret;
                        if ((ret = ScanInput()) > 0) {
                                status_open = FALSE;

                                if (ret == 1) {
                                        CloseStatus();
                                }

                                return 1;
                        }

                        SetStatus("%s", status.input);
                        KillTimer(status.hwnd, TIMER_CLOSESTATUS); // HACK Don't close Status Window since we're awaiting input
                }

                return 1;
        } else if (key->vkCode == status.activate_key) {
                SetStatus("");
                KillTimer(status.hwnd, TIMER_CLOSESTATUS); // HACK Don't close Status Window since we're awaiting input
                status_open = TRUE;

                return 1;
        }

        return 0;
}

// Minimal KeyboardProc that passes keypresses off to keyhook logic
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam)
{
        if (nCode < 0) {
                return CallNextHookEx(key_hook, nCode, wParam, lParam);
        }

        if (nCode == HC_ACTION) {
                switch (wParam)
                {
                        case WM_KEYDOWN:
                        case WM_SYSKEYDOWN:
                                return HandleKeyPress((PKBDLLHOOKSTRUCT)lParam);
                }
        }

        return CallNextHookEx(key_hook, nCode, wParam, lParam);
}

// Cleanup stuff!
static void Cleanup()
{
        DeregisterShellHookWindow(status.hwnd);

        // Release group and window structures
        group_t *group = groups;

        while (group) {
                window_t *window = group->first_window;

                while (window) {
                        window_t *temp = window;
                        window = temp->next;
                        free(temp);
                }

                group_t *temp = group;
                group = temp->next;
                free(temp);
        }

        DestroyLua();

        DestroyWindow(status.hwnd);
}

BOOL proc(HWND hwnd, LPARAM user)
{
	InsertWindow(groups, hwnd);
	return TRUE;
}

void SetupExistingWindows()
{
	EnumChildWindows(GetDesktopWindow(), proc, 0);
	TileWindowsInGroup(groups);
}

// Handle window messages, including shell messages :D
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
        // We got a shell message!
        if (msg == shell_message_id) {
                HWND message_hwnd = (HWND)lParam;

                switch (wParam) // Type
                {
                        case HSHELL_WINDOWCREATED:
                                if (InsertWindow(groups, message_hwnd)) {
                                        TileWindowsInGroup(groups);
                                }
                                break;

                        case HSHELL_WINDOWDESTROYED:
                                if (RemoveWindow(groups, message_hwnd)) {
                                        TileWindowsInGroup(groups);
                                }
                                break;

                        case HSHELL_GETMINRECT:
                                TileWindowsInGroup(groups);
                                break;

                        case HSHELL_WINDOWACTIVATED:
                                printd("%i (%s) Activated\n", message_hwnd, WindowTitle(message_hwnd));
                                break;

                        case HSHELL_ACTIVATESHELLWINDOW:
                                SetStatus("Hey there!");
                                break;
                }

                return 0;
        }

        // Normal window messages
        switch (msg)
        {
                case WM_CREATE:
                        break;

                case WM_CLOSE:
                        Cleanup();
                        break;

                case WM_DESTROY:
                        PostQuitMessage(WM_QUIT);
                        break;

                case WM_PAINT:
                        DrawStatus();
                        break;

                case WM_TIMER:
                        switch (wParam) {
                                case TIMER_CLOSESTATUS:
                                        CloseStatus();
                                        break;

                                case TIMER_QUIT:
                                        KillTimer(hwnd, TIMER_QUIT);
                                        PostMessage(hwnd, WM_CLOSE, 0, 0);
                                        break;
                        }
                        break;

                default:
                        return DefWindowProc(hwnd, msg, wParam, lParam);
        }

        return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
        MSG msg = { 0 };

        WNDCLASSEX winClass = {
                .cbSize = sizeof(WNDCLASSEX),
                .lpfnWndProc = WndProc,
                .hInstance = hInstance,
                .hCursor = LoadCursor(NULL, IDC_ARROW),
                .hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH),
                .lpszClassName = WINDOW_NAME
        };

        // Fatal, this shouldn't happen though
        if (!RegisterClassEx(&winClass)) {
                MessageBox(NULL, "Failed to Register Window Class", "Error", MB_OK | MB_ICONERROR);
                return 0;
        }

        if (!CreateLua()) {
                MessageBox(NULL, "Failed to initialize Lua VM", "Error", MB_OK | MB_ICONERROR);
                return 0;
        }

        LoadSettings();

        // TODO Create as many groups as there are Monitors * wanted workspaces?
        groups = (group_t*)calloc(1, sizeof(group_t));

        // For Window position
        RECT rc;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &rc, 0);
        groups->x = rc.left;
        groups->y = rc.top;
        groups->width = rc.right;
        groups->height = rc.bottom;

        status.hwnd = CreateWindowEx(WINDOW_STYLE_EX, WINDOW_NAME, WINDOW_NAME, WINDOW_STYLE, groups->x, groups->y, groups->width, 24, NULL, NULL, hInstance, NULL);

        // Mostly fatal, although it could be possible to try again with a MESSAGE only window..
        if (!status.hwnd) {
                MessageBox(NULL, "Failed to Create Status Window", "Error", MB_OK | MB_ICONERROR);
                return 0;
        }

        // Fatal, there would be no way to control HashTWM
        if (!(key_hook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, hInstance, 0))) {
                MessageBox(NULL, "Failed to set keyboard Hook", "Error", MB_OK | MB_ICONERROR);
                return 0;
        }

        // Get address of RegisterShellHookWindow
        typedef BOOL (*RegisterShellHookWindowProc) (HWND);
        RegisterShellHookWindowProc RegisterShellHookWindow =
                (RegisterShellHookWindowProc)GetProcAddress(GetModuleHandle("USER32.DLL"), "RegisterShellHookWindow");

        if (!RegisterShellHookWindow) {
                MessageBox(NULL, "Unable to find RegisterShellHookWindow in USER32.DLL\r\nAutomatic Tiling will not work", "Warning", MB_OK | MB_ICONWARNING);
                return 0;
        }

        RegisterShellHookWindow(status.hwnd);

        shell_message_id = RegisterWindowMessage("SHELLHOOK"); 

	SetupExistingWindows();

        // Pump messages
        while (GetMessage(&msg, NULL, 0, 0) > 0)
        {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
        }

        return msg.wParam;
}
