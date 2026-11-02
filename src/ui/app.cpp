#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <commdlg.h>
#include <wincodec.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "api/http_server.hpp"
#include "api/registry.hpp"
#include "api/desktop_connector.hpp"
#include "api/types.hpp"
#include "core/event.hpp"
#include "core/layout.hpp"
#include "core/queries.hpp"
#include "core/store.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

constexpr std::int64_t kDaySeconds = 86400;
constexpr float kTopBarH = 108.0f;
constexpr float kStatusBarH = 30.0f;
constexpr float kPxPerHour = 64.0f;
constexpr float kLaneShift = 44.0f;
constexpr float kAxisX = 66.0f;
constexpr UINT kEventsChangedMessage = WM_APP + 1;
constexpr UINT kSyncDoneMessage = WM_APP + 2;

D2D1_COLOR_F Clr(float r, float g, float b, float a = 1.0f) {
    return D2D1::ColorF(r, g, b, a);
}

const D2D1_COLOR_F kPalette[] = {
    Clr(0.36f, 0.55f, 0.95f), Clr(0.62f, 0.47f, 0.93f),
    Clr(0.30f, 0.80f, 0.77f), Clr(0.95f, 0.48f, 0.66f),
    Clr(0.42f, 0.80f, 0.47f), Clr(0.96f, 0.66f, 0.35f),
};
constexpr int kPaletteCount = sizeof(kPalette) / sizeof(kPalette[0]);

D2D1_COLOR_F AccentOf(const bp::Event& e) {
    if (e.kind == bp::Kind::Deadline) {
        return Clr(1.0f, 0.44f, 0.42f);
    }
    unsigned h = 2166136261u;
    const std::string& key = e.course.empty() ? e.title : e.course;
    for (char ch : key) {
        h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
    }
    return kPalette[h % kPaletteCount];
}

ID2D1Factory* g_factory = nullptr;
ID2D1HwndRenderTarget* g_target = nullptr;
IDWriteFactory* g_dwrite = nullptr;
IWICImagingFactory* g_wic = nullptr;

IDWriteTextFormat* g_fmtApp = nullptr;
IDWriteTextFormat* g_fmtBig = nullptr;
IDWriteTextFormat* g_fmtBtn = nullptr;
IDWriteTextFormat* g_fmtTitle = nullptr;
IDWriteTextFormat* g_fmtTime = nullptr;
IDWriteTextFormat* g_fmtAxis = nullptr;
IDWriteTextFormat* g_fmtStatus = nullptr;
IDWriteTextFormat* g_fmtWeekHead = nullptr;

enum class ViewMode { Day, Week, Month };
ViewMode g_view = ViewMode::Day;
std::int64_t g_dayBase = 0;
std::int64_t g_weekBase = 0;
std::int64_t g_monthBase = 0;
int g_scrollY = 448;
float g_hoverX=-1, g_hoverY=-1;
std::unique_ptr<bp::api::DesktopConnector> g_connector;

struct SolidBrushes {
    ID2D1SolidColorBrush* text = nullptr;
    ID2D1SolidColorBrush* dim = nullptr;
    ID2D1SolidColorBrush* faint = nullptr;
    ID2D1SolidColorBrush* line = nullptr;
    ID2D1SolidColorBrush* lineStrong = nullptr;
    ID2D1SolidColorBrush* now = nullptr;
    ID2D1SolidColorBrush* chip = nullptr;
    ID2D1SolidColorBrush* bar = nullptr;

    bool Create(ID2D1RenderTarget* t) {
        bool ok = true;
        ok = ok && SUCCEEDED(t->CreateSolidColorBrush(Clr(0.12f,0.16f,0.24f), &text));
        ok = ok && SUCCEEDED(t->CreateSolidColorBrush(Clr(0.36f,0.40f,0.49f), &dim));
        ok = ok && SUCCEEDED(t->CreateSolidColorBrush(Clr(0.47f,0.51f,0.59f), &faint));
        ok = ok && SUCCEEDED(t->CreateSolidColorBrush(Clr(0.90f,0.92f,0.95f), &line));
        ok = ok && SUCCEEDED(t->CreateSolidColorBrush(Clr(0.85f,0.88f,0.93f), &lineStrong));
        ok = ok && SUCCEEDED(t->CreateSolidColorBrush(Clr(1.0f, 0.36f, 0.34f, 0.95f), &now));
        ok = ok && SUCCEEDED(t->CreateSolidColorBrush(Clr(0.94f,0.95f,0.98f), &chip));
        ok = ok && SUCCEEDED(t->CreateSolidColorBrush(Clr(0, 0, 0, 0.28f), &bar));
        return ok;
    }
    void Release() {
        for (auto* b : {text, dim, faint, line, lineStrong, now, chip, bar}) {
            if (b) b->Release();
        }
        text = dim = faint = line = lineStrong = now = chip = bar = nullptr;
    }
};

ID2D1LinearGradientBrush* VGrad(ID2D1RenderTarget* t, float y0, float y1,
                                D2D1_COLOR_F c0, D2D1_COLOR_F c1) {
    ID2D1GradientStopCollection* stops = nullptr;
    const D2D1_GRADIENT_STOP gs[2] = {{0.0f, c0}, {1.0f, c1}};
    if (FAILED(t->CreateGradientStopCollection(gs, 2, &stops))) {
        return nullptr;
    }
    ID2D1LinearGradientBrush* brush = nullptr;
    t->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, y0), D2D1::Point2F(0, y1)),
        stops, &brush);
    stops->Release();
    return brush;
}

std::wstring toWide(const std::string& text) {
    if (text.empty()) return L"";
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (len <= 1) return L"";
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), len);
    out.resize(static_cast<size_t>(len - 1));
    return out;
}

std::tm localTm(std::int64_t t) {
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm out{};
    localtime_s(&out, &tt);
    return out;
}

std::int64_t TodayBase() {
    std::tm lt = localTm(static_cast<std::int64_t>(std::time(nullptr)));
    lt.tm_hour = 0;
    lt.tm_min = 0;
    lt.tm_sec = 0;
    return static_cast<std::int64_t>(_mktime64(&lt));
}

std::wstring formatClock(std::int64_t t) {
    const std::tm lt = localTm(t);
    wchar_t buf[8];
    swprintf(buf, 8, L"%02d:%02d", lt.tm_hour, lt.tm_min);
    return buf;
}

