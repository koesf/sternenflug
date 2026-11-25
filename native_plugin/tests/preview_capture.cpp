// Real production simulation + D3D11 renderer. stdout is RGB24 video, never logs.
#include "renderer.hpp"
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/base.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

namespace {
void require(bool ok, const std::string &message) { if (!ok) throw std::runtime_error(message); }
struct Options {
    std::filesystem::path obsBin, manifest, poster;
    std::string scene = "sternenflug";
    unsigned width = 1280, height = 720, fps = 30, seed = 1337;
    double seconds = 8, prime = 24, posterTime = 2;
};
Options options(int argc, char **argv) {
    Options result;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i]; require(i + 1 < argc, "Missing option value");
        const std::string value = argv[++i];
        if (name == "--obs-bin") result.obsBin = std::filesystem::absolute(value);
        else if (name == "--manifest") result.manifest = std::filesystem::absolute(value);
        else if (name == "--poster-rgb") result.poster = std::filesystem::absolute(value);
        else if (name == "--scene") result.scene = value;
        else if (name == "--width") result.width = unsigned(std::stoul(value));
        else if (name == "--height") result.height = unsigned(std::stoul(value));
        else if (name == "--fps") result.fps = unsigned(std::stoul(value));
        else if (name == "--seed") result.seed = unsigned(std::stoul(value));
        else if (name == "--seconds") result.seconds = std::stod(value);
        else if (name == "--prime") result.prime = std::stod(value);
        else if (name == "--poster-time") result.posterTime = std::stod(value);
        else throw std::runtime_error("Unknown option: " + name);
    }
    require(std::filesystem::exists(result.obsBin / "libobs-d3d11.dll"), "Pass --obs-bin <OBS/bin/64bit>");
    require(result.width >= 64 && result.width <= 3840 && result.height >= 64 && result.height <= 2160 &&
            result.fps >= 1 && result.fps <= 60 && std::isfinite(result.seconds) && result.seconds > 0 && result.seconds <= 60 &&
            std::isfinite(result.prime) && result.prime >= 0 && result.prime <= 120 &&
            std::isfinite(result.posterTime) && result.posterTime >= 0, "Invalid capture dimensions/duration");
    require(result.scene == "sternenflug" || result.scene == "regen" || result.scene == "herzen" || result.scene == "sanft-abwaerts",
            "Unknown preview scene");
    return result;
}
sf::Settings settings(const Options &opt) {
    sf::Settings s; s.seed = opt.seed;
    if (opt.scene == "regen") {
        s.motionMode = 1; s.set(sf::Param::Speed, 2.4); s.set(sf::Param::Density, 650);
        s.set(sf::Param::StarSize, 1.5); s.set(sf::Param::TrailLength, 80);
        s.set(sf::Param::TrailDensity, 100); s.set(sf::Param::TrailFalloff, 1.8);
    } else if (opt.scene == "sanft-abwaerts") {
        s.motionMode = 1; s.set(sf::Param::Speed, .5); s.set(sf::Param::Density, 220);
        s.set(sf::Param::StarSize, 3); s.set(sf::Param::TrailLength, 0); s.set(sf::Param::TrailDensity, 0);
    } else if (opt.scene == "herzen") {
        s.emissionForever = true; s.set(sf::Param::SymbolShare, 35);
        s.set(sf::Param::SymbolSize, 420); s.set(sf::Param::SizeJitter, 30);
        s.set(sf::Param::HueA, 330); s.set(sf::Param::SatA, 85); s.set(sf::Param::ValA, 100);
        s.set(sf::Param::HueB, 190); s.set(sf::Param::SatB, 85); s.set(sf::Param::ValB, 100);
    }
    return s;
}
std::shared_ptr<sf::Drawing> heart() {
    auto drawing = std::make_shared<sf::Drawing>(); drawing->id = 97;
    std::vector<sf::Point> points;
    for (int i = 0; i <= 96; ++i) {
        const double t = 6.283185307179586 * i / 96;
        const double x = 16 * std::pow(std::sin(t), 3);
        const double y = 13 * std::cos(t) - 5 * std::cos(2*t) - 2 * std::cos(3*t) - std::cos(4*t);
        points.push_back({.5 + x/(36*16.0/9.0), .5-y/36});
    }
    drawing->strokes.push_back(std::move(points)); return drawing;
}
double linear(double srgb) { return srgb <= .04045 ? srgb/12.92 : std::pow((srgb+.055)/1.055,2.4); }
std::array<unsigned char,65536> srgbLookup() {
    std::array<unsigned char,65536> result{};
    for (std::size_t i=0; i<result.size(); ++i) {
        const double value=double(i)/65535;
        const double encoded=value<=.0031308 ? value*12.92 : 1.055*std::pow(value,1/2.4)-.055;
        result[i]=static_cast<unsigned char>(std::round(std::clamp(encoded,0.0,1.0)*255));
    }
    return result;
}
void manifest(const Options &opt,const sf::Settings &s,unsigned frames,std::size_t minVertices,std::size_t maxVertices) {
    if (opt.manifest.empty()) return;
    std::ofstream out(opt.manifest); require(bool(out),"Cannot write scene provenance");
    out.imbue(std::locale::classic()); out<<std::setprecision(12);
    out<<"{\"version\":1,\"plugin_version\":\""<<STERNENFLUG_VERSION<<"\",\"scene\":\""<<opt.scene
       <<"\",\"width\":"<<opt.width<<",\"height\":"<<opt.height<<",\"fps\":"<<opt.fps<<",\"frames\":"<<frames
       <<",\"duration_seconds\":"<<double(frames)/opt.fps<<",\"virtual_start_seconds\":"<<opt.prime
       <<",\"seed\":"<<opt.seed<<",\"offscreen_format\":\"RGBA32F linear premultiplied\""
       <<",\"background_srgb\":\"#0B1020\",\"heart_points\":"<<(opt.scene=="herzen"?97:0)
       <<",\"motion_mode\":"<<s.motionMode<<",\"color_mode\":"<<s.colorMode
       <<",\"emission_forever\":"<<(s.emissionForever?"true":"false")
       <<",\"poster_frame\":"<<std::min(frames-1,unsigned(std::llround(opt.fps*opt.posterTime)))
       <<",\"minimum_vertices\":"<<minVertices<<",\"maximum_vertices\":"<<maxVertices<<",\"parameters\":{";
    bool first=true;
    for(const auto &p:sf::parameterDefs()) {if(!first)out<<',';first=false;out<<'"'<<p.key<<"\":"<<s.get(p.id);}
    out<<"}}\n"; require(bool(out),"Scene provenance write failed");
}
void capture(const Options &opt) {
    sf::Settings s=settings(opt); sf::Simulation simulation(s);
    if(opt.scene=="herzen") require(simulation.submit(heart()),"Heart submit failed");
    const auto priming=unsigned(std::llround(opt.prime*60));
    for(unsigned i=0;i<priming;++i) simulation.tick(1.0/60);
    sf::Renderer renderer; require(renderer.initialize(),renderer.error());
    auto *target=gs_texrender_create(GS_RGBA32F,GS_ZS_NONE);
    auto *stage=gs_stagesurface_create(opt.width,opt.height,GS_RGBA32F);
    require(target&&stage,"Offscreen allocation failed");
    const auto lookup=srgbLookup();
    constexpr std::array<unsigned char,3> background{11,16,32};
    const std::array<double,3> backgroundLinear{linear(11.0/255),linear(16.0/255),linear(32.0/255)};
    std::vector<unsigned char> rgb(std::size_t(opt.width)*opt.height*3);
    const auto frames=unsigned(std::llround(opt.seconds*opt.fps));
    std::size_t minVertices=std::size_t(-1),maxVertices=0;
    try {
        for(unsigned frame=0;frame<frames;++frame) {
            const auto particles=simulation.renderFrame(opt.width,opt.height);
            gs_begin_scene();gs_texrender_reset(target);
            require(gs_texrender_begin(target,opt.width,opt.height),"Offscreen begin failed");
            gs_matrix_identity();gs_ortho(0,float(opt.width),0,float(opt.height),-1,1);
            gs_set_cull_mode(GS_NEITHER);gs_enable_depth_test(false);
            vec4 transparent{};gs_clear(GS_CLEAR_COLOR,&transparent,1,0);
            const bool rendered=renderer.render(particles,opt.width,opt.height);
            gs_texrender_end(target);gs_end_scene();require(rendered,renderer.error());
            minVertices=std::min(minVertices,renderer.vertexCount());maxVertices=std::max(maxVertices,renderer.vertexCount());
            gs_stage_texture(stage,gs_texrender_get_texture(target));
            std::uint8_t *bytes=nullptr;std::uint32_t pitch=0;
            require(gs_stagesurface_map(stage,&bytes,&pitch),"GPU readback failed");
            bool valid=true;
            for(unsigned y=0;y<opt.height;++y) {
                const auto *row=reinterpret_cast<const float *>(bytes+std::size_t(y)*pitch);
                for(unsigned x=0;x<opt.width;++x) {
                    const auto *pixel=row+std::size_t(x)*4;
                    auto *output=rgb.data()+(std::size_t(y)*opt.width+x)*3;
                    const double alpha=std::clamp(double(pixel[3]),0.0,1.0);
                    if(alpha==0) {std::copy(background.begin(),background.end(),output);continue;}
                    for(int c=0;c<3;++c) {
                        const double value=double(pixel[c])+backgroundLinear[c]*(1-alpha);
                        if(!std::isfinite(value)) {valid=false;output[c]=0;continue;}
                        const auto index=std::size_t(std::llround(std::clamp(value,0.0,1.0)*65535));
                        output[c]=lookup[index];
                    }
                }
            }
            gs_stagesurface_unmap(stage);require(valid,"Nonfinite GPU output");
            if(!opt.poster.empty()&&frame==std::min(frames-1,unsigned(std::llround(opt.fps*opt.posterTime)))) {
                std::ofstream poster(opt.poster,std::ios::binary);poster.write(reinterpret_cast<const char *>(rgb.data()),std::streamsize(rgb.size()));
                require(bool(poster),"Poster write failed");
            }
            require(std::fwrite(rgb.data(),1,rgb.size(),stdout)==rgb.size(),"Video pipe closed");
            simulation.tick(1.0/opt.fps);
        }
        require(std::fflush(stdout)==0,"Video flush failed");
        manifest(opt,s,frames,minVertices,maxVertices);
    } catch(...) {gs_stagesurface_destroy(stage);gs_texrender_destroy(target);throw;}
    gs_stagesurface_destroy(stage);gs_texrender_destroy(target);
    std::cerr<<"Captured "<<opt.scene<<": "<<frames<<" real GPU frames\n";
}
} // namespace
int main(int argc,char **argv) {
    graphics_t *graphics=nullptr;
    try {
        const auto opt=options(argc,argv);
        // Keep binary stdout clean; no device/display identifiers enter media.
        base_set_log_handler([](int,const char *,va_list,void *){},nullptr);
#ifdef _WIN32
        _setmode(_fileno(stdout),_O_BINARY);
#endif
        std::filesystem::current_path(opt.obsBin);
        const auto driver=(opt.obsBin/"libobs-d3d11.dll").string();
        require(gs_create(&graphics,driver.c_str(),0)==GS_SUCCESS&&graphics,"D3D11 startup failed");
        gs_enter_context(graphics);
        try {capture(opt);}catch(...) {gs_leave_context();throw;}
        gs_leave_context();gs_destroy(graphics);graphics=nullptr;return 0;
    }catch(const std::exception &error){std::cerr<<"CAPTURE FAILED: "<<error.what()<<'\n';if(graphics)gs_destroy(graphics);return 1;}
}
