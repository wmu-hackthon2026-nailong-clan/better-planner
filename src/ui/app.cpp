#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "api/http_server.hpp"
#include "api/registry.hpp"
#include "api/types.hpp"
#include "core/event.hpp"
#include "core/layout.hpp"
#include "core/queries.hpp"
#include "core/store.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "comdlg32.lib")

namespace {

constexpr std::int64_t kDayStart = 0;
constexpr std::int64_t kDaySeconds = 86400;
constexpr float kLaneShift = 30.0f;
constexpr float kTitleLines = 22.0f;
constexpr float kTimeLines = 16.0f;
constexpr float kTopBar = 64.0f;
constexpr UINT kEventsChangedMessage = WM_APP + 1;

constexpr float kPixelsPerHour = 80.0f;
constexpr float kAxisWidth = 62.0f;
constexpr float kRightMargin = 24.0f;
constexpr float kBottomMargin = 20.0f;

ID2D1Factory* g_factory = nullptr;
ID2D1HwndRenderTarget* g_target = nullptr;
IDWriteFactory* g_dwrite = nullptr;
IDWriteTextFormat* g_titleFormat = nullptr;
IDWriteTextFormat* g_timeFormat = nullptr;
IDWriteTextFormat* g_axisFormat = nullptr;
IWICImagingFactory* g_wic = nullptr;

enum class ViewMode { Day, Week, Month };
ViewMode g_view = ViewMode::Day;
int g_selectedDay = 0;
int g_scrollY = 0;

struct HitBlock {
    D2D1_RECT_F rect;
    std::vector<bp::Event> events;
};

struct HitDay {
    D2D1_RECT_F rect;
    int day;
};

struct Brushes {
    ID2D1SolidColorBrush* fill = nullptr;
    ID2D1SolidColorBrush* edge = nullptr;
    ID2D1SolidColorBrush* highlight = nullptr;
    ID2D1SolidColorBrush* text = nullptr;
    ID2D1SolidColorBrush* dim = nullptr;
};

std::vector<HitBlock> g_hits;
std::vector<HitDay> g_dayHits;

std::wstring toWide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        wide.data(), count);
    return wide;
}

std::wstring formatClock(std::int64_t seconds) {
    const int hours = static_cast<int>(seconds / 3600);
    const int minutes = static_cast<int>((seconds % 3600) / 60);
    wchar_t buffer[16] = {};
    swprintf(buffer, 16, L"%02d:%02d", hours, minutes);
    return buffer;
}

std::wstring shortTitle(const bp::Event& event) {
    std::wstring wide = toWide(event.title);
    if (wide.size() > 5) {
        wide = wide.substr(0, 5) + L"\u2026";
    }
    return wide;
}

std::wstring spanText(const bp::Event& event) {
    if (event.kind == bp::Kind::Deadline) {
        return std::wstring(L"\u622a\u6b62 ") + formatClock(event.start);
    }
    return formatClock(event.start) + L" - " + formatClock(event.end);
}

std::wstring detailText(const std::vector<bp::Event>& events) {
    std::wostringstream stream;
    stream << L"\u5171 " << events.size() << L" \u4e2a\u4e8b\u4ef6\uff1a";
    for (const auto& event : events) {
        stream << L"\n\n" << toWide(event.title) << L"\n"
               << spanText(event) << L"\n"
               << toWide(event.note);
    }
    return stream.str();
}

std::wstring viewName() {
    switch (g_view) {
        case ViewMode::Day:
            return L"\u65e5\u8fdb\u7a0b";
        case ViewMode::Week:
            return L"\u5468\u8fdb\u7a0b";
        case ViewMode::Month:
            return L"\u6708\u8fdb\u7a0b";
    }
    return L"";
}