std::wstring formatDate(std::int64_t dayBase) {
    const std::tm lt = localTm(dayBase);
    const wchar_t* kWeek[] = {L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
    wchar_t buf[48];
    swprintf(buf, 48, L"%d月%d日 周%s", lt.tm_mon + 1, lt.tm_mday,
             kWeek[lt.tm_wday]);
    return buf;
}

std::wstring formatMonth(std::int64_t monthBase) {
    const std::tm lt = localTm(monthBase);
    wchar_t buf[32];
    swprintf(buf, 32, L"%d年%d月", lt.tm_year + 1900, lt.tm_mon + 1);
    return buf;
}

std::wstring spanText(const bp::Event& e) {
    if (e.kind == bp::Kind::Deadline) {
        return L"截止 " + formatClock(e.end);
    }
    return formatClock(e.start) + L" – " + formatClock(e.end);
}

std::wstring shortTitle(const bp::Event& e) {
    std::wstring t = toWide(e.title);
    if (t.size() > 5) {
        return t.substr(0, 4) + L"…";
    }
    return t;
}

std::wstring detailText(const std::vector<bp::Event>& events) {
    std::wostringstream stream;
    stream << L"共 " << events.size() << L" 个事件：";
    for (const auto& e : events) {
        stream << L"\n\n" << toWide(e.title) << L"\n" << spanText(e);
        if (!e.course.empty()) stream << L"\n课程：" << toWide(e.course);
        if (!e.location.empty()) stream << L"\n地点：" << toWide(e.location);
        if (!e.teacher.empty()) stream << L"\n教师：" << toWide(e.teacher);
        if (!e.note.empty()) stream << L"\n" << toWide(e.note);
        if (!e.source.empty()) stream << L"\n来源：" << toWide(e.source);
        if (e.done) stream << L"\n已完成";
    }
    return stream.str();
}

struct HitBlock {
    D2D1_RECT_F rect;
    std::vector<bp::Event> events;
};
struct DayHit {
    D2D1_RECT_F rect;
    std::int64_t dayBase;
};
enum class Btn { ViewDay, ViewWeek, ViewMonth, Today, NavPrev, NavNext, Sync, Export };
struct UiButton {
    D2D1_RECT_F rect;
    Btn id;
};
std::vector<HitBlock> g_hits;
std::vector<DayHit> g_dayHits;
std::vector<UiButton> g_ui;

float dayViewContentHeight() {
    return 24.0f * kPxPerHour + 24.0f;
}

void updateScrollRange(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const float viewH =
        static_cast<float>(rc.bottom) - kTopBarH - kStatusBarH;
    const int maxScroll =
        static_cast<int>(dayViewContentHeight() - viewH) > 0
            ? static_cast<int>(dayViewContentHeight() - viewH)
            : 0;
    if (g_scrollY > maxScroll) g_scrollY = maxScroll;
    if (g_scrollY < 0) g_scrollY = 0;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = static_cast<int>(dayViewContentHeight()) - 1;
    si.nPage = static_cast<UINT>(viewH > 0 ? viewH : 1);
    si.nPos = g_scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

void DrawTextLine(ID2D1RenderTarget* t, const std::wstring& text,
                  const D2D1_RECT_F& rect, IDWriteTextFormat* format,
                  ID2D1Brush* brush) {
    t->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()), format, rect,
                 brush, D2D1_DRAW_TEXT_OPTIONS_CLIP,
                 DWRITE_MEASURING_MODE_NATURAL);
}

void FillPanel(ID2D1RenderTarget* t,D2D1_RECT_F r,D2D1_COLOR_F color,float radius=12) {
    ID2D1SolidColorBrush* brush=nullptr;
    if(SUCCEEDED(t->CreateSolidColorBrush(color,&brush))) {
        t->FillRoundedRectangle(D2D1::RoundedRect(r,radius,radius),brush);
        brush->Release();
    }
}
void DrawEventBlock(ID2D1RenderTarget* t,const D2D1_RECT_F& r,const D2D1_COLOR_F& a) {
    const float radius = 9.0f;
    for (int i = 3; i >= 1; --i) {
        ID2D1SolidColorBrush* sh = nullptr;
        if (SUCCEEDED(t->CreateSolidColorBrush(Clr(0.16f,0.20f,0.30f,0.05f*i), &sh))) {
            const float off = static_cast<float>(i) * 1.2f;
            t->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(r.left, r.top + off, r.right, r.bottom + off),
                radius, radius), sh);
            sh->Release();
        }
    }
    ID2D1LinearGradientBrush* body = VGrad(t, r.top, r.bottom,
        Clr(0.995f - (0.995f-a.r)*0.30f, 0.995f-(0.995f-a.g)*0.30f, 0.995f-(0.995f-a.b)*0.30f, 1.0f),
        Clr(0.965f - (0.965f-a.r)*0.18f, 0.965f-(0.965f-a.g)*0.18f, 0.965f-(0.965f-a.b)*0.18f, 1.0f));
    if (body) {
        t->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), body);
        body->Release();
    }
    ID2D1LinearGradientBrush* sheen = VGrad(t, r.top, r.top + (r.bottom-r.top)*0.5f,
        Clr(1,1,1,0.28f), Clr(1,1,1,0.0f));
    if (sheen) {
        t->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), sheen);
        sheen->Release();
    }
    ID2D1SolidColorBrush* edge = nullptr;
    if (SUCCEEDED(t->CreateSolidColorBrush(Clr(a.r,a.g,a.b,0.35f), &edge))) {
        t->DrawRoundedRectangle(D2D1::RoundedRect(r, radius, radius), edge, 1.0f);
        edge->Release();
    }
    ID2D1SolidColorBrush* bar = nullptr;
    if (SUCCEEDED(t->CreateSolidColorBrush(Clr(a.r,a.g,a.b,0.95f), &bar))) {
        t->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(r.left+3.0f, r.top+9.0f, r.left+6.5f, r.bottom-9.0f), 2.0f, 2.0f), bar);
        bar->Release();
    }
}

