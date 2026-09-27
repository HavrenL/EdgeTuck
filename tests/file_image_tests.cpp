#include "file_images.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <chrono>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
static void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
static void check(HRESULT hr) { require(SUCCEEDED(hr),"image fixture creation failed"); }
static void fixture(const fs::path& path,bool transparent=false) {
    ComPtr<IWICImagingFactory> wic; check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)));
    ComPtr<IWICStream> stream; check(wic->CreateStream(&stream)); check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder; check(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder)); check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame; check(encoder->CreateNewFrame(&frame,nullptr)); check(frame->Initialize(nullptr)); check(frame->SetSize(120,240));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA; check(frame->SetPixelFormat(&format));
    std::vector<DWORD> pixels(120*240);
    for(int y=0;y<240;++y) for(int x=0;x<120;++x)
        pixels[y*120+x]=transparent?(y<120?0x80FFFFFF:0x80204060):(y<120?0xFFE03040:0xFF2040E0);
    check(frame->WritePixels(240,120*4,static_cast<UINT>(pixels.size()*4),reinterpret_cast<BYTE*>(pixels.data()))); check(frame->Commit()); check(encoder->Commit());
}
static void icon_fixture(const fs::path& path) {
    // A 64 px straight-alpha ICO: soft circle, translucent white / dark colors.
    // Bright and dark samples detect both missing and double premultiplication.
    std::ofstream file(path,std::ios::binary);
    auto word=[&](WORD v){file.write(reinterpret_cast<const char*>(&v),sizeof(v));};
    auto dword=[&](DWORD v){file.write(reinterpret_cast<const char*>(&v),sizeof(v));};
    word(0); word(1); word(1); file.put(64); file.put(64); file.put(0); file.put(0);
    word(1); word(32); dword(40+64*64*4+64*8); dword(22);
    BITMAPINFOHEADER info{sizeof(info)}; info.biWidth=64; info.biHeight=128; info.biPlanes=1; info.biBitCount=32;
    file.write(reinterpret_cast<const char*>(&info),sizeof(info));
    for(int y=63;y>=0;--y) for(int x=0;x<64;++x) {
        const float distance=std::hypot(x-31.5f,y-31.5f);
        const auto alpha=static_cast<DWORD>(std::lround(128*std::clamp(28.0f-distance,0.0f,1.0f)));
        dword((alpha<<24)|(y<32?0xFFFFFF:0x204060));
    }
    for(int i=0;i<64*8;++i) file.put(0);
}
static void shortcut_fixture(const fs::path& path,const fs::path& target,const fs::path& icon) {
    ComPtr<IShellLinkW> link; check(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&link)));
    check(link->SetPath(target.c_str())); check(link->SetIconLocation(icon.c_str(),0));
    ComPtr<IPersistFile> persist; check(link.As(&persist)); check(persist->Save(path.c_str(),TRUE));
}
static void check_alpha(const edge::FileImage& image,bool circle) {
    for(size_t i=0;i<image.pixels.size();i+=4)
        require(std::max({image.pixels[i],image.pixels[i+1],image.pixels[i+2]})<=image.pixels[i+3],"transparent edge RGB must not exceed alpha (white halo)");
    const unsigned x=image.width/2;
    const auto bright=(image.height/4*image.width+x)*4,dark=(image.height*3/4*image.width+x)*4;
    std::cout<<"Alpha sample "<<image.width<<'x'<<image.height<<" icon="<<circle<<" white(B,A)="<<int(image.pixels[bright])<<','<<int(image.pixels[bright+3])<<" dark(B,R,A)="<<int(image.pixels[dark])<<','<<int(image.pixels[dark+2])<<','<<int(image.pixels[dark+3])<<'\n';
    require(image.pixels[bright+3]>=115 && image.pixels[bright+3]<=140,"semi-transparent alpha is retained");
    require(std::abs(int(image.pixels[bright])-int(image.pixels[bright+3]))<=3,"white edge is premultiplied exactly once");
    // Shell can center the 64 px source inside a larger bitmap; this sample may
    // hit the soft circle edge. Expected color follows its actual coverage.
    const int dark_alpha=image.pixels[dark+3];
    require(dark_alpha>=32 && std::abs(int(image.pixels[dark])-(96*dark_alpha+127)/255)<=4 &&
        std::abs(int(image.pixels[dark+2])-(32*dark_alpha+127)/255)<=3,"dark colors are not premultiplied twice");
    if(circle) {
        // Shell may add a translucent frame at nonstandard icon sizes, so
        // inspect actual transparent pixels instead of assuming an empty corner.
        bool transparent=false;
        for(size_t i=0;i<image.pixels.size();i+=4) transparent|=image.pixels[i+3]==0;
        require(transparent,"icon retains its transparent background");
    }
}
static std::vector<char> bytes(const fs::path& path) { std::ifstream file(path,std::ios::binary); return {std::istreambuf_iterator<char>(file),{}}; }
static std::shared_ptr<const edge::FileImage> wait_image(edge::FileImages& images,const fs::path& path,int size) {
    const auto end=GetTickCount64()+5000;
    while(GetTickCount64()<end) { auto image=images.request(path.wstring(),size,nullptr); if(image)return image; Sleep(5); }
    return {};
}
int main() {
    const auto folder=fs::temp_directory_path()/(L"EdgeTuck-image-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    const auto path=folder/L"portrait.png",document=folder/L"notes.txt",transparent=folder/L"transparent.png",
        icon=folder/L"alpha.ico",shortcut=folder/L"alpha.lnk";
    int result=0; const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    try {
        check(initialized); require(fs::create_directory(folder),"unique fixture directory"); fixture(path);
        { std::ofstream file(document); file<<"Owned thumbnail fallback test."; }
        const auto original=bytes(path); const auto attributes=GetFileAttributesW(path.c_str()); const auto modified=fs::last_write_time(path);
        edge::FileImages images;
        const auto image=wait_image(images,path,64);
        require(image && image->thumbnail,"PNG must produce a thumbnail, not a generic file icon");
        require(image->width<image->height && image->height<=64,"portrait thumbnail preserves aspect ratio");
        const auto pixel=[&](unsigned y) { return *reinterpret_cast<const DWORD*>(&image->pixels[(y*image->width+image->width/2)*4]); };
        const auto red=pixel(image->height/4),blue=pixel(image->height*3/4);
        require(((red>>16)&255)>150 && (red&255)<100,"upper image pixels reach the preview");
        require((blue&255)>150 && ((blue>>16)&255)<100,"lower image pixels reach the preview");
        require((red>>24)>240 && (blue>>24)>240,"thumbnail alpha remains visible");
        require(images.request(path.wstring(),64,nullptr)==image,"cached image is reused");
        const auto larger=wait_image(images,path,128);
        require(larger && larger->height>image->height,"larger DPI gets a distinct higher-resolution image");
        const auto fallback=wait_image(images,document,40);
        require(fallback && !fallback->pixels.empty(),"a document without a picture still has a Shell representation");
        fixture(transparent,true); const auto transparent_original=bytes(transparent);
        const auto translucent=wait_image(images,transparent,64);
        require(translucent && translucent->thumbnail,"transparent PNG retains thumbnail preview");
        check_alpha(*translucent,false);
        icon_fixture(icon); shortcut_fixture(shortcut,document,icon);
        const auto icon_original=bytes(icon),shortcut_original=bytes(shortcut);
        for(int size:{40,50,60,64,80}) {
            const auto representation=wait_image(images,shortcut,size);
            require(representation && !representation->thumbnail,"shortcut uses Shell icon extraction");
            check_alpha(*representation,true);
        }
        require(bytes(transparent)==transparent_original && bytes(icon)==icon_original && bytes(shortcut)==shortcut_original,"alpha extraction preserves PNG ICO and shortcut bytes");
        require(original==bytes(path) && attributes==GetFileAttributesW(path.c_str()) && modified==fs::last_write_time(path),"thumbnail extraction must not change the original file");
        std::cout<<"File images PASS: portrait pixels, transparent PNG, translucent shortcut icon at 5 sizes, premultiplied edges, dark color fidelity, DPI, cache, original unchanged\n";
    } catch(std::exception const& error) { std::cerr<<error.what()<<'\n'; result=1; }
    std::error_code error;
    if(result==0) { for(const auto& file:{path,document,transparent,icon,shortcut}) fs::remove(file,error); fs::remove(folder,error); }
    else std::wcerr<<L"Fixtures retained: "<<folder.c_str()<<L'\n';
    if(SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