void SeedDemoData(const std::shared_ptr<bp::EventStore>& store) {
    const auto schedule = [&](int day, int sh, int sm, int eh, int em,
                              const char* title, const char* note,
                              bool byDuration) {
        bp::api::ScheduleInput input;
        input.start = static_cast<std::int64_t>(day) * kDaySeconds +
                      sh * 3600 + sm * 60;
        if (byDuration) {
            input.durationSeconds = (eh * 3600 + em * 60) -
                                    static_cast<std::int64_t>(sh * 3600 + sm * 60);
        } else {
            input.end = static_cast<std::int64_t>(day) * kDaySeconds +
                        eh * 3600 + em * 60;
        }
        input.title = title;
        input.note = note;
        store->add(bp::api::toEvent(input));
    };
    const auto deadline = [&](int day, int hour, const char* title,
                              const char* note) {
        bp::api::DeadlineInput input;
        input.when = static_cast<std::int64_t>(day) * kDaySeconds + hour * 3600;
        input.title = title;
        input.note = note;
        store->add(bp::api::toEvent(input));
    };

    schedule(0, 8, 0, 9, 30, "\u9ad8\u7b49\u6570\u5b66",
             "3-201 \u6559\u5ba4", false);
    schedule(0, 10, 0, 11, 30, "\u6570\u636e\u7ed3\u6784",
             "\u5357\u697c 402\uff0c\u5e26\u7b14\u8bb0\u672c", true);
    schedule(0, 10, 0, 11, 30, "\u6570\u636e\u7ed3\u6784\u5b9e\u9a8c",
             "\u5e26\u4e0a\u5b9e\u9a8c\u62a5\u544a", false);
    schedule(0, 13, 0, 17, 0, "\u5b9e\u9a8c\u5ba4\u503c\u73ed",
             "\u503c\u73ed\u8868\u8d34\u5728\u95e8\u53e3", false);
    schedule(0, 14, 0, 15, 0, "\u5c0f\u7ec4\u4f1a\u8bae",
             "\u6c47\u62a5\u8fdb\u5ea6", true);
    schedule(0, 16, 0, 18, 0, "\u7bee\u7403\u8bad\u7ec3",
             "\u4f53\u80b2\u9986\u96c6\u5408", false);
    deadline(0, 21, "\u82f1\u8bed\u4f5c\u4e1a\u4ea4",
             "U5 \u9605\u8bfb + \u4f5c\u6587");

    schedule(1, 9, 0, 10, 30, "\u4f53\u80b2\u8bfe",
             "\u64cd\u573a\u96c6\u5408", true);
    deadline(1, 19, "\u7ebf\u4ee3\u4f5c\u4e1a\u4ea4",
             "\u7b2c 3 \u7ae0\u4e60\u540e\u4e60\u9898");

    schedule(2, 10, 0, 12, 0, "\u6570\u636e\u5e93\u8bfe\u7a0b",
             "\u5b9e\u9a8c\u697c B202", false);
    schedule(2, 15, 0, 16, 30, "\u82f1\u8bed\u89d2",
             "\u56fe\u4e66\u9986\u4e00\u697c", true);

    schedule(4, 8, 30, 12, 0, "\u7ade\u8d5b\u7b54\u8fa9",
             "\u5e26\u597d PPT \u548c\u6f14\u793a\u673a", false);

    schedule(6, 14, 0, 16, 0, "\u793e\u56e2\u6d3b\u52a8",
             "\u5b66\u751f\u4e2d\u5fc3 305", true);
}

void CreateTextFormats() {
    g_dwrite->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"zh-CN",
        &g_titleFormat);
    g_dwrite->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN",
        &g_timeFormat);
    g_dwrite->CreateTextFormat(
        L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN",
        &g_axisFormat);
}

void CreateTarget(HWND hwnd) {
    RECT rect = {};
    GetClientRect(hwnd, &rect);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(rect.right - rect.left),
        static_cast<UINT32>(rect.bottom - rect.top));
    g_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(hwnd, size),
        &g_target);
}

void ReleaseTextFormats() {
    if (g_titleFormat) g_titleFormat->Release();
    if (g_timeFormat) g_timeFormat->Release();
    if (g_axisFormat) g_axisFormat->Release();
    if (g_dwrite) g_dwrite->Release();
}

void DrawTextLine(const std::wstring& text, const D2D1_RECT_F& rect,
                  IDWriteTextFormat* format, ID2D1SolidColorBrush* brush) {
    g_target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format,
                        rect, brush);
}

