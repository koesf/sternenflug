#include "renderer.hpp"
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition,const char *message) { if(!condition) throw std::runtime_error(message); }
std::vector<float> capture(sf::Renderer &renderer,const std::vector<sf::RenderParticle> &frame,unsigned w,unsigned h,const vec4 *background=nullptr) {
    std::cerr<<"capture "<<w<<"x"<<h<<" particles="<<frame.size()<<std::endl;
    auto *target=gs_texrender_create(GS_RGBA32F,GS_ZS_NONE);
    auto *stage=gs_stagesurface_create(w,h,GS_RGBA32F);
    require(target && stage,"render targets missing");
    gs_begin_scene();
    require(gs_texrender_begin(target,w,h),"texrender begin failed");
    gs_matrix_identity(); gs_ortho(0,float(w),0,float(h),-1,1);
    gs_set_cull_mode(GS_NEITHER); gs_enable_depth_test(false);
    vec4 clear{}; if(background) clear=*background; gs_clear(GS_CLEAR_COLOR,&clear,1,0);
    const bool rendered=renderer.render(frame,w,h);
    std::cerr<<"draw done verts="<<renderer.vertexCount()<<std::endl;
    gs_texrender_end(target); gs_end_scene();
    require(rendered,renderer.error().c_str());
    gs_stage_texture(stage,gs_texrender_get_texture(target));
    uint8_t *bytes=nullptr; uint32_t pitch=0;
    require(gs_stagesurface_map(stage,&bytes,&pitch),"map failed");
    std::vector<float> pixels(size_t(w)*h*4);
    for(unsigned y=0;y<h;++y) std::memcpy(pixels.data()+size_t(y)*w*4,bytes+size_t(y)*pitch,size_t(w)*16);
    gs_stagesurface_unmap(stage); gs_stagesurface_destroy(stage); gs_texrender_destroy(target);
    for(float value:pixels) require(std::isfinite(value)&&value>=-.00001f&&value<=1.0001f,"nonfinite or out-of-range pixel");
    return pixels;
}
void ppm(const std::filesystem::path &path,const std::vector<float> &pixels,unsigned w,unsigned h) {
    std::ofstream file(path,std::ios::binary); file<<"P6\n"<<w<<" "<<h<<"\n255\n";
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        const size_t p=(size_t(y)*w+x)*4;
        const double alpha=pixels[p+3],check=((x/24+y/24)%2)? .15:.24;
        for(int c=0;c<3;++c) {
            double linear=std::clamp(double(pixels[p+c])+check*(1-alpha),0.0,1.0);
            double srgb=linear<=.0031308?linear*12.92:1.055*std::pow(linear,1/2.4)-.055;
            file.put(char(std::clamp(srgb,0.0,1.0)*255+.5));
        }
    }
}
void visibleTransparentOutput(const std::vector<float> &pixels) {
    size_t visible=0,transparent=0;
    for(size_t p=0;p<pixels.size();p+=4) {
        const float alpha=pixels[p+3];
        if(alpha>.0001f) ++visible;
        if(alpha==0) {
            ++transparent;
            require(pixels[p]==0 && pixels[p+1]==0 && pixels[p+2]==0,
                    "downward stream has color in fully transparent pixels");
        }
        for(int c=0;c<3;++c)
            require(pixels[p+c]<=alpha+.0001f,"downward stream RGB is not premultiplied by alpha");
    }
    require(visible>100,"downward stream is not visibly rendered");
    require(transparent>pixels.size()/8,"downward stream unexpectedly covers the transparent canvas");
}
void saveCapture(const std::filesystem::path &directory,const char *name,
                 const std::vector<float> &pixels,unsigned w,unsigned h) {
    ppm(directory/(std::string(name)+".ppm"),pixels,w,h);
    std::ofstream floats(directory/(std::string(name)+".rgba32f"),std::ios::binary);
    require(bool(floats),"could not create downward stream GPU capture");
    floats.write(reinterpret_cast<const char *>(pixels.data()),std::streamsize(pixels.size()*sizeof(float)));
    require(bool(floats),"could not write downward stream GPU capture");
}
}
int main(int argc,char **argv) {
    const char *configuredBin=std::getenv("STERNENFLUG_OBS_BIN");
    const auto bin=std::filesystem::absolute(configuredBin && *configuredBin
        ? std::filesystem::path(configuredBin)
        : std::filesystem::path(R"(C:\Program Files (x86)\Steam\steamapps\common\OBS Studio\bin\64bit)"));
    std::filesystem::current_path(bin);
    graphics_t *graphics=nullptr;
    const auto driver=(bin/"libobs-d3d11.dll").string();
    if(gs_create(&graphics,driver.c_str(),0)!=GS_SUCCESS || !graphics) return 2;
    int result=0;
    gs_enter_context(graphics);
    try {
        sf::Renderer renderer;
        if(!renderer.initialize()) throw std::runtime_error(renderer.error());
        std::cerr<<"effect initialized"<<std::endl;
        const auto empty=capture(renderer,{},640,360);
        require(std::all_of(empty.begin(),empty.end(),[](float f){return f==0;}),"empty area is not RGBA zero");
        sf::RenderParticle dot;
        dot.center={.5,.5}; dot.radiusPixels=9; dot.fade=1; dot.color={1,.5,.2};
        const auto full=capture(renderer,{dot},640,360);
        const size_t middle=(size_t(180)*640+320)*4;
        require(full[middle+3]>.9,"star is not visible");
        dot.fade=.5;
        const auto half=capture(renderer,{dot},640,360);
        require(std::abs(full[middle+3]*.5-half[middle+3])<1e-5,"opacity does not scale alpha");
        vec4 background; vec4_set(&background,.2f,.7f,.1f,1.f);
        const auto composite=capture(renderer,{dot},640,360,&background);
        const double alpha=half[middle+3];
        const double rgb[3]{dot.color.r,dot.color.g,dot.color.b};
        const double bg[3]{.2,.7,.1};
        for(int c=0;c<3;++c)
            require(std::abs(composite[middle+c]-(rgb[c]*alpha+bg[c]*(1-alpha)))<1e-5,
                    "premultiplied alpha composite over colored background is wrong");
        require(std::abs(composite[middle+3]-1)<1e-5,"opaque background alpha changed");
        dot.fade=0;
        const auto zero=capture(renderer,{dot},640,360);
        require(std::all_of(zero.begin(),zero.end(),[](float f){return f==0;}),"opacity zero leaves pixels");
        auto outline=std::make_shared<sf::Drawing>(); outline->id=99;
        outline->strokes={{{.3,.2},{.7,.2},{.7,.8},{.3,.8},{.3,.2}}};
        dot.fade=1; dot.radiusPixels=240; dot.drawing=outline;
        const auto shape=capture(renderer,{dot},640,360);
        require(shape[middle+3]==0,"outline interior is opaque");
        require(std::any_of(shape.begin(),shape.end(),[](float f){return f>.1f;}),"outline is missing");
        dot.radiusPixels=2048;
        capture(renderer,{dot},640,360);

        auto heart=std::make_shared<sf::Drawing>(); heart->id=100;
        std::vector<sf::Point> heartStroke;
        for(int i=0;i<=96;++i) {
            const double t=6.283185307179586*i/96;
            const double x=16*std::pow(std::sin(t),3);
            const double y=13*std::cos(t)-5*std::cos(2*t)-2*std::cos(3*t)-std::cos(4*t);
            heartStroke.push_back({.5+x/(36*16.0/9.0),.5-y/36});
        }
        heart->strokes.push_back(heartStroke);

        sf::Settings settings;
        settings.emissionForever=true; settings.set(sf::Param::SymbolShare,35);
        sf::Simulation simulation(settings); simulation.submit(heart); simulation.tick(7);
        const auto frame=simulation.renderFrame(1920,1080);
        const auto start=std::chrono::steady_clock::now();
        const auto image=capture(renderer,frame,1920,1080);
        const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        std::filesystem::path out=argc>1?argv[1]:std::filesystem::temp_directory_path()/"sternenflug-native-gpu";
        std::filesystem::create_directories(out);
        ppm(out/"symbols-1080p.ppm",image,1920,1080);
        {std::ofstream floats(out/"symbols-1080p.rgba32f",std::ios::binary);floats.write(reinterpret_cast<const char *>(image.data()),std::streamsize(image.size()*sizeof(float)));}
        const auto start4k=std::chrono::steady_clock::now();
        capture(renderer,simulation.renderFrame(3840,2160),3840,2160);
        const auto ms4k=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start4k).count();

        // Match the two movement templates; colors and motif sizes are explicit
        // visualization fixtures and do not change the production templates.
        for(bool rain : {true,false}) {
            sf::Settings stream;
            stream.motionMode=1;
            stream.set(sf::Param::Speed,rain?2.4:.5);
            stream.set(sf::Param::Density,rain?650:220);
            stream.set(sf::Param::StarSize,rain?1.5:3.0);
            stream.set(sf::Param::TrailLength,rain?80:0);
            stream.set(sf::Param::TrailDensity,rain?100:0);
            stream.set(sf::Param::TrailFalloff,rain?1.8:1.5);
            sf::Simulation streamSimulation(stream);
            streamSimulation.tick(6);
            const auto streamImage=capture(renderer,streamSimulation.renderFrame(1920,1080),1920,1080);
            visibleTransparentOutput(streamImage);
            saveCapture(out,rain?"rain-1080p":"soft-downward-1080p",streamImage,1920,1080);

            // A flowing preset must still produce visible, transparent output
            // after extended motion rather than drift off screen.
            streamSimulation.tick(24);
            visibleTransparentOutput(capture(renderer,streamSimulation.renderFrame(640,360),640,360));
            stream.set(sf::Param::Opacity,0);
            streamSimulation.setSettings(stream);
            const auto streamZero=capture(renderer,streamSimulation.renderFrame(640,360),640,360);
            require(std::all_of(streamZero.begin(),streamZero.end(),[](float f){return f==0;}),
                    "downward stream opacity zero leaves pixels");
        }
        std::cout<<"PASS GPU: transparent empty/opacity0; opacity50; open outlines; 4096px motif; 1080p+4K. "
                 <<"Rain+soft downward mode1: visible/finite/premultiplied/transparent, replenishment and opacity0. "
                 <<"Render+readback1080="<<ms<<"ms, 4K="<<ms4k<<"ms; vertices="<<renderer.vertexCount()<<"\n";
    } catch(const std::exception &error) {std::cerr<<"FAIL GPU: "<<error.what()<<"\n";result=1;}
    gs_leave_context(); gs_destroy(graphics);
    return result;
}