void RenderDayScene(ID2D1RenderTarget* t, const SolidBrushes& sb, float x,
                    float y, float w, std::int64_t dayBase, float scrollPx,
                    std::vector<HitBlock>* hits) {
    const float x0 = x + kAxisX + 14.0f;
    const float x1 = x + w - 26.0f;
    const auto timeToY = [&](std::int64_t daySec) {
        return y + static_cast<float>(daySec) / 3600.0f * kPxPerHour -
               scrollPx;
    };

    for (int h = 0; h <= 24; ++h) {
        const float yy = timeToY(h * 3600);
        t->DrawLine(D2D1::Point2F(x + kAxisX, yy), D2D1::Point2F(x1, yy),
                    h % 6 == 0 ? sb.lineStrong : sb.line, 1.0f);
        if (h % 2 == 0 && h < 24) {
            const std::tm lt = localTm(dayBase + h * 3600);
            wchar_t buf[8];
            swprintf(buf, 8, L"%02d:00", lt.tm_hour);
            DrawTextLine(t, buf,
                         D2D1::RectF(x + 6.0f, yy - 8.0f, x + kAxisX - 8.0f,
                                     yy + 8.0f),
                         g_fmtAxis, sb.faint);
        }
    }
    for (int h = 0; h < 24; ++h) {
        const float yy = timeToY(h * 3600 + 1800);
        t->DrawLine(D2D1::Point2F(x + kAxisX, yy), D2D1::Point2F(x1, yy),
                    sb.line, 0.5f);
    }

    auto& registry = bp::api::Registry::instance();
    const std::vector<bp::Event> events =
        registry.store()->range(dayBase, dayBase + kDaySeconds);
    const std::vector<bp::LayoutEngine::Block> blocks =
        registry.layoutEngine()->layout(events);
    const float workWidth = x1 - x0;
    if (hits) hits->reserve(blocks.size());

    for (const auto& block : blocks) {
        std::vector<bp::Event> blockEvents;
        blockEvents.reserve(block.eventIndices.size());
        for (const auto index : block.eventIndices) {
            blockEvents.push_back(events[index]);
        }
        if (blockEvents.empty()) continue;
        const auto& first = blockEvents.front();
        const std::int64_t blockStart = first.start - dayBase;
        const std::int64_t blockEnd = first.end - dayBase;
        const bool isDeadline = first.kind == bp::Kind::Deadline;
        const D2D1_COLOR_F accent = AccentOf(first);

        const float shift = (std::min)(static_cast<float>(block.lane) * kLaneShift,
                                       workWidth * 0.65f);
        const float blockX = x0 + shift;
        const float blockW = workWidth * 0.92f;
        float yTop = timeToY(blockStart);
        float yBottom = timeToY(blockEnd);
        if (isDeadline) {
            yTop -= 15.0f;
            yBottom = yTop + 32.0f;
        }
        if (yBottom - yTop < 30.0f) yBottom = yTop + 30.0f;

        const D2D1_RECT_F rect =
            D2D1::RectF(blockX, yTop, blockX + blockW, yBottom);
        DrawEventBlock(t, rect, accent);
        if (block.lane > 0) {
            ID2D1SolidColorBrush* ring = nullptr;
            if (SUCCEEDED(t->CreateSolidColorBrush(Clr(1,1,1,0.9f), &ring))) {
                t->DrawRoundedRectangle(D2D1::RoundedRect(rect, 9.0f, 9.0f),
                                        ring, 2.0f);
                ring->Release();
            }
        }
        if (hits) hits->push_back(HitBlock{rect, blockEvents});
        t->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        const float blockH = yBottom - yTop;
        float cursor = yTop + 9.0f;
        const float textLeft = blockX + 14.0f;
        const float textRight = blockX + blockW - 12.0f;
        const bool tall = blockH >= 62.0f;
        const bool medium = blockH >= 44.0f;
        for (std::size_t i = 0; i < blockEvents.size(); ++i) {
            const auto& e = blockEvents[i];
            const std::wstring title = block.summary ? shortTitle(e)
                                                     : toWide(e.title);
            DrawTextLine(t, title,
                         D2D1::RectF(textLeft, cursor, textRight,
                                     cursor + 20.0f),
                         g_fmtTitle, sb.text);
            cursor += 20.0f;
            if (!block.summary && (medium || tall)) {
                DrawTextLine(t, spanText(e),
                             D2D1::RectF(textLeft, cursor, textRight,
                                         cursor + 15.0f),
                             g_fmtTime, sb.dim);
                cursor += 15.0f;
                if (tall) {
                    const std::wstring meta = e.course.empty()
                                                  ? toWide(e.location)
                                                  : toWide(e.course);
                    if (!meta.empty() && yBottom - cursor > 15.0f) {
                        DrawTextLine(t, meta,
                                     D2D1::RectF(textLeft, cursor, textRight,
                                                 cursor + 15.0f),
                                     g_fmtTime, sb.faint);
                        cursor += 15.0f;
                    }
                }
            }
            if (i + 1 < blockEvents.size()) {
                ID2D1SolidColorBrush* sep = nullptr;
                t->CreateSolidColorBrush(Clr(1, 1, 1, 0.14f), &sep);
                t->DrawLine(D2D1::Point2F(textLeft, cursor + 3.0f),
                            D2D1::Point2F(textRight, cursor + 3.0f), sep, 0.5f);
                sep->Release();
                cursor += 10.0f;
            }
            if (cursor > yBottom - 4.0f) break;
        }
        t->PopAxisAlignedClip();
    }

    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    if (now >= dayBase && now < dayBase + kDaySeconds) {
        const float yy = timeToY(now - dayBase);
        t->DrawLine(D2D1::Point2F(x + kAxisX, yy), D2D1::Point2F(x1, yy),
                    sb.now, 1.6f);
        t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + kAxisX, yy), 4.5f, 4.5f),
                       sb.now);
    }
}

void DrawChip(ID2D1RenderTarget* t, const SolidBrushes& sb,
              const D2D1_RECT_F& rect, const std::wstring& label,
              bool active) {
    bool hover=g_hoverX>=rect.left && g_hoverX<=rect.right && g_hoverY>=rect.top && g_hoverY<=rect.bottom;
    FillPanel(t,rect,active?Clr(0.88f,0.90f,1.0f):(hover?Clr(0.90f,0.92f,0.97f):Clr(0.96f,0.97f,0.99f)),8);
    DrawTextLine(t,label,rect,g_fmtBtn,active?sb.text:sb.dim);
}

