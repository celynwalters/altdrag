/*
	Copyright (C) 2010  Stefan Sundin (recover89@gmail.com)
	
	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0501

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <windows.h>
#include <shlwapi.h>
#include <commctrl.h>

//App
#define APP_NAME L"AltDrag"
#define STICKY_THRESHOLD 20
#define AERO_THRESHOLD   5

//Timers
HWND g_hwnd;
#define UNLOCK_TIMER WM_APP+1

//Some variables must be shared so that CallWndProc hooks can access them
#define shareattr __attribute__((section ("shared"), shared))

//Enumerators
enum action {ACTION_NOTHING, ACTION_MOVE, ACTION_RESIZE, ACTION_MINIMIZE, ACTION_CENTER, ACTION_ALWAYSONTOP, ACTION_CLOSE};
enum button {BUTTON_NONE, BUTTON_LMB, BUTTON_MMB, BUTTON_RMB, BUTTON_MB4, BUTTON_MB5};
enum resize {RESIZE_NONE, RESIZE_TOP, RESIZE_RIGHT, RESIZE_BOTTOM, RESIZE_LEFT, RESIZE_CENTER};

//Window database
#define NUMWNDDB 30
struct wnddata {
	HWND hwnd;
	int width;
	int height;
};
struct {
	struct wnddata items[NUMWNDDB];
	struct wnddata *pos;
} wnddb;

//State
struct {
	HWND hwnd;
	int alt;
	unsigned int clicktime;
	POINT offset;
	struct {
		enum resize x, y;
	} resize;
	int blockaltup;
	int updatecount;
	int locked;
	int aero;
	struct wnddata *wndentry;
	struct {
		HMONITOR monitor;
		int maximized;
		int width;
		int height;
	} origin;
} state;

struct {
	int shift;
	int move;
	int resize;
} sharedstate shareattr = {0, 0, 0};

//Sticky
RECT *monitors = NULL;
int nummonitors = 0;
RECT *wnds = NULL;
int numwnds = 0;
HWND progman = NULL;

//Settings
struct {
	int AutoStick;
	int Autofocus;
	int Aero;
	int AutoRemaximize;
	struct {
		int Cursor;
		int UpdateRate;
	} Performance;
	struct {
		unsigned char *keys;
		int length;
	} Hotkeys;
	struct {
		enum action LMB, MMB, RMB, MB4, MB5;
	} Mouse;
} sharedsettings shareattr;
int sharedsettings_loaded shareattr = 0;
wchar_t inipath[MAX_PATH] shareattr;

//Blacklist
struct blacklistitem {
	wchar_t *title;
	wchar_t *classname;
};
struct blacklist {
	struct blacklistitem *items;
	int length;
	wchar_t *data;
};
struct {
	struct blacklist Blacklist;
	struct blacklist Blacklist_Sticky;
	struct blacklist Whitelist_Sticky;
} settings = {{NULL,0}, {NULL,0}, {NULL,0}};

//Cursor data
HWND cursorwnd shareattr = NULL;
HCURSOR cursor[6] shareattr;
enum {HAND, SIZENWSE, SIZENESW, SIZENS, SIZEWE, SIZEALL} resizecursor;

//Mousehook data
HINSTANCE hinstDLL = NULL;
HHOOK mousehook = NULL;

//Msghook data
BOOL subclassed = FALSE;
enum {MOVE, RESIZE, NONE} msgaction = NONE;

//Error()
#ifdef DEBUG
#define ERROR_WRITETOFILE
#include "include/error.c"
#else
#define Error(a,b,c,d,e)
#endif

//Blacklist
int blacklisted(HWND hwnd, struct blacklist *list) {
	wchar_t title[256], classname[256];
	GetWindowText(hwnd, title, sizeof(title)/sizeof(wchar_t));
	GetClassName(hwnd, classname, sizeof(classname)/sizeof(wchar_t));
	int i;
	for (i=0; i < list->length; i++) {
		if ((list->items[i].title == NULL && !wcscmp(classname,list->items[i].classname))
		 || (list->items[i].classname == NULL && !wcscmp(title,list->items[i].title))
		 || (list->items[i].title != NULL && list->items[i].classname != NULL && !wcscmp(title,list->items[i].title) && !wcscmp(classname,list->items[i].classname))) {
			return 1;
		}
	}
	return 0;
}

//Check if action is bound to the button
int IsButton(int button, int action) {
	if ((button == BUTTON_LMB && sharedsettings.Mouse.LMB == action)
	 || (button == BUTTON_MMB && sharedsettings.Mouse.MMB == action)
	 || (button == BUTTON_RMB && sharedsettings.Mouse.RMB == action)
	 || (button == BUTTON_MB4 && sharedsettings.Mouse.MB4 == action)
	 || (button == BUTTON_MB5 && sharedsettings.Mouse.MB5 == action)) {
		return 1;
	}
	return 0;
}

//Check if key is assigned
int IsHotKey(int key) {
	int i;
	for (i=0; i < sharedsettings.Hotkeys.length; i++) {
		if (key == sharedsettings.Hotkeys.keys[i]) {
			return 1;
		}
	}
	return 0;
}

//Enumerate
int monitors_alloc = 0;
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
	//Make sure we have enough space allocated
	if (nummonitors == monitors_alloc) {
		monitors_alloc++;
		monitors = realloc(monitors, monitors_alloc*sizeof(RECT));
		if (monitors == NULL) {
			Error(L"realloc(monitors)", L"Out of memory?", -1, TEXT(__FILE__), __LINE__);
			return FALSE;
		}
	}
	//Add monitor
	monitors[nummonitors++] = *lprcMonitor;
	return TRUE;
}

int wnds_alloc = 0;
BOOL CALLBACK EnumWindowsProc(HWND window, LPARAM lParam) {
	//Make sure we have enough space allocated
	if (numwnds == wnds_alloc) {
		wnds_alloc += 20;
		wnds = realloc(wnds, wnds_alloc*sizeof(RECT));
		if (wnds == NULL) {
			Error(L"realloc(wnds)", L"Out of memory?", -1, TEXT(__FILE__), __LINE__);
			return FALSE;
		}
	}
	
	//Only store window if it's visible, not minimized to taskbar, not the window we are dragging and not blacklisted
	RECT wnd;
	if (window != state.hwnd && window != progman
	 && IsWindowVisible(window) && !IsIconic(window)
	 && !blacklisted(window,&settings.Blacklist_Sticky)
	 && (GetWindowLongPtr(window,GWL_STYLE)&WS_CAPTION || blacklisted(window,&settings.Whitelist_Sticky))
	 && GetWindowRect(window,&wnd) != 0
	) {
		//Maximized?
		if (IsZoomed(window)) {
			//Get monitor size
			HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
			MONITORINFO monitorinfo;
			monitorinfo.cbSize = sizeof(MONITORINFO);
			GetMonitorInfo(monitor, &monitorinfo);
			RECT mon = monitorinfo.rcMonitor;
			//Crop this window so that it does not exceed the size of the monitor
			//I do this since the window has an extra, invisible, border when maximized (a border that stretches onto other monitors)
			wnd.left = (wnd.left < mon.left)?mon.left:wnd.left;
			wnd.top = (wnd.top < mon.top)?mon.top:wnd.top;
			wnd.right = (wnd.right > mon.right)?mon.right:wnd.right;
			wnd.bottom = (wnd.bottom > mon.bottom)?mon.bottom:wnd.bottom;
		}
		
		//Return if this window is overlapped by another window
		int i;
		for (i=0; i < numwnds; i++) {
			if (wnd.left >= wnds[i].left && wnd.top >= wnds[i].top && wnd.right <= wnds[i].right && wnd.bottom <= wnds[i].bottom) {
				return TRUE;
			}
		}
		
		//Add window
		wnds[numwnds++] = wnd;
		
		//Use this to print the title and classname of the windows that are stickable
		/*FILE *f = fopen("C:\\altdrag-log.txt", "ab");
		char title[100], classname[100];
		GetWindowTextA(window, title, 100);
		GetClassNameA(window, classname, 100);
		fprintf(f, "window: %s|%s\n", title, classname);
		fclose(f);*/
	}
	return TRUE;
}

