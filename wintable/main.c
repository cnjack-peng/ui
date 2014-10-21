// 19 october 2014
#define UNICODE
#define _UNICODE
#define STRICT
#define STRICT_TYPED_ITEMIDS
// get Windows version right; right now Windows XP
#define WINVER 0x0501
#define _WIN32_WINNT 0x0501
#define _WIN32_WINDOWS 0x0501		/* according to Microsoft's winperf.h */
#define _WIN32_IE 0x0600			/* according to Microsoft's sdkddkver.h */
#define NTDDI_VERSION 0x05010000	/* according to Microsoft's sdkddkver.h */
#include <windows.h>
#include <commctrl.h>
#include <stdint.h>
#include <uxtheme.h>
#include <string.h>
#include <wchar.h>
#include <windowsx.h>
#include <vsstyle.h>
#include <vssym32.h>

// #qo LIBS: user32 kernel32 gdi32 comctl32

// TODO
// - http://blogs.msdn.com/b/oldnewthing/archive/2003/09/09/54826.aspx (relies on the integrality parts? IDK)
// 	- might want to http://blogs.msdn.com/b/oldnewthing/archive/2003/09/17/54944.aspx instead
// - http://msdn.microsoft.com/en-us/library/windows/desktop/bb775574%28v=vs.85%29.aspx
// - hscroll (harder)
// 	- keyboard navigation
// 	- mousewheel navigation

#define tableWindowClass L"gouitable"

struct table {
	HWND hwnd;
	HFONT defaultFont;
	HFONT font;
	intptr_t selected;
	intptr_t count;
	intptr_t firstVisible;
	intptr_t pagesize;		// in rows
	int wheelCarry;
	HWND header;
	int headerHeight;
	intptr_t nColumns;
};

static LONG rowHeight(struct table *t)
{
	HFONT thisfont, prevfont;
	TEXTMETRICW tm;
	HDC dc;

	dc = GetDC(t->hwnd);
	if (dc == NULL)
		abort();
	thisfont = t->font;		// in case WM_SETFONT happens before we return
	prevfont = (HFONT) SelectObject(dc, thisfont);
	if (prevfont == NULL)
		abort();
	if (GetTextMetricsW(dc, &tm) == 0)
		abort();
	if (SelectObject(dc, prevfont) != (HGDIOBJ) (thisfont))
		abort();
	if (ReleaseDC(t->hwnd, dc) == 0)
		abort();
	return tm.tmHeight;
}

static void redrawAll(struct table *t)
{
	if (InvalidateRect(t->hwnd, NULL, TRUE) == 0)
		abort();
	if (UpdateWindow(t->hwnd) == 0)
		abort();
}

static RECT realClientRect(struct table *t)
{
	RECT r;

	if (GetClientRect(t->hwnd, &r) == 0)
		abort();
	r.top += t->headerHeight;
	return r;
}

static void recomputeHScroll(struct table *t)
{
	HDITEMW item;
	intptr_t i;
	int width = 0;
	RECT r;
	SCROLLINFO si;

	// TODO count dividers
	for (i = 0; i < t->nColumns; i++) {
		ZeroMemory(&item, sizeof (HDITEMW));
		item.mask = HDI_WIDTH;
		if (SendMessageW(t->header, HDM_GETITEM, (WPARAM) i, (LPARAM) (&item)) == FALSE)
			abort();
		width += item.cxy;
	}

	if (GetClientRect(t->hwnd, &r) == 0)
		abort();
	ZeroMemory(&si, sizeof (SCROLLINFO));
	si.cbSize = sizeof (SCROLLINFO);
	si.fMask = SIF_PAGE | SIF_RANGE;
	si.nPage = r.right - r.left;
	si.nMin = 0;
	si.nMax = width - 1;			// - 1 because endpoints inclusive
	SetScrollInfo(t->hwnd, SB_HORZ, &si, TRUE);
}

static void finishSelect(struct table *t)
{
	if (t->selected < 0)
		t->selected = 0;
	if (t->selected >= t->count)
		t->selected = t->count - 1;
	// TODO update only the old and new selected items
	redrawAll(t);
	// TODO scroll to the selected item if it's not entirely visible
}

