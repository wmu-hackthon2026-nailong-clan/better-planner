#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>

#include <string>

#include "api/registry.hpp"
#include "api/types.hpp"

#pragma comment(lib, "d2d1.lib")

namespace {

ID2D1Factory* g_factory = nullptr;
ID2D1HwndRenderTarget* g_target = nullptr;

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

void RenderDemoGlass(HWND hwnd) {
    g_target->BeginDraw();
    g_target->Clear(D2D1::ColorF(0.043f, 0.055f, 0.086f, 1.0f));

    const D2D1_RECT_F rect = D2D1::RectF(80.0f, 90.0f, 520.0f, 210.0f);
    ID2D1SolidColorBrush* fill = nullptr;
    ID2D1SolidColorBrush* edge = nullptr;
    ID2D1SolidColorBrush* highlight = nullptr;
    g_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f), &fill);
    g_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.40f), &edge);
    g_target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f), &highlight);

    g_target->FillRectangle(rect, fill);
    g_target->DrawRectangle(rect, edge, 0.5f);
    g_target->DrawLine(D2D1::Point2F(rect.left + 8.0f, rect.top + 1.5f),
                       D2D1::Point2F(rect.right - 8.0f, rect.top + 1.5f),
                       highlight, 1.5f);
    g_target->DrawLine(D2D1::Point2F(rect.left + 8.0f, rect.bottom - 1.5f),
                       D2D1::Point2F(rect.right - 8.0f, rect.bottom - 1.5f),
                       highlight, 1.0f);

    fill->Release();
    edge->Release();
    highlight->Release();
    const HRESULT hr = g_target->EndDraw();
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
            if (g_target) RenderDemoGlass(hwnd);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (g_target) {
                g_target->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            }
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

    bp::api::DeadlineInput input;
    input.when = 1792000000;
    input.title = "placeholder";
    input.note = "";
    store->add(bp::api::toEvent(input));

    const std::wstring title =
        L"Better Planner — events: " + std::to_wstring(store->size());

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BetterPlannerWnd";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, title.c_str(),
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              720, 480, nullptr, nullptr, instance, nullptr);
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_target) g_target->Release();
    if (g_factory) g_factory->Release();
    return static_cast<int>(msg.wParam);
}
