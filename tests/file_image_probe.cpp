#include "file_images.hpp"
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
using Microsoft::WRL::ComPtr;
static void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("Shell/WIC image probe failed"); }
static void stats(const char* mode,const edge::FileImage& image) {
    unsigned partial{},invalid{},transparent_rgb{};
    for(size_t i=0;i<image.pixels.size();i+=4) {
        auto a=image.pixels[i+3]; const auto rgb=std::max({image.pixels[i],image.pixels[i+1],image.pixels[i+2]});
        partial+=a>0 && a<255; invalid+=rgb>a; transparent_rgb+=a==0 && rgb>0;
    }
    std::cout<<mode<<" "<<image.width<<"x"<<image.height<<" thumbnail="<<image.thumbnail<<" partial="<<partial<<" RGB_exceeds_alpha="<<invalid<<" transparent_RGB="<<transparent_rgb<<'\n';
}
static edge::FileImage shell_image(const wchar_t* path,int size,WICBitmapAlphaChannelOption alpha) {
    ComPtr<IShellItemImageFactory> factory; check(SHCreateItemFromParsingName(path,nullptr,IID_PPV_ARGS(&factory)));
    HBITMAP bitmap{};
    const bool thumbnail=SUCCEEDED(factory->GetImage({size,size},SIIGBF_THUMBNAILONLY,&bitmap));
    if(!thumbnail) check(factory->GetImage({size,size},SIIGBF_ICONONLY,&bitmap));
    ComPtr<IWICImagingFactory> wic; check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)));
    ComPtr<IWICBitmap> source; const auto hr=wic->CreateBitmapFromHBITMAP(bitmap,nullptr,alpha,&source); DeleteObject(bitmap); check(hr);
    edge::FileImage image; image.thumbnail=thumbnail; check(source->GetSize(&image.width,&image.height));
    ComPtr<IWICFormatConverter> converter; check(wic->CreateFormatConverter(&converter));
    check(converter->Initialize(source.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    image.pixels.resize(image.width*image.height*4);
    check(converter->CopyPixels(nullptr,image.width*4,static_cast<UINT>(image.pixels.size()),image.pixels.data()));
    return image;
}
static void png(const std::filesystem::path& path,const edge::FileImage& image) {
    // Diagnostic composites on dark and light backgrounds, with 4x pixel zoom.
    const unsigned width=image.width*8,height=image.height*4;
    std::vector<BYTE> pixels(width*height*4);
    for(unsigned y=0;y<image.height*4;++y) for(unsigned x=0;x<image.width*8;++x) {
        const auto offset=((y/4)*image.width+(x/4)%image.width)*4;
        const unsigned bg=x<image.width*4?25:230,a=image.pixels[offset+3];
        for(int c=0;c<3;++c) pixels[(y*width+x)*4+c]=static_cast<BYTE>(std::min(255u,image.pixels[offset+c]+(bg*(255-a)+127)/255));
        pixels[(y*width+x)*4+3]=255;
    }
    ComPtr<IWICImagingFactory> wic; check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)));
    ComPtr<IWICStream> stream; check(wic->CreateStream(&stream)); check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder; check(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)); check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; check(encoder->CreateNewFrame(&frame,nullptr)); check(frame->Initialize(nullptr)); check(frame->SetSize(width,height));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA; check(frame->SetPixelFormat(&format));
    check(frame->WritePixels(height,width*4,static_cast<UINT>(pixels.size()),pixels.data())); check(frame->Commit()); check(encoder->Commit());
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) return 2;
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try {
        check(initialized); const std::filesystem::path output=argv[2]; std::filesystem::create_directories(output);
        edge::FileImages images;
        for(int size:{40,50,60,80}) {
            const auto end=GetTickCount64()+8000; std::shared_ptr<const edge::FileImage> current;
            while(GetTickCount64()<end && !(current=images.request(argv[1],size,nullptr))) Sleep(5);
            if(!current) throw std::runtime_error("image extraction timeout");
            stats("app",*current); png(output/(L"app-"+std::to_wstring(size)+L".png"),*current);
            for(auto alpha:{WICBitmapUsePremultipliedAlpha,WICBitmapUseAlpha}) {
                const auto image=shell_image(argv[1],size,alpha);
                const auto name=alpha==WICBitmapUseAlpha?L"straight-":L"premul-";
                stats(alpha==WICBitmapUseAlpha?"straight":"premul",image);
                png(output/(name+std::to_wstring(size)+L".png"),image);
            }
        }
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; if(SUCCEEDED(initialized)) CoUninitialize(); return 1; }
    CoUninitialize(); return 0;
}
