#pragma once
#include <d2d1.h>
#include <memory>

namespace bp::ui {

// 原生玻璃材质：缓存应用背景、三次盒式模糊近似高斯、圆角 SDF 边缘折射与微弱色散。
// 灵感来自 rdev/liquid-glass-react；只处理底层背景，文字在调用 draw 后独立绘制。
// 不采集桌面或其他窗口。高对比度模式/关闭特效时回退为实色。
class LiquidGlass {
public:
    LiquidGlass();
    ~LiquidGlass();
    // 每次绘制前绑定目标，尺寸或设备变化会清理位图缓存；鼠标坐标为窗口 DIP。
    void begin(ID2D1RenderTarget* target, float mouseX, float mouseY, bool enabled);
    void background(ID2D1RenderTarget* target);
    // Animate small controls; layout and hit targets remain stable.
    D2D1_RECT_F control(D2D1_RECT_F rect);
    bool animating() const;
    // 支持平移变换；tint 决定底色，radius 为圆角半径。
    void draw(ID2D1RenderTarget* target, D2D1_RECT_F rect, D2D1_COLOR_F tint, float radius);
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
