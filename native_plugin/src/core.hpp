#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Portable effect state. This module deliberately has no OBS, Qt or GPU dependency.
namespace sf {

enum class Param : std::size_t {
    Opacity, Density, Speed, StarSize, SymbolSize, SizeJitter,
    SymbolShare, TimeScatter, TrailLength, TrailDensity, TrailFalloff,
    ColorVariation, HueVariation, SatVariation, Brightness,
    UVX, UVY, DriftX, DriftY, EmissionDuration,
    HueA, SatA, ValA, HueB, SatB, ValB, Count
};
constexpr std::size_t parameterCount = static_cast<std::size_t>(Param::Count);

struct ParamDef {
    Param id;
    const char *key, *label, *group, *tooltip;
    double min, max, step, initial;
    bool animatable;
};
const std::vector<ParamDef> &parameterDefs();
const ParamDef &parameterDef(Param id);

struct KeyPoint { double time = 0.0, value = 0.0; };
// phase is measured in cycles; amount is measured in the target parameter's units.
// Modes: 0 off, 1 sine, 2 triangle, 3 saw, 4 pulse, 5 smooth random,
// 6 linear, 7 held random, 8 user curve. Curve times are normalized to 0..1.
struct Animation {
    int mode = 0;
    double amount = 0.0, period = 4.0, phase = 0.0;
    bool pingPong = false;
    std::vector<KeyPoint> points{{0.0, 0.0}, {0.5, 1.0}, {1.0, 0.0}};
};
struct Settings {
    std::array<double, parameterCount> values{};
    std::array<Animation, parameterCount> animations{};
    std::uint32_t seed = 1;
    int colorMode = 1; // 0: linear RGB interpolation; 1: shortest-path HSV.
    int motionMode = 0; // 0: perspective starflight; 1: continuous downward screen flow.
    bool emissionForever = false;
    Settings();
    double get(Param id) const;
    void set(Param id, double value);
};
Settings sanitizeSettings(const Settings &settings);
double animationValue(const Animation &animation, double timeSeconds,
                      std::uint32_t seed = 1);
double parameterValue(const Settings &settings, Param id, double timeSeconds);

struct Point { double x = 0.0, y = 0.0; };
struct Drawing {
    std::uint64_t id = 0;
    // Coordinates are normalized to a logical 16:9 drawing board.
    std::vector<std::vector<Point>> strokes;
};
struct Color { double r = 1.0, g = 1.0, b = 1.0; };
struct RenderParticle {
    Point center;                 // projected output coordinates, 0..1 is on screen
    double radiusPixels = 1.0;    // at a reference output height of 1080
    double fade = 1.0;            // includes live global opacity
    Color color;                 // linear RGB, includes live brightness
    Point trailEnd;
    double trailStrength = 0.0, trailFalloff = 1.5;
    // Null means a star. Each live particle owns its immutable drawing generation.
    std::shared_ptr<const Drawing> drawing;
    std::uint64_t id = 0;
};

class Simulation {
public:
    Simulation();
    explicit Simulation(const Settings &settings);
    ~Simulation();
    Simulation(Simulation &&) noexcept;
    Simulation &operator=(Simulation &&) noexcept;
    Simulation(const Simulation &) = delete;
    Simulation &operator=(const Simulation &) = delete;

    void setSettings(const Settings &settings);
    const Settings &settings() const;
    // Invalid/empty drawings are rejected without interrupting the active generation.
    bool submit(std::shared_ptr<const Drawing> drawing);
    void stopEmission();
    void setPaused(bool paused);
    bool paused() const;
    void restart();
    // Stops position/drift animation and eases the existing offset back to center.
    // Effect time drives the return; pause holds it. New controls overlay the return.
    void resetDrift();
    // Stops drift and any return at the current offset; leaves UV controls unchanged.
    void stopDrift();
    void tick(double seconds);
    std::vector<RenderParticle> frame() const;
    std::vector<RenderParticle> renderFrame(unsigned width, unsigned height) const;
    double time() const;
    bool emitting() const;
    std::size_t particleCount() const;
    static constexpr std::size_t maxParticles = 4096;
    static constexpr double driftResetDuration = 1.2;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sf