static void keySelect(struct table *t, WPARAM wParam, LPARAM lParam)
{
	// TODO figure out correct behavior with nothing selected
	if (t->count == 0)		// don't try to do anything if there's nothing to do
		return;
	switch (wParam) {
	case VK_UP:
		t->selected--;
		break;
	case VK_DOWN:
		t->selected++;
		break;
	case VK_PRIOR:
		t->selected -= t->pagesize;
		break;
	case VK_NEXT:
		t->selected += t->pagesize;
		break;
	case VK_HOME:
		t->selected = 0;
		break;
	case VK_END:
		t->selected = t->count - 1;
		break;
	default:
		// don't touch anything
		return;
	}
	finishSelect(t);
}

static void selectItem(struct table *t, WPARAM wParam, LPARAM lParam)
{
	int x, y;
	LONG h;

	x = GET_X_LPARAM(lParam);
	y = GET_Y_LPARAM(lParam);
	h = rowHeight(t);
	y += t->firstVisible * h;
	y -= t->headerHeight;
	y /= h;
	t->selected = y;
	if (t->selected >= t->count)
		t->selected = -1;
	finishSelect(t);
}

// TODO on initial show the items are not arranged properly
// TODO the lowest visible row does not redraw properly after scrolling
// TODO the row behind the header bar does not redraw properly after scrolling
static void vscrollto(struct table *t, intptr_t newpos)
{
	SCROLLINFO si;
	RECT scrollArea;

	if (newpos < 0)
		newpos = 0;
	if (newpos > (t->count - t->pagesize))
		newpos = (t->count - t->pagesize);

	scrollArea = realClientRect(t);

	// negative because ScrollWindowEx() is "backwards"
	if (ScrollWindowEx(t->hwnd, 0, (-(newpos - t->firstVisible)) * rowHeight(t),
		&scrollArea, &scrollArea, NULL, NULL,
		SW_ERASE | SW_INVALIDATE) == ERROR)
		abort();
	t->firstVisible = newpos;

	ZeroMemory(&si, sizeof (SCROLLINFO));
	si.cbSize = sizeof (SCROLLINFO);
	si.fMask = SIF_PAGE | SIF_POS | SIF_RANGE;
	si.nPage = t->pagesize;
	si.nMin = 0;
	si.nMax = t->count - 1;		// nMax is inclusive
	si.nPos = t->firstVisible;
	SetScrollInfo(t->hwnd, SB_VERT, &si, TRUE);
}

static void vscrollby(struct table *t, intptr_t n)
{
	vscrollto(t, t->firstVisible + n);
}

static void wheelscroll(struct table *t, WPARAM wParam)
{
	int delta;
	int lines;
	UINT scrollAmount;

	delta = GET_WHEEL_DELTA_WPARAM(wParam);
	if (SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &scrollAmount, 0) == 0)
		abort();
	if (scrollAmount == WHEEL_PAGESCROLL)
		scrollAmount = t->pagesize;
	if (scrollAmount == 0)		// no mouse wheel scrolling (or t->pagesize == 0)
		return;
	// the rest of this is basically http://blogs.msdn.com/b/oldnewthing/archive/2003/08/07/54615.aspx and http://blogs.msdn.com/b/oldnewthing/archive/2003/08/11/54624.aspx
	// see those pages for information on subtleties
	delta += t->wheelCarry;
	lines = delta * ((int) scrollAmount) / WHEEL_DELTA;
	t->wheelCarry = delta - lines * WHEEL_DELTA / ((int) scrollAmount);
	vscrollby(t, -lines);
}

