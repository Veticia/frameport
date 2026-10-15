// xr_sim_capture: save what Monado's compositor window shows (the simulated HMD's display = both eyes as
// the headset would get them) to a BMP, so a headset-free test can see what a game draws.
//
//   xr_sim_capture.exe <out.bmp> [window title, default "Monado"]
//
// Finds the first top-level window whose title starts with the given text (Monado's own window is
// "Monado (Windowed)", its peek window "Monado peek") and copies it with PrintWindow(PW_RENDERFULLCONTENT),
// which also works for a window behind others. Prints "capture: <w>x<h> <file>" or "capture: fail ...".
//
// Build: cl /nologo /O2 xr_sim_capture.cpp /link user32.lib gdi32.lib
// SPDX-License-Identifier: MIT

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <vector>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

struct Find {
	const wchar_t *prefix;
	HWND hwnd;
};

static BOOL CALLBACK enum_cb(HWND h, LPARAM lp)
{
	Find *f = (Find *)lp;
	wchar_t title[256];
	if (!IsWindowVisible(h) || GetWindowTextW(h, title, 256) <= 0)
		return TRUE;
	if (std::wcsncmp(title, f->prefix, std::wcslen(f->prefix)) == 0) {
		f->hwnd = h;
		return FALSE;
	}
	return TRUE;
}

int wmain(int argc, wchar_t **argv)
{
	if (argc < 2) {
		std::printf("usage: xr_sim_capture <out.bmp> [window title prefix]\n");
		return 1;
	}
	// Not DPI aware on purpose: Monado's window isn't either, so its client size in this process's
	// (scaled) coordinates is the size of its swapchain, which PrintWindow copies 1:1.
	Find f{argc > 2 ? argv[2] : L"Monado", nullptr};
	EnumWindows(enum_cb, (LPARAM)&f);
	if (!f.hwnd) {
		std::printf("capture: fail no window\n");
		return 2;
	}
	RECT r;
	GetClientRect(f.hwnd, &r);
	int w = r.right - r.left, h = r.bottom - r.top;
	if (w <= 0 || h <= 0) {
		std::printf("capture: fail empty window (minimised?)\n");
		return 2;
	}
	HDC wdc = GetDC(f.hwnd);
	HDC mdc = CreateCompatibleDC(wdc);
	HBITMAP bmp = CreateCompatibleBitmap(wdc, w, h);
	HGDIOBJ old = SelectObject(mdc, bmp);
	if (!PrintWindow(f.hwnd, mdc, PW_CLIENTONLY | PW_RENDERFULLCONTENT))
		BitBlt(mdc, 0, 0, w, h, wdc, 0, 0, SRCCOPY);
	SelectObject(mdc, old);

	BITMAPINFOHEADER bi{sizeof(bi)};
	bi.biWidth = w;
	bi.biHeight = h; // bottom-up, as BMP files store it
	bi.biPlanes = 1;
	bi.biBitCount = 24;
	bi.biCompression = BI_RGB;
	int row = (w * 3 + 3) & ~3;
	std::vector<unsigned char> px((size_t)row * h);
	GetDIBits(mdc, bmp, 0, h, px.data(), (BITMAPINFO *)&bi, DIB_RGB_COLORS);
	DeleteObject(bmp);
	DeleteDC(mdc);
	ReleaseDC(f.hwnd, wdc);

	FILE *out = _wfopen(argv[1], L"wb");
	if (!out) {
		std::printf("capture: fail cannot write\n");
		return 2;
	}
	BITMAPFILEHEADER fh{};
	fh.bfType = 0x4D42;
	fh.bfOffBits = sizeof(fh) + sizeof(bi);
	fh.bfSize = fh.bfOffBits + (DWORD)px.size();
	std::fwrite(&fh, sizeof(fh), 1, out);
	std::fwrite(&bi, sizeof(bi), 1, out);
	std::fwrite(px.data(), 1, px.size(), out);
	std::fclose(out);
	std::printf("capture: %dx%d %ls\n", w, h, argv[1]);
	return 0;
}