constexpr float kSidebar=200;
void RenderTopBar(ID2D1RenderTarget* t,const SolidBrushes& sb,const D2D1_SIZE_F& size) {
    FillPanel(t,D2D1::RectF(0,0,kSidebar,size.height),Clr(0.96f,0.97f,0.99f),0);
    FillPanel(t,D2D1::RectF(22,24,54,56),Clr(0.36f,0.39f,0.84f),10);
    ID2D1SolidColorBrush* white=nullptr;
    t->CreateSolidColorBrush(Clr(1,1,1),&white);
    DrawTextLine(t,L"B",D2D1::RectF(22,24,54,56),g_fmtBtn,white);
    white->Release();
    DrawTextLine(t,L"Better Planner",D2D1::RectF(64,27,195,50),g_fmtApp,sb.text);
    DrawTextLine(t,L"让每一天，井然有序",D2D1::RectF(22,70,190,92),g_fmtTime,sb.faint);
    DrawTextLine(t,L"我的日历",D2D1::RectF(22,122,180,144),g_fmtTime,sb.faint);
    const wchar_t* names[]={L"日视图",L"周视图",L"月视图"};
    const Btn ids[]={Btn::ViewDay,Btn::ViewWeek,Btn::ViewMonth};
    for(int i=0;i<3;++i) {
        auto r=D2D1::RectF(16,154+i*46.0f,184,194+i*46.0f);
        DrawChip(t,sb,r,names[i],static_cast<int>(g_view)==i);
        g_ui.push_back({r,ids[i]});
    }
    DrawTextLine(t,L"日程分类",D2D1::RectF(22,324,180,346),g_fmtTime,sb.faint);
    const wchar_t* cats[]={L"课程与学习",L"个人安排",L"截止事项"};
    for(int i=0;i<3;++i) {
        FillPanel(t,D2D1::RectF(24,365+i*34.0f,32,373+i*34.0f),i==2?Clr(0.94f,0.40f,0.36f):kPalette[i],4);
        DrawTextLine(t,cats[i],D2D1::RectF(44,358+i*34.0f,184,382+i*34.0f),g_fmtBtn,sb.dim);
    }
    DrawTextLine(t,L"专注当下，从容计划",D2D1::RectF(22,size.height-60,190,size.height-32),g_fmtTime,sb.faint);
    std::wstring heading=g_view==ViewMode::Month?formatMonth(g_monthBase):(g_view==ViewMode::Week?formatDate(g_weekBase)+L" 起的一周":formatDate(g_dayBase));
    DrawTextLine(t,heading,D2D1::RectF(230,25,size.width-250,60),g_fmtBig,sb.text);
    DrawTextLine(t,L"安排好时间，也为生活留一点空白。",D2D1::RectF(232,68,size.width-220,90),g_fmtTime,sb.faint);
    float x=size.width-218;
    const auto add=[&](D2D1_RECT_F r,const wchar_t* label,Btn id) {
        DrawChip(t,sb,r,label,false);g_ui.push_back({r,id});
    };
    add(D2D1::RectF(x,26,x+86,62),L"连接与同步",Btn::Sync);
    add(D2D1::RectF(x+96,26,x+194,62),L"导出长图",Btn::Export);
    add(D2D1::RectF(size.width-178,72,size.width-118,100),L"今天",Btn::Today);
    add(D2D1::RectF(size.width-106,72,size.width-72,100),L"‹",Btn::NavPrev);
    add(D2D1::RectF(size.width-64,72,size.width-30,100),L"›",Btn::NavNext);
    t->DrawLine(D2D1::Point2F(kSidebar,kTopBarH),D2D1::Point2F(size.width,kTopBarH),sb.line);
}
void RenderStatusBar(ID2D1RenderTarget* t,const SolidBrushes& sb,const D2D1_SIZE_F& size) {
    FillPanel(t,D2D1::RectF(kSidebar,size.height-kStatusBarH,size.width,size.height),Clr(1,1,1),0);
    DrawTextLine(t,g_connector ? g_connector->status() : L"点击连接与同步，登录平台并导入课表",
        D2D1::RectF(kSidebar+24,size.height-24,size.width-30,size.height-4),g_fmtTime,sb.faint);
}
void RenderAgenda(ID2D1RenderTarget* t,const SolidBrushes& sb,const D2D1_SIZE_F& size) {
    float x=size.width-264;
    FillPanel(t,D2D1::RectF(x,kTopBarH+20,size.width-20,size.height-50),Clr(0.97f,0.98f,1),16);
    DrawTextLine(t,L"当日概览",D2D1::RectF(x+20,144,size.width-34,171),g_fmtTitle,sb.text);
    auto events=bp::api::Registry::instance().store()->range(g_dayBase,g_dayBase+kDaySeconds);
    std::sort(events.begin(),events.end(),[](const auto& a,const auto& b){return a.start<b.start;});
    DrawTextLine(t,std::to_wstring(events.size())+L" 项安排 · "+formatDate(g_dayBase),D2D1::RectF(x+20,179,size.width-30,202),g_fmtTime,sb.faint);
    float y=220;
    for(const auto& e:events) {
        if(y+78>size.height-70)break;
        FillPanel(t,D2D1::RectF(x+14,y,size.width-34,y+70),Clr(1,1,1),10);
        FillPanel(t,D2D1::RectF(x+25,y+16,x+31,y+22),AccentOf(e),3);
        DrawTextLine(t,toWide(e.title),D2D1::RectF(x+42,y+10,size.width-44,y+33),g_fmtTitle,sb.text);
        DrawTextLine(t,spanText(e),D2D1::RectF(x+42,y+39,size.width-44,y+58),g_fmtTime,sb.dim);
        g_hits.push_back({D2D1::RectF(x+14,y,size.width-34,y+70),{e}});
        y+=82;
    }
    if(events.empty())DrawTextLine(t,L"今天还没有安排\n享受一段自由时间吧。",D2D1::RectF(x+20,y,size.width-34,y+70),g_fmtBtn,sb.faint);
}

