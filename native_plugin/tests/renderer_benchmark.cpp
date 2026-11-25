#include "renderer.hpp"
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
void require(bool ok, const std::string &message) { if (!ok) throw std::runtime_error(message); }
std::string quoted(const std::string &text) {
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '\\' || c == '"') { result += '\\'; result += char(c); }
        else if (c == '\n') result += "\\n";
        else if (c >= 32) result += char(c);
    }
    return result + '"';
}
struct Options {
    std::filesystem::path obsBin, output;
    int warmup = 30, repetitions = 3, samples = 60, adapter = 0;
};
struct AdapterInfo { std::string name; std::uint64_t vram = 0; };
AdapterInfo adapterInfo(int index) {
    IDXGIFactory1 *factory = nullptr;
    require(SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))), "DXGI factory failed");
    IDXGIAdapter1 *adapter = nullptr;
    const auto selected = factory->EnumAdapters1(unsigned(index), &adapter);
    factory->Release();
    require(SUCCEEDED(selected) && adapter, "DXGI adapter lookup failed");
    DXGI_ADAPTER_DESC1 description{};
    const auto described = adapter->GetDesc1(&description); adapter->Release();
    require(SUCCEEDED(described), "DXGI description failed");
    const int length = WideCharToMultiByte(CP_UTF8,0,description.Description,-1,nullptr,0,nullptr,nullptr);
    std::string name(std::size_t(length), '\0');
    WideCharToMultiByte(CP_UTF8,0,description.Description,-1,name.data(),length,nullptr,nullptr);
    if (!name.empty()) name.pop_back();
    return {name,std::uint64_t(description.DedicatedVideoMemory)};
}
Options options(int argc, char **argv) {
    Options out;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        require(i + 1 < argc, "Every option needs a value");
        const std::string value = argv[++i];
        if (key == "--obs-bin") out.obsBin = std::filesystem::absolute(value);
        else if (key == "--output") out.output = std::filesystem::absolute(value);
        else if (key == "--warmup") out.warmup = std::stoi(value);
        else if (key == "--repetitions") out.repetitions = std::stoi(value);
        else if (key == "--samples") out.samples = std::stoi(value);
        else if (key == "--adapter") out.adapter = std::stoi(value);
        else throw std::runtime_error("Unknown option: " + key);
    }
    require(!out.output.empty() && std::filesystem::exists(out.obsBin / "libobs-d3d11.dll"),
            "Pass --obs-bin <OBS/bin/64bit> and --output <raw.json>");
    require(out.warmup >= 1 && out.repetitions >= 2 && out.repetitions <= 20 &&
            out.samples >= 10 && out.samples <= 1000 && out.adapter >= 0,
            "Invalid benchmark sample settings");
    return out;
}
std::shared_ptr<sf::Drawing> heart() {
    auto drawing = std::make_shared<sf::Drawing>(); drawing->id = 97;
    std::vector<sf::Point> points;
    for (int i = 0; i <= 96; ++i) {
        const double t = 6.283185307179586 * i / 96;
        const double x = 16 * std::pow(std::sin(t), 3);
        const double y = 13 * std::cos(t) - 5 * std::cos(2*t) - 2 * std::cos(3*t) - std::cos(4*t);
        points.push_back({.5 + x / (36 * 16.0 / 9.0), .5 - y / 36});
    }
    drawing->strokes.push_back(std::move(points)); return drawing;
}
struct Case { const char *name; int density; bool symbols = false, rain = false, synthetic = false; };
sf::Settings settings(const Case &test) {
    sf::Settings result; result.seed = 1337; result.emissionForever = test.symbols;
    result.set(sf::Param::Density, test.density);
    result.set(sf::Param::SymbolShare, 35);
    if (test.rain) {
        result.motionMode = 1; result.set(sf::Param::Speed, 2.4);
        result.set(sf::Param::StarSize, 1.5); result.set(sf::Param::TrailLength, 80);
        result.set(sf::Param::TrailDensity, 100); result.set(sf::Param::TrailFalloff, 1.8);
    }
    return result;
}
std::vector<sf::RenderParticle> stressFrame(const std::shared_ptr<sf::Drawing> &shape) {
    std::vector<sf::RenderParticle> frame; frame.reserve(4096);
    for (int i = 0; i < 4096; ++i) {
        sf::RenderParticle p;
        p.id = unsigned(i + 1); p.center = {(i % 64 + .5) / 64, (i / 64 + .5) / 64};
        p.radiusPixels = (i % 100 < 35) ? 120 : 1;
        p.fade = .75; p.color = {.7, .85, 1};
        p.trailEnd = {p.center.x, p.center.y - 80.0 / 1080};
        p.trailStrength = i % 100 < 35 ? 1 : 0;
        if (i % 100 < 35) p.drawing = shape;
        frame.push_back(std::move(p));
    }
    return frame;
}
struct Sample {
    int repetition;
    double simulation, frame, submit, gpu, wait, wall;
    std::size_t particles, symbols, vertices;
};
struct Resources {
    gs_texrender_t *target = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
    gs_timer_t *timer = gs_timer_create();
    gs_timer_range_t *range = gs_timer_range_create();
    ~Resources() {
        if (range) gs_timer_range_destroy(range);
        if (timer) gs_timer_destroy(timer);
        if (target) gs_texrender_destroy(target);
    }
};
Sample draw(sf::Renderer &renderer, sf::Simulation *simulation,
            const std::vector<sf::RenderParticle> &synthetic, Resources &r,
            unsigned w, unsigned h, int repetition) {
    Sample result{}; result.repetition = repetition;
    const auto wallStart = Clock::now();
    std::vector<sf::RenderParticle> generated;
    if (simulation) {
        const auto start = Clock::now(); simulation->tick(1.0/60);
        const auto ticked = Clock::now(); generated = simulation->renderFrame(w, h);
        const auto prepared = Clock::now();
        result.simulation = milliseconds(start, ticked); result.frame = milliseconds(ticked, prepared);
    } else { result.simulation = result.frame = -1; }
    const auto &frame = simulation ? generated : synthetic;
    result.particles = frame.size();
    result.symbols = std::count_if(frame.begin(), frame.end(), [](const auto &p){return bool(p.drawing);});
    gs_begin_scene(); gs_texrender_reset(r.target);
    require(gs_texrender_begin(r.target, w, h), "Offscreen target begin failed");
    gs_matrix_identity(); gs_ortho(0, float(w), 0, float(h), -1, 1);
    gs_set_cull_mode(GS_NEITHER); gs_enable_depth_test(false);
    vec4 clear{}; gs_clear(GS_CLEAR_COLOR, &clear, 1, 0);
    // GPU timestamps bracket the production renderer unchanged. This interval
    // may include GPU idle time while its CPU-side geometry is being prepared.
    gs_timer_range_begin(r.range); gs_timer_begin(r.timer);
    const auto submitStart = Clock::now();
    const bool rendered = renderer.render(frame, w, h);
    const auto submitEnd = Clock::now();
    gs_timer_end(r.timer); gs_timer_range_end(r.range);
    gs_texrender_end(r.target); gs_end_scene();
    require(rendered, renderer.error());
    result.submit = milliseconds(submitStart, submitEnd);
    result.vertices = renderer.vertexCount();
    const auto waitStart = Clock::now();
    gs_flush();
    bool disjoint = true; std::uint64_t frequency = 0, ticks = 0;
    require(gs_timer_range_get_data(r.range, &disjoint, &frequency), "GPU frequency query failed");
    require(gs_timer_get_data(r.timer, &ticks), "GPU timestamp query failed");
    const auto completed = Clock::now();
    result.wait = milliseconds(waitStart, completed);
    result.wall = milliseconds(wallStart, completed);
    result.gpu = (!disjoint && frequency) ? double(ticks) * 1000 / double(frequency) : -1;
    return result;
}
double percentile(std::vector<double> values, double q) {
    if (values.empty()) return -1;
    std::sort(values.begin(), values.end());
    const double index = q * (values.size() - 1);
    const auto lo = std::size_t(std::floor(index)), hi = std::size_t(std::ceil(index));
    return values[lo] + (values[hi] - values[lo]) * (index - double(lo));
}
void distribution(std::ostream &out, const std::vector<double> &values) {
    if (values.empty()) { out << "null"; return; }
    out << "{\"median\":" << percentile(values, .5) << ",\"p95\":" << percentile(values, .95)
        << ",\"min\":" << *std::min_element(values.begin(), values.end())
        << ",\"max\":" << *std::max_element(values.begin(), values.end()) << '}';
}
std::vector<double> readback(Resources &r, unsigned w, unsigned h) {
    auto *stage = gs_stagesurface_create(w, h, GS_RGBA16F);
    require(stage != nullptr, "Readback staging allocation failed");
    std::vector<std::uint8_t> copy(std::size_t(w) * h * 8);
    std::vector<double> samples;
    try {
        for (int i = 0; i < 12; ++i) {
            const auto begin = Clock::now();
            gs_stage_texture(stage, gs_texrender_get_texture(r.target));
            std::uint8_t *bytes = nullptr; std::uint32_t pitch = 0;
            require(gs_stagesurface_map(stage, &bytes, &pitch), "Readback map failed");
            for (unsigned y = 0; y < h; ++y)
                std::memcpy(copy.data() + std::size_t(y) * w * 8, bytes + std::size_t(y) * pitch, std::size_t(w) * 8);
            gs_stagesurface_unmap(stage);
            if (i >= 3) samples.push_back(milliseconds(begin, Clock::now()));
        }
    } catch (...) { gs_stagesurface_destroy(stage); throw; }
    gs_stagesurface_destroy(stage); return samples;
}
void runCase(std::ostream &out, const Options &opt, const Case &test, unsigned w, unsigned h) {
    Resources r; require(r.target && r.timer && r.range, "GPU query/target allocation failed");
    sf::Renderer renderer; require(renderer.initialize(), renderer.error());
    auto drawing = heart();
    const auto synthetic = test.synthetic ? stressFrame(drawing) : std::vector<sf::RenderParticle>{};
    std::vector<Sample> samples;
    for (int repetition = 0; repetition < opt.repetitions; ++repetition) {
        std::unique_ptr<sf::Simulation> simulation;
        if (!test.synthetic) {
            simulation = std::make_unique<sf::Simulation>(settings(test));
            if (test.symbols) require(simulation->submit(drawing), "Heart drawing was rejected");
            for (int i = 0; i < 24 * 60; ++i) simulation->tick(1.0/60);
        }
        for (int i = 0; i < opt.warmup + opt.samples; ++i) {
            auto sample = draw(renderer, simulation.get(), synthetic, r, w, h, repetition + 1);
            if (i >= opt.warmup) samples.push_back(sample);
        }
    }
    std::vector<double> sim, frame, submit, cpuTotal, gpu, wait, wall, particles, symbols, vertices;
    int disjoint = 0;
    for (const auto &s : samples) {
        if (s.simulation >= 0) { sim.push_back(s.simulation); frame.push_back(s.frame); cpuTotal.push_back(s.simulation+s.frame+s.submit); }
        submit.push_back(s.submit); wait.push_back(s.wait); wall.push_back(s.wall);
        if (s.gpu >= 0) gpu.push_back(s.gpu); else ++disjoint;
        particles.push_back(double(s.particles)); symbols.push_back(double(s.symbols)); vertices.push_back(double(s.vertices));
    }
    require(gpu.size() >= samples.size() * .95, "Too many invalid/disjoint GPU queries");
    const auto copies = readback(r, w, h);
    out << "{\"case\":" << quoted(test.name) << ",\"width\":" << w << ",\"height\":" << h
        << ",\"configured_density\":" << test.density << ",\"synthetic\":" << (test.synthetic ? "true" : "false")
        << ",\"sample_count\":" << samples.size() << ",\"gpu_disjoint_samples\":" << disjoint
        << ",\"particles_median\":" << percentile(particles,.5) << ",\"symbols_median\":" << percentile(symbols,.5)
        << ",\"vertices_median\":" << percentile(vertices,.5) << ",\"metrics_ms\":{";
    const std::pair<const char *,const std::vector<double> *> metrics[]{
        {"cpu_simulation",&sim},{"cpu_frame_projection",&frame},{"cpu_renderer_submit",&submit},
        {"cpu_total",&cpuTotal},{"gpu_timestamp_interval",&gpu},{"cpu_query_wait",&wait},
        {"synchronized_frame_wall",&wall},{"readback_stage_map_copy",&copies}};
    bool first = true;
    for (const auto &[name, values] : metrics) {
        if (!first) out << ','; first = false; out << quoted(name) << ':'; distribution(out,*values);
    }
    out << "},\"samples\":[";
    first = true;
    for (const auto &s : samples) {
        if (!first) out << ','; first = false;
        out << "{\"repeat\":" << s.repetition << ",\"cpu_simulation\":";
        if (s.simulation < 0) out << "null"; else out << s.simulation;
        out << ",\"cpu_frame_projection\":"; if (s.frame < 0) out << "null"; else out << s.frame;
        out << ",\"cpu_renderer_submit\":" << s.submit << ",\"gpu_timestamp_interval\":";
        if (s.gpu < 0) out << "null"; else out << s.gpu;
        out << ",\"cpu_query_wait\":" << s.wait << ",\"synchronized_frame_wall\":" << s.wall << '}';
    }
    out << "]}";
    std::cout << "BENCH " << test.name << ' ' << w << 'x' << h << " CPU submit median=" << percentile(submit,.5)
              << "ms GPU interval median=" << percentile(gpu,.5) << "ms\n";
}
} // namespace