void DrawGlassBlock(const D2D1_RECT_F& rect, const Brushes& b) {
    g_target->FillRectangle(rect, b.fill);
    g_target->DrawRectangle(rect, b.edge, 0.5f);
    g_target->DrawLine(D2D1::Point2F(rect.left + 8.0f, rect.top + 1.5f),
                       D2D1::Point2F(rect.right - 8.0f, rect.top + 1.5f),
                       b.highlight, 1.5f);
    g_target->DrawLine(D2D1::Point2F(rect.left + 8.0f, rect.bottom - 1.5f),
                       D2D1::Point2F(rect.right - 8.0f, rect.bottom - 1.5f),
                       b.highlight, 1.0f);
}

void RenderTopBar(const Brushes& b, const D2D1_RECT_F& buttonRect) {
    g_target->FillRectangle(buttonRect, b.fill);
    g_target->DrawRectangle(buttonRect, b.edge, 0.5f);
    for (int i = 0; i < 3; ++i) {
        const float y = buttonRect.top + 7.0f + static_cast<float>(i) * 6.0f;
        g_target->DrawLine(
            D2D1::Point2F(buttonRect.left + 8.0f, y),
            D2D1::Point2F(buttonRect.right - 8.0f, y), b.highlight, 1.2f);
    }
    DrawTextLine(viewName(),
                 D2D1::RectF(buttonRect.right + 12.0f, 12.0f, 400.0f, 40.0f),
                 g_titleFormat, b.text);

    const D2D1_RECT_F exportRect = D2D1::RectF(64.0f, 12.0f, 120.0f, 40.0f);
    g_target->FillRectangle(exportRect, b.fill);
    g_target->DrawRectangle(exportRect, b.edge, 0.5f);
    DrawTextLine(L"\u5bfc\u51fa PNG", exportRect, g_axisFormat, b.text);
}

// 时间轴总跨度覆盖完整一天，超出 7:00–22:00 的事件也可滚动看到。
constexpr std::int64_t kAxisStart = 0;
constexpr std::int64_t kAxisEnd = 86400;

float timeToY(std::int64_t seconds) {
    return static_cast<float>(seconds) / 3600.0f * kPixelsPerHour;
}

int dayViewContentHeight() {
    return static_cast<int>(timeToY(kAxisEnd) + kBottomMargin);
}

void UpdateScrollRange(HWND hwnd) {
    RECT rect = {};
    GetClientRect(hwnd, &rect);
    const int viewport = rect.bottom - static_cast<int>(kTopBar);
    const int content = dayViewContentHeight();
    SCROLLINFO info = {};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE;
    info.nMin = 0;
    info.nMax = content > viewport ? content - viewport : 0;
    info.nPage = viewport;
    SetScrollInfo(hwnd, SB_VERT, &info, TRUE);
}