void RenderWeekView(ID2D1RenderTarget* t, const SolidBrushes& sb,
                    const D2D1_SIZE_F& size) {
    const float headH = 46.0f;
    const float y0 = kTopBarH + headH;
    const float colW = (size.width - 28.0f) / 7.0f;
    const float bodyH = size.height - kStatusBarH - y0;
    const std::int64_t today = TodayBase();
    const wchar_t* kWeek[] = {L"周一", L"周二", L"周三", L"周四",
                              L"周五", L"周六", L"周日"};

    auto& registry = bp::api::Registry::instance();
    const std::vector<bp::Event> all =
        registry.store()->range(g_weekBase, g_weekBase + 7 * kDaySeconds);
    const auto grouped = bp::groupByDay(all, g_weekBase, kDaySeconds);

    for (int d = 0; d < 7; ++d) {
        const float cx = 14.0f + d * colW;
        const std::int64_t dayBase = g_weekBase + d * kDaySeconds;
        const bool isToday = dayBase == today;
        if (isToday) {
            ID2D1SolidColorBrush* hl = nullptr;
            t->CreateSolidColorBrush(Clr(0.36f, 0.55f, 0.95f, 0.07f), &hl);
            t->FillRoundedRectangle(
                D2D1::RoundedRect(
                    D2D1::RectF(cx + 2.0f, kTopBarH + 4.0f, cx + colW - 2.0f,
                                y0 + bodyH - 4.0f),
                    10.0f, 10.0f),
                hl);
            hl->Release();
        }
        const std::tm lt = localTm(dayBase);
        wchar_t dayNum[8];
        swprintf(dayNum, 8, L"%d", lt.tm_mday);
        std::wstring head = std::wstring(kWeek[d]) + L" " + dayNum;
        DrawTextLine(t, head,
                     D2D1::RectF(cx, kTopBarH + 12.0f, cx + colW,
                                 kTopBarH + headH),
                     g_fmtWeekHead, isToday ? sb.text : sb.dim);
        t->DrawLine(D2D1::Point2F(cx, y0), D2D1::Point2F(cx, y0 + bodyH),
                    sb.line, 1.0f);

        g_dayHits.push_back(DayHit{
            D2D1::RectF(cx, kTopBarH, cx + colW, y0 + bodyH), dayBase});
        auto it = grouped.find(dayBase);
        if (it == grouped.end()) continue;
        const auto blocks = registry.layoutEngine()->layout(it->second);
        float ey = y0 + 8.0f;
        for (const auto& block : blocks) {
            if (block.eventIndices.empty()) continue;
            const auto& e = it->second[block.eventIndices.front()];
            const D2D1_COLOR_F accent = AccentOf(e);
            const float bh = 34.0f;
            const D2D1_RECT_F r = D2D1::RectF(cx + 5.0f, ey, cx + colW - 5.0f,
                                              ey + bh);
            ID2D1LinearGradientBrush* bg =
                VGrad(t, r.top, r.bottom, Clr(accent.r, accent.g, accent.b, 0.28f),
                      Clr(accent.r, accent.g, accent.b, 0.12f));
            if (bg) {
                t->FillRoundedRectangle(D2D1::RoundedRect(r, 7.0f, 7.0f), bg);
                bg->Release();
            }
            ID2D1SolidColorBrush* dot = nullptr;
            t->CreateSolidColorBrush(accent, &dot);
            t->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(r.left + 9.0f, r.top + 10.5f), 3.0f,
                              3.0f),
                dot);
            dot->Release();
            DrawTextLine(t, toWide(e.title),
                         D2D1::RectF(r.left + 17.0f, r.top + 2.0f, r.right - 4.0f,
                                     r.top + 19.0f),
                         g_fmtTime, sb.text);
            DrawTextLine(t, spanText(e),
                         D2D1::RectF(r.left + 17.0f, r.top + 17.0f,
                                     r.right - 4.0f, r.top + 32.0f),
                         g_fmtStatus, sb.faint);
            ey += bh + 5.0f;
            if (ey + bh > y0 + bodyH) break;
        }
    }
}

void RenderMonthView(ID2D1RenderTarget* t, const SolidBrushes& sb,
                     const D2D1_SIZE_F& size) {
    const std::tm first = localTm(g_monthBase);
    const int lead = (first.tm_wday + 6) % 7;
    const std::int64_t gridBase = g_monthBase - lead * kDaySeconds;
    const float headH = 30.0f;
    const float y0 = kTopBarH + headH;
    const float colW = (size.width - 28.0f) / 7.0f;
    const float rowH = (size.height - kStatusBarH - y0 - 8.0f) / 6.0f;
    const std::int64_t today = TodayBase();
    const int curMonth = first.tm_mon;
    const wchar_t* kWeek[] = {L"一", L"二", L"三", L"四", L"五", L"六", L"日"};

    for (int d = 0; d < 7; ++d) {
        DrawTextLine(t, kWeek[d],
                     D2D1::RectF(14.0f + d * colW, kTopBarH + 4.0f,
                                 14.0f + (d + 1) * colW, y0),
                     g_fmtWeekHead, sb.faint);
    }

    auto& registry = bp::api::Registry::instance();
    const std::vector<bp::Event> all =
        registry.store()->range(gridBase, gridBase + 42 * kDaySeconds);
    const auto grouped = bp::groupByDay(all, gridBase, kDaySeconds);

    for (int cell = 0; cell < 42; ++cell) {
        const int row = cell / 7;
        const int col = cell % 7;
        const std::int64_t dayBase = gridBase + cell * kDaySeconds;
        const std::tm lt = localTm(dayBase);
        const bool inMonth = lt.tm_mon == curMonth;
        const bool isToday = dayBase == today;
        const float cx = 14.0f + col * colW;
        const float cy = y0 + 4.0f + row * rowH;
        const D2D1_RECT_F r = D2D1::RectF(cx + 3.0f, cy, cx + colW - 3.0f,
                                          cy + rowH - 6.0f);

        ID2D1SolidColorBrush* edge = nullptr;
        t->CreateSolidColorBrush(
            isToday ? Clr(0.36f, 0.55f, 0.95f, 0.75f) : Clr(0.94f,0.95f,0.98f),
            &edge);
        if (isToday) {
            ID2D1SolidColorBrush* bg = nullptr;
            t->CreateSolidColorBrush(Clr(0.36f, 0.55f, 0.95f, 0.08f), &bg);
            t->FillRoundedRectangle(D2D1::RoundedRect(r, 9.0f, 9.0f), bg);
            bg->Release();
        }
        t->DrawRoundedRectangle(D2D1::RoundedRect(r, 9.0f, 9.0f), edge, 1.0f);
        edge->Release();

        wchar_t dayNum[8];
        swprintf(dayNum, 8, L"%d", lt.tm_mday);
        DrawTextLine(t, dayNum,
                     D2D1::RectF(r.left + 9.0f, r.top + 6.0f, r.right,
                                 r.top + 26.0f),
                     g_fmtWeekHead,
                     !inMonth ? sb.faint : (isToday ? sb.text : sb.dim));

        auto it = grouped.find(dayBase);
        if (it != grouped.end()) {
            float ey = r.top + 28.0f;
            std::size_t shown = 0;
            for (const auto& e : it->second) {
                if (ey + 15.0f > r.bottom - 16.0f &&
                    shown + 1 < it->second.size()) {
                    wchar_t more[16];
                    swprintf(more, 16, L"+%zu", it->second.size() - shown);
                    DrawTextLine(t, more,
                                 D2D1::RectF(r.left + 9.0f, ey, r.right,
                                             ey + 15.0f),
                                 g_fmtStatus, sb.faint);
                    break;
                }
                const D2D1_COLOR_F accent = AccentOf(e);
                ID2D1SolidColorBrush* dot = nullptr;
                t->CreateSolidColorBrush(accent, &dot);
                t->FillEllipse(
                    D2D1::Ellipse(D2D1::Point2F(r.left + 13.0f, ey + 6.0f),
                                  2.6f, 2.6f),
                    dot);
                dot->Release();
                DrawTextLine(t, toWide(e.title),
                             D2D1::RectF(r.left + 21.0f, ey, r.right - 5.0f,
                                         ey + 15.0f),
                             g_fmtStatus, inMonth ? sb.dim : sb.faint);
                ey += 16.0f;
                ++shown;
            }
        }
        g_dayHits.push_back(DayHit{r, dayBase});
    }
}