int main(int argc, char **argv) {
    graphics_t *graphics = nullptr;
    try {
        const auto opt = options(argc,argv);
        const auto adapter = adapterInfo(opt.adapter);
        std::filesystem::current_path(opt.obsBin);
        const auto module = (opt.obsBin / "libobs-d3d11.dll").string();
        require(gs_create(&graphics,module.c_str(),unsigned(opt.adapter)) == GS_SUCCESS && graphics, "D3D11 device creation failed");
        gs_enter_context(graphics);
        try {
            std::ofstream out(opt.output); require(bool(out), "Cannot create result JSON");
            out.imbue(std::locale::classic()); out << std::fixed << std::setprecision(9);
            out << "{\"schema_version\":1,\"benchmark\":\"sternenflug-offscreen-v1\",\"plugin_version\":"
                << quoted(STERNENFLUG_VERSION) << ",\"gpu\":" << quoted(adapter.name)
#ifdef _MSC_FULL_VER
                << ",\"compiler_msc_full_ver\":" << _MSC_FULL_VER
#endif
                << ",\"vram_bytes\":" << adapter.vram
                << ",\"adapter_index\":" << opt.adapter << ",\"target_format\":\"RGBA16F\",\"seed\":1337"
                << ",\"simulation_step_seconds\":0.016666666667,\"simulation_priming_seconds\":24"
                << ",\"warmup_frames_per_repeat\":" << opt.warmup << ",\"repetitions\":" << opt.repetitions
                << ",\"frames_per_repeat\":" << opt.samples << ",\"readback_samples\":9,\"cases\":[";
            const Case cases[]{{"standard350",350},{"symbols350_35pct_heart97",350,true},
                               {"rain650",650,false,true},{"density2000_ui_max",2000},
                               {"stress4096_35pct_heart97",4096,true,false,true}};
            bool first = true;
            for (const auto &test : cases) for (const auto size : {std::pair{1920u,1080u},std::pair{3840u,2160u}}) {
                if (!first) out << ','; first = false; runCase(out,opt,test,size.first,size.second); out.flush();
            }
            out << "]}\n"; require(bool(out), "Result write failed");
        } catch (...) { gs_leave_context(); throw; }
        gs_leave_context(); gs_destroy(graphics); graphics = nullptr; return 0;
    } catch (const std::exception &error) {
        std::cerr << "BENCHMARK FAILED: " << error.what() << '\n';
        if (graphics) gs_destroy(graphics); return 1;
    }
}
