#include "ui/liquid_glass.hpp"
#include <d2d1helper.h>
#include <algorithm>
#include <cmath>
#include <chrono>
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
    float scale=1;
    std::size_t panelBytes=0;
    float mouseX=-1,mouseY=-1;
    bool enabled=true;
    struct Motion { float x=0,y=0,grow=0; };
    std::map<std::string,Motion> motions;
    bool moving=false;
    float dt=.016f;
    std::chrono::steady_clock::time_point frame=std::chrono::steady_clock::now();
    ~Impl() { clear(); }
    void clear() {
        for(auto& entry:panels) entry.second->Release();
        panels.clear();
        motions.clear();
        panelBytes=0;
        if(backdrop) backdrop->Release();
        backdrop=nullptr;
        blurred.clear();
        owner=nullptr;
    }
    Pixel sample(float x,float y) const {
        x*=scale;y*=scale;
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
    auto now=std::chrono::steady_clock::now();
    s.dt=clamp(std::chrono::duration<float>(now-s.frame).count(),.001f,.05f);
    s.frame=now;s.moving=false;
    s.mouseX=mx;s.mouseY=my;s.enabled=enabled;
    const auto size=target->GetSize();
    float dpiX=96,dpiY=96;target->GetDpi(&dpiX,&dpiY);
    const float scale=dpiX/96;
    int w=(std::max)(1,static_cast<int>(std::ceil(size.width*scale)));
    int h=(std::max)(1,static_cast<int>(std::ceil(size.height*scale)));
    if(s.owner==target && s.width==w && s.height==h && s.scale==scale && s.backdrop) return;
    s.clear();s.owner=target;s.width=w;s.height=h;s.scale=scale;
    if(static_cast<std::uint64_t>(w)*h>12000000) {s.enabled=false;return;}
    s.blurred.resize(static_cast<std::size_t>(w)*h);
    std::vector<std::uint32_t> bytes(s.blurred.size());
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) {
        float u=static_cast<float>(x)/w,v=static_cast<float>(y)/h;
        // Layered contour wallpaper supplies actual high-frequency features for
        // the lens to bend. Broad pastel blobs alone make displacement invisible.
        Pixel p={.86f-.08f*v,.94f-.04f*v,.98f};
        const Pixel layers[]={{.57f,.77f,.91f},{.68f,.87f,.92f},
                              {.36f,.67f,.80f},{.63f,.81f,.85f}};
        for(int layer=0;layer<4;++layer) {
            const float boundary=.08f+layer*.245f+.16f*std::sin(u*5.5f+layer*.85f);
            const float d=v-boundary;
            float mix=clamp(d*190+.5f,0,1);
            mix=mix*mix*(3-2*mix);
            p=p*(1-mix)+layers[layer]*mix;
            const float ridge=std::exp(-std::abs(d)*h/1.8f)*.23f;
            p=p*(1-ridge)+Pixel{1,1,1}*ridge;
            // Delicate contour lines visible through the clear lens shoulder.
            const float contour=std::exp(-std::abs(d-.018f)*h/1.2f)*.12f;
            p=p*(1-contour)+Pixel{1,1,1}*contour;
        }
        const float sunlight=std::exp(-((u-.92f)*(u-.92f)*5+(v-.10f)*(v-.10f)*6))*.38f;
        p=p*(1-sunlight)+Pixel{1,.98f,.91f}*sunlight;
        s.blurred[y*w+x]=p;bytes[y*w+x]=pack(p);
    }
    const auto props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96*s.scale,96*s.scale);
    if(FAILED(target->CreateBitmap(D2D1::SizeU(w,h),bytes.data(),w*4,props,&s.backdrop))) {s.enabled=false;return;}
    for(int i=0;i<3;++i) boxBlur(s.blurred,w,h,static_cast<int>(std::ceil(2*scale)));
}