static void vscroll(struct table *t, WPARAM wParam)
{
	SCROLLINFO si;
	intptr_t newpos;

	ZeroMemory(&si, sizeof (SCROLLINFO));
	si.cbSize = sizeof (SCROLLINFO);
	si.fMask = SIF_POS | SIF_TRACKPOS;
	if (GetScrollInfo(t->hwnd, SB_VERT, &si) == 0)
		abort();

	newpos = t->firstVisible;
	switch (LOWORD(wParam)) {
	case SB_TOP:
		newpos = 0;
		break;
	case SB_BOTTOM:
		newpos = t->count - t->pagesize;
		break;
	case SB_LINEUP:
		newpos--;
		break;
	case SB_LINEDOWN:
		newpos++;
		break;
	case SB_PAGEUP:
		newpos -= t->pagesize;
		break;
	case SB_PAGEDOWN:
		newpos += t->pagesize;
		break;
	case SB_THUMBPOSITION:
		newpos = (intptr_t) (si.nPos);
		break;
	case SB_THUMBTRACK:
		newpos = (intptr_t) (si.nTrackPos);
	}

	vscrollto(t, newpos);
}

static void resize(struct table *t)
{
	RECT r;
	SCROLLINFO si;
	HDLAYOUT headerlayout;
	WINDOWPOS headerpos;

	// do this first so our scrollbar calculations can be correct
	if (GetClientRect(t->hwnd, &r) == 0)		// use the whole client rect
		abort();
	headerlayout.prc = &r;
	headerlayout.pwpos = &headerpos;
	if (SendMessageW(t->header, HDM_LAYOUT, 0, (LPARAM) (&headerlayout)) == FALSE)
		abort();
	if (SetWindowPos(t->header, headerpos.hwndInsertAfter, headerpos.x, headerpos.y, headerpos.cx, headerpos.cy, headerpos.flags | SWP_SHOWWINDOW) == 0)
		abort();
	t->headerHeight = headerpos.cy;

	// now adjust the scrollbars
	r = realClientRect(t);
	t->pagesize = (r.bottom - r.top) / rowHeight(t);
	ZeroMemory(&si, sizeof (SCROLLINFO));
	si.cbSize = sizeof (SCROLLINFO);
	si.fMask = SIF_RANGE | SIF_PAGE;
	si.nMin = 0;
	si.nMax = t->count - 1;
	si.nPage = t->pagesize;
	SetScrollInfo(t->hwnd, SB_VERT, &si, TRUE);

	recomputeHScroll(t);
}