void RenderView() {
    g_hits.clear();
    g_dayHits.clear();
    g_ui.clear();
    g_target->BeginDraw();

    g_target->Clear(Clr(1,1,1));
    SolidBrushes sb;
    sb.Create(g_target);
    const D2D1_SIZE_F size=g_target->GetSize();
    float agendaW=size.width>=1060 && g_view==ViewMode::Day?280.0f:0;
    g_target->SetTransform(D2D1::Matrix3x2F::Translation(kSidebar,0));
    g_target->PushAxisAlignedClip(D2D1::RectF(0,kTopBarH,size.width-kSidebar-agendaW,size.height-kStatusBarH),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    auto cs=D2D1::SizeF(size.width-kSidebar-agendaW,size.height);
    if(g_view==ViewMode::Day)RenderDayScene(g_target,sb,0,kTopBarH,cs.width,g_dayBase,static_cast<float>(g_scrollY),&g_hits);
    else if(g_view==ViewMode::Week)RenderWeekView(g_target,sb,cs);
    else RenderMonthView(g_target,sb,cs);
    g_target->PopAxisAlignedClip();
    g_target->SetTransform(D2D1::Matrix3x2F::Identity());
    for(auto& hit:g_hits) {
        hit.rect.left+=kSidebar;hit.rect.right+=kSidebar;
        hit.rect.top=(std::max)(hit.rect.top,kTopBarH);
        hit.rect.bottom=(std::min)(hit.rect.bottom,size.height-kStatusBarH);
    }
    for(auto& hit:g_dayHits){hit.rect.left+=kSidebar;hit.rect.right+=kSidebar;}
    RenderTopBar(g_target,sb,size);
    if(agendaW>0)RenderAgenda(g_target,sb,size);
    RenderStatusBar(g_target,sb,size);

    const HRESULT hr = g_target->EndDraw();
    sb.Release();
    if (hr == D2DERR_RECREATE_TARGET) {
        g_target->Release();
        g_target = nullptr;
    }
}

bool ExportDayViewPng(const std::wstring& path) {
    if (!g_wic) {
        CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    }
    if (!g_wic) return false;

    const UINT width = 1080;
    const UINT height = static_cast<UINT>(dayViewContentHeight() + 60.0f);

    IWICBitmap* wicBitmap = nullptr;
    if (FAILED(g_wic->CreateBitmap(width, height,
                                   GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapCacheOnDemand, &wicBitmap))) {
        return false;
    }
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1RenderTarget* rt = nullptr;
    if (FAILED(g_factory->CreateWicBitmapRenderTarget(wicBitmap, props, &rt))) {
        wicBitmap->Release();
        return false;
    }

    rt->BeginDraw();
    ID2D1LinearGradientBrush* bgGrad =
        VGrad(rt, 0.0f, static_cast<float>(height), Clr(1,1,1),
              Clr(0.98f,0.99f,1));
    if (bgGrad) {
        rt->FillRectangle(
            D2D1::RectF(0, 0, static_cast<float>(width),
                        static_cast<float>(height)),
            bgGrad);
        bgGrad->Release();
    }
    SolidBrushes sb;
    sb.Create(rt);
    {
        const std::wstring heading = formatDate(g_dayBase);
        DrawTextLine(rt, heading, D2D1::RectF(22.0f, 12.0f, 600.0f, 48.0f),
                     g_fmtBig, sb.text);
    }
    RenderDayScene(rt, sb, 0.0f, 56.0f, static_cast<float>(width), g_dayBase,
                   0.0f, nullptr);
    const HRESULT hr = rt->EndDraw();
    sb.Release();
    rt->Release();
    if (FAILED(hr)) {
        wicBitmap->Release();
        return false;
    }

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_wic->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromFilename(path.c_str(),
                                                 GENERIC_WRITE)) &&
        SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                       &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(width, height))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
        if (SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
            SUCCEEDED(frame->WriteSource(wicBitmap, nullptr)) &&
            SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit())) {
            ok = true;
        }
    }
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    wicBitmap->Release();
    return ok;
}

