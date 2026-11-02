#include "ui/liquid_glass.hpp"
#include <d2d1helper.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bp::ui {
namespace {
struct Pixel { float r=0, g=0, b=0; };
Pixel operator+(Pixel a, Pixel b) { return {a.r+b.r,a.g+b.g,a.b+b.b}; }
Pixel operator-(Pixel a, Pixel b) { return {a.r-b.r,a.g-b.g,a.b-b.b}; }
Pixel operator*(Pixel a, float scale) { return {a.r*scale,a.g*scale,a.b*scale}; }
float clamp(float value,float low,float high) { return (std::max)(low,(std::min)(value,high)); }
std::uint32_t pack(Pixel p,float alpha=1) {
    auto byte=[&](float c){return static_cast<std::uint32_t>(clamp(c*alpha,0,1)*255+0.5f);};
    return (byte(1)<<24)|(byte(p.r)<<16)|(byte(p.g)<<8)|byte(p.b);
}
void boxBlur(std::vector<Pixel>& pixels,int w,int h,int radius) {
    std::vector<Pixel> temp(pixels.size());
    for(int axis=0;axis<2;++axis) {
        int major=axis?h:w,minor=axis?w:h;
        auto index=[&](int a,int b){return axis?a*w+b:b*w+a;};
        for(int line=0;line<minor;++line) {
            Pixel sum;
            for(int i=-radius;i<=radius;++i) sum=sum+pixels[index((std::max)(0,(std::min)(major-1,i)),line)];
            for(int i=0;i<major;++i) {
                temp[index(i,line)]=sum*(1.0f/(radius*2+1));
                sum=sum-pixels[index((std::max)(0,i-radius),line)];
                sum=sum+pixels[index((std::min)(major-1,i+radius+1),line)];
            }
        }
        pixels.swap(temp);
    }
}
}

struct LiquidGlass::Impl {
    ID2D1RenderTarget* owner=nullptr;
    ID2D1Bitmap* backdrop=nullptr;
    std::map<std::string,ID2D1Bitmap*> panels;
    std::vector<Pixel> blurred;
    int width=0,height=0;
    std::size_t panelBytes=0;
    float mouseX=-1,mouseY=-1;
    bool enabled=true;
    ~Impl() { clear(); }
    void clear() {
        for(auto& entry:panels) entry.second->Release();
        panels.clear();
        panelBytes=0;
        if(backdrop) backdrop->Release();
        backdrop=nullptr;
        blurred.clear();
        owner=nullptr;
    }
    Pixel sample(float x,float y) const {
        x=clamp(x,0,static_cast<float>(width-1));
        y=clamp(y,0,static_cast<float>(height-1));
        const int ix=static_cast<int>(x),iy=static_cast<int>(y);
        const int nx=(std::min)(ix+1,width-1),ny=(std::min)(iy+1,height-1);
        const float fx=x-ix,fy=y-iy;
        return (blurred[iy*width+ix]*(1-fx)+blurred[iy*width+nx]*fx)*(1-fy)+
               (blurred[ny*width+ix]*(1-fx)+blurred[ny*width+nx]*fx)*fy;
    }
};

LiquidGlass::LiquidGlass():impl_(std::make_unique<Impl>()) {}
LiquidGlass::~LiquidGlass()=default;
void LiquidGlass::reset() { impl_->clear(); }
void LiquidGlass::begin(ID2D1RenderTarget* target,float mx,float my,bool enabled) {
    auto& s=*impl_;
    s.mouseX=mx;s.mouseY=my;s.enabled=enabled;
    const auto size=target->GetSize();
    int w=(std::max)(1,static_cast<int>(std::ceil(size.width)));
    int h=(std::max)(1,static_cast<int>(std::ceil(size.height)));
    if(s.owner==target && s.width==w && s.height==h && s.backdrop) return;
    s.clear();s.owner=target;s.width=w;s.height=h;
    if(static_cast<std::uint64_t>(w)*h>12000000) {s.enabled=false;return;}
    s.blurred.resize(static_cast<std::size_t>(w)*h);
    std::vector<std::uint32_t> bytes(s.blurred.size());
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        float u=static_cast<float>(x)/w,v=static_cast<float>(y)/h;
        float violet=std::exp(-((u-.16f)*(u-.16f)*7+(v-.15f)*(v-.15f)*9));
        float blue=std::exp(-((u-.80f)*(u-.80f)*8+(v-.60f)*(v-.60f)*6));
        float peach=std::exp(-((u-.42f)*(u-.42f)*10+(v-.95f)*(v-.95f)*8));
        float wave=(v-.30f-.15f*std::sin(u*6.0f))*20;
        float ribbon=std::exp(-wave*wave);
        Pixel p={.97f-.15f*violet-.13f*blue+.01f*peach,
                 .98f-.18f*violet-.025f*blue-.09f*peach,
                 1.0f-.03f*violet-.015f*blue-.12f*peach};
        p=p*(1-.045f*ribbon);
        s.blurred[y*w+x]=p;bytes[y*w+x]=pack(p);
    }
    const auto props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    if(FAILED(target->CreateBitmap(D2D1::SizeU(w,h),bytes.data(),w*4,props,&s.backdrop))) {s.enabled=false;return;}
    for(int i=0;i<3;++i) boxBlur(s.blurred,w,h,6);
}