void Enum() {
	//Update handle to progman
	if (!IsWindow(progman)) {
		progman = FindWindow(L"Progman", L"Program Manager");
	}
	
	//Enumerate monitors
	nummonitors = 0;
	EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
	
	//Enumerate windows
	numwnds = 0;
	if (sharedstate.shift || sharedsettings.AutoStick > 0) {
		HWND taskbar = FindWindow(L"Shell_TrayWnd", NULL);
		RECT wnd;
		if (taskbar != NULL && GetWindowRect(taskbar,&wnd) != 0) {
			wnds[numwnds++] = wnd;
		}
	}
	if (sharedstate.shift || sharedsettings.AutoStick >= 2) {
		EnumWindows(EnumWindowsProc, 0);
	}
	
	//Use this to print the monitors and windows
	/*FILE *f = fopen("C:\\altdrag-log.txt", "wb");
	fprintf(f, "nummonitors: %d\n", nummonitors);
	int k;
	for (k=0; k < nummonitors; k++) {
		fprintf(f, "mon #%02d: left %d, top %d, right %d, bottom %d\n", k, monitors[k].left, monitors[k].top, monitors[k].right, monitors[k].bottom);
	}
	fprintf(f, "\n");
	fprintf(f, "numwnds: %d\n", numwnds);
	//char title[100], classname[100];
	for (k=0; k < numwnds; k++) {
		//GetWindowTextA(wnds[k], title, 100);
		//GetClassNameA(wnds[k], classname, 100);
		//RECT wnd;
		//GetWindowRect(wnds[k], &wnd);
		//fprintf(f, "wnd #%02d: %s [%s] (%dx%d @ %dx%d)\n", k, title, classname, wnd.right-wnd.left, wnd.bottom-wnd.top, wnd.left, wnd.top);
		fprintf(f, "wnd #%02d: %dx%d @ %dx%d\n", k, wnds[k].right-wnds[k].left, wnds[k].bottom-wnds[k].top, wnds[k].left, wnds[k].top);
	}
	fclose(f);*/
}

void MoveStick(int *posx, int *posy, int wndwidth, int wndheight) {
	//Enumerate monitors and windows
	Enum();
	
	//thresholdx and thresholdy will shrink to make sure the dragged window will stick to the closest windows
	int i, j, thresholdx, thresholdy, stuckx=0, stucky=0, stickx=0, sticky=0;
	thresholdx = thresholdy = STICKY_THRESHOLD;
	//Loop monitors and windows
	for (i=0, j=0; i < nummonitors || j < numwnds; ) {
		RECT stickywnd;
		int stickinside;
		
		//Get stickywnd
		if (i < nummonitors) {
			stickywnd = monitors[i];
			stickinside = 1;
			i++;
		}
		else if (j < numwnds) {
			stickywnd = wnds[j];
			stickinside = (sharedstate.shift || sharedsettings.AutoStick != 2);
			j++;
		}
		
		//Check if posx sticks
		if ((stickywnd.top-thresholdx < *posy && *posy < stickywnd.bottom+thresholdx)
		 || (*posy-thresholdx < stickywnd.top && stickywnd.top < *posy+wndheight+thresholdx)) {
			int stickinside_cond = (stickinside || *posy+wndheight-thresholdx < stickywnd.top || stickywnd.bottom < *posy+thresholdx);
			if (*posx-thresholdx < stickywnd.right && stickywnd.right < *posx+thresholdx) {
				//The left edge of the dragged window will stick to this window's right edge
				stuckx = 1;
				stickx = stickywnd.right;
				thresholdx = stickywnd.right-*posx;
			}
			else if (stickinside_cond && *posx+wndwidth-thresholdx < stickywnd.right && stickywnd.right < *posx+wndwidth+thresholdx) {
				//The right edge of the dragged window will stick to this window's right edge
				stuckx = 1;
				stickx = stickywnd.right-wndwidth;
				thresholdx = stickywnd.right-(*posx+wndwidth);
			}
			else if (stickinside_cond && *posx-thresholdx < stickywnd.left && stickywnd.left < *posx+thresholdx) {
				//The left edge of the dragged window will stick to this window's left edge
				stuckx = 1;
				stickx = stickywnd.left;
				thresholdx = stickywnd.left-*posx;
			}
			else if (*posx+wndwidth-thresholdx < stickywnd.left && stickywnd.left < *posx+wndwidth+thresholdx) {
				//The right edge of the dragged window will stick to this window's left edge
				stuckx = 1;
				stickx = stickywnd.left-wndwidth;
				thresholdx = stickywnd.left-(*posx+wndwidth);
			}
		}
		
		//Check if posy sticks
		if ((stickywnd.left-thresholdy < *posx && *posx < stickywnd.right+thresholdy)
		 || (*posx-thresholdy < stickywnd.left && stickywnd.left < *posx+wndwidth+thresholdy)) {
			int stickinside_cond = (stickinside || *posx+wndwidth-thresholdy < stickywnd.left || stickywnd.right < *posx+thresholdy);
			if (*posy-thresholdy < stickywnd.bottom && stickywnd.bottom < *posy+thresholdy) {
				//The top edge of the dragged window will stick to this window's bottom edge
				stucky = 1;
				sticky = stickywnd.bottom;
				thresholdy = stickywnd.bottom-*posy;
			}
			else if (stickinside_cond && *posy+wndheight-thresholdy < stickywnd.bottom && stickywnd.bottom < *posy+wndheight+thresholdy) {
				//The bottom edge of the dragged window will stick to this window's bottom edge
				stucky = 1;
				sticky = stickywnd.bottom-wndheight;
				thresholdy = stickywnd.bottom-(*posy+wndheight);
			}
			else if (stickinside_cond && *posy-thresholdy < stickywnd.top && stickywnd.top < *posy+thresholdy) {
				//The top edge of the dragged window will stick to this window's top edge
				stucky = 1;
				sticky = stickywnd.top;
				thresholdy = stickywnd.top-*posy;
			}
			else if (*posy+wndheight-thresholdy < stickywnd.top && stickywnd.top < *posy+wndheight+thresholdy) {
				//The bottom edge of the dragged window will stick to this window's top edge
				stucky = 1;
				sticky = stickywnd.top-wndheight;
				thresholdy = stickywnd.top-(*posy+wndheight);
			}
		}
	}
	
	//Update posx and posy
	if (stuckx) {
		*posx = stickx;
	}
	if (stucky) {
		*posy = sticky;
	}
}