void RenderDayView(const Brushes& b, const D2D1_SIZE_F& size) {
    const float axisX = kAxisWidth;
    const float x0 = axisX + 14.0f;
    const float x1 = size.width - kRightMargin;
    const float y0 = kTopBar;
    const float offset = -static_cast<float>(g_scrollY);

    for (int hour = 0; hour <= 24; ++hour) {
        const float y = y0 + timeToY(hour * 3600) + offset;
        g_target->DrawLine(D2D1::Point2F(axisX - 6.0f, y),
                           D2D1::Point2F(axisX, y), b.dim, 1.0f);
        if (hour % 2 == 0) {
            DrawTextLine(formatClock(hour * 3600),
                         D2D1::RectF(8.0f, y - 8.0f, axisX - 8.0f, y + 8.0f),
                         g_axisFormat, b.dim);
        }
    }
    g_target->DrawLine(D2D1::Point2F(axisX, y0),
                       D2D1::Point2F(axisX, y0 + timeToY(kAxisEnd) + offset),
                       b.dim, 1.0f);

    auto& registry = bp::api::Registry::instance();
    const std::int64_t dayBase =
        kDayStart + static_cast<std::int64_t>(g_selectedDay) * kDaySeconds;
    const std::vector<bp::Event> events =
        registry.store()->range(dayBase, dayBase + kDaySeconds);
    const std::vector<bp::LayoutEngine::Block> blocks =
        registry.layoutEngine()->layout(events);

    const float workWidth = x1 - x0;
    g_hits.reserve(blocks.size());

    for (const auto& block : blocks) {
        std::vector<bp::Event> blockEvents;
        blockEvents.reserve(block.eventIndices.size());
        for (const auto index : block.eventIndices) {
            blockEvents.push_back(events[index]);
        }
        if (blockEvents.empty()) {
            continue;
        }

        const std::int64_t blockStart = blockEvents.front().start;
        const std::int64_t blockEnd = blockEvents.front().end;
        const bool isDeadline =
            blockEvents.front().kind == bp::Kind::Deadline;

        const float shift = static_cast<float>(block.lane) * kLaneShift;
        const float blockX = x0 + shift;
        const float blockW = workWidth * 0.68f - shift;
        float yTop = y0 + timeToY(blockStart - dayBase) + offset;
        float yBottom = y0 + timeToY(blockEnd - dayBase) + offset;
        if (isDeadline) {
            yTop -= 12.0f;
            yBottom = yTop + 30.0f;
        }
        if (yBottom - yTop < 26.0f) {
            yBottom = yTop + 26.0f;
        }

        const D2D1_RECT_F rect =
            D2D1::RectF(blockX, yTop, blockX + blockW, yBottom);
        DrawGlassBlock(rect, b);
        g_hits.push_back(HitBlock{rect, blockEvents});

        float cursor = yTop + 7.0f;
        const float textRight = blockX + blockW - 8.0f;
        for (std::size_t i = 0; i < blockEvents.size(); ++i) {
            const auto& event = blockEvents[i];
            const std::wstring title =
                block.summary ? shortTitle(event) : toWide(event.title);
            DrawTextLine(title,
                         D2D1::RectF(blockX + 10.0f, cursor, textRight,
                                     cursor + kTitleLines),
                         g_titleFormat, b.text);
            cursor += kTitleLines;
            if (!block.summary) {
                DrawTextLine(spanText(event),
                             D2D1::RectF(blockX + 10.0f, cursor, textRight,
                                         cursor + kTimeLines),
                             g_timeFormat, b.dim);
                cursor += kTimeLines;
                const std::wstring note = toWide(event.note);
                if (!note.empty() && yBottom - cursor > kTimeLines) {
                    DrawTextLine(note,
                                 D2D1::RectF(blockX + 10.0f, cursor, textRight,
                                             cursor + kTimeLines),
                                 g_timeFormat, b.dim);
                    cursor += kTimeLines;
                }
            }
            if (i + 1 < blockEvents.size()) {
                g_target->DrawLine(
                    D2D1::Point2F(blockX + 10.0f, cursor + 3.0f),
                    D2D1::Point2F(blockX + blockW - 10.0f, cursor + 3.0f),
                    b.edge, 0.5f);
                cursor += 10.0f;
            }
            if (cursor > yBottom - 4.0f) {
                break;
            }
        }
    }
}

void RenderWeekView(const Brushes& b, const D2D1_SIZE_F& size) {
    const float x0 = 16.0f;
    const float x1 = size.width - 16.0f;
    const float y0 = kTopBar;
    const float colW = (x1 - x0) / 7.0f;
    const wchar_t* weekdays[7] = {L"\u5468\u4e00", L"\u5468\u4e8c",
                                  L"\u5468\u4e09", L"\u5468\u56db",
                                  L"\u5468\u4e94", L"\u5468\u516d",
                                  L"\u5468\u65e5"};

    auto& registry = bp::api::Registry::instance();
    const std::vector<bp::Event> events = registry.store()->range(
        kDayStart, kDayStart + 7 * kDaySeconds);
    const auto grouped = bp::groupByDay(events, kDayStart, kDaySeconds);

    g_dayHits.reserve(7);
    for (int day = 0; day < 7; ++day) {
        const float left = x0 + static_cast<float>(day) * colW + 3.0f;
        const float right = left + colW - 6.0f;
        DrawTextLine(std::wstring(weekdays[day]) + L" " +
                         std::to_wstring(day + 1) + L"\u65e5",
                     D2D1::RectF(left, y0, right, y0 + 20.0f), g_timeFormat,
                     b.text);
        g_target->DrawLine(D2D1::Point2F(left, y0 + 24.0f),
                           D2D1::Point2F(right, y0 + 24.0f), b.dim, 0.5f);

        g_dayHits.push_back(HitDay{
            D2D1::RectF(left, y0 + 24.0f, right, size.height - 20.0f), day});

        const auto it = grouped.find(
            kDayStart + static_cast<std::int64_t>(day) * kDaySeconds);
        if (it == grouped.end()) {
            continue;
        }
        float cursor = y0 + 32.0f;
        for (const auto& event : it->second) {
            const D2D1_RECT_F rect =
                D2D1::RectF(left, cursor, right, cursor + 40.0f);
            DrawGlassBlock(rect, b);
            DrawTextLine(toWide(event.title),
                         D2D1::RectF(left + 6.0f, cursor + 3.0f, right - 4.0f,
                                     cursor + 20.0f),
                         g_axisFormat, b.text);
            DrawTextLine(spanText(event),
                         D2D1::RectF(left + 6.0f, cursor + 21.0f, right - 4.0f,
                                     cursor + 37.0f),
                         g_axisFormat, b.dim);
            cursor += 46.0f;
        }
    }
}