static void drawItems(struct table *t, HDC dc, RECT cliprect)
{
	HFONT thisfont, prevfont;
	TEXTMETRICW tm;
	LONG y;
	intptr_t i;
	RECT controlSize;		// for filling the entire selected row
	intptr_t first, last;
	POINT prevOrigin, prevViewportOrigin;

	if (GetClientRect(t->hwnd, &controlSize) == 0)
		abort();

	thisfont = t->font;		// in case WM_SETFONT happens before we return
	prevfont = (HFONT) SelectObject(dc, thisfont);
	if (prevfont == NULL)
		abort();
	if (GetTextMetricsW(dc, &tm) == 0)
		abort();

	// adjust the clip rect and the window so that (0, 0) is always the first item
	// adjust the viewport so that everything is shifted down t->headerHeight pixels
	if (OffsetRect(&cliprect, 0, t->firstVisible * tm.tmHeight) == 0)
		abort();
	if (GetWindowOrgEx(dc, &prevOrigin) == 0)
		abort();
	if (SetWindowOrgEx(dc, prevOrigin.x, prevOrigin.y + (t->firstVisible * tm.tmHeight), NULL) == 0)
		abort();
	if (SetViewportOrgEx(dc, 0, t->headerHeight, &prevViewportOrigin) == 0)
		abort();

	// see http://blogs.msdn.com/b/oldnewthing/archive/2003/07/29/54591.aspx and http://blogs.msdn.com/b/oldnewthing/archive/2003/07/30/54600.aspx
	first = cliprect.top / tm.tmHeight;
	if (first < 0)
		first = 0;
	last = (cliprect.bottom + tm.tmHeight - 1) / tm.tmHeight;
	if (last >= t->count)
		last = t->count;

	y = first * tm.tmHeight;
	for (i = first; i < last; i++) {
		RECT rsel;
		HBRUSH background;
		int textColor;
		WCHAR msg[100];
		RECT headeritem;
		intptr_t j;

		// TODO verify these two
		background = (HBRUSH) (COLOR_WINDOW + 1);
		textColor = COLOR_WINDOWTEXT;
		if (t->selected == i) {
			// these are the colors wine uses (http://source.winehq.org/source/dlls/comctl32/listview.c)
			// the two for unfocused are also suggested by http://stackoverflow.com/questions/10428710/windows-forms-inactive-highlight-color
			background = (HBRUSH) (COLOR_HIGHLIGHT + 1);
			textColor = COLOR_HIGHLIGHTTEXT;
			if (GetFocus() != t->hwnd) {
				background = (HBRUSH) (COLOR_BTNFACE + 1);
				textColor = COLOR_BTNTEXT;
			}
		}

		// first fill the selection rect
		rsel.left = controlSize.left;
		rsel.top = y;
		rsel.right = controlSize.right - controlSize.left;
		rsel.bottom = y + tm.tmHeight;
		if (FillRect(dc, &rsel, background) == 0)
			abort();

		// now draw the cells
		if (SetTextColor(dc, GetSysColor(textColor)) == CLR_INVALID)
			abort();
		if (SetBkMode(dc, TRANSPARENT) == 0)
			abort();
		for (j = 0; j < t->nColumns; j++) {
			if (SendMessageW(t->header, HDM_GETITEMRECT, (WPARAM) j, (LPARAM) (&headeritem)) == 0)
				abort();
			rsel.left = headeritem.left + SendMessageW(t->header, HDM_GETBITMAPMARGIN, 0, 0);
			rsel.top = y;
			rsel.right = headeritem.right;
			rsel.bottom = y + tm.tmHeight;
			if (DrawTextExW(dc, msg, wsprintf(msg, L"Item %d", i), &rsel, DT_END_ELLIPSIS | DT_LEFT | DT_NOPREFIX | DT_SINGLELINE, NULL) == 0)
				abort();
		}
		y += tm.tmHeight;
	}

	// reset everything
	if (SetViewportOrgEx(dc, prevViewportOrigin.x, prevViewportOrigin.y, NULL) == 0)
		abort();
	if (SetWindowOrgEx(dc, prevOrigin.x, prevOrigin.y, NULL) == 0)
		abort();
	if (SelectObject(dc, prevfont) != (HGDIOBJ) (thisfont))
		abort();
}

