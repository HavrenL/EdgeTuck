#include "optical_effect.hpp"
#include "material.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1effects.h>
#include <wrl/client.h>
#include <vector>
#include <iostream>
#include <algorithm>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if(FAILED(hr)) { std::cerr<<"HRESULT="<<std::hex<<static_cast<unsigned long>(hr)<<std::dec<<'\n'; throw std::runtime_error("GPU effect call failed"); } }
static void require(bool v,const char* label) { if(!v) throw std::runtime_error(label); }
int main() {
    try {
        constexpr int w=400,h=300;
        ComPtr<ID3D11Device> d3d; ComPtr<IDXGIDevice> dxgi; ComPtr<ID2D1Factory1> factory; ComPtr<ID2D1Device> device; ComPtr<ID2D1DeviceContext> ctx;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&d3d,nullptr,nullptr));
        check(d3d.As(&dxgi)); check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,IID_PPV_ARGS(&factory)));
        check(edge::register_optical_effect(factory.Get())); check(factory->CreateDevice(dxgi.Get(),&device)); check(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,&ctx));
        const auto pixel=D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED);
        ComPtr<ID2D1Bitmap1> source,target,readback;
        check(ctx->CreateBitmap(D2D1::SizeU(w,h),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,pixel),&source));
        check(ctx->CreateBitmap(D2D1::SizeU(w,h),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,pixel),&target));
        check(ctx->CreateBitmap(D2D1::SizeU(w,h),nullptr,0,D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,pixel),&readback));
        ctx->SetTarget(target.Get());
        ComPtr<ID2D1Effect> blur,scatter,optical;
        check(ctx->CreateEffect(CLSID_D2D1GaussianBlur,&blur)); check(ctx->CreateEffect(CLSID_D2D1GaussianBlur,&scatter)); check(ctx->CreateEffect(edge::CLSID_EdgeOptical,&optical));
        blur->SetInput(0,source.Get()); scatter->SetInput(0,source.Get()); optical->SetInputEffect(0,blur.Get()); optical->SetInputEffect(1,scatter.Get());
        check(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_HARD)); check(scatter->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_HARD));
        auto render=[&](edge::GlassMaterial m,const D2D1_RECT_F* crop=nullptr) {
            check(blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,m.blur)); check(scatter->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,m.blur+2));
            edge::OpticalConstants c{w,h,1,edge::drawer_corner_dip,m.depth,m.light,m.dispersion,edge::drawer_visual_inset_dip,m.refraction?1.0f:0,m.lighting?1.0f:0,m.chromatic?1.0f:0,0};
            check(optical->SetValueByName(L"Constants",D2D1_PROPERTY_TYPE_BLOB,reinterpret_cast<const BYTE*>(&c),sizeof(c)));
            ctx->BeginDraw(); ctx->Clear(D2D1::ColorF(0,0.0f));
            if(crop) ctx->DrawImage(optical.Get(),D2D1::Point2F(crop->left,crop->top),*crop);
            else ctx->DrawImage(optical.Get());
            check(ctx->EndDraw());
            check(readback->CopyFromBitmap(nullptr,target.Get(),nullptr)); D2D1_MAPPED_RECT mapped{}; check(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped));
            std::vector<DWORD> result(w*h);
            for(int y=0;y<h;++y) std::copy_n(reinterpret_cast<const DWORD*>(mapped.bits+y*mapped.pitch),w,result.begin()+y*w);
            check(readback->Unmap()); return result;
        };
        std::vector<DWORD> grid(w*h);
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) grid[y*w+x]=0xFF000000|((x%13<3?220:55)<<16)|((y%17<4?200:75)<<8)|(80+(x+y)%100);
        check(source->CopyFromMemory(nullptr,grid.data(),w*4));
        const auto clear=render({});
        auto m=edge::GlassMaterial{}; m.refraction=false; require(render(m)!=clear,"refraction must affect rendered pixels");
        m={}; m.chromatic=false; require(render(m)!=clear,"dispersion must affect rendered pixels");
        m={}; m.lighting=false; require(render(m)!=clear,"lighting must affect rendered pixels");
        require(render(edge::glass_preset(1))!=clear,"blur preset must affect pixels");
        require((clear[0]>>24)==0 && (clear[w-1]>>24)==0 && (clear[(h-1)*w]>>24)==0 && (clear.back()>>24)==0,"all outer corners must stay transparent");
        require((clear[h/2*w+w/2]>>24)==255,"material center must fully replace sharp backdrop");
        for(auto material:{edge::glass_preset(0),edge::glass_preset(1),edge::glass_preset(2),edge::GlassMaterial{6,36,1.5f,2}}) {
            const auto full=render(material);
            for(const auto crop:{D2D1::RectF(0,0,14,h),D2D1::RectF(w-14,0,w,h),D2D1::RectF(0,0,w,14),D2D1::RectF(0,h-14,w,h)}) {
                const auto strip=render(material,&crop);
                for(int y=static_cast<int>(crop.top);y<crop.bottom;++y) for(int x=static_cast<int>(crop.left);x<crop.right;++x) {
                    const auto a=strip[y*w+x],b=full[y*w+x];
                    for(int channel=0;channel<4;++channel) {
                        const int difference=std::abs(static_cast<int>((a>>(channel*8))&255)-static_cast<int>((b>>(channel*8))&255));
                        if(difference>1) std::cerr<<"crop="<<crop.left<<','<<crop.top<<','<<crop.right<<','<<crop.bottom
                            <<" pixel="<<x<<','<<y<<" channel="<<channel<<" delta="<<difference
                            <<" blur="<<material.blur<<" depth="<<material.depth<<'\n';
                        require(difference<=1,"cropped liquid edge must match full-panel rendering");
                    }
                }
            }
        }
        // Flat input isolates light from bent grid lines. On each straight edge,
        // no positive inner peak may reappear after the outer highlight fades.
        std::fill(grid.begin(),grid.end(),0xFF606060); check(source->CopyFromMemory(nullptr,grid.data(),w*4));
        for(int preset=0;preset<3;++preset) {
            const auto pixels=render(edge::glass_preset(preset));
            for(int side=0;side<4;++side) {
                int previous=10000;
                for(int inset=1;inset<24;++inset) {
                    int x=w/2,y=h/2;
                    const int offset=static_cast<int>(edge::drawer_visual_inset_dip);
                    if(side==0) x=offset+inset; if(side==1) x=w-1-offset-inset; if(side==2) y=offset+inset; if(side==3) y=h-1-offset-inset;
                    const DWORD c=pixels[y*w+x];
                    const int light=std::max(0,static_cast<int>((c&255)+((c>>8)&255)+((c>>16)&255))-3*96);
                    require(light<=previous+3,"second inner highlight on a straight edge"); previous=light;
                }
            }
        }
        std::cout<<"PASS: native GPU blur, refraction, dispersion, light, alpha and single-rim profiles (all presets, all four edges)\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1; }
}