void RenderMonthView(const Brushes& b, const D2D1_SIZE_F& size) {
    const float x0 = 16.0f;
    const float x1 = size.width - 16.0f;
    const float y0 = kTopBar + 12.0f;
    const float colW = (x1 - x0) / 7.0f;
    const float cellH = (size.height - y0 - 20.0f) / 2.0f;

    auto& registry = bp::api::Registry::instance();
    const std::vector<bp::Event> events = registry.store()->range(
        kDayStart, kDayStart + 7 * kDaySeconds);
    const auto grouped = bp::groupByDay(events, kDayStart, kDaySeconds);

    for (int day = 0; day < 7; ++day) {
        const float left = x0 + static_cast<float>(day) * colW + 4.0f;
        const float right = left + colW - 8.0f;
        const float top = y0;
        const float bottom = top + cellH - 8.0f;
        const D2D1_RECT_F cell = D2D1::RectF(left, top, right, bottom);
        g_target->FillRectangle(cell, b.fill);
        g_target->DrawRectangle(cell, b.edge, 0.5f);

        DrawTextLine(std::to_wstring(day + 1) + L"\u65e5",
                     D2D1::RectF(left + 8.0f, top + 6.0f, right - 8.0f,
                                 top + 26.0f),
                     g_timeFormat, b.text);

        g_dayHits.push_back(HitDay{cell, day});

        const auto it = grouped.find(
            kDayStart + static_cast<std::int64_t>(day) * kDaySeconds);
        const int count =
            it == grouped.end() ? 0 : static_cast<int>(it->second.size());
        DrawTextLine(std::to_wstring(count) + L" \u4e2a\u4e8b\u4ef6",
                     D2D1::RectF(left + 8.0f, top + 30.0f, right - 8.0f,
                                 top + 46.0f),
                     g_axisFormat, b.dim);
        if (it != grouped.end()) {
            float cursor = top + 52.0f;
            for (std::size_t i = 0;
                 i < it->second.size() && i < 3; ++i) {
                DrawTextLine(L"\u00b7 " + toWide(it->second[i].title),
                             D2D1::RectF(left + 8.0f, cursor, right - 6.0f,
                                         cursor + 16.0f),
                             g_axisFormat, b.dim);
                cursor += 18.0f;
            }
        }
    }
}