static LRESULT CALLBACK tableWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	struct table *t;
	HDC dc;
	PAINTSTRUCT ps;
	NMHDR *nmhdr = (NMHDR *) lParam;
	NMHEADERW *nm = (NMHEADERW *) lParam;

	t = (struct table *) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
	if (t == NULL) {
		// we have to do things this way because creating the header control will fail mysteriously if we create it first thing
		// (which is fine; we can get the parent hInstance this way too)
		if (uMsg == WM_NCCREATE) {
			CREATESTRUCTW *cs = (CREATESTRUCTW *) lParam;

			t = (struct table *) malloc(sizeof (struct table));
			if (t == NULL)
				abort();
			ZeroMemory(t, sizeof (struct table));
			t->hwnd = hwnd;
			// TODO this should be a global
			t->defaultFont = (HFONT) GetStockObject(SYSTEM_FONT);
			if (t->defaultFont == NULL)
				abort();
			t->font = t->defaultFont;
t->selected = 5;t->count=100;//TODO
			t->header = CreateWindowExW(0,
				WC_HEADERW, L"",
				// TODO is HOTTRACK needed?
				WS_CHILD | HDS_FULLDRAG | HDS_HORZ | HDS_HOTTRACK,
				0, 0, 0, 0,
				t->hwnd, (HMENU) 100, cs->hInstance, NULL);
			if (t->header == NULL)
				abort();
{HDITEMW item;
ZeroMemory(&item, sizeof (HDITEMW));
item.mask = HDI_WIDTH | HDI_TEXT | HDI_FORMAT;
item.cxy = 200;
item.pszText = L"Column";
item.fmt = HDF_LEFT | HDF_STRING;
if (SendMessage(t->header, HDM_INSERTITEM, 0, (LPARAM) (&item)) == (LRESULT) (-1))
abort();
ZeroMemory(&item, sizeof (HDITEMW));
item.mask = HDI_WIDTH | HDI_TEXT | HDI_FORMAT;
item.cxy = 150;
item.pszText = L"Column 2";
item.fmt = HDF_LEFT | HDF_STRING;
if (SendMessage(t->header, HDM_INSERTITEM, 1, (LPARAM) (&item)) == (LRESULT) (-1))
abort();
t->nColumns=2;}
			SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) t);
		}
		// even if we did the above, fall through
		return DefWindowProcW(hwnd, uMsg, wParam, lParam);
	}
	switch (uMsg) {
	case WM_PAINT:
		dc = BeginPaint(hwnd, &ps);
		if (dc == NULL)
			abort();
		drawItems(t, dc, ps.rcPaint);
		EndPaint(hwnd, &ps);
		return 0;
	case WM_SETFONT:
		t->font = (HFONT) wParam;
		if (t->font == NULL)
			t->font = t->defaultFont;
		// also set the header font
		SendMessageW(t->header, WM_SETFONT, wParam, lParam);
		if (LOWORD(lParam) != FALSE) {
			// the scrollbar page size will change so redraw that too
			// also recalculate the header height
			// TODO do that when this is FALSE too somehow
			resize(t);
			redrawAll(t);
		}
		return 0;
	case WM_GETFONT:
		return (LRESULT) t->font;
	case WM_VSCROLL:
		vscroll(t, wParam);
		return 0;
	case WM_MOUSEWHEEL:
		wheelscroll(t, wParam);
		return 0;
	case WM_SIZE:
		resize(t);
		return 0;
	case WM_LBUTTONDOWN:
		selectItem(t, wParam, lParam);
		return 0;
	case WM_SETFOCUS:
	case WM_KILLFOCUS:
		// all we need to do here is redraw the highlight
		// TODO localize to just the selected item
		// TODO ensure giving focus works right
		redrawAll(t);
		return 0;
	case WM_KEYDOWN:
		keySelect(t, wParam, lParam);
		return 0;
	// TODO header double-click
	case WM_NOTIFY:
		if (nmhdr->hwndFrom == t->header)
			switch (nmhdr->code) {
			// I could use HDN_TRACK but wine doesn't emit that
			case HDN_ITEMCHANGING:
			case HDN_ITEMCHANGED:		// TODO needed?
				recomputeHScroll(t);
				redrawAll(t);
				return FALSE;
			}
		// otherwise fall through
	default:
		return DefWindowProcW(hwnd, uMsg, wParam, lParam);
	}
	abort();
	return 0;		// unreached
}

void makeTableWindowClass(void)
{
	WNDCLASSW wc;

	ZeroMemory(&wc, sizeof (WNDCLASSW));
	wc.lpszClassName = tableWindowClass;
	wc.lpfnWndProc = tableWndProc;
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
	wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);		// TODO correct?
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.hInstance = GetModuleHandle(NULL);
	if (RegisterClassW(&wc) == 0)
		abort();
}

int main(void)
{
	HWND mainwin;
	MSG msg;
	INITCOMMONCONTROLSEX icc;

	ZeroMemory(&icc, sizeof (INITCOMMONCONTROLSEX));
	icc.dwSize = sizeof (INITCOMMONCONTROLSEX);
	icc.dwICC = ICC_LISTVIEW_CLASSES;
	if (InitCommonControlsEx(&icc) == 0)
		abort();
	makeTableWindowClass();
	mainwin = CreateWindowExW(0,
		tableWindowClass, L"Main Window",
		WS_OVERLAPPEDWINDOW | WS_HSCROLL | WS_VSCROLL,
		CW_USEDEFAULT, CW_USEDEFAULT,
		400, 400,
		NULL, NULL, GetModuleHandle(NULL), NULL);
	if (mainwin == NULL)
		abort();
	ShowWindow(mainwin, SW_SHOWDEFAULT);
	if (UpdateWindow(mainwin) == 0)
		abort();
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	return 0;
}
