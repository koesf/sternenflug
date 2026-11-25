#include <obs.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr const char *sourceId = "sternenflug_native_source";
void require(bool condition, const std::string &message)
{
    if (!condition) throw std::runtime_error(message);
}
struct DataDeleter { void operator()(obs_data_t *p) const { if (p) obs_data_release(p); } };
using Data = std::unique_ptr<obs_data_t, DataDeleter>;
struct SourceDeleter {
    void operator()(obs_source_t *p) const
    {
        if (p) { obs_source_remove(p); obs_source_release(p); }
    }
};
using Source = std::unique_ptr<obs_source_t, SourceDeleter>;
struct Session {
    bool started = false;
    ~Session() { if (started) { obs_wait_for_destroy_queue(); obs_shutdown(); } }
};
#ifdef _WIN32
struct DllDirectory {
    DLL_DIRECTORY_COOKIE cookie = nullptr;
    explicit DllDirectory(const std::filesystem::path &path)
    {
        // libobs uses LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, which intentionally ignores PATH.
        require(SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) != 0,
                "could not establish the isolated process DLL search policy");
        cookie = AddDllDirectory(path.wstring().c_str());
        require(cookie != nullptr, "could not add the installed OBS runtime dependency directory");
    }
    ~DllDirectory() { if (cookie) RemoveDllDirectory(cookie); }
};
#endif
struct Graphics {
    Graphics() { obs_enter_graphics(); }
    ~Graphics() { obs_leave_graphics(); }
};
struct CallData {
    calldata_t data{};
    CallData() { calldata_init(&data); }
    ~CallData() { calldata_free(&data); }
};
struct Status { long long particles; bool emitting, paused; double time; };
Status status(obs_source_t *source)
{
    CallData args;
    require(proc_handler_call(obs_source_get_proc_handler(source), "get_status", &args.data),
            "get_status procedure is missing");
    return {calldata_int(&args.data, "particles"), calldata_bool(&args.data, "emitting"),
            calldata_bool(&args.data, "paused"), calldata_float(&args.data,"time")};
}
void call(obs_source_t *source, const char *procedure)
{
    CallData args;
    require(proc_handler_call(obs_source_get_proc_handler(source), procedure, &args.data),
            std::string("procedure failed: ") + procedure);
}
void update(obs_source_t *source, obs_data_t *patch)
{
    std::atomic<bool> complete{false};
    auto callback = [](void *opaque, calldata_t *) {
        static_cast<std::atomic<bool> *>(opaque)->store(true, std::memory_order_release);
    };
    auto *signals = obs_source_get_signal_handler(source);
    signal_handler_connect(signals, "update", callback, &complete);
    obs_source_update(source, patch);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    signal_handler_disconnect(signals, "update", callback, &complete);
    require(complete.load(std::memory_order_acquire), "video-thread source update timed out");
}
Data configuration(obs_source_t *source)
{
    Data settings(obs_source_get_settings(source));
    Data config(obs_data_create_from_json(obs_data_get_string(settings.get(), "configuration")));
    require(bool(config), "configuration JSON is invalid");
    return config;
}
double value(obs_source_t *source, const char *key)
{
    auto config = configuration(source);
    Data settings(obs_data_get_obj(config.get(), "settings"));
    Data values(obs_data_get_obj(settings.get(), "values"));
    return obs_data_get_double(values.get(), key);
}
void checkPersistence(obs_source_t *source, double opacity, unsigned width, unsigned height, int motionMode = 0)
{
    require(std::strcmp(obs_source_get_id(source), sourceId) == 0, "wrong registered source id");
    require(obs_source_get_type(source) == OBS_SOURCE_TYPE_INPUT, "source is not an OBS input");
    require(obs_source_get_width(source) == width && obs_source_get_height(source) == height,
            std::string("source dimensions did not persist for ") + obs_source_get_name(source) +
            ": got " + std::to_string(obs_source_get_width(source)) + "x" +
            std::to_string(obs_source_get_height(source)) + ", expected " +
            std::to_string(width) + "x" + std::to_string(height));
    Data saved(obs_save_source(source)); // Invokes the actual plugin save callback.
    require(bool(saved), "obs_save_source failed");
    require(std::abs(value(source, "opacity") - opacity) < 1e-9, "opacity did not persist");
    require(std::abs(value(source, "symbol_size") - 1024) < 1e-9, "symbol size did not persist");
    require(std::abs(value(source, "emission_duration") - 5.125) < 1e-9, "decimal duration did not persist");
    auto config = configuration(source);
    Data settings(obs_data_get_obj(config.get(), "settings"));
    require(obs_data_get_int(settings.get(), "seed") == 314159, "seed did not persist");
    require(obs_data_get_int(settings.get(), "motionMode") == motionMode, "motion mode did not persist");
    Data animations(obs_data_get_obj(settings.get(), "animations"));
    Data animation(obs_data_get_obj(animations.get(), "uv_x"));
    require(obs_data_get_int(animation.get(), "mode") == 1 &&
            std::abs(obs_data_get_double(animation.get(), "period") - 3.25) < 1e-9 &&
            obs_data_get_bool(animation.get(), "pingPong"), "animation settings did not persist");
    Data draft(obs_data_get_obj(config.get(), "draft"));
    require(std::strcmp(obs_data_get_string(draft.get(), "id"), "1234") == 0,
            "drawing identity did not persist");
    // libobs data arrays only represent objects, not JSON arrays of numeric pairs.
    // The plugin stores its complete Qt JSON as a string, so verify those pairs there.
    Data rawSettings(obs_source_get_settings(source));
    const std::string raw = obs_data_get_string(rawSettings.get(), "configuration");
    require(raw.find("\"strokes\":[[[0.1,0.2],[0.8,0.7],[0.2,0.8]]]") != std::string::npos,
            "drawing point coordinates did not persist");
}
std::string configJson(double opacity, double duration = 5.125, double size = 1024, int motionMode = 0)
{
    return std::string(R"({"schemaVersion":1,"settings":{"seed":314159,"colorMode":1,"motionMode":)") +
        std::to_string(motionMode) + R"(,"emissionForever":false,"values":{"opacity":)" +
        std::to_string(opacity) +
        R"(,"density":250,"speed":0.75,"symbol_size":)" + std::to_string(size) +
        R"(,"symbol_share":62,"emission_duration":)" + std::to_string(duration) +
        R"(,"hue_a":125},"animations":{"uv_x":{"mode":1,"amount":0.17,"period":3.25,"phase":0.125,"pingPong":true,"points":[[0,0],[0.5,1],[1,0]]}}},"draft":{"id":"1234","strokes":[[[0.1,0.2],[0.8,0.7],[0.2,0.8]]]}})";
}
Data makeSettings(double opacity, unsigned width, unsigned height, int motionMode = 0)
{
    Data settings(obs_data_create());
    obs_data_set_int(settings.get(), "width", width);
    obs_data_set_int(settings.get(), "height", height);
    const auto json = configJson(opacity, 5.125, 1024, motionMode);
    obs_data_set_string(settings.get(), "configuration", json.c_str());
    return settings;
}
struct Targets {
    gs_texrender_t *target = nullptr;
    gs_stagesurf_t *stage = nullptr;
    bool scene = false, begun = false, mapped = false;
    ~Targets()
    {
        if (mapped) gs_stagesurface_unmap(stage);
        if (begun) gs_texrender_end(target);
        if (scene) gs_end_scene();
        if (stage) gs_stagesurface_destroy(stage);
        if (target) gs_texrender_destroy(target);
    }
};
std::vector<float> capture(obs_source_t *source)
{
    constexpr unsigned width = 640, height = 360;
    Graphics graphics;
    Targets targets;
    targets.target = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
    targets.stage = gs_stagesurface_create(width, height, GS_RGBA32F);
    require(targets.target && targets.stage, "GPU capture targets could not be created");
    gs_begin_scene(); targets.scene = true;
    require(gs_texrender_begin(targets.target, width, height), "GPU capture could not begin");
    targets.begun = true;
    gs_matrix_identity();
    gs_ortho(0, float(obs_source_get_width(source)), 0, float(obs_source_get_height(source)), -1, 1);
    gs_set_cull_mode(GS_NEITHER); gs_enable_depth_test(false);
    vec4 clear{}; gs_clear(GS_CLEAR_COLOR, &clear, 1, 0);
    obs_source_video_render(source); // Actual dynamically loaded source callback and renderer.
    gs_texrender_end(targets.target); targets.begun = false;
    gs_end_scene(); targets.scene = false;
    gs_stage_texture(targets.stage, gs_texrender_get_texture(targets.target));
    uint8_t *bytes = nullptr;
    uint32_t pitch = 0;
    require(gs_stagesurface_map(targets.stage, &bytes, &pitch), "GPU capture readback failed");
    targets.mapped = true;
    std::vector<float> pixels(std::size_t(width) * height * 4);
    for (unsigned y = 0; y < height; ++y)
        std::memcpy(pixels.data() + std::size_t(y) * width * 4,
                    bytes + std::size_t(y) * pitch, std::size_t(width) * 4 * sizeof(float));
    for (float pixel : pixels)
        require(std::isfinite(pixel) && pixel >= -.00001f && pixel <= 1.0001f,
                "source produced nonfinite or out-of-range pixels");
    return pixels;
}
} // namespace