void LiquidGlass::background(ID2D1RenderTarget* target) {
    target->Clear(D2D1::ColorF(.96f,.97f,.99f));
    if(impl_->enabled && impl_->backdrop) target->DrawBitmap(impl_->backdrop);
}

void LiquidGlass::draw(ID2D1RenderTarget* target,D2D1_RECT_F rect,D2D1_COLOR_F tint,float radius) {
    auto& s=*impl_;
    float w=rect.right-rect.left,h=rect.bottom-rect.top;
    if(w<=0 || h<=0) return;
    radius=clamp(radius,0,(std::min)(w,h)*.5f);
    ID2D1SolidColorBrush* brush=nullptr;
    if(FAILED(target->CreateSolidColorBrush(tint,&brush))) return;
    if(!s.enabled || !s.backdrop || target!=s.owner) {
        tint.a=1;brush->SetColor(tint);
        target->FillRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),brush);
        brush->Release();return;
    }
    D2D1_MATRIX_3X2_F matrix;target->GetTransform(&matrix);
    float ox=rect.left+matrix._31,oy=rect.top+matrix._32;
    if(oy+h<0 || oy>s.height || ox+w<0 || ox>s.width) {brush->Release();return;}
    int bw=static_cast<int>(std::ceil(w)),bh=static_cast<int>(std::ceil(h));
    const auto key=std::to_string(ox)+":"+std::to_string(oy)+":"+std::to_string(bw)+":"+std::to_string(bh)+":"+std::to_string(radius)+":"+std::to_string(pack({tint.r,tint.g,tint.b}));
    ID2D1Bitmap* bitmap=nullptr;
    if(auto it=s.panels.find(key);it!=s.panels.end()) bitmap=it->second;
    else if(static_cast<std::uint64_t>(bw)*bh<4000000) {
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(bw)*bh);
        for(int y=0;y<bh;++y) for(int x=0;x<bw;++x) {
            const float px=x+.5f-w*.5f,py=y+.5f-h*.5f;
            const float qx=std::abs(px)-(w*.5f-radius),qy=std::abs(py)-(h*.5f-radius);
            const float ex=(std::max)(qx,0.0f),ey=(std::max)(qy,0.0f);
            const float length=std::sqrt(ex*ex+ey*ey);
            const float sdf=length+(std::min)((std::max)(qx,qy),0.0f)-radius;
            const float coverage=clamp(.5f-sdf,0,1);
            if(coverage==0) continue;
            float nx=0,ny=0;
            if(length>.001f) {nx=ex/length;ny=ey/length;}
            else if(qx>qy) nx=1;else ny=1;
            nx*=px<0?-1.0f:1.0f;ny*=py<0?-1.0f:1.0f;
            float edge=std::exp(-(std::max)(0.0f,-sdf)/7.0f);
            float shift=7.0f*edge;
            float sx=ox+x+.5f-nx*shift,sy=oy+y+.5f-ny*shift;
            Pixel refracted=s.sample(sx,sy);
            refracted.r=s.sample(sx+nx*.75f*edge,sy+ny*.75f*edge).r;
            refracted.b=s.sample(sx-nx*.75f*edge,sy-ny*.75f*edge).b;
            Pixel p=refracted*.64f+Pixel{tint.r,tint.g,tint.b}*.24f+Pixel{1,1,1}*.12f;
            const float rim=std::exp(-std::abs(sdf)/1.15f)*.25f;
            p=p*(1-rim)+Pixel{1,1,1}*rim;
            pixels[y*bw+x]=pack(p,coverage);
        }
        auto props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        if(SUCCEEDED(target->CreateBitmap(D2D1::SizeU(bw,bh),pixels.data(),bw*4,props,&bitmap))) {
            if(s.panels.size()>=64 || s.panelBytes+pixels.size()*4>32*1024*1024) {
                for(auto& entry:s.panels)entry.second->Release();
                s.panels.clear();s.panelBytes=0;
            }
            s.panels.emplace(key,bitmap);
            s.panelBytes+=pixels.size()*4;
        }
    }
    brush->SetColor(D2D1::ColorF(.22f,.24f,.40f,.035f));
    for(int i=3;i>=1;--i) {
        auto shadow=rect;shadow.top+=i*1.5f;shadow.bottom+=i*1.5f;
        target->FillRoundedRectangle(D2D1::RoundedRect(shadow,radius,radius),brush);
    }
    if(bitmap) target->DrawBitmap(bitmap,rect);
    else {brush->SetColor(tint);target->FillRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),brush);}
    D2D1_GRADIENT_STOP stops[]={{0,D2D1::ColorF(1,1,1,.90f)},{.45f,D2D1::ColorF(1,1,1,.16f)},{1,D2D1::ColorF(.58f,.64f,.85f,.25f)}};
    ID2D1GradientStopCollection* collection=nullptr;
    ID2D1LinearGradientBrush* rim=nullptr;
    if(SUCCEEDED(target->CreateGradientStopCollection(stops,3,&collection))) {
        float influence=clamp((s.mouseX-ox)/w,0,1);
        target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left+w*influence,rect.top),D2D1::Point2F(rect.right,rect.bottom)),collection,&rim);
        if(rim) {target->DrawRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),rim,1.25f);rim->Release();}
        collection->Release();
    }
    if(s.mouseX>=ox && s.mouseX<=ox+w && s.mouseY>=oy && s.mouseY<=oy+h) {
        brush->SetColor(D2D1::ColorF(1,1,1,.20f));
        target->DrawRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),brush,2);
    }
    brush->Release();
}
}