bool ExportDayViewPng(const std::wstring& path) {
    if (!g_wic) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    }
    if (!g_wic) {
        return false;
    }

    const UINT width = 1080;
    const UINT height = static_cast<UINT>(dayViewContentHeight());

    IWICBitmap* wicBitmap = nullptr;
    if (FAILED(g_wic->CreateBitmap(
            width, height, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, &wicBitmap))) {
        return false;
    }

    ID2D1RenderTarget* bitmapTarget = nullptr;
    if (FAILED(g_factory->CreateWicBitmapRenderTarget(
            wicBitmap, D2D1::RenderTargetProperties(), &bitmapTarget))) {
        wicBitmap->Release();
        return false;
    }

    bitmapTarget->BeginDraw();
    bitmapTarget->Clear(D2D1::ColorF(0.043f, 0.055f, 0.086f, 1.0f));

    ID2D1SolidColorBrush* fill = nullptr;
    ID2D1SolidColorBrush* edge = nullptr;
    ID2D1SolidColorBrush* highlight = nullptr;
    ID2D1SolidColorBrush* text = nullptr;
    ID2D1SolidColorBrush* dim = nullptr;
    bitmapTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), &fill);
    bitmapTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.40f), &edge);
    bitmapTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.85f), &highlight);
    bitmapTarget->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.95f, 1.0f, 0.95f),
                                        &text);
    bitmapTarget->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.10f), &dim);

    const float axisX = kAxisWidth;
    const float x0 = axisX + 14.0f;
    const float x1 = static_cast<float>(width) - kRightMargin;
    const float y0 = 0.0f;

    for (int hour = 0; hour <= 24; ++hour) {
        const float y = y0 + timeToY(hour * 3600);
        bitmapTarget->DrawLine(D2D1::Point2F(axisX - 6.0f, y),
                               D2D1::Point2F(axisX, y), dim, 1.0f);
        if (hour % 2 == 0) {
            const std::wstring label = formatClock(hour * 3600);
            bitmapTarget->DrawTextW(label.c_str(),
                                    static_cast<UINT32>(label.size()),
                                    g_axisFormat,
                                    D2D1::RectF(8.0f, y - 8.0f, axisX - 8.0f,
                                                y + 8.0f),
                                    dim);
        }
    }
    bitmapTarget->DrawLine(D2D1::Point2F(axisX, y0),
                           D2D1::Point2F(axisX, y0 + timeToY(kAxisEnd)), dim,
                           1.0f);

    auto& registry = bp::api::Registry::instance();
    const std::int64_t dayBase =
        kDayStart + static_cast<std::int64_t>(g_selectedDay) * kDaySeconds;
    const std::vector<bp::Event> events =
        registry.store()->range(dayBase, dayBase + kDaySeconds);
    const std::vector<bp::LayoutEngine::Block> blocks =
        registry.layoutEngine()->layout(events);

    const float workWidth = x1 - x0;
    for (const auto& block : blocks) {
        std::vector<bp::Event> blockEvents;
        blockEvents.reserve(block.eventIndices.size());
        for (const auto index : block.eventIndices) {
            blockEvents.push_back(events[index]);
        }
        if (blockEvents.empty()) {
            continue;
        }
        const std::int64_t blockStart = blockEvents.front().start;
        const std::int64_t blockEnd = blockEvents.front().end;
        const bool isDeadline = blockEvents.front().kind == bp::Kind::Deadline;

        const float shift = static_cast<float>(block.lane) * kLaneShift;
        const float blockX = x0 + shift;
        const float blockW = workWidth * 0.68f - shift;
        float yTop = y0 + timeToY(blockStart - dayBase);
        float yBottom = y0 + timeToY(blockEnd - dayBase);
        if (isDeadline) {
            yTop -= 12.0f;
            yBottom = yTop + 30.0f;
        }
        if (yBottom - yTop < 26.0f) {
            yBottom = yTop + 26.0f;
        }

        const D2D1_RECT_F rect =
            D2D1::RectF(blockX, yTop, blockX + blockW, yBottom);
        bitmapTarget->FillRectangle(rect, fill);
        bitmapTarget->DrawRectangle(rect, edge, 0.5f);
        bitmapTarget->DrawLine(D2D1::Point2F(rect.left + 8.0f, rect.top + 1.5f),
                               D2D1::Point2F(rect.right - 8.0f, rect.top + 1.5f),
                               highlight, 1.5f);
        bitmapTarget->DrawLine(
            D2D1::Point2F(rect.left + 8.0f, rect.bottom - 1.5f),
            D2D1::Point2F(rect.right - 8.0f, rect.bottom - 1.5f), highlight,
            1.0f);

        float cursor = yTop + 7.0f;
        const float textRight = blockX + blockW - 8.0f;
        for (std::size_t i = 0; i < blockEvents.size(); ++i) {
            const auto& event = blockEvents[i];
            const std::wstring title = block.summary ? shortTitle(event)
                                                      : toWide(event.title);
            bitmapTarget->DrawTextW(
                title.c_str(), static_cast<UINT32>(title.size()), g_titleFormat,
                D2D1::RectF(blockX + 10.0f, cursor, textRight,
                            cursor + kTitleLines),
                text);
            cursor += kTitleLines;
            if (!block.summary) {
                const std::wstring span = spanText(event);
                bitmapTarget->DrawTextW(
                    span.c_str(), static_cast<UINT32>(span.size()),
                    g_timeFormat,
                    D2D1::RectF(blockX + 10.0f, cursor, textRight,
                                cursor + kTimeLines),
                    dim);
                cursor += kTimeLines;
                const std::wstring note = toWide(event.note);
                if (!note.empty() && yBottom - cursor > kTimeLines) {
                    bitmapTarget->DrawTextW(
                        note.c_str(), static_cast<UINT32>(note.size()),
                        g_timeFormat,
                        D2D1::RectF(blockX + 10.0f, cursor, textRight,
                                    cursor + kTimeLines),
                        dim);
                    cursor += kTimeLines;
                }
            }
            if (i + 1 < blockEvents.size()) {
                bitmapTarget->DrawLine(
                    D2D1::Point2F(blockX + 10.0f, cursor + 3.0f),
                    D2D1::Point2F(blockX + blockW - 10.0f, cursor + 3.0f), edge,
                    0.5f);
                cursor += 10.0f;
            }
            if (cursor > yBottom - 4.0f) {
                break;
            }
        }
    }

    bitmapTarget->EndDraw();

    IWICBitmapEncoder* encoder = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;

    if (SUCCEEDED(g_wic->CreateStream(&stream))) {
        if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
            stream->Release();
            stream = nullptr;
        }
    }
    if (stream && SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng,
                                                 nullptr, &encoder))) {
        if (SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr))) {
            frame->Initialize(nullptr);
            frame->SetSize(width, height);
            WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppPBGRA;
            frame->SetPixelFormat(&pixelFormat);
            if (SUCCEEDED(frame->WriteSource(wicBitmap, nullptr)) &&
                SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
                ok = true;
            }
        }
    }

    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (fill) fill->Release();
    if (edge) edge->Release();
    if (highlight) highlight->Release();
    if (text) text->Release();
    if (dim) dim->Release();
    bitmapTarget->Release();
    wicBitmap->Release();
    return ok;
}