bool LiquidGlass::animating() const { return impl_->moving; }
D2D1_RECT_F LiquidGlass::control(D2D1_RECT_F r) {
    auto& s=*impl_;
    if(!s.enabled) return r;
    const float cx=(r.left+r.right)*.5f,cy=(r.top+r.bottom)*.5f;
    const bool hover=s.mouseX>=r.left && s.mouseX<=r.right && s.mouseY>=r.top && s.mouseY<=r.bottom;
    const float tx=hover?clamp((s.mouseX-cx)*.09f,-4,4):0;
    const float ty=hover?clamp((s.mouseY-cy)*.12f,-2,2):0;
    auto key=std::to_string(r.left)+":"+std::to_string(r.top)+":"+std::to_string(r.right);
    if(s.motions.size()>64) s.motions.clear();
    auto& m=s.motions[key];
    const float ease=1-std::exp(-s.dt*18);
    auto approach=[&](float& value,float goal) {
        value+=(goal-value)*ease;
        if(std::abs(goal-value)<.015f)value=goal;else s.moving=true;
    };
    approach(m.x,tx);approach(m.y,ty);approach(m.grow,hover?2.3f:0);
    return D2D1::RectF(r.left+m.x-m.grow,r.top+m.y-m.grow*.65f,
                       r.right+m.x+m.grow,r.bottom+m.y+m.grow*.65f);
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
    if(oy+h<0 || oy>s.height/s.scale || ox+w<0 || ox>s.width/s.scale) {brush->Release();return;}
    int bw=static_cast<int>(std::ceil(w*s.scale)),bh=static_cast<int>(std::ceil(h*s.scale));
    const auto key=std::to_string(ox)+":"+std::to_string(oy)+":"+std::to_string(bw)+":"+std::to_string(bh)+":"+std::to_string(radius)+":"+std::to_string(pack({tint.r,tint.g,tint.b}));
    ID2D1Bitmap* bitmap=nullptr;
    if(auto it=s.panels.find(key);it!=s.panels.end()) bitmap=it->second;
    else if(static_cast<std::uint64_t>(bw)*bh<4000000) {
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(bw)*bh);
        for(int y=0;y<bh;++y) for(int x=0;x<bw;++x) {
            const float lx=(x+.5f)/s.scale,ly=(y+.5f)/s.scale;
            const float px=lx-w*.5f,py=ly-h*.5f;
            const float qx=std::abs(px)-(w*.5f-radius),qy=std::abs(py)-(h*.5f-radius);
            const float ex=(std::max)(qx,0.0f),ey=(std::max)(qy,0.0f);
            const float length=std::sqrt(ex*ex+ey*ey);
            const float sdf=length+(std::min)((std::max)(qx,qy),0.0f)-radius;
            const float coverage=clamp(.5f-sdf*s.scale,0,1);
            if(coverage==0) continue;
            float nx=0,ny=0;
            if(length>.001f) {nx=ex/length;ny=ey/length;}
            else if(qx>qy) nx=1;else ny=1;
            nx*=px<0?-1.0f:1.0f;ny*=py<0?-1.0f:1.0f;
            // Rounded lens shoulder: displacement peaks inside the rim and
            // fades smoothly to an undistorted center, as in the SVG edge mask.
            const float depth=(std::max)(0.0f,-sdf);
            const float band=(std::min)(32.0f,(std::min)(w,h)*.42f);
            float edge=clamp(1-depth/band,0,1);
            edge=edge*edge*(3-2*edge);
            if(radius==0) edge=0;
            float shift=band*.45f*std::sin(edge*3.14159265f);
            float sx=ox+lx-nx*shift,sy=oy+ly-ny*shift;
            const float dispersion=.06f*shift;
            Pixel refracted=s.sample(sx,sy);
            refracted.r=s.sample(sx+nx*dispersion,sy+ny*dispersion).r;
            refracted.b=s.sample(sx-nx*dispersion,sy-ny*dispersion).b;
            const float luminance=.2126f*refracted.r+.7152f*refracted.g+.0722f*refracted.b;
            refracted=refracted*1.15f+Pixel{luminance,luminance,luminance}*(-.15f);
            // Clear edge, softly illuminated center; no uniform bevel/shadow ring.
            const float veil=.24f*(1-edge);
            Pixel p=refracted*(.94f-veil)+Pixel{tint.r,tint.g,tint.b}*.06f+Pixel{1,1,1}*veil;
            const float lighting=clamp(.55f-.45f*(nx*.6f+ny*.8f),0,1);
            const float rim=radius>0?std::exp(-std::abs(sdf)/.6f)*.50f*lighting:0;
            p=p*(1-rim)+Pixel{1,1,1}*rim;
            pixels[y*bw+x]=pack(p,coverage);
        }
        auto props=D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96*s.scale,96*s.scale);
        if(SUCCEEDED(target->CreateBitmap(D2D1::SizeU(bw,bh),pixels.data(),bw*4,props,&bitmap))) {
            if(s.panels.size()>=64 || s.panelBytes+pixels.size()*4>32*1024*1024) {
                for(auto& entry:s.panels)entry.second->Release();
                s.panels.clear();s.panelBytes=0;
            }
            s.panels.emplace(key,bitmap);
            s.panelBytes+=pixels.size()*4;
        }
    }
    brush->SetColor(D2D1::ColorF(.06f,.21f,.29f,.018f));
    for(int i=radius>0?6:0;i>=1;--i) {
        auto shadow=rect;shadow.top+=i*1.8f;shadow.bottom+=i*1.8f;
        target->FillRoundedRectangle(D2D1::RoundedRect(shadow,radius,radius),brush);
    }
    if(bitmap) target->DrawBitmap(bitmap,rect);
    else {brush->SetColor(tint);target->FillRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),brush);}
    if(radius==0) {brush->Release();return;}
    D2D1_GRADIENT_STOP stops[]={{0,D2D1::ColorF(1,1,1,.82f)},{.45f,D2D1::ColorF(1,1,1,.05f)},{1,D2D1::ColorF(1,1,1,.45f)}};
    ID2D1GradientStopCollection* collection=nullptr;
    ID2D1LinearGradientBrush* rim=nullptr;
    if(SUCCEEDED(target->CreateGradientStopCollection(stops,3,&collection))) {
        float influence=clamp((s.mouseX-ox)/w,0,1);
        target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left+w*influence,rect.top),D2D1::Point2F(rect.right,rect.bottom)),collection,&rim);
        if(rim) {target->DrawRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),rim,.8f);rim->Release();}
        collection->Release();
    }
    if(s.mouseX>=ox && s.mouseX<=ox+w && s.mouseY>=oy && s.mouseY<=oy+h) {
        // A directional specular rim follows both mouse axes, leaving text clean.
        ID2D1GradientStopCollection* shineStops=nullptr;
        ID2D1RadialGradientBrush* shine=nullptr;
        D2D1_GRADIENT_STOP spec[]={{0,D2D1::ColorF(1,1,1,.95f)},{1,D2D1::ColorF(1,1,1,0)}};
        if(SUCCEEDED(target->CreateGradientStopCollection(spec,2,&shineStops))) {
            target->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
                D2D1::Point2F(s.mouseX-matrix._31,s.mouseY-matrix._32),D2D1::Point2F(0,0),
                (std::max)(60.0f,w*.55f),(std::max)(40.0f,h*.8f)),shineStops,&shine);
            if(shine) {target->DrawRoundedRectangle(D2D1::RoundedRect(rect,radius,radius),shine,2);shine->Release();}
            shineStops->Release();
        }
    }
    brush->Release();
}
}