void ResizeStick(int *posx, int *posy, int *wndwidth, int *wndheight) {
	//Enumerate monitors and windows
	Enum();
	
	//thresholdx and thresholdy will shrink to make sure the dragged window will stick to the closest windows
	int i, j, thresholdx, thresholdy, stuckleft=0, stucktop=0, stuckright=0, stuckbottom=0, stickleft=0, sticktop=0, stickright=0, stickbottom=0;
	thresholdx = thresholdy = STICKY_THRESHOLD;
	//Loop monitors and windows
	for (i=0, j=0; i < nummonitors || j < numwnds; ) {
		RECT stickywnd;
		int stickinside;
		
		//Get stickywnd
		if (i < nummonitors) {
			stickywnd = monitors[i];
			stickinside = 1;
			i++;
		}
		else if (j < numwnds) {
			stickywnd = wnds[j];
			stickinside = (sharedstate.shift || sharedsettings.AutoStick != 2);
			j++;
		}
		
		//Check if posx sticks
		if ((stickywnd.top-thresholdx < *posy && *posy < stickywnd.bottom+thresholdx)
		 || (*posy-thresholdx < stickywnd.top && stickywnd.top < *posy+*wndheight+thresholdx)) {
			int stickinside_cond = (stickinside || *posy+*wndheight-thresholdx < stickywnd.top || stickywnd.bottom < *posy+thresholdx);
			if (state.resize.x == RESIZE_LEFT && *posx-thresholdx < stickywnd.right && stickywnd.right < *posx+thresholdx) {
				//The left edge of the dragged window will stick to this window's right edge
				stuckleft = 1;
				stickleft = stickywnd.right;
				thresholdx = stickywnd.right-*posx;
			}
			else if (stickinside_cond && state.resize.x == RESIZE_RIGHT && *posx+*wndwidth-thresholdx < stickywnd.right && stickywnd.right < *posx+*wndwidth+thresholdx) {
				//The right edge of the dragged window will stick to this window's right edge
				stuckright = 1;
				stickright = stickywnd.right;
				thresholdx = stickywnd.right-(*posx+*wndwidth);
			}
			else if (stickinside_cond && state.resize.x == RESIZE_LEFT && *posx-thresholdx < stickywnd.left && stickywnd.left < *posx+thresholdx) {
				//The left edge of the dragged window will stick to this window's left edge
				stuckleft = 1;
				stickleft = stickywnd.left;
				thresholdx = stickywnd.left-*posx;
			}
			else if (state.resize.x == RESIZE_RIGHT && *posx+*wndwidth-thresholdx < stickywnd.left && stickywnd.left < *posx+*wndwidth+thresholdx) {
				//The right edge of the dragged window will stick to this window's left edge
				stuckright = 1;
				stickright = stickywnd.left;
				thresholdx = stickywnd.left-(*posx+*wndwidth);
			}
		}
		
		//Check if posy sticks
		if ((stickywnd.left-thresholdy < *posx && *posx < stickywnd.right+thresholdy)
		 || (*posx-thresholdy < stickywnd.left && stickywnd.left < *posx+*wndwidth+thresholdy)) {
			int stickinside_cond = (stickinside || *posx+*wndwidth-thresholdy < stickywnd.left || stickywnd.right < *posx+thresholdy);
			if (state.resize.y == RESIZE_TOP && *posy-thresholdy < stickywnd.bottom && stickywnd.bottom < *posy+thresholdy) {
				//The top edge of the dragged window will stick to this window's bottom edge
				stucktop = 1;
				sticktop = stickywnd.bottom;
				thresholdy = stickywnd.bottom-*posy;
			}
			else if (stickinside_cond && state.resize.y == RESIZE_BOTTOM && *posy+*wndheight-thresholdy < stickywnd.bottom && stickywnd.bottom < *posy+*wndheight+thresholdy) {
				//The bottom edge of the dragged window will stick to this window's bottom edge
				stuckbottom = 1;
				stickbottom = stickywnd.bottom;
				thresholdy = stickywnd.bottom-(*posy+*wndheight);
			}
			else if (stickinside_cond && state.resize.y == RESIZE_TOP && *posy-thresholdy < stickywnd.top && stickywnd.top < *posy+thresholdy) {
				//The top edge of the dragged window will stick to this window's top edge
				stucktop = 1;
				sticktop = stickywnd.top;
				thresholdy = stickywnd.top-*posy;
			}
			else if (state.resize.y == RESIZE_BOTTOM && *posy+*wndheight-thresholdy < stickywnd.top && stickywnd.top < *posy+*wndheight+thresholdy) {
				//The bottom edge of the dragged window will stick to this window's top edge
				stuckbottom = 1;
				stickbottom = stickywnd.top;
				thresholdy = stickywnd.top-(*posy+*wndheight);
			}
		}
	}
	
	//Update pos, posy, wndwidth and widthheight
	if (stuckleft) {
		*wndwidth = *wndwidth+*posx-stickleft;
		*posx = stickleft;
	}
	if (stucktop) {
		*wndheight = *wndheight+*posy-sticktop;
		*posy = sticktop;
	}
	if (stuckright) {
		*wndwidth = stickright-*posx;
	}
	if (stuckbottom) {
		*wndheight = stickbottom-*posy;
	}
}

#ifdef _WIN64

//x64 keyhook needs only to check when the shift key is depressed
__declspec(dllexport) LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION) {
		int vkey = ((PKBDLLHOOKSTRUCT)lParam)->vkCode;
		
		if (vkey == VK_LSHIFT || vkey == VK_RSHIFT) {
			if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
				sharedstate.shift = 1;
			}
			else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
				sharedstate.shift = 0;
			}
		}
	}
	
	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

#else

BOOL IncUpdateRate() {
	if (sharedsettings.Performance.UpdateRate) {
		state.updatecount = (state.updatecount+1)%sharedsettings.Performance.UpdateRate;
	}
	return (sharedsettings.Performance.UpdateRate && state.updatecount==0)?TRUE:FALSE;
}

void MouseMove() {
	//Make sure we got something to do
	if (state.locked || (!sharedstate.move && !sharedstate.resize)) {
		return;
	}
	
	//Check if window still exists
	if (!IsWindow(state.hwnd)) {
		sharedstate.move = 0;
		sharedstate.resize = 0;
		UnhookMouse();
		return;
	}
	
	//Double check if the shift is being pressed
	if (sharedstate.shift && !(GetAsyncKeyState(VK_SHIFT)&0x8000)) {
		sharedstate.shift = 0;
	}
	
	//Get state
	int maximized = IsZoomed(state.hwnd);
	WINDOWPLACEMENT wndpl;
	wndpl.length = sizeof(WINDOWPLACEMENT);
	GetWindowPlacement(state.hwnd, &wndpl);
	RECT wnd = wndpl.rcNormalPosition;
	
	//Get new position for window
	POINT pt;
	GetCursorPos(&pt);
	int posx, posy, wndwidth, wndheight;
	if (sharedstate.move) {
		posx = pt.x-state.offset.x;
		posy = pt.y-state.offset.y;
		wndwidth = wnd.right-wnd.left;
		wndheight = wnd.bottom-wnd.top;
		
		//Get monitor
		HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
		
		if (sharedsettings.Aero) {
			//Get monitor info
			MONITORINFO monitorinfo;
			monitorinfo.cbSize = sizeof(MONITORINFO);
			GetMonitorInfo(monitor, &monitorinfo);
			RECT mon = monitorinfo.rcWork;
			RECT fmon = monitorinfo.rcMonitor;
			
			//Restore window?
			if (maximized && (pt.y > fmon.top+AERO_THRESHOLD
			 || ((fmon.left < pt.x && pt.x < fmon.left+2*AERO_THRESHOLD)
			  || (fmon.right-2*AERO_THRESHOLD < pt.x && pt.x < fmon.right)))) {
				//Restore window
				wndpl.showCmd = SW_RESTORE;
				SetWindowPlacement(state.hwnd, &wndpl);
			}
			
			//Move window
			if (fmon.left <= pt.x && pt.x < fmon.left+2*AERO_THRESHOLD
			 && fmon.top <= pt.y && pt.y < fmon.top+2*AERO_THRESHOLD) {
				//Topleft
				state.aero = 1;
				wndwidth = (mon.right-mon.left)/2;
				wndheight = (mon.bottom-mon.top)/2;
				posx = mon.left;
				posy = mon.top;
			}
			else if (fmon.right-2*AERO_THRESHOLD < pt.x && pt.x < fmon.right
			      && fmon.top <= pt.y && pt.y < fmon.top+2*AERO_THRESHOLD) {
				//Topright
				state.aero = 1;
				wndwidth = (mon.right-mon.left)/2;
				wndheight = (mon.bottom-mon.top)/2;
				posx = mon.right-wndwidth;
				posy = mon.top;
			}
			else if (fmon.left <= pt.x && pt.x < fmon.left+2*AERO_THRESHOLD
			      && fmon.bottom-2*AERO_THRESHOLD < pt.y && pt.y < fmon.bottom) {
				//Bottomleft
				state.aero = 1;
				wndwidth = (mon.right-mon.left)/2;
				wndheight = (mon.bottom-mon.top)/2;
				posx = mon.left;
				posy = mon.bottom-wndheight;
			}
			else if (fmon.right-2*AERO_THRESHOLD < pt.x && pt.x < fmon.right
			      && fmon.bottom-2*AERO_THRESHOLD < pt.y && pt.y < fmon.bottom) {
				//Bottomright
				state.aero = 1;
				wndwidth = (mon.right-mon.left)/2;
				wndheight = (mon.bottom-mon.top)/2;
				posx = mon.right-wndwidth;
				posy = mon.bottom-wndheight;
			}
			else if (fmon.top <= pt.y && pt.y < fmon.top+AERO_THRESHOLD) {
				//Top
				if (!maximized) {
					state.aero = 1;
					//Center window on monitor and maximize it
					wndpl.rcNormalPosition.left = mon.left+(mon.right-mon.left)/2-state.origin.width/2;
					wndpl.rcNormalPosition.top = mon.top+(mon.bottom-mon.top)/2-state.origin.height/2;
					wndpl.rcNormalPosition.right = wndpl.rcNormalPosition.left+state.origin.width;
					wndpl.rcNormalPosition.bottom = wndpl.rcNormalPosition.top+state.origin.height;
					wndpl.showCmd = SW_MAXIMIZE;
					SetWindowPlacement(state.hwnd, &wndpl);
				}
				return;
			}
			else if (fmon.left <= pt.x && pt.x < fmon.left+AERO_THRESHOLD) {
				//Left
				state.aero = 1;
				wndwidth = (mon.right-mon.left)/2;
				wndheight = (mon.bottom-mon.top);
				posx = mon.left;
				posy = mon.top;
			}
			else if (fmon.right-AERO_THRESHOLD < pt.x && pt.x < fmon.right) {
				//Right
				state.aero = 1;
				wndwidth = (mon.right-mon.left)/2;
				wndheight = (mon.bottom-mon.top);
				posx = mon.right-wndwidth;
				posy = mon.top;
			}
			else if (state.aero) {
				//Restore original window size
				state.aero = 0;
				wndwidth = state.origin.width;
				wndheight = state.origin.height;
			}
			
			//Store in wnddb
			if (state.aero && state.wndentry == NULL) {
				state.wndentry = wnddb.pos;
				wnddb.pos->hwnd = state.hwnd;
				wnddb.pos->width = state.origin.width;
				wnddb.pos->height = state.origin.height;
				wnddb.pos = (wnddb.pos == &wnddb.items[NUMWNDDB-1])?&wnddb.items[0]:wnddb.pos+1;
			}
		}
		
		//Maximize window again if moved from another monitor
		if (sharedsettings.AutoRemaximize && state.origin.maximized
		 && !state.locked && monitor != state.origin.monitor) {
			//Get monitor rect
			MONITORINFO monitorinfo;
			monitorinfo.cbSize = sizeof(MONITORINFO);
			GetMonitorInfo(monitor, &monitorinfo);
			RECT mon = monitorinfo.rcWork;
			//Center window on monitor and maximize it
			WINDOWPLACEMENT wndpl;
			wndpl.length = sizeof(WINDOWPLACEMENT);
			GetWindowPlacement(state.hwnd, &wndpl);
			wndpl.rcNormalPosition.left = mon.left+(mon.right-mon.left)/2-wndwidth/2;
			wndpl.rcNormalPosition.top = mon.top+(mon.bottom-mon.top)/2-wndheight/2;
			wndpl.rcNormalPosition.right = wndpl.rcNormalPosition.left+wndwidth;
			wndpl.rcNormalPosition.bottom = wndpl.rcNormalPosition.top+wndheight;
			wndpl.showCmd = SW_MAXIMIZE;
			SetWindowPlacement(state.hwnd, &wndpl);
			//Set this monitor as the origin
			state.origin.monitor = monitor;
			//Lock the current state, but restore window after a timeout
			state.locked = 1;
			SetTimer(g_hwnd, UNLOCK_TIMER, 1000, NULL);
			return;
		}
		
		
		//Check if the window will stick anywhere
		if (sharedstate.shift || sharedsettings.AutoStick) {
			MoveStick(&posx, &posy, wndwidth, wndheight);
		}
	}
	else if (sharedstate.resize) {
		if (state.resize.x == RESIZE_CENTER && state.resize.y == RESIZE_CENTER) {
			posx = wnd.left-(pt.x-state.offset.x);
			posy = wnd.top-(pt.y-state.offset.y);
			wndwidth = wnd.right-wnd.left+2*(pt.x-state.offset.x);
			wndheight = wnd.bottom-wnd.top+2*(pt.y-state.offset.y);
			state.offset.x = pt.x;
			state.offset.y = pt.y;
		}
		else {
			if (state.resize.y == RESIZE_TOP) {
				posy = pt.y-state.offset.y;
				wndheight = wnd.bottom-pt.y+state.offset.y;
			}
			else if (state.resize.y == RESIZE_CENTER) {
				posy = wnd.top;
				wndheight = wnd.bottom-wnd.top;
			}
			else if (state.resize.y == RESIZE_BOTTOM) {
				posy = wnd.top;
				wndheight = pt.y-wnd.top+state.offset.y;
			}
			if (state.resize.x == RESIZE_LEFT) {
				posx = pt.x-state.offset.x;
				wndwidth = wnd.right-pt.x+state.offset.x;
			}
			else if (state.resize.x == RESIZE_CENTER) {
				posx = wnd.left;
				wndwidth = wnd.right-wnd.left;
			}
			else if (state.resize.x == RESIZE_RIGHT) {
				posx = wnd.left;
				wndwidth = pt.x-wnd.left+state.offset.x;
			}
		}
		
		//Check if the window will stick anywhere
		if ((sharedstate.shift || sharedsettings.AutoStick) && (state.resize.x != RESIZE_CENTER || state.resize.y != RESIZE_CENTER)) {
			ResizeStick(&posx, &posy, &wndwidth, &wndheight);
		}
	}
	
	//Move
	BOOL repaint = IncUpdateRate();
	MoveWindow(state.hwnd, posx, posy, wndwidth, wndheight, repaint);
}

