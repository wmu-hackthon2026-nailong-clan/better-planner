#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "api/registry.hpp"
#include "api/types.hpp"
#include "core/event.hpp"
#include "core/layout.hpp"
#include "core/store.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace {

constexpr std::int64_t kDayStart = 0;
constexpr std::int64_t kViewStart = 7 * 3600;
constexpr std::int64_t kViewEnd = 22 * 3600;
constexpr float kLaneShift = 30.0f;
constexpr float kTitleLines = 22.0f;
constexpr float kTimeLines = 16.0f;

ID2D1Factory* g_factory = nullptr;
ID2D1HwndRenderTarget* g_target = nullptr;
IDWriteFactory* g_dwrite = nullptr;
IDWriteTextFormat* g_titleFormat = nullptr;
IDWriteTextFormat* g_timeFormat = nullptr;
IDWriteTextFormat* g_axisFormat = nullptr;

struct HitBlock {
    D2D1_RECT_F rect;
    std::vector<bp::Event> events;
};

std::vector<HitBlock> g_hits;

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

void SeedDemoData(const std::shared_ptr<bp::EventStore>& store) {
    const auto schedule = [&](int sh, int sm, int eh, int em, const char* title,
                              const char* note, bool byDuration) {
        bp::api::ScheduleInput input;
        input.start = sh * 3600 + sm * 60;
        if (byDuration) {
            input.durationSeconds = (eh * 3600 + em * 60) - input.start;
        } else {
            input.end = eh * 3600 + em * 60;
        }
        input.title = title;
        input.note = note;
        store->add(bp::api::toEvent(input));
    };

    schedule(8, 0, 9, 30, "\u9ad8\u7b49\u6570\u5b66",
             "3-201 \u6559\u5ba4", false);
    schedule(10, 0, 11, 30, "\u6570\u636e\u7ed3\u6784",
             "\u5357\u697c 402\uff0c\u5e26\u7b14\u8bb0\u672c", true);
    schedule(10, 0, 11, 30, "\u6570\u636e\u7ed3\u6784\u5b9e\u9a8c",
             "\u5e26\u4e0a\u5b9e\u9a8c\u62a5\u544a", false);
    schedule(13, 0, 17, 0, "\u5b9e\u9a8c\u5ba4\u503c\u73ed",
             "\u503c\u73ed\u8868\u8d34\u5728\u95e8\u53e3", false);
    schedule(14, 0, 15, 0, "\u5c0f\u7ec4\u4f1a\u8bae",
             "\u6c47\u62a5\u8fdb\u5ea6", true);
    schedule(16, 0, 18, 0, "\u7bee\u7403\u8bad\u7ec3",
             "\u4f53\u80b2\u9986\u96c6\u5408", false);

    bp::api::DeadlineInput deadline;
    deadline.when = 21 * 3600;
    deadline.title = "\u82f1\u8bed\u4f5c\u4e1a\u4ea4";
    deadline.note = "U5 \u9605\u8bfb + \u4f5c\u6587";
    store->add(bp::api::toEvent(deadline));
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

void DrawGlassBlock(const D2D1_RECT_F& rect,
                    ID2D1SolidColorBrush* fill,
                    ID2D1SolidColorBrush* edge,
                    ID2D1SolidColorBrush* highlight) {
    g_target->FillRectangle(rect, fill);
    g_target->DrawRectangle(rect, edge, 0.5f);
    g_target->DrawLine(D2D1::Point2F(rect.left + 8.0f, rect.top + 1.5f),
                       D2D1::Point2F(rect.right - 8.0f, rect.top + 1.5f),
                       highlight, 1.5f);
    g_target->DrawLine(D2D1::Point2F(rect.left + 8.0f, rect.bottom - 1.5f),
                       D2D1::Point2F(rect.right - 8.0f, rect.bottom - 1.5f),
                       highlight, 1.0f);
}

void RenderDayView(HWND hwnd) {
    g_hits.clear();
    g_target->BeginDraw();
    g_target->Clear(D2D1::ColorF(0.043f, 0.055f, 0.086f, 1.0f));

    const D2D1_SIZE_F size = g_target->GetSize();
    const float axisX = 62.0f;
    const float x0 = axisX + 14.0f;
    const float x1 = size.width - 24.0f;
    const float y0 = 20.0f;
    const float y1 = size.height - 20.0f;
    const double span = static_cast<double>(kViewEnd - kViewStart);

    ID2D1SolidColorBrush* fill = nullptr;
    ID2D1SolidColorBrush* edge = nullptr;
    ID2D1SolidColorBrush* highlight = nullptr;
    ID2D1SolidColorBrush* text = nullptr;
    ID2D1SolidColorBrush* dim = nullptr;
    g_target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), &fill);
    g_target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.40f), &edge);
    g_target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.85f), &highlight);
    g_target->CreateSolidColorBrush(D2D1::ColorF(0.92f, 0.95f, 1.0f, 0.95f),
                                    &text);
    g_target->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.10f), &dim);

    const auto timeToY = [&](std::int64_t t) {
        const double ratio = static_cast<double>(t - kViewStart) / span;
        return y0 + static_cast<float>(ratio) * (y1 - y0);
    };

    for (int hour = 7; hour <= 22; ++hour) {
        const float y = timeToY(hour * 3600);
        g_target->DrawLine(D2D1::Point2F(axisX - 6.0f, y),
                           D2D1::Point2F(axisX, y), dim, 1.0f);
        if (hour % 2 == 0 || hour == 7) {
            const std::wstring label = formatClock(hour * 3600);
            const D2D1_RECT_F rect =
                D2D1::RectF(8.0f, y - 8.0f, axisX - 8.0f, y + 8.0f);
            g_target->DrawTextW(label.c_str(),
                                static_cast<UINT32>(label.size()),
                                g_axisFormat, rect, dim);
        }
    }
    g_target->DrawLine(D2D1::Point2F(axisX, y0), D2D1::Point2F(axisX, y1),
                       dim, 1.0f);

    auto& registry = bp::api::Registry::instance();
    const std::vector<bp::Event> events =
        registry.store()->range(kDayStart, kDayStart + 24 * 3600);
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
        float yTop = timeToY(blockStart);
        float yBottom = timeToY(blockEnd);
        if (isDeadline) {
            yTop -= 12.0f;
            yBottom = yTop + 30.0f;
        }
        if (yBottom - yTop < 26.0f) {
            yBottom = yTop + 26.0f;
        }

        const D2D1_RECT_F rect =
            D2D1::RectF(blockX, yTop, blockX + blockW, yBottom);
        DrawGlassBlock(rect, fill, edge, highlight);
        g_hits.push_back(HitBlock{rect, blockEvents});

        float cursor = yTop + 7.0f;
        const float textRight = blockX + blockW - 8.0f;
        for (std::size_t i = 0; i < blockEvents.size(); ++i) {
            const auto& event = blockEvents[i];
            const std::wstring title =
                block.summary ? shortTitle(event) : toWide(event.title);
            g_target->DrawTextW(title.c_str(),
                                static_cast<UINT32>(title.size()),
                                g_titleFormat,
                                D2D1::RectF(blockX + 10.0f, cursor, textRight,
                                            cursor + kTitleLines),
                                text);
            cursor += kTitleLines;
            if (!block.summary) {
                const std::wstring spanLabel = spanText(event);
                g_target->DrawTextW(spanLabel.c_str(),
                                    static_cast<UINT32>(spanLabel.size()),
                                    g_timeFormat,
                                    D2D1::RectF(blockX + 10.0f, cursor,
                                                textRight,
                                                cursor + kTimeLines),
                                    dim);
                cursor += kTimeLines;
                const std::wstring note = toWide(event.note);
                if (!note.empty() && yBottom - cursor > kTimeLines) {
                    g_target->DrawTextW(
                        note.c_str(), static_cast<UINT32>(note.size()),
                        g_timeFormat,
                        D2D1::RectF(blockX + 10.0f, cursor, textRight,
                                    cursor + kTimeLines),
                        dim);
                    cursor += kTimeLines;
                }
            }
            if (i + 1 < blockEvents.size()) {
                g_target->DrawLine(
                    D2D1::Point2F(blockX + 10.0f, cursor + 3.0f),
                    D2D1::Point2F(blockX + blockW - 10.0f, cursor + 3.0f),
                    edge, 0.5f);
                cursor += 10.0f;
            }
            if (cursor > yBottom - 4.0f) {
                break;
            }
        }
    }

    const HRESULT hr = g_target->EndDraw();

    fill->Release();
    edge->Release();
    highlight->Release();
    text->Release();
    dim->Release();

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
            if (g_target && g_titleFormat) RenderDayView(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (g_target) {
                g_target->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const float mx = static_cast<float>(GET_X_LPARAM(lp));
            const float my = static_cast<float>(GET_Y_LPARAM(lp));
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

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BetterPlannerWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, title.c_str(),
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              900, 640, nullptr, nullptr, instance, nullptr);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_target) g_target->Release();
    ReleaseTextFormats();
    if (g_factory) g_factory->Release();
    return static_cast<int>(msg.wParam);
}