void CreateTextFormats() {
    const wchar_t* family = L"Microsoft YaHei UI";
    g_dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn",
                               &g_fmtApp);
    g_dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_BOLD,
                               DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 24.0f, L"zh-cn",
                               &g_fmtBig);
    g_dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-cn",
                               &g_fmtBtn);
    g_dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 13.5f, L"zh-cn",
                               &g_fmtTitle);
    g_dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 11.5f, L"zh-cn",
                               &g_fmtTime);
    g_dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-cn",
                               &g_fmtAxis);
    g_dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                               DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"zh-cn",
                               &g_fmtStatus);
    g_dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                               DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, 12.5f, L"zh-cn",
                               &g_fmtWeekHead);
    for (auto* f : {g_fmtBtn, g_fmtStatus, g_fmtWeekHead}) {
        if (f) {
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
    if (g_fmtBtn) g_fmtBtn->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void ReleaseTextFormats() {
    for (auto* f : {g_fmtApp, g_fmtBig, g_fmtBtn, g_fmtTitle, g_fmtTime,
                    g_fmtAxis, g_fmtStatus, g_fmtWeekHead}) {
        if (f) f->Release();
    }
    g_fmtApp = g_fmtBig = g_fmtBtn = g_fmtTitle = g_fmtTime = g_fmtAxis =
        g_fmtStatus = g_fmtWeekHead = nullptr;
}

void CreateTarget(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    g_factory->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(
            hwnd, D2D1::SizeU(static_cast<UINT32>(rc.right),
                              static_cast<UINT32>(rc.bottom))),
        &g_target);
}

void SeedDemoData(const std::shared_ptr<bp::EventStore>& store) {
    const std::int64_t today = TodayBase();
    struct Seed {
        int dayOffset;
        int startHour;
        int startMin;
        int endHour;
        int endMin;
        const char* title;
        const char* note;
        const char* course;
        const char* location;
        bool deadline;
    };
    const Seed seeds[] = {
        {0, 8, 0, 9, 0, "晨读英语", "背诵 Unit 5 单词", "大学英语", "图书馆 3F", false},
        {0, 8, 0, 9, 0, "晨读英语", "背诵 Unit 5 单词", "大学英语", "图书馆 3F", false},
        {0, 9, 30, 11, 0, "高等数学·第7讲", "带习题册", "高等数学", "一教 301", false},
        {0, 10, 0, 11, 0, "线性代数辅导", "答疑 30 分钟", "线性代数", "二教 102", false},
        {0, 13, 0, 15, 0, "操作系统实验", "Lab4 内存管理", "操作系统", "实验楼 B403", false},
        {0, 14, 0, 15, 0, "助教答疑", "实验难点讲解", "", "实验楼 B405", false},
        {0, 15, 30, 17, 0, "数据结构·第9讲", "", "数据结构", "一教 205", false},
        {0, 20, 0, 20, 0, "物理作业截止", "第5章 习题 1-8", "大学物理", "", true},
        {1, 9, 0, 10, 30, "线性代数", "", "线性代数", "二教 102", false},
        {1, 10, 30, 12, 0, "体育·羽毛球", "", "", "体育馆", false},
        {1, 15, 0, 17, 0, "黑客松开发冲刺", "完成 M6 联调", "", "图书馆 4F", false},
        {2, 8, 30, 10, 0, "数据库原理", "", "数据库", "一教 305", false},
        {2, 13, 0, 14, 30, "机器学习导论", "第6章 SVM", "机器学习", "三教 401", false},
        {2, 0, 0, 23, 59, "英语作文截止", "Topic: My Campus", "大学英语", "", true},
        {3, 10, 0, 11, 30, "编译原理", "", "编译原理", "二教 208", false},
        {3, 19, 0, 21, 0, "社团招新摆摊", "", "", "学生活动中心", false},
        {4, 9, 0, 12, 0, "操作系统·第10讲", "", "操作系统", "一教 301", false},
        {4, 14, 0, 16, 0, "项目答辩彩排", "准备演示脚本", "", "实验楼 A201", false},
    };
    for (const auto& s : seeds) {
        bp::Event e;
        e.kind = s.deadline ? bp::Kind::Deadline : bp::Kind::Schedule;
        const std::int64_t base = today + s.dayOffset * kDaySeconds;
        if (s.deadline) {
            e.start = base + s.endHour * 3600 + s.endMin * 60;
            e.end = e.start;
        } else {
            e.start = base + s.startHour * 3600 + s.startMin * 60;
            e.end = base + s.endHour * 3600 + s.endMin * 60;
        }
        e.title = s.title;
        e.note = s.note;
        e.course = s.course;
        e.location = s.location;
        store->add(e);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_GETMINMAXINFO: {
            auto* info=reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize={1000,640};
            return 0;
        }
        case WM_MOUSEMOVE:
            g_hoverX=static_cast<float>(GET_X_LPARAM(lp));
            g_hoverY=static_cast<float>(GET_Y_LPARAM(lp));
            InvalidateRect(hwnd,nullptr,FALSE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            if (!g_target) CreateTarget(hwnd);
            if (g_target && g_fmtTitle) RenderView();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SIZE:
            if (g_target) {
                g_target->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            }
            updateScrollRange(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEWHEEL: {
            if (g_view != ViewMode::Day) return 0;
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            g_scrollY -= delta / 120 * 48;
            updateScrollRange(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            if (g_view != ViewMode::Day) return 0;
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_ALL;
            GetScrollInfo(hwnd, SB_VERT, &si);
            switch (LOWORD(wp)) {
                case SB_LINEUP: g_scrollY -= 32; break;
                case SB_LINEDOWN: g_scrollY += 32; break;
                case SB_PAGEUP: g_scrollY -= static_cast<int>(si.nPage); break;
                case SB_PAGEDOWN: g_scrollY += static_cast<int>(si.nPage); break;
                case SB_THUMBTRACK: g_scrollY = si.nTrackPos; break;
                default: break;
            }
            updateScrollRange(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const float mx = static_cast<float>(GET_X_LPARAM(lp));
            const float my = static_cast<float>(GET_Y_LPARAM(lp));
            for (const auto& btn : g_ui) {
                if (mx < btn.rect.left || mx > btn.rect.right ||
                    my < btn.rect.top || my > btn.rect.bottom) {
                    continue;
                }
                switch (btn.id) {
                    case Btn::ViewDay: g_view = ViewMode::Day; break;
                    case Btn::ViewWeek: {
                        const auto date = localTm(g_dayBase);
                        g_weekBase = g_dayBase - ((date.tm_wday + 6) % 7) * kDaySeconds;
                        g_view = ViewMode::Week;
                        break;
                    }
                    case Btn::ViewMonth: {
                        auto date = localTm(g_dayBase);
                        date.tm_mday = 1;
                        g_monthBase = static_cast<std::int64_t>(_mktime64(&date));
                        g_view = ViewMode::Month;
                        break;
                    }
                    case Btn::Today: {
                        g_dayBase = TodayBase();
                        auto date = localTm(g_dayBase);
                        g_weekBase = g_dayBase - ((date.tm_wday + 6) % 7) * kDaySeconds;
                        date.tm_mday = 1;
                        g_monthBase = static_cast<std::int64_t>(_mktime64(&date));
                        g_scrollY = 448;
                        break;
                    }
                    case Btn::NavPrev:
                        if (g_view == ViewMode::Day) g_dayBase -= kDaySeconds;
                        if (g_view == ViewMode::Week) g_weekBase -= 7 * kDaySeconds;
                        if (g_view == ViewMode::Month) {
                            std::tm lt = localTm(g_monthBase);
                            lt.tm_mon -= 1;
                            g_monthBase = static_cast<std::int64_t>(_mktime64(&lt));
                        }
                        break;
                    case Btn::NavNext:
                        if (g_view == ViewMode::Day) g_dayBase += kDaySeconds;
                        if (g_view == ViewMode::Week) g_weekBase += 7 * kDaySeconds;
                        if (g_view == ViewMode::Month) {
                            std::tm lt = localTm(g_monthBase);
                            lt.tm_mon += 1;
                            g_monthBase = static_cast<std::int64_t>(_mktime64(&lt));
                        }
                        break;
                    case Btn::Sync: {
                        if (g_connector) g_connector->requestOpen();
                        return 0;
                    }
                    case Btn::Export: {
                        wchar_t path[MAX_PATH] = L"day-view.png";
                        OPENFILENAMEW ofn{};
                        ofn.lStructSize = sizeof(ofn);
                        ofn.hwndOwner = hwnd;
                        ofn.lpstrFilter = L"PNG 图片 (*.png)\0*.png\0";
                        ofn.lpstrFile = path;
                        ofn.nMaxFile = MAX_PATH;
                        ofn.Flags = OFN_OVERWRITEPROMPT;
                        if (GetSaveFileNameW(&ofn)) {
                            if (ExportDayViewPng(path)) {
                                const std::wstring msgText =
                                    L"已导出到 " + std::wstring(path);
                                MessageBoxW(hwnd, msgText.c_str(), L"导出成功",
                                            MB_OK);
                            } else {
                                MessageBoxW(hwnd, L"导出失败", L"错误",
                                            MB_ICONERROR);
                            }
                        }
                        return 0;
                    }
                }
                updateScrollRange(hwnd);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (g_view == ViewMode::Day) {
                for (auto it = g_hits.rbegin(); it != g_hits.rend(); ++it) {
                    const auto& hit = *it;
                    if (mx >= hit.rect.left && mx <= hit.rect.right &&
                        my >= hit.rect.top && my <= hit.rect.bottom) {
                        const std::wstring detail = detailText(hit.events);
                        MessageBoxW(hwnd, detail.c_str(), L"事件详情", MB_OK);
                        break;
                    }
                }
                return 0;
            }
            for (const auto& hit : g_dayHits) {
                if (mx >= hit.rect.left && mx <= hit.rect.right &&
                    my >= hit.rect.top && my <= hit.rect.bottom) {
                    g_dayBase = hit.dayBase;
                    g_view = ViewMode::Day;
                    g_scrollY = 448;
                    updateScrollRange(hwnd);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
            return 0;
        }
        case kEventsChangedMessage:
        case kSyncDoneMessage:
            if (g_connector) {
                const auto address = g_connector->takeOpenUrl();
                if (!address.empty()) {
                    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd, L"open", address.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
                    if (result <= 32) MessageBoxW(hwnd, (L"浏览器未能打开，请复制此本机地址：\n" + address).c_str(), L"连接账号", MB_OK);
                }
            }
            updateScrollRange(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DESTROY:
            if (g_connector) g_connector->stop();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    auto& registry = bp::api::Registry::instance();
    const auto store = std::make_shared<bp::EventStore>();
    registry.provideStore(store);
    const LPWSTR cmd = GetCommandLineW();
    if (cmd && (wcsstr(cmd, L"--demo") || wcsstr(cmd, L"--export"))) SeedDemoData(store);
    if (cmd && wcsstr(cmd, L"--export")) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(cmd, &argc);
        std::wstring out = L"day-export.png";
        for (int i = 0; i + 1 < argc; ++i) {
            if (wcscmp(argv[i], L"--export") == 0) out = argv[i + 1];
        }
        const std::int64_t today = TodayBase();
        g_dayBase = today;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                            __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&g_dwrite));
        CreateTextFormats();
        const bool ok = ExportDayViewPng(out);
        ReleaseTextFormats();
        if (g_factory) g_factory->Release();
        if (g_wic) g_wic->Release();
        CoUninitialize();
        return ok ? 0 : 2;
    }

    const std::int64_t today = TodayBase();
    g_dayBase = today;
    std::tm lt = localTm(today);
    g_weekBase = today - ((lt.tm_wday + 6) % 7) * kDaySeconds;
    lt.tm_mday = 1;
    g_monthBase = static_cast<std::int64_t>(_mktime64(&lt));

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(&g_dwrite));
    CreateTextFormats();

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"BetterPlannerWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Better Planner", WS_OVERLAPPEDWINDOW | WS_VSCROLL,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, nullptr, nullptr, instance,
        nullptr);
    if (!hwnd) return 1;

    const BOOL dark = FALSE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

    bp::api::HttpServer httpServer(8765);
    httpServer.start(store,
                     [hwnd] { PostMessageW(hwnd, kEventsChangedMessage, 0, 0); });
    const std::wstring title = L"Better Planner · 日程与课表";
    SetWindowTextW(hwnd, title.c_str());

    g_connector = std::make_unique<bp::api::DesktopConnector>(store, [hwnd] {
        PostMessageW(hwnd, kSyncDoneMessage, 0, 0);
    });
    g_connector->start();
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

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