__declspec(dllexport) LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION) {
		int vkey = ((PKBDLLHOOKSTRUCT)lParam)->vkCode;
		
		if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
			if (!state.alt && IsHotKey(vkey)) {
				state.alt = 1;
				state.blockaltup = 0;
				state.clicktime = 0; //Reset double-click time
				//Don't hook the mouse if the foreground window is fullscreen
				HWND window = GetForegroundWindow();
				progman = FindWindow(L"Progman", L"Program Manager");
				if (window != progman && !(GetWindowLongPtr(window,GWL_STYLE)&WS_CAPTION)) {
					//Get window size
					RECT wnd;
					if (GetWindowRect(window,&wnd) == 0) {
						return CallNextHookEx(NULL, nCode, wParam, lParam);
					}
					//Get monitor size
					HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
					MONITORINFO monitorinfo;
					monitorinfo.cbSize = sizeof(MONITORINFO);
					GetMonitorInfo(monitor, &monitorinfo);
					RECT mon = monitorinfo.rcMonitor;
					//Return if the window is fullscreen
					if (wnd.left == mon.left && wnd.top == mon.top && wnd.right == mon.right && wnd.bottom == mon.bottom) {
						return CallNextHookEx(NULL, nCode, wParam, lParam);
					}
				}
				//Hook mouse
				HookMouse();
			}
			else if (vkey == VK_LSHIFT || vkey == VK_RSHIFT) {
				if (!sharedstate.shift) {
					sharedstate.shift = 1;
					MouseMove();
				}
				if (state.alt && (sharedstate.move || sharedstate.resize)) {
					//Block keypress to prevent Windows from changing keyboard layout
					return 1;
				}
			}
			else if (sharedstate.move && (vkey == VK_LCONTROL || vkey == VK_RCONTROL)) {
				SetForegroundWindow(state.hwnd);
			}
		}
		else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
			if (IsHotKey(vkey)) {
				//Double check that all the hotkeys have been released
				int i;
				for (i=0; i < sharedsettings.Hotkeys.length; i++) {
					if (vkey != sharedsettings.Hotkeys.keys[i] && GetAsyncKeyState(sharedsettings.Hotkeys.keys[i])&0x8000) {
						return CallNextHookEx(NULL, nCode, wParam, lParam);
					}
				}
				
				//Okay, all hotkeys have been released
				state.alt = 0;
				
				//Block the alt keyup to prevent the window menu to be selected.
				//The way this works is that the alt key is "disguised" by sending ctrl keydown/keyup events just before the altup.
				//For more information, see issue 20.
				if (state.blockaltup) {
					KEYBDINPUT ctrl[2];
					ctrl[0].wVk = ctrl[1].wVk = VK_CONTROL;
					ctrl[0].wScan = ctrl[0].time = ctrl[1].wScan = ctrl[1].time = 0;
					ctrl[0].dwExtraInfo = ctrl[1].dwExtraInfo = GetMessageExtraInfo();
					ctrl[0].dwFlags = 0;
					ctrl[1].dwFlags = KEYEVENTF_KEYUP;
					INPUT input[2];
					input[0].type = input[1].type = INPUT_KEYBOARD;
					input[0].ki = ctrl[0];
					input[1].ki = ctrl[1];
					SendInput(2, input, sizeof(INPUT));
				}
				
				if (!sharedstate.move && !sharedstate.resize) {
					UnhookMouse();
				}
			}
			else if (vkey == VK_LSHIFT || vkey == VK_RSHIFT) {
				sharedstate.shift = 0;
				MouseMove();
			}
		}
	}
	
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