void RenderView(HWND hwnd) {
    g_hits.clear();
    g_dayHits.clear();
    g_target->BeginDraw();
    g_target->Clear(D2D1::ColorF(0.043f, 0.055f, 0.086f, 1.0f));

    const D2D1_SIZE_F size = g_target->GetSize();
    Brushes b;
    g_target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), &b.fill);
    g_target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.40f), &b.edge);
    g_target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.85f), &b.highlight);
    g_target->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.95f, 1.0f, 0.95f),
                                    &b.text);
    g_target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.10f), &b.dim);

    const D2D1_RECT_F buttonRect = D2D1::RectF(12.0f, 12.0f, 52.0f, 40.0f);
    RenderTopBar(b, buttonRect);

    switch (g_view) {
        case ViewMode::Day:
            RenderDayView(b, size);
            break;
        case ViewMode::Week:
            RenderWeekView(b, size);
            break;
        case ViewMode::Month:
            RenderMonthView(b, size);
            break;
    }

    const HRESULT hr = g_target->EndDraw();
    if (b.fill) b.fill->Release();
    if (b.edge) b.edge->Release();
    if (b.highlight) b.highlight->Release();
    if (b.text) b.text->Release();
    if (b.dim) b.dim->Release();

    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
        g_target->Release();
        g_target = nullptr;
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            BeginPaint(hwnd, &ps);
            if (!g_target) CreateTarget(hwnd);
            if (g_target && g_titleFormat) RenderView(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (g_target) {
                g_target->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            }
            UpdateScrollRange(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            if (g_view != ViewMode::Day) {
                return DefWindowProcW(hwnd, msg, wp, lp);
            }
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            g_scrollY -= delta * 64 / WHEEL_DELTA;
            if (g_scrollY < 0) {
                g_scrollY = 0;
            }
            UpdateScrollRange(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            const int request = LOWORD(wp);
            const int step = 40;
            int newPos = g_scrollY;
            switch (request) {
                case SB_LINEUP: newPos -= step; break;
                case SB_LINEDOWN: newPos += step; break;
                case SB_PAGEUP: newPos -= 200; break;
                case SB_PAGEDOWN: newPos += 200; break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: newPos = HIWORD(wp); break;
                default: break;
            }
            RECT rect = {};
            GetClientRect(hwnd, &rect);
            const int viewport = rect.bottom - static_cast<int>(kTopBar);
            const int maxPos = dayViewContentHeight() - viewport;
            if (newPos < 0) newPos = 0;
            if (maxPos > 0 && newPos > maxPos) newPos = maxPos;
            if (newPos != g_scrollY) {
                g_scrollY = newPos;
                SetScrollPos(hwnd, SB_VERT, g_scrollY, TRUE);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const float mx = static_cast<float>(GET_X_LPARAM(lp));
            const float my = static_cast<float>(GET_Y_LPARAM(lp));
            if (mx >= 12.0f && mx <= 52.0f && my >= 12.0f && my <= 40.0f) {
                g_view = static_cast<ViewMode>(
                    (static_cast<int>(g_view) + 1) % 3);
                g_scrollY = 0;
                UpdateScrollRange(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (mx >= 64.0f && mx <= 120.0f && my >= 12.0f && my <= 40.0f) {
                wchar_t path[MAX_PATH] = L"dayview.png";
                OPENFILENAMEW ofn = {};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"PNG \u56fe\u7247 (*.png)\0*.png\0\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (GetSaveFileNameW(&ofn)) {
                    if (ExportDayViewPng(path)) {
                        const std::wstring savedMsg =
                            L"\u5df2\u5bfc\u51fa\u5230 " + std::wstring(path);
                        MessageBoxW(hwnd, savedMsg.c_str(),
                                    L"\u5bfc\u51fa\u6210\u529f", MB_OK);
                    } else {
                        MessageBoxW(hwnd, L"\u5bfc\u51fa\u5931\u8d25",
                                    L"\u9519\u8bef", MB_ICONERROR);
                    }
                }
                return 0;
            }
            if (g_view == ViewMode::Day) {
                for (const auto& hit : g_hits) {
                    if (mx >= hit.rect.left && mx <= hit.rect.right &&
                        my >= hit.rect.top && my <= hit.rect.bottom) {
                        const std::wstring detail = detailText(hit.events);
                        MessageBoxW(hwnd, detail.c_str(),
                                    L"\u4e8b\u4ef6\u8be6\u60c5", MB_OK);
                        break;
                    }
                }
                return 0;
            }
            for (const auto& hit : g_dayHits) {
                if (mx >= hit.rect.left && mx <= hit.rect.right &&
                    my >= hit.rect.top && my <= hit.rect.bottom) {
                    g_selectedDay = hit.day;
                    g_view = ViewMode::Day;
                    g_scrollY = 0;
                    UpdateScrollRange(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
            return 0;
        }
        case kEventsChangedMessage:
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    auto store = std::make_shared<bp::EventStore>();
    bp::api::Registry::instance().provideStore(store);
    SeedDemoData(store);

    const std::wstring title =
        L"Better Planner — events: " + std::to_wstring(store->size());

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&g_dwrite));
    CreateTextFormats();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     IID_PPV_ARGS(&g_wic));

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BetterPlannerWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, title.c_str(),
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              1000, 680, nullptr, nullptr, instance, nullptr);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    bp::api::HttpServer httpServer;
    if (httpServer.start(store, [hwnd] {
            PostMessageW(hwnd, kEventsChangedMessage, 0, 0);
        })) {
        const std::wstring httpTitle =
            title + L" | http://127.0.0.1:" +
            std::to_wstring(httpServer.port());
        SetWindowTextW(hwnd, httpTitle.c_str());
    } else {
        const std::wstring error = toWide(httpServer.lastError());
        SetWindowTextW(hwnd, (title + L" | HTTP failed: " + error).c_str());
    }

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    httpServer.stop();
    if (g_target) g_target->Release();
    ReleaseTextFormats();
    if (g_factory) g_factory->Release();
    if (g_wic) g_wic->Release();
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
