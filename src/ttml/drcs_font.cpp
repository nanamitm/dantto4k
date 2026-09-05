// Rasterizes a character from an installed font into a DRCS pattern.
//
// An ARIB caption can only carry the characters its graphic sets have codes
// for, and a 4K subtitle is Unicode text that is not limited that way. The
// handful of characters that fall outside - see canEncodeCaption() - would
// otherwise be dropped, so they are drawn here instead and sent as a DRCS
// pattern, which every caption decoder can display.
//
// Windows only: the fallback needs a system font rasterizer, and there is no
// portable one here. Elsewhere this returns nothing and the character is
// dropped as before.
#include "ttml/drcs.h"

#include "config.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

void subtitleDebugLog(const std::string& line);

namespace arib {

namespace ttml {

namespace {

// A caption cell is square and small, so a gothic face at the cell size is what
// a broadcast DRCS looks like. The name is configurable because the fallback is
// only as good as the font behind it.
std::wstring fallbackFaceName()
{
    if (!config.drcsFallbackFont.empty()) {
        const int needed = MultiByteToWideChar(CP_UTF8, 0, config.drcsFallbackFont.c_str(),
                                               static_cast<int>(config.drcsFallbackFont.size()),
                                               nullptr, 0);
        if (needed > 0) {
            std::wstring wide(static_cast<size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, config.drcsFallbackFont.c_str(),
                                static_cast<int>(config.drcsFallbackFont.size()),
                                wide.data(), needed);
            return wide;
        }
    }
    return L"Yu Gothic";
}

// The pattern is 1 bit per pixel, rows packed continuously with no padding -
// the layout build_drcs_data_unit writes and a decoder expects.
void setPixel(std::vector<uint8_t>& bits, uint8_t width, int x, int y)
{
    const size_t bit = static_cast<size_t>(y) * width + x;
    bits[bit / 8] |= static_cast<uint8_t>(0x80 >> (bit % 8));
}

} // namespace

DrcsPattern rasterize_font_glyph(uint32_t codepoint, uint8_t width, uint8_t height)
{
    DrcsPattern pattern;
    if (width == 0 || height == 0 || codepoint == 0) {
        return pattern;
    }

    // Surrogate pairs for anything outside the BMP; GDI takes UTF-16.
    wchar_t text[2] = {};
    int textLength = 0;
    if (codepoint <= 0xFFFF) {
        text[0] = static_cast<wchar_t>(codepoint);
        textLength = 1;
    }
    else if (codepoint <= 0x10FFFF) {
        const uint32_t v = codepoint - 0x10000;
        text[0] = static_cast<wchar_t>(0xD800 + (v >> 10));
        text[1] = static_cast<wchar_t>(0xDC00 + (v & 0x3FF));
        textLength = 2;
    }
    else {
        return pattern;
    }

    const HDC screen = GetDC(nullptr);
    const HDC dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!dc) {
        return pattern;
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    const HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        DeleteDC(dc);
        return pattern;
    }
    const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    const std::wstring face = fallbackFaceName();
    const HFONT font = CreateFontW(-static_cast<int>(height), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   SHIFTJIS_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face.c_str());
    const HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;

    RECT rect{ 0, 0, width, height };
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, OPAQUE);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &rect, nullptr, 0, nullptr);

    // Fit the glyph to the cell: a Kanji drawn at the cell height overflows it
    // once the font's ascent and descent are counted, and a caption cell has no
    // room to spare.
    SIZE extent{};
    if (font && GetTextExtentPoint32W(dc, text, textLength, &extent) && extent.cx > 0) {
        if (extent.cx > width) {
            const int fitted = MulDiv(static_cast<int>(height), width, static_cast<int>(extent.cx));
            SelectObject(dc, oldFont);
            DeleteObject(font);
            const HFONT refitted = CreateFontW(-fitted, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                               SHIFTJIS_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                               ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                               face.c_str());
            if (refitted) {
                SelectObject(dc, refitted);
                DrawTextW(dc, text, textLength, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(dc, oldFont);
                DeleteObject(refitted);
            }
        }
        else {
            DrawTextW(dc, text, textLength, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            DeleteObject(font);
        }
    }
    else if (font) {
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }

    // The glyph is anti-aliased; a DRCS is not, so take everything above half
    // brightness. Anything thinner than that is not readable in a caption cell.
    const auto* rgba = static_cast<const uint8_t*>(pixels);
    std::vector<uint8_t> ink(static_cast<size_t>(width) * height, 0);
    int lit = 0;
    int minX = width, maxX = -1, minY = height, maxY = -1;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = rgba + (static_cast<size_t>(y) * width + x) * 4;
            const int luma = (p[2] * 77 + p[1] * 151 + p[0] * 28) >> 8;
            if (luma >= 128) {
                ink[static_cast<size_t>(y) * width + x] = 1;
                ++lit;
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }

    // Centre what was actually drawn rather than the font's line box, whose
    // ascent and descent leave the glyph sitting high in the cell.
    std::vector<uint8_t> bits((static_cast<size_t>(width) * height + 7) / 8, 0);
    if (lit > 0) {
        const int shiftX = (width - 1 - maxX - minX) / 2;
        const int shiftY = (height - 1 - maxY - minY) / 2;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                if (!ink[static_cast<size_t>(y) * width + x]) {
                    continue;
                }
                const int dx = x + shiftX, dy = y + shiftY;
                if (dx >= 0 && dx < width && dy >= 0 && dy < height) {
                    setPixel(bits, width, dx, dy);
                }
            }
        }
    }

    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);

    if (lit == 0) {
        return pattern;
    }

    pattern.width = width;
    pattern.height = height;
    pattern.bits = std::move(bits);
    return pattern;
}

} // namespace ttml

} // namespace arib

#else

namespace arib {

namespace ttml {

DrcsPattern rasterize_font_glyph(uint32_t, uint8_t, uint8_t)
{
    return {};
}

} // namespace ttml

} // namespace arib

#endif