__declspec(dllexport) LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION) {
		//Set up some variables
		PMSLLHOOKSTRUCT msg = (PMSLLHOOKSTRUCT)lParam;
		int button =
			(wParam==WM_LBUTTONDOWN||wParam==WM_LBUTTONUP)?BUTTON_LMB:
			(wParam==WM_MBUTTONDOWN||wParam==WM_MBUTTONUP)?BUTTON_MMB:
			(wParam==WM_RBUTTONDOWN||wParam==WM_RBUTTONUP)?BUTTON_RMB:
			(HIWORD(msg->mouseData)==XBUTTON1)?BUTTON_MB4:
			(HIWORD(msg->mouseData)==XBUTTON2)?BUTTON_MB5:BUTTON_NONE;
		enum {STATE_NONE, STATE_DOWN, STATE_UP} buttonstate =
			(wParam==WM_LBUTTONDOWN||wParam==WM_MBUTTONDOWN||wParam==WM_RBUTTONDOWN||wParam==WM_XBUTTONDOWN)?STATE_DOWN:
			(wParam==WM_LBUTTONUP||wParam==WM_MBUTTONUP||wParam==WM_RBUTTONUP||wParam==WM_XBUTTONUP)?STATE_UP:STATE_NONE;
		
		if (state.alt && buttonstate == STATE_DOWN) {
			//Double check if any of the hotkeys are being pressed
			int i;
			for (i=0; i < sharedsettings.Hotkeys.length; i++) {
				if (GetAsyncKeyState(sharedsettings.Hotkeys.keys[i])&0x8000) {
					break;
				}
				else if (i+1 == sharedsettings.Hotkeys.length) {
					state.alt = 0;
					UnhookMouse();
					return CallNextHookEx(NULL, nCode, wParam, lParam);
				}
			}
			
			//Okay, at least one trigger key is being pressed
			POINT pt = msg->pt;
			
			//Make sure cursorwnd isn't in the way
			if (sharedsettings.Performance.Cursor) {
				ShowWindow(cursorwnd, SW_HIDE);
			}
			
			//Get window
			state.hwnd = WindowFromPoint(pt);
			if (state.hwnd == NULL) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			state.hwnd = GetAncestor(state.hwnd, GA_ROOT);
			
			//Return if window is blacklisted
			if (blacklisted(state.hwnd,&settings.Blacklist)) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Get window placement
			WINDOWPLACEMENT wndpl;
			wndpl.length = sizeof(WINDOWPLACEMENT);
			GetWindowPlacement(state.hwnd, &wndpl);
			RECT wnd = wndpl.rcNormalPosition;
			
			//Return if the window is fullscreen and no border
			if (!(GetWindowLongPtr(state.hwnd,GWL_STYLE)&WS_CAPTION)) {
				HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
				MONITORINFO monitorinfo;
				monitorinfo.cbSize = sizeof(MONITORINFO);
				GetMonitorInfo(monitor, &monitorinfo);
				RECT mon = monitorinfo.rcMonitor;
				if (wnd.left == mon.left && wnd.top == mon.top && wnd.right == mon.right && wnd.bottom == mon.bottom) {
					return CallNextHookEx(NULL, nCode, wParam, lParam);
				}
			}
			
			//Update state
			state.blockaltup = 1;
			state.locked = 0;
			state.aero = 0;
			state.wndentry = NULL;
			state.origin.maximized = IsZoomed(state.hwnd);
			state.origin.width = wnd.right-wnd.left;
			state.origin.height = wnd.bottom-wnd.top;
			
			//Autofocus
			if (sharedsettings.Autofocus) {
				SetForegroundWindow(state.hwnd);
			}
			
			//Do things depending on what button was pressed
			if (IsButton(button,ACTION_MOVE)) {
				//Ready to move window
				sharedstate.move = 1;
				
				//Check if window is in the wnddb database
				for (i=0; i < NUMWNDDB; i++) {
					if (wnddb.items[i].hwnd == state.hwnd) {
						//Set offset
						state.offset.x = (float)(pt.x-wnd.left)/(wnd.right-wnd.left)*wnddb.items[i].width;
						state.offset.y = (float)(pt.y-wnd.top)/(wnd.bottom-wnd.top)*wnddb.items[i].height;
						//Restore old window size
						MoveWindow(state.hwnd, pt.x-state.offset.x, pt.y-state.offset.y, wnddb.items[i].width, wnddb.items[i].height, TRUE);
						//Remember time of this click so we can check for double-click
						state.clicktime = GetTickCount();
						//Correct origin width/height
						state.origin.width = wnddb.items[i].width;
						state.origin.height = wnddb.items[i].height;
						//Remove window from database
						wnddb.items[i].hwnd = NULL;
						//Prevent mousedown from propagating
						return;
					}
				}
				
				//Maximize window if this is a double-click
				if (GetTickCount()-state.clicktime <= GetDoubleClickTime()) {
					//Alt+double-clicking a window maximizes it
					//Maximize window
					wndpl.showCmd = SW_MAXIMIZE;
					SetWindowPlacement(state.hwnd, &wndpl);
					//Hide cursorwnd
					if (sharedsettings.Performance.Cursor) {
						ShowWindow(cursorwnd, SW_HIDE);
						SetWindowLongPtr(cursorwnd, GWL_EXSTYLE, WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
						//Maybe show IDC_SIZEALL cursor here really quick somehow?
					}
					//Stop move action
					sharedstate.move = 0;
					//Prevent mousedown from propagating
					return 1;
				}
				
				//Restore the window if it's maximized
				if (state.origin.maximized) {
					state.origin.monitor = MonitorFromWindow(state.hwnd, MONITOR_DEFAULTTONEAREST);
					
					//Get maximized size
					RECT wndmax;
					GetWindowRect(state.hwnd, &wndmax);
					
					//Restore window
					wndpl.showCmd = SW_RESTORE;
					SetWindowPlacement(state.hwnd, &wndpl);
					
					//Set offset
					state.offset.x = (float)(pt.x-wndmax.left)/(wndmax.right-wndmax.left)*(wnd.right-wnd.left);
					state.offset.y = (float)(pt.y-wndmax.top)/(wndmax.bottom-wndmax.top)*(wnd.bottom-wnd.top);
					
					//Move
					MouseMove();
				}
				else {
					//Remember time of this click so we can check for double-click
					state.clicktime = GetTickCount();
					
					//Set offset
					state.offset.x = pt.x-wnd.left;
					state.offset.y = pt.y-wnd.top;
				}
				
				//Show cursorwnd
				if (sharedsettings.Performance.Cursor) {
					SetClassLongPtr(cursorwnd, GCLP_HCURSOR, (LONG_PTR)cursor[HAND]);
					if (!sharedstate.resize) {
						MoveWindow(cursorwnd, pt.x-20, pt.y-20, 41, 41, FALSE);
						SetWindowLongPtr(cursorwnd, GWL_EXSTYLE, WS_EX_LAYERED|WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
						SetLayeredWindowAttributes(cursorwnd, 0, 1, LWA_ALPHA); //Almost transparent
					}
					ShowWindowAsync(cursorwnd, SW_SHOWNA);
				}
				
				//Prevent mousedown from propagating
				return 1;
			}
			else if (IsButton(button,ACTION_RESIZE)) {
				//Ready to resize window
				sharedstate.resize = 1;
				
				//Remove window from wnddb if present
				for (i=0; i < NUMWNDDB; i++) {
					if (wnddb.items[i].hwnd == state.hwnd) {
						wnddb.items[i].hwnd = NULL;
						break;
					}
				}
				
				//Move this window to the left or right side of the monitor if this is a double-click
				if (GetTickCount()-state.clicktime <= GetDoubleClickTime()) {
					//Store window in wnddb
					state.wndentry = wnddb.pos;
					wnddb.pos->hwnd = state.hwnd;
					wnddb.pos->width = state.origin.width;
					wnddb.pos->height = state.origin.height;
					wnddb.pos = (wnddb.pos == &wnddb.items[NUMWNDDB-1])?&wnddb.items[0]:wnddb.pos+1;
					//Get monitor info
					HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
					MONITORINFO monitorinfo;
					monitorinfo.cbSize = sizeof(MONITORINFO);
					GetMonitorInfo(monitor, &monitorinfo);
					RECT mon = monitorinfo.rcWork;
					//Get new position
					int wndwidth = (mon.right-mon.left)/2;
					int wndheight = mon.bottom-mon.top;
					int posx = mon.left;
					int posy = mon.top;
					//Move window to the left
					MoveWindow(state.hwnd, posx, posy, wndwidth, wndheight, TRUE);
					//Hide cursorwnd
					if (sharedsettings.Performance.Cursor) {
						ShowWindow(cursorwnd, SW_HIDE);
						SetWindowLongPtr(cursorwnd, GWL_EXSTYLE, WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
					}
					//Stop resize action
					sharedstate.resize = 0;
					//Prevent mousedown from propagating
					return 1;
				}
				
				//Remember time of this click so we can check for double-click
				state.clicktime = GetTickCount();
				
				//Restore the window if it's maximized
				if (state.origin.maximized) {
					//Restore window
					wndpl.showCmd = SW_RESTORE;
					//Get new size
					HMONITOR monitor = MonitorFromWindow(state.hwnd, MONITOR_DEFAULTTONEAREST);
					MONITORINFO monitorinfo;
					monitorinfo.cbSize = sizeof(MONITORINFO);
					GetMonitorInfo(monitor, &monitorinfo);
					wnd = monitorinfo.rcWork;
					//Update window
					wndpl.rcNormalPosition = wnd;
					SetWindowPlacement(state.hwnd, &wndpl);
				}
				
				//Set edge and offset
				//Think of the window as nine boxes
				if (pt.y-wnd.top < (wnd.bottom-wnd.top)/3) {
					state.resize.y = RESIZE_TOP;
					state.offset.y = pt.y-wnd.top;
				}
				else if (pt.y-wnd.top < (wnd.bottom-wnd.top)*2/3) {
					state.resize.y = RESIZE_CENTER;
					state.offset.y = pt.y; //Used only if both x and y are CENTER
				}
				else {
					state.resize.y = RESIZE_BOTTOM;
					state.offset.y = wnd.bottom-pt.y;
				}
				if (pt.x-wnd.left < (wnd.right-wnd.left)/3) {
					state.resize.x = RESIZE_LEFT;
					state.offset.x = pt.x-wnd.left;
				}
				else if (pt.x-wnd.left < (wnd.right-wnd.left)*2/3) {
					state.resize.x = RESIZE_CENTER;
					state.offset.x = pt.x; //Used only if both x and y are CENTER
				}
				else {
					state.resize.x = RESIZE_RIGHT;
					state.offset.x = wnd.right-pt.x;
				}
				
				//Show cursorwnd
				if (sharedsettings.Performance.Cursor) {
					//Determine shape of cursor
					if ((state.resize.y == RESIZE_TOP && state.resize.x == RESIZE_LEFT)
					 || (state.resize.y == RESIZE_BOTTOM && state.resize.x == RESIZE_RIGHT)) {
						resizecursor = SIZENWSE;
					}
					else if ((state.resize.y == RESIZE_TOP && state.resize.x == RESIZE_RIGHT)
					 || (state.resize.y == RESIZE_BOTTOM && state.resize.x == RESIZE_LEFT)) {
						resizecursor = SIZENESW;
					}
					else if ((state.resize.y == RESIZE_TOP && state.resize.x == RESIZE_CENTER)
					 || (state.resize.y == RESIZE_BOTTOM && state.resize.x == RESIZE_CENTER)) {
						resizecursor = SIZENS;
					}
					else if ((state.resize.y == RESIZE_CENTER && state.resize.x == RESIZE_LEFT)
					 || (state.resize.y == RESIZE_CENTER && state.resize.x == RESIZE_RIGHT)) {
						resizecursor = SIZEWE;
					}
					else {
						resizecursor = SIZEALL;
					}
					//Change cursor
					if (!sharedstate.move) {
						MoveWindow(cursorwnd, pt.x-20, pt.y-20, 41, 41, FALSE);
						SetClassLongPtr(cursorwnd, GCLP_HCURSOR, (LONG_PTR)cursor[resizecursor]);
						SetWindowLongPtr(cursorwnd, GWL_EXSTYLE, WS_EX_LAYERED|WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
						SetLayeredWindowAttributes(cursorwnd, 0, 1, LWA_ALPHA); //Almost transparent
					}
					ShowWindowAsync(cursorwnd, SW_SHOWNA);
				}
				
				//Prevent mousedown from propagating
				return 1;
			}
			else if (IsButton(button,ACTION_MINIMIZE)) {
				//Minimize window
				SendMessage(state.hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
				//Prevent mousedown from propagating
				return 1;
			}
			else if (IsButton(button,ACTION_CENTER)) {
				//Center window
				HMONITOR monitor = MonitorFromWindow(state.hwnd, MONITOR_DEFAULTTONEAREST);
				MONITORINFO monitorinfo;
				monitorinfo.cbSize = sizeof(MONITORINFO);
				GetMonitorInfo(monitor, &monitorinfo);
				RECT mon = monitorinfo.rcWork;
				MoveWindow(state.hwnd, mon.left+(mon.right-mon.left)/2-(wnd.right-wnd.left)/2, mon.top+(mon.bottom-mon.top)/2-(wnd.bottom-wnd.top)/2, wnd.right-wnd.left, wnd.bottom-wnd.top, TRUE);
				//Prevent mousedown from propagating
				return 1;
			}
			else if (IsButton(button,ACTION_ALWAYSONTOP)) {
				//Toggle always on top
				int topmost = GetWindowLongPtr(state.hwnd,GWL_EXSTYLE)&WS_EX_TOPMOST;
				SetWindowPos(state.hwnd, (topmost?HWND_NOTOPMOST:HWND_TOPMOST), 0, 0, 0, 0, SWP_NOMOVE|SWP_NOSIZE);
				//Prevent mousedown from propagating
				return 1;
			}
			else if (IsButton(button,ACTION_CLOSE)) {
				//Close window
				SendMessage(state.hwnd, WM_CLOSE, 0, 0);
				//Prevent mousedown from propagating
				return 1;
			}
		}
		else if (buttonstate == STATE_UP) {
			if (sharedstate.move && IsButton(button,ACTION_MOVE)) {
				sharedstate.move = 0;
				if (!state.alt) {
					UnhookMouse();
				}
				//Hide cursorwnd
				if (sharedsettings.Performance.Cursor) {
					if (sharedstate.resize) {
						SetClassLongPtr(cursorwnd, GCLP_HCURSOR, (LONG_PTR)cursor[resizecursor]);
					}
					else {
						ShowWindow(cursorwnd, SW_HIDE);
						SetWindowLongPtr(cursorwnd, GWL_EXSTYLE, WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
					}
				}
				//Prevent mouseup from propagating
				return 1;
			}
			else if (sharedstate.resize && IsButton(button,ACTION_RESIZE)) {
				sharedstate.resize = 0;
				if (!state.alt) {
					UnhookMouse();
				}
				//Hide cursorwnd
				if (sharedsettings.Performance.Cursor && !sharedstate.move) {
					ShowWindow(cursorwnd, SW_HIDE);
					SetWindowLongPtr(cursorwnd, GWL_EXSTYLE, WS_EX_TOOLWINDOW); //Workaround for http://support.microsoft.com/kb/270624/
				}
				//Prevent mouseup from propagating
				return 1;
			}
			else if (state.alt && (IsButton(button,ACTION_MINIMIZE) || IsButton(button,ACTION_CENTER) || IsButton(button,ACTION_ALWAYSONTOP) || IsButton(button,ACTION_CLOSE))) {
				//Prevent mouseup from propagating
				return 1;
			}
		}
		else if (wParam == WM_MOUSEMOVE) {
			//Reset double-click time
			state.clicktime = 0; //This prevents me from double-clicking when running Windows virtualized.
			if (sharedstate.move || sharedstate.resize) {
				//Move cursorwnd
				if (sharedsettings.Performance.Cursor) {
					POINT pt = msg->pt;
					MoveWindow(cursorwnd, pt.x-20, pt.y-20, 41, 41, TRUE);
					//MoveWindow(cursorwnd,(prevpt.x<pt.x?prevpt.x:pt.x)-3,(prevpt.y<pt.y?prevpt.y:pt.y)-3,(pt.x>prevpt.x?pt.x-prevpt.x:prevpt.x-pt.x)+7,(pt.y>prevpt.y?pt.y-prevpt.y:prevpt.y-pt.y)+7,FALSE);
				}
				//Move window
				MouseMove();
			}
		}
	}
	
	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

int HookMouse() {
	if (mousehook) {
		//Mouse already hooked
		return 1;
	}
	
	//Set up the mouse hook
	mousehook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc, hinstDLL, 0);
	if (mousehook == NULL) {
		Error(L"SetWindowsHookEx(WH_MOUSE_LL)", L"HookMouse()", GetLastError(), TEXT(__FILE__), __LINE__);
		return 1;
	}
	
	//Success
	return 0;
}

int UnhookMouse() {
	if (!mousehook) {
		//Mouse not hooked
		return 1;
	}
	
	//Remove mouse hook
	if (UnhookWindowsHookEx(mousehook) == 0) {
		Error(L"UnhookWindowsHookEx(mousehook)", L"", GetLastError(), TEXT(__FILE__), __LINE__);
		mousehook = NULL;
		return 1;
	}
	
	//Success
	mousehook = NULL;
	state.clicktime = 0;
	return 0;
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_TIMER) {
		KillTimer(g_hwnd, UNLOCK_TIMER);
		state.locked = 0;
		
		if (sharedstate.move) {
			//Restore window
			WINDOWPLACEMENT wndpl;
			wndpl.length = sizeof(WINDOWPLACEMENT);
			GetWindowPlacement(state.hwnd, &wndpl);
			wndpl.showCmd = SW_RESTORE;
			SetWindowPlacement(state.hwnd, &wndpl);
			
			//Move
			MouseMove();
		}
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

#endif

//Msghook
__declspec(dllexport) LRESULT CALLBACK CustomWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
	if (msg == WM_WINDOWPOSCHANGING && (sharedstate.shift || sharedsettings.AutoStick)) {
		WINDOWPOS *wndpos = (WINDOWPOS*)lParam;
		if (msgaction == MOVE && !(wndpos->flags&SWP_NOMOVE)) {
			MoveStick(&wndpos->x, &wndpos->y, wndpos->cx, wndpos->cy);
		}
		else if (msgaction == RESIZE && !(wndpos->flags&SWP_NOSIZE)) {
			ResizeStick(&wndpos->x, &wndpos->y, &wndpos->cx, &wndpos->cy);
		}
	}
	else if (msg == WM_EXITSIZEMOVE || msg == WM_DESTROY) {
		subclassed = !RemoveWindowSubclass(hwnd, CustomWndProc, 0);
		if (subclassed) {
			Error(L"RemoveWindowSubclass(hwnd, CustomWndProc, 0)", L"Failed to remove window subclassing.", -1, TEXT(__FILE__), __LINE__);
			return DefSubclassProc(hwnd, msg, wParam, lParam);
		}
		hwnd = NULL;
		msgaction = NONE;
	}
	
	/*
	//Fun code to trap window on screen
	if (msg == WM_WINDOWPOSCHANGING) {
		WINDOWPOS *wndpos = (WINDOWPOS*)lParam;
		if (wndpos->x < 0) {
			wndpos->x = 0;
		}
		if (wndpos->y < 0) {
			wndpos->y = 0;
		}
		if (wndpos->x+wndpos->cx > 1920) {
			wndpos->x = 1920-wndpos->cx;
		}
		if (wndpos->y+wndpos->cy > 1140) {
			wndpos->y = 1140-wndpos->cy;
		}
	}
	*/
	
	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

//CallWndProc is called in the context of the thread that calls SendMessage, not the thread that receives the message.
//Thus we have to explicitly share the memory we want CallWndProc to be able to access (e.g. sharedstate)
//Variables that are not shared, e.g. the blacklist, are loaded individually for each process.
__declspec(dllexport) LRESULT CALLBACK CallWndProc(int nCode, WPARAM wParam, LPARAM lParam) {
	if (nCode == HC_ACTION && !sharedstate.move && !sharedstate.resize) {
		CWPSTRUCT *msg = (CWPSTRUCT*)lParam;
		
		if (msg->message == WM_ENTERSIZEMOVE
		 && (!subclassed || state.hwnd != msg->hwnd)
		 && IsWindowVisible(msg->hwnd)
		 && GetWindowLongPtr(msg->hwnd,GWL_STYLE)&WS_CAPTION // || !(GetWindowLongPtr(msg->hwnd,GWL_EXSTYLE)&WS_EX_TOOLWINDOW))
		 && !IsIconic(msg->hwnd) && !IsZoomed(msg->hwnd)
		 && msg->hwnd == GetAncestor(msg->hwnd,GA_ROOT)
		) {
			//Return if window is blacklisted
			if (blacklisted(msg->hwnd,&settings.Blacklist)) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Remove old subclassing if another window is currently subclassed
			if (subclassed && IsWindow(state.hwnd)) {
				subclassed = !RemoveWindowSubclass(state.hwnd, CustomWndProc, 0);
				if (subclassed) {
					Error(L"RemoveWindowSubclass(hwnd, CustomWndProc, 0)", L"Failed to remove window subclassing.", -1, TEXT(__FILE__), __LINE__);
				}
			}
			
			//Set hwnd
			state.hwnd = msg->hwnd;
			
			//Double check if a shift key is still being pressed
			if (sharedstate.shift && !(GetAsyncKeyState(VK_SHIFT)&0x8000)) {
				sharedstate.shift = 0;
			}
			if (!sharedstate.shift && !sharedsettings.AutoStick) {
				return CallNextHookEx(NULL, nCode, wParam, lParam);
			}
			
			//Subclass window
			subclassed = SetWindowSubclass(state.hwnd, CustomWndProc, 0, 0);
			if (!subclassed) {
				Error(L"SetWindowSubclass(state.hwnd, CustomWndProc, 0, 0)", L"Failed to subclass window.", -1, TEXT(__FILE__), __LINE__);
			}
		}
		else if (msg->message == WM_WINDOWPOSCHANGING
		 && !subclassed && state.hwnd == msg->hwnd && msgaction != NONE
		 && (sharedstate.shift || sharedsettings.AutoStick)) {
			
			//Subclass window
			subclassed = SetWindowSubclass(state.hwnd, CustomWndProc, 0, 0);
			if (!subclassed) {
				Error(L"SetWindowSubclass(state.hwnd, CustomWndProc, 0, 0)", L"Failed to subclass window.", -1, TEXT(__FILE__), __LINE__);
			}
		}
		else if (msg->message == WM_SYSCOMMAND) {
			WPARAM action = msg->wParam&0xFFF0;
			if (action == SC_MOVE) {
				msgaction = MOVE;
			}
			else if (action == SC_SIZE) {
				msgaction = RESIZE;
				int edge = msg->wParam&0x000F; //These are the undocumented bits (compatible with WMSZ_*)
				//Set offset
				//resize.x
				if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
					state.resize.x = RESIZE_CENTER;
				}
				if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) {
					state.resize.x = RESIZE_LEFT;
				}
				else if (edge == WMSZ_RIGHT || edge == WMSZ_TOPRIGHT || edge == WMSZ_BOTTOMRIGHT) {
					state.resize.x = RESIZE_RIGHT;
				}
				//resize.y
				if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) {
					state.resize.y = RESIZE_CENTER;
				}
				if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
					state.resize.y = RESIZE_TOP;
				}
				else if (edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT) {
					state.resize.y = RESIZE_BOTTOM;
				}
				state.offset.x = 0;
				state.offset.y = 0;
			}
		}
		else if (msg->message == WM_EXITSIZEMOVE) {
			msgaction = NONE;
		}
	}
	
	return CallNextHookEx(NULL, nCode, wParam, lParam);
}

//This is needed sometimes when an msghook thread lingers around for no apparent reason.
__declspec(dllexport) void ClearSettings() {
	sharedsettings_loaded = 0;
}

BOOL APIENTRY DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved) {
	if (reason == DLL_PROCESS_ATTACH) {
		hinstDLL = hInst;
		//Load settings
		wchar_t txt[1000];
		//Settings shared with CallWndProc hook
		if (!sharedsettings_loaded) {
			sharedsettings_loaded = 1;
			//Store path to ini file at initial load so CallWndProc hooks can find it
			GetModuleFileName(NULL, inipath, sizeof(inipath)/sizeof(wchar_t));
			PathRemoveFileSpec(inipath);
			wcscat(inipath, L"\\"APP_NAME".ini");
			//[AltDrag]
			GetPrivateProfileString(APP_NAME, L"Autofocus", L"0", txt, sizeof(txt)/sizeof(wchar_t), inipath);
			swscanf(txt, L"%d", &sharedsettings.Autofocus);
			GetPrivateProfileString(APP_NAME, L"AutoStick", L"0", txt, sizeof(txt)/sizeof(wchar_t), inipath);
			swscanf(txt, L"%d", &sharedsettings.AutoStick);
			GetPrivateProfileString(APP_NAME, L"Aero", L"2", txt, sizeof(txt)/sizeof(wchar_t), inipath);
			swscanf(txt, L"%d", &sharedsettings.Aero);
			GetPrivateProfileString(APP_NAME, L"AutoRemaximize", L"0", txt, sizeof(txt)/sizeof(wchar_t), inipath);
			swscanf(txt, L"%d", &sharedsettings.AutoRemaximize);
			//Detect if Aero Snap is enabled
			if (sharedsettings.Aero == 2) {
				HKEY key;
				wchar_t aero[2] = L"";
				DWORD len = sizeof(aero);
				RegOpenKeyEx(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_QUERY_VALUE, &key);
				int error = RegQueryValueEx(key, L"WindowArrangementActive", NULL, NULL, (LPBYTE)aero, &len);
				RegCloseKey(key);
				if (error == ERROR_SUCCESS) {
					sharedsettings.Aero = _wtoi(aero);
				}
			}
			//[Performance]
			GetPrivateProfileString(L"Performance", L"Cursor", L"1", txt, sizeof(txt)/sizeof(wchar_t), inipath);
			swscanf(txt, L"%d", &sharedsettings.Performance.Cursor);
			if (sharedsettings.Performance.Cursor) {
				cursorwnd = FindWindow(APP_NAME, NULL);
				#ifndef _WIN64
				cursor[HAND]     = LoadImage(NULL, IDC_HAND,     IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				cursor[SIZENWSE] = LoadImage(NULL, IDC_SIZENWSE, IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				cursor[SIZENESW] = LoadImage(NULL, IDC_SIZENESW, IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				cursor[SIZENS]   = LoadImage(NULL, IDC_SIZENS,   IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				cursor[SIZEWE]   = LoadImage(NULL, IDC_SIZEWE,   IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				cursor[SIZEALL]  = LoadImage(NULL, IDC_SIZEALL,  IMAGE_CURSOR, 0, 0, LR_DEFAULTCOLOR|LR_SHARED);
				#endif
			}
			GetPrivateProfileString(L"Performance", L"UpdateRate", L"1", txt, sizeof(txt)/sizeof(wchar_t), inipath);
			swscanf(txt, L"%d", &sharedsettings.Performance.UpdateRate);
			//[Keyboard]
			int keys_alloc = 0;
			unsigned char temp;
			int numread;
			sharedsettings.Hotkeys.length = 0;
			GetPrivateProfileString(L"Keyboard", L"Hotkeys", L"A4 A5", txt, sizeof(txt)/sizeof(wchar_t), inipath);
			wchar_t *pos = txt;
			while (*pos != '\0' && swscanf(pos,L"%02X%n",&temp,&numread) != EOF) {
				//Make sure we have enough space
				if (sharedsettings.Hotkeys.length == keys_alloc) {
					keys_alloc += 10;
					sharedsettings.Hotkeys.keys = realloc(sharedsettings.Hotkeys.keys, keys_alloc*sizeof(int));
				}
				//Store key
				sharedsettings.Hotkeys.keys[sharedsettings.Hotkeys.length++] = temp;
				pos += numread;
			}
			//[Mouse]
			struct {
				wchar_t *key;
				wchar_t *def;
				enum action *ptr;
			} buttons[] = {
				{L"LMB", L"Move",    &sharedsettings.Mouse.LMB},
				{L"MMB", L"Resize",  &sharedsettings.Mouse.MMB},
				{L"RMB", L"Resize",  &sharedsettings.Mouse.RMB},
				{L"MB4", L"Nothing", &sharedsettings.Mouse.MB4},
				{L"MB5", L"Nothing", &sharedsettings.Mouse.MB5},
				{NULL}
			};
			int i;
			for (i=0; buttons[i].key != NULL; i++) {
				GetPrivateProfileString(L"Mouse", buttons[i].key, buttons[i].def, txt, sizeof(txt)/sizeof(wchar_t), inipath);
				if      (!wcsicmp(txt,L"Move"))        *buttons[i].ptr = ACTION_MOVE;
				else if (!wcsicmp(txt,L"Resize"))      *buttons[i].ptr = ACTION_RESIZE;
				else if (!wcsicmp(txt,L"Minimize"))    *buttons[i].ptr = ACTION_MINIMIZE;
				else if (!wcsicmp(txt,L"Center"))      *buttons[i].ptr = ACTION_CENTER;
				else if (!wcsicmp(txt,L"AlwaysOnTop")) *buttons[i].ptr = ACTION_ALWAYSONTOP;
				else if (!wcsicmp(txt,L"Close"))       *buttons[i].ptr = ACTION_CLOSE;
				else                                   *buttons[i].ptr = ACTION_NOTHING;
			}
			//Zero-out wnddb hwnds
			for (i=0; i < NUMWNDDB; i++) {
				wnddb.items[i].hwnd = NULL;
			}
			wnddb.pos = &wnddb.items[0];
			#ifndef _WIN64
			//Create window for timers
			WNDCLASSEX wnd = {sizeof(WNDCLASSEX), 0, WindowProc, 0, 0, hInst, NULL, NULL, NULL, NULL, APP_NAME, NULL};
			RegisterClassEx(&wnd);
			g_hwnd = CreateWindowEx(0, wnd.lpszClassName, APP_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, HWND_MESSAGE, NULL, hInst, NULL);
			#endif
		}
		//[Blacklist]
		int blacklist_alloc = 0;
		struct blacklist *blacklist = &settings.Blacklist;
		//Process Blacklist first
		GetPrivateProfileString(L"Blacklist", L"Blacklist", L"", txt, sizeof(txt)/sizeof(wchar_t), inipath);
		blacklist->data = malloc((wcslen(txt)+1)*sizeof(wchar_t));
		wcscpy(blacklist->data, txt);
		wchar_t *pos = blacklist->data;
		while (pos != NULL) {
			wchar_t *title = pos;
			wchar_t *classname = wcsstr(pos, L"|");
			//Move pos to next item (if any)
			pos = wcsstr(pos, L",");
			if (pos != NULL) {
				*pos = '\0';
				pos++;
			}
			if (classname != NULL) {
				*classname = '\0';
				classname++;
			}
			//Check if title or classname is wildcard
			if (!wcscmp(title,L"*")) {
				title = NULL;
			}
			if (classname != NULL && !wcscmp(classname,L"*")) {
				classname = NULL;
			}
			//Store item if it's not empty
			if (title != NULL || classname != NULL) {
				//Make sure we have enough space
				if (blacklist->length == blacklist_alloc) {
					blacklist_alloc += 15;
					blacklist->items = realloc(blacklist->items, blacklist_alloc*sizeof(struct blacklistitem));
					if (blacklist->items == NULL) {
						Error(L"realloc(blacklist->items)", L"Out of memory?", -1, TEXT(__FILE__), __LINE__);
					}
				}
				//Store item
				blacklist->items[blacklist->length].title = title;
				blacklist->items[blacklist->length].classname = classname;
				blacklist->length++;
			}
			//Switch gears?
			if (pos == NULL && blacklist != &settings.Whitelist_Sticky) {
				blacklist_alloc = 0;
				if (blacklist == &settings.Blacklist) {
					blacklist = &settings.Blacklist_Sticky;
					GetPrivateProfileString(L"Blacklist", L"Blacklist_Sticky", L"", txt, sizeof(txt)/sizeof(wchar_t), inipath);
				}
				else {
					blacklist = &settings.Whitelist_Sticky;
					GetPrivateProfileString(L"Blacklist", L"Whitelist_Sticky", L"", txt, sizeof(txt)/sizeof(wchar_t), inipath);
				}
				blacklist->data = malloc((wcslen(txt)+1)*sizeof(wchar_t));
				wcscpy(blacklist->data, txt);
				pos = blacklist->data;
			}
		}
		//Allocate space for wnds
		wnds_alloc += 10;
		wnds = realloc(wnds, wnds_alloc*sizeof(RECT));
		if (wnds == NULL) {
			Error(L"realloc(wnds)", L"Out of memory?", -1, TEXT(__FILE__), __LINE__);
			return FALSE;
		}
	}
	else if (reason == DLL_PROCESS_DETACH) {
		//Remove subclassing if a window is currently subclassed
		if (subclassed && IsWindow(state.hwnd)) {
			subclassed = !RemoveWindowSubclass(state.hwnd, CustomWndProc, 0);
			if (subclassed) {
				Error(L"RemoveWindowSubclass(hwnd, CustomWndProc, 0)", L"Failed to remove window subclassing.", -1, TEXT(__FILE__), __LINE__);
			}
		}
		//Free memory
		//Do not free any shared variables
		free(settings.Blacklist.items);
		free(settings.Blacklist.data);
		free(settings.Blacklist_Sticky.items);
		free(settings.Blacklist_Sticky.data);
		free(wnds);
	}
	return TRUE;
}