int main(int argc, char **argv)
{
    try {
        // This executable creates its own libobs context. It never connects to OBS Studio.
        const char *configuredBin = std::getenv("STERNENFLUG_OBS_BIN");
        const auto bin = std::filesystem::absolute(configuredBin && *configuredBin
            ? std::filesystem::path(configuredBin)
            : std::filesystem::path(R"(C:\Program Files (x86)\Steam\steamapps\common\OBS Studio\bin\64bit)"));
        std::filesystem::path binary;
        if (argc > 1) binary = std::filesystem::absolute(argv[1]);
        else {
            const char *profile = std::getenv("USERPROFILE");
            require(profile && *profile, "USERPROFILE is unavailable; pass the plugin DLL path as the first argument");
            const auto cache = std::filesystem::path(profile) / ".cache/sternenflug-native/build";
            binary = cache / "bin/RelWithDebInfo/sternenflug.dll";
            if (!std::filesystem::exists(binary))
                binary = cache / "package/sternenflug/bin/64bit/sternenflug.dll";
        }
        require(std::filesystem::exists(binary), "sternenflug.dll is missing; pass its path as the first argument");
        std::filesystem::current_path(bin);
#ifdef _WIN32
        DllDirectory runtimeDirectory(bin); // Stays active until after obs_shutdown.
#endif
        Session session;
        session.started = obs_startup("en-US", nullptr, nullptr);
        require(session.started, "isolated libobs startup failed");
        // Required libobs effects are read from the existing installation, never modified.
        const auto dataPath = (bin / "../../data/libobs").lexically_normal().string();
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
        obs_add_data_path(dataPath.c_str());
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        const auto driver = (bin / "libobs-d3d11.dll").string();
        obs_video_info video{};
        video.graphics_module = driver.c_str();
        video.fps_num = 60; video.fps_den = 1;
        video.base_width = video.output_width = 640;
        video.base_height = video.output_height = 360;
        video.output_format = VIDEO_FORMAT_RGBA;
        video.colorspace = VIDEO_CS_709; video.range = VIDEO_RANGE_FULL;
        video.scale_type = OBS_SCALE_BILINEAR;
        require(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "isolated D3D11 video initialization failed");

        obs_module_t *module = nullptr;
        const auto data = (binary.parent_path() / "../../data").lexically_normal().string();
        require(obs_open_module(&module, binary.string().c_str(), data.c_str()) == MODULE_SUCCESS && module,
                "actual sternenflug module could not be opened");
        require(obs_init_module(module), "actual sternenflug module initialization failed");
        obs_post_load_modules(); // Headless module post-load must safely skip all GUI integration.

        auto firstSettings = makeSettings(55, 1280, 720);
        auto secondSettings = makeSettings(82, 640, 360);
        Source first(obs_source_create(sourceId, "Sternenflug smoke A", firstSettings.get(), nullptr));
        Source second(obs_source_create(sourceId, "Sternenflug smoke B", secondSettings.get(), nullptr));
        require(first && second && first.get() != second.get(), "two distinct native source instances could not be created");
        checkPersistence(first.get(), 55, 1280, 720);
        checkPersistence(second.get(), 82, 640, 360);
        require(status(first.get()).particles > 0 && status(second.get()).particles > 0,
                "native source simulations have no particles");
        require(!status(first.get()).emitting && !status(second.get()).emitting,
                "source creation unexpectedly triggered drawings");
        call(first.get(), "trigger");
        require(status(first.get()).emitting && !status(second.get()).emitting,
                "trigger leaked between source instances");
        call(second.get(), "trigger");
        call(first.get(), "stop_emission");
        require(!status(first.get()).emitting && status(second.get()).emitting,
                "stop leaked between source instances");
        call(first.get(), "trigger");
        require(status(first.get()).emitting, "repeated trigger failed without a shader reload");
        const double durations[]{.125, .25, 1.125, 5.125};
        for (int i = 0; i < 20; ++i) {
            Data patch(obs_data_create());
            const auto changed = configJson(55, durations[i % 4], 8 + i * 200);
            obs_data_set_string(patch.get(), "configuration", changed.c_str());
            update(first.get(), patch.get());
            call(first.get(), "trigger");
            require(status(first.get()).emitting, "duration/size edit broke repeated drawing submission");
            call(first.get(), "stop_emission");
        }
        // obs_source_create retains the supplied obs_data object; later updates mutate it.
        // Restore from a fresh fixture rather than reusing that now-mutated object.
        auto restoreAfterStress = makeSettings(55, 1280, 720);
        update(first.get(), restoreAfterStress.get());
        call(first.get(), "trigger");

        // A drift reset changes only the movement targets. It must not restart
        // the source or clear the active symbol event, and must persist at once.
        const auto beforeDriftReset=status(first.get());
        require(beforeDriftReset.time>0,"source did not advance before drift reset");
        call(first.get(),"reset_drift");
        const auto afterDriftReset=status(first.get());
        require(afterDriftReset.time>=beforeDriftReset.time && afterDriftReset.emitting,
                "drift reset restarted time or stopped the symbol event");
        for(const char *key:{"uv_x","uv_y","drift_x","drift_y"})
            require(value(first.get(),key)==0,"drift reset did not persist neutral movement values");
        require(value(first.get(),"opacity")==55 && value(first.get(),"symbol_size")==1024,
                "drift reset changed unrelated effect values");
        auto resetConfig=configuration(first.get());
        Data resetSettings(obs_data_get_obj(resetConfig.get(),"settings"));
        Data resetAnimations(obs_data_get_obj(resetSettings.get(),"animations"));
        for(const char *key:{"uv_x","uv_y","drift_x","drift_y"}) {
            Data animation(obs_data_get_obj(resetAnimations.get(),key));
            require(obs_data_get_int(animation.get(),"mode")==0,"reset left an XY animation running");
        }
        call(first.get(),"reset_drift");
        require(status(first.get()).emitting,"repeated drift reset interrupted the active drawing");
        call(first.get(),"stop_drift");
        require(status(first.get()).emitting,"stopping drift interrupted the active drawing");

        const auto visible = capture(first.get());
        bool lit = false, transparent = false;
        for (std::size_t i = 3; i < visible.size(); i += 4) {
            lit = lit || visible[i] > .001f;
            transparent = transparent || visible[i] == 0.0f;
        }
        require(lit && transparent, "native source did not render visible particles over transparent areas");

        const auto beforeRain = status(first.get());
        auto rain = makeSettings(55, 1280, 720, 1);
        update(first.get(), rain.get());
        checkPersistence(first.get(), 55, 1280, 720, 1);
        require(status(first.get()).time >= beforeRain.time && status(first.get()).emitting,
                "switching to downward flow restarted time or interrupted the active drawing");
        const auto rainPixels = capture(first.get());
        require(std::any_of(rainPixels.begin(), rainPixels.end(), [](float v) { return v > .001f; }),
                "downward flow did not produce visible GPU output");

        auto zero = makeSettings(0, 960, 540);
        update(first.get(), zero.get());
        checkPersistence(first.get(), 0, 960, 540);
        checkPersistence(second.get(), 82, 640, 360);
        const auto empty = capture(first.get());
        require(std::all_of(empty.begin(), empty.end(), [](float v) { return v == 0.0f; }),
                "native source opacity zero does not produce transparent RGBA zero");
        auto restoreAfterZero = makeSettings(55, 1280, 720, 1);
        update(first.get(), restoreAfterZero.get());

        Source duplicate(obs_source_duplicate(first.get(), "Sternenflug smoke duplicate", false));
        require(duplicate && duplicate.get() != first.get(), "native source duplication failed");
        checkPersistence(duplicate.get(), 55, 1280, 720, 1);
        require(!status(duplicate.get()).emitting, "duplication copied transient emission state");
        Data saved(obs_save_source(first.get()));
        const std::string serialized = obs_data_get_json(saved.get());
        Data restoredData(obs_data_create_from_json(serialized.c_str()));
        require(bool(restoredData), "source scene JSON roundtrip failed");
        first.reset();
        obs_wait_for_destroy_queue();
        Source restored(obs_load_source(restoredData.get()));
        require(bool(restored), "native source could not be recreated from saved scene JSON");
        checkPersistence(restored.get(), 55, 1280, 720, 1);
        require(!status(restored.get()).emitting, "recreation resumed a transient trigger");
        call(restored.get(), "trigger");
        require(status(restored.get()).emitting, "recreated source could not trigger its persisted drawing");
        capture(restored.get());

        restored.reset(); duplicate.reset(); second.reset();
        obs_wait_for_destroy_queue();
        std::cout << "PASS plugin: actual module load/post-load; two isolated native sources; trigger/stop/retrigger; 20 duration/size edits; "
                     "soft drift reset without time/event restart; downward flow and motion-mode persistence; settings+animation+drawing persistence; duplication; delete/JSON-recreate; D3D11 rendering; "
                     "transparent opacity zero; clean source teardown.\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL plugin: " << error.what() << '\n';
        return 1;
    }
}
