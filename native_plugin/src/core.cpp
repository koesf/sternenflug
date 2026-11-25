#include "core.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace sf {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double fixedStep = 1.0 / 120.0;
constexpr double meanFlight = 12.0;
constexpr double farDepth = 64.0;
constexpr double nearDepth = 0.5;
constexpr std::size_t maxDrawingPoints = 4096;
constexpr std::size_t maxDrawingGenerations = 64;

double finiteOr(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}
double clamp(double value, double low, double high)
{
    return std::max(low, std::min(high, value));
}
double fract(double value) { return value - std::floor(value); }
double smooth(double x)
{
    x = clamp(x, 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}
std::uint64_t mix(std::uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
double random01(std::uint64_t id, std::uint32_t seed, std::uint64_t channel)
{
    const auto bits = mix(id ^ (std::uint64_t(seed) << 32) ^ mix(channel));
    return double(bits >> 11) * (1.0 / 9007199254740992.0);
}
double cyclicRandom(double phase, std::uint32_t seed, bool interpolate)
{
    const double location = fract(phase) * 8.0;
    const auto a = static_cast<unsigned>(std::floor(location));
    const double first = random01(a, seed, 441) * 2.0 - 1.0;
    if (!interpolate)
        return first;
    const double second = random01((a + 1) % 8, seed, 441) * 2.0 - 1.0;
    return first + (second - first) * smooth(fract(location));
}
std::vector<KeyPoint> cleanPoints(const std::vector<KeyPoint> &source)
{
    std::vector<KeyPoint> result;
    result.reserve(std::min<std::size_t>(source.size() + 2, 258));
    for (const auto &point : source) {
        if (result.size() >= 256)
            break;
        if (std::isfinite(point.time) && std::isfinite(point.value))
            result.push_back({clamp(point.time, 0.0, 1.0), clamp(point.value, -1.0, 1.0)});
    }
    std::stable_sort(result.begin(), result.end(), [](const KeyPoint &a, const KeyPoint &b) {
        return a.time < b.time;
    });
    std::vector<KeyPoint> unique;
    for (const auto &point : result) {
        if (!unique.empty() && std::abs(unique.back().time - point.time) < 1e-9)
            unique.back() = point;
        else
            unique.push_back(point);
    }
    if (unique.empty())
        unique = {{0.0, 0.0}, {1.0, 0.0}};
    if (unique.front().time > 0.0)
        unique.insert(unique.begin(), {0.0, unique.front().value});
    if (unique.back().time < 1.0)
        unique.push_back({1.0, unique.back().value});
    return unique;
}
double curveValue(const std::vector<KeyPoint> &source, double position)
{
    const auto points = cleanPoints(source);
    if (position <= points.front().time)
        return points.front().value;
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (position <= points[i].time) {
            const auto &a = points[i - 1];
            const auto &b = points[i];
            const double t = (position - a.time) / (b.time - a.time);
            return a.value + (b.value - a.value) * smooth(t);
        }
    }
    return points.back().value;
}
double srgbToLinear(double x)
{
    x = clamp(x, 0.0, 1.0);
    return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
}
Color hsv(double hue, double saturation, double value)
{
    hue = fract(hue / 360.0) * 6.0;
    saturation = clamp(saturation, 0.0, 1.0);
    value = clamp(value, 0.0, 1.0);
    const double chroma = value * saturation;
    const double x = chroma * (1.0 - std::abs(std::fmod(hue, 2.0) - 1.0));
    const double m = value - chroma;
    Color c;
    if (hue < 1.0) c = {chroma, x, 0.0};
    else if (hue < 2.0) c = {x, chroma, 0.0};
    else if (hue < 3.0) c = {0.0, chroma, x};
    else if (hue < 4.0) c = {0.0, x, chroma};
    else if (hue < 5.0) c = {x, 0.0, chroma};
    else c = {chroma, 0.0, x};
    return {srgbToLinear(c.r + m), srgbToLinear(c.g + m), srgbToLinear(c.b + m)};
}
Color lerp(Color a, Color b, double t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
            a.b + (b.b - a.b) * t};
}
} // namespace

const std::vector<ParamDef> &parameterDefs()
{
    static const std::vector<ParamDef> defs{
        {Param::Opacity, "opacity", "Deckkraft", "Ausgabe", "Deckkraft des gesamten freigestellten Effekts. Leere Bereiche bleiben transparent.", 0, 100, 1, 100, true},
        {Param::Density, "density", "Partikeldichte", "Ausgabe", "Ungefähre Anzahl gleichzeitig fliegender Partikel. Ändert die Geburt neuer Partikel ohne bestehende zu löschen.", 20, 2000, 1, 350, true},
        {Param::Speed, "speed", "Flugtempo", "Bewegung", "Geschwindigkeitsfaktor des gemeinsamen Sternenflugs. Änderungen bewegen vorhandene Partikel ohne Zeitsprung.", 0, 4, .01, 1, true},
        {Param::StarSize, "star_size", "Sterngröße", "Größe", "Stern-Durchmesser in Pixeln bei 1080p als Projektionsreferenz. Neue Partikel übernehmen diesen Wert.", .5, 12, .1, 2, true},
        {Param::SymbolSize, "symbol_size", "Symbolgröße", "Größe", "Symbol-Durchmesser in Pixeln bei 1080p als Projektionsreferenz. Vorhandene Symbole behalten ihre Größe.", 8, 4096, 1, 240, true},
        {Param::SizeJitter, "size_jitter", "Größenstreuung", "Größe", "Zufällige Größenabweichung je neuem Partikel: 35 Prozent bedeutet plus/minus 35 Prozent. Die Mindestgröße beträgt ein Prozent.", 0, 100, 1, 35, true},
        {Param::SymbolShare, "symbol_share", "Symbolanteil", "Symbole", "Anteil neuer Sterne, die während der Einmischung durch die gesendete Zeichnung ersetzt werden.", 0, 100, 1, 35, true},
        {Param::TimeScatter, "time_scatter", "Zeitliche Streuung", "Symbole", "Verteilt neue Partikel zeitlich unregelmäßiger. Die mittlere Dichte bleibt gleich.", 0, 100, 1, 80, true},
        {Param::TrailLength, "trail_length", "Schweiflänge", "Schweif", "Länge der schmalen Schweiflinie in Referenzpixeln bei 1080p.", 0, 160, 1, 80, true},
        {Param::TrailDensity, "trail_density", "Schweifanteil", "Schweif", "Anteil neuer Partikel mit Schweif. Die Entscheidung bleibt pro Partikel stabil.", 0, 100, 1, 35, true},
        {Param::TrailFalloff, "trail_falloff", "Schweif-Abfall", "Schweif", "Stärke des weichen Helligkeitsabfalls zur Schweifspitze.", .2, 6, .05, 1.5, true},
        {Param::ColorVariation, "color_variation", "Farbmischung", "Farben", "Verteilt die Partikelfarben zwischen Farbkreis A und B. Null verwendet ausschließlich A.", 0, 100, 1, 100, true},
        {Param::HueVariation, "hue_variation", "Farbtonstreuung", "Farben", "Zusätzliche Farbtonstreuung pro Partikel in Grad, gleichmäßig um die gewählte Farbe.", 0, 360, 1, 0, true},
        {Param::SatVariation, "sat_variation", "Sättigungsstreuung", "Farben", "Zusätzliche zufällige Sättigungsabweichung in Prozentpunkten.", 0, 100, 1, 0, true},
        {Param::Brightness, "brightness", "Helligkeit", "Farben", "Gemeinsame Helligkeit aller laufenden Sterne, Symbole und Schweife.", 0, 200, 1, 100, true},
        {Param::UVX, "uv_x", "Position X", "Bewegung", "Horizontale Verschiebung in Bildbreiten. Die Animation Linear läuft ohne Rücksprung weiter.", -2, 2, .01, 0, true},
        {Param::UVY, "uv_y", "Position Y", "Bewegung", "Vertikale Verschiebung in Bildhöhen. Die Animation Linear läuft ohne Rücksprung weiter.", -2, 2, .01, 0, true},
        {Param::DriftX, "drift_x", "Drift X", "Bewegung", "Horizontale Geschwindigkeit in Bildbreiten pro Sekunde. Positive Werte bewegen nach rechts; Pause friert die Bewegung ein.", -1, 1, .001, 0, true},
        {Param::DriftY, "drift_y", "Drift Y", "Bewegung", "Vertikale Geschwindigkeit in Bildhöhen pro Sekunde. Positive Werte bewegen nach unten; Pause friert die Bewegung ein.", -1, 1, .001, 0, true},
        {Param::EmissionDuration, "emission_duration", "Einmischdauer", "Symbole", "So lange werden ab dem Senden neue Symbole geboren. Bereits fliegende Symbole fliegen danach zu Ende.", .1, 600, .1, 8, false},
        {Param::HueA, "hue_a", "Farbton A", "Farben", "Farbkreis A: Farbton in Grad.", 0, 360, 1, 200, true},
        {Param::SatA, "sat_a", "Sättigung A", "Farben", "Farbkreis A: Sättigung in Prozent.", 0, 100, 1, 65, true},
        {Param::ValA, "val_a", "Helligkeit A", "Farben", "Farbkreis A: Helligkeit in Prozent.", 0, 100, 1, 100, true},
        {Param::HueB, "hue_b", "Farbton B", "Farben", "Farbkreis B: Farbton in Grad.", 0, 360, 1, 320, true},
        {Param::SatB, "sat_b", "Sättigung B", "Farben", "Farbkreis B: Sättigung in Prozent.", 0, 100, 1, 45, true},
        {Param::ValB, "val_b", "Helligkeit B", "Farben", "Farbkreis B: Helligkeit in Prozent.", 0, 100, 1, 100, true}
    };
    return defs;
}
const ParamDef &parameterDef(Param id)
{
    const auto index = static_cast<std::size_t>(id);
    return parameterDefs()[index < parameterCount ? index : 0];
}
Settings::Settings()
{
    for (const auto &def : parameterDefs())
        values[static_cast<std::size_t>(def.id)] = def.initial;
}
double Settings::get(Param id) const
{
    const auto i = static_cast<std::size_t>(id);
    return i < parameterCount ? values[i] : 0.0;
}
void Settings::set(Param id, double value)
{
    const auto i = static_cast<std::size_t>(id);
    if (i < parameterCount) {
        const auto &def = parameterDef(id);
        values[i] = clamp(finiteOr(value, def.initial), def.min, def.max);
    }
}
Settings sanitizeSettings(const Settings &source)
{
    Settings result = source;
    result.colorMode = source.colorMode == 0 ? 0 : 1;
    result.motionMode = source.motionMode == 1 ? 1 : 0;
    for (const auto &def : parameterDefs()) {
        const auto i = static_cast<std::size_t>(def.id);
        result.set(def.id, source.values[i]);
        auto &a = result.animations[i];
        a.mode = def.animatable && a.mode >= 0 && a.mode <= 8 ? a.mode : 0;
        a.amount = clamp(finiteOr(a.amount, 0.0), -(def.max - def.min), def.max - def.min);
        a.period = clamp(finiteOr(a.period, 4.0), .05, 3600.0);
        a.phase = fract(finiteOr(a.phase, 0.0));
        a.points = cleanPoints(a.points);
    }
    return result;
}
double animationValue(const Animation &a, double timeSeconds, std::uint32_t seed)
{
    const double period = clamp(finiteOr(a.period, 4.0), .05, 3600.0);
    const double cycles = finiteOr(timeSeconds, 0.0) / period + finiteOr(a.phase, 0.0);
    double phase = fract(cycles);
    if (a.pingPong)
        phase = 1.0 - std::abs(2.0 * phase - 1.0);
    switch (a.mode) {
    case 1: return std::sin(2.0 * pi * phase);
    case 2: return (2.0 / pi) * std::asin(std::sin(2.0 * pi * phase));
    case 3: return 2.0 * phase - 1.0;
    case 4: return phase < .5 ? 1.0 : -1.0;
    case 5: return cyclicRandom(phase, seed, true);
    case 6: return a.pingPong ? phase : cycles;
    case 7: return cyclicRandom(phase, seed, false);
    case 8: return curveValue(a.points, phase);
    default: return 0.0;
    }
}
double parameterValue(const Settings &settings, Param id, double timeSeconds)
{
    const auto index = static_cast<std::size_t>(id);
    if (index >= parameterCount)
        return 0.0;
    const auto &def = parameterDef(id);
    const auto &a = settings.animations[index];
    const double base = clamp(finiteOr(settings.values[index], def.initial), def.min, def.max);
    const double offset = def.animatable ? finiteOr(a.amount, 0.0) *
        animationValue(a, timeSeconds, settings.seed ^ static_cast<std::uint32_t>(index * 9176U)) : 0.0;
    if ((id == Param::UVX || id == Param::UVY) && a.mode == 6)
        return clamp(finiteOr(base + offset, base), -1000000.0, 1000000.0);
    return clamp(finiteOr(base + offset, base), def.min, def.max);
}

struct Simulation::Impl {
    struct Particle {
        std::uint64_t id;
        double born, lifetime, x, y, size, flowX, colorMix, hueJitter, satJitter;
        bool trail;
        std::shared_ptr<const Drawing> drawing;
    };
    Settings settings;
    std::vector<Particle> particles;
    std::shared_ptr<const Drawing> activeDrawing;
    std::vector<std::weak_ptr<const Drawing>> generations;
    std::uint64_t nextId = 1, nextDrawingId = 1;
    double clock = 0.0, motionClock = 0.0, pending = 0.0;
    double driftX = 0.0, driftY = 0.0, budget = 0.0, interval = 1.0;
    double returningX = 0.0, returningY = 0.0, returnStart = 0.0;
    double emissionStart = 0.0, emissionEnd = 0.0;
    bool isPaused = false, emissionStopped = true;

    explicit Impl(const Settings &s) : settings(sanitizeSettings(s)) { reset(); }
    double value(Param p, double t) const { return parameterValue(settings, p, t); }
    Point offset() const
    {
        const double weight = 1.0 - smooth((clock - returnStart) / Simulation::driftResetDuration);
        return {value(Param::UVX, clock) + driftX + returningX * weight,
                value(Param::UVY, clock) + driftY + returningY * weight};
    }
    bool emittingAt(double t) const
    {
        return !emissionStopped && activeDrawing &&
            (settings.emissionForever || t < emissionEnd);
    }
    void chooseInterval()
    {
        const double scatter = value(Param::TimeScatter, clock) / 100.0;
        // Bounded renewal jitter: no simultaneous burst and exactly one shared birth stream.
        interval = 1.0 + (random01(nextId, settings.seed, 3) * 2.0 - 1.0) * scatter * .95;
    }
    void spawn(bool prewarm = false, double ageFraction = 0.0)
    {
        const auto id = nextId++;
        if (particles.size() >= Simulation::maxParticles)
            return;
        const auto random = [&](std::uint64_t channel) {
            return random01(id, settings.seed, channel);
        };
        std::shared_ptr<const Drawing> drawing;
        if (!prewarm && emittingAt(clock)) {
            const double ramp = smooth((clock - emissionStart) / .35);
            if (random(8) < value(Param::SymbolShare, clock) * .01 * ramp)
                drawing = activeDrawing;
        }
        const double life = meanFlight * (.8 + .4 * random(4));
        const double angle = random(5) * 2.0 * pi;
        const double radius = .008 + .11 * std::sqrt(random(6));
        const double size = value(drawing ? Param::SymbolSize : Param::StarSize, clock) *
            std::max(.01, 1.0 + (random(7) * 2.0 - 1.0) * value(Param::SizeJitter, clock) * .01);
        particles.push_back({id, motionClock - clamp(ageFraction, 0.0, .999999) * life,
            life, std::cos(angle) * radius, std::sin(angle) * radius, size, random(13),
            random(9), random(10) * 2.0 - 1.0, random(11) * 2.0 - 1.0,
            random(12) < value(Param::TrailDensity, clock) * .01, std::move(drawing)});
    }
    void reset()
    {
        particles.clear();
        generations.clear();
        activeDrawing.reset();
        clock = motionClock = pending = driftX = driftY = budget = 0.0;
        returningX = returningY = returnStart = 0.0;
        emissionStart = emissionEnd = 0.0;
        emissionStopped = true;
        nextId = nextDrawingId = 1;
        const auto count = static_cast<std::size_t>(std::llround(settings.get(Param::Density)));
        particles.reserve(Simulation::maxParticles);
        for (std::size_t i = 0; i < count; ++i)
            spawn(true, (double(i) + random01(i, settings.seed, 21)) / double(count));
        chooseInterval();
    }
    void step()
    {
        const double middle = clock + fixedStep * .5;
        const double movement = value(Param::Speed, middle) * fixedStep;
        motionClock += movement;
        driftX += value(Param::DriftX, middle) * fixedStep;
        driftY += value(Param::DriftY, middle) * fixedStep;
        clock += fixedStep;
        particles.erase(std::remove_if(particles.begin(), particles.end(), [&](const Particle &p) {
            return motionClock - p.born >= p.lifetime;
        }), particles.end());
        budget += value(Param::Density, middle) / meanFlight * movement;
        while (budget >= interval) {
            budget -= interval;
            spawn();
            chooseInterval();
        }
        if (!emittingAt(clock) && activeDrawing) {
            activeDrawing.reset();
            emissionStopped = true;
        }
    }
};

Simulation::Simulation() : Simulation(Settings{}) {}
Simulation::Simulation(const Settings &s) : impl_(new Impl(s)) {}
Simulation::~Simulation() = default;
Simulation::Simulation(Simulation &&) noexcept = default;
Simulation &Simulation::operator=(Simulation &&) noexcept = default;
void Simulation::setSettings(const Settings &s) { impl_->settings = sanitizeSettings(s); }
const Settings &Simulation::settings() const { return impl_->settings; }
bool Simulation::submit(std::shared_ptr<const Drawing> drawing)
{
    if (!drawing)
        return false;
    auto copy = std::make_shared<Drawing>();
    copy->id = drawing->id ? drawing->id : impl_->nextDrawingId++;
    std::size_t count = 0;
    for (const auto &stroke : drawing->strokes) {
        if (copy->strokes.size() >= 128 || count >= maxDrawingPoints)
            break;
        std::vector<Point> clean;
        for (const auto &p : stroke) {
            if (count >= maxDrawingPoints)
                break;
            if (!std::isfinite(p.x) || !std::isfinite(p.y))
                continue;
            Point q{clamp(p.x, 0.0, 1.0), clamp(p.y, 0.0, 1.0)};
            if (!clean.empty() && std::hypot(q.x - clean.back().x, q.y - clean.back().y) < 1e-7)
                continue;
            clean.push_back(q);
            ++count;
        }
        if (clean.size() >= 2)
            copy->strokes.push_back(std::move(clean));
    }
    if (copy->strokes.empty())
        return false;
    auto &generations = impl_->generations;
    generations.erase(std::remove_if(generations.begin(), generations.end(),
        [](const std::weak_ptr<const Drawing> &p) { return p.expired(); }), generations.end());
    if (generations.size() >= maxDrawingGenerations)
        return false;
    generations.push_back(copy);
    impl_->activeDrawing = std::move(copy);
    impl_->emissionStart = impl_->clock;
    impl_->emissionEnd = impl_->clock + impl_->settings.get(Param::EmissionDuration);
    impl_->emissionStopped = false;
    return true;
}
void Simulation::stopEmission()
{
    impl_->emissionStopped = true;
    impl_->activeDrawing.reset();
}
void Simulation::setPaused(bool value) { impl_->isPaused = value; }
bool Simulation::paused() const { return impl_->isPaused; }
void Simulation::restart() { impl_->reset(); }
void Simulation::resetDrift()
{
    // Capture the complete currently visible offset, including an unfinished return.
    // Subsequent manual position/drift edits overlay this residual; unrelated edits
    // never cancel it. Repeated reset captures the interpolated position again.
    const auto current = impl_->offset();
    impl_->returningX = current.x;
    impl_->returningY = current.y;
    impl_->returnStart = impl_->clock;
    impl_->driftX = impl_->driftY = 0.0;
    for (const auto id : {Param::UVX, Param::UVY, Param::DriftX, Param::DriftY}) {
        const auto index = static_cast<std::size_t>(id);
        impl_->settings.values[index] = 0.0;
        impl_->settings.animations[index].mode = 0;
        impl_->settings.animations[index].amount = 0.0;
    }
}
void Simulation::stopDrift()
{
    const double weight = 1.0 - smooth((impl_->clock - impl_->returnStart) / driftResetDuration);
    impl_->driftX += impl_->returningX * weight;
    impl_->driftY += impl_->returningY * weight;
    impl_->returningX = impl_->returningY = 0.0;
    for (const auto id : {Param::DriftX, Param::DriftY}) {
        const auto index = static_cast<std::size_t>(id);
        impl_->settings.values[index] = 0.0;
        impl_->settings.animations[index].mode = 0;
        impl_->settings.animations[index].amount = 0.0;
    }
}
void Simulation::tick(double seconds)
{
    if (impl_->isPaused || !std::isfinite(seconds) || seconds <= 0.0)
        return;
    // A broken host timestamp cannot request billions of catch-up iterations.
    impl_->pending += std::min(seconds, 60.0);
    while (impl_->pending + 1e-12 >= fixedStep) {
        impl_->step();
        impl_->pending -= fixedStep;
    }
    impl_->pending = std::max(0.0, impl_->pending);
}
std::vector<RenderParticle> Simulation::frame() const { return renderFrame(1920, 1080); }
std::vector<RenderParticle> Simulation::renderFrame(unsigned width, unsigned height) const
{
    const auto &state = *impl_;
    const double aspect = double(std::max(1U, width)) / double(std::max(1U, height));
    const auto offset = state.offset();
    const double offsetX = offset.x;
    const double offsetY = offset.y;
    const double opacity = state.value(Param::Opacity, state.clock) * .01;
    const double brightness = state.value(Param::Brightness, state.clock) * .01;
    const double hueA = state.value(Param::HueA, state.clock);
    const double hueB = state.value(Param::HueB, state.clock);
    const double satA = state.value(Param::SatA, state.clock) * .01;
    const double satB = state.value(Param::SatB, state.clock) * .01;
    const double valA = state.value(Param::ValA, state.clock) * .01;
    const double valB = state.value(Param::ValB, state.clock) * .01;
    const double colorVariation = state.value(Param::ColorVariation, state.clock) * .01;
    const double hueVariation = state.value(Param::HueVariation, state.clock) * .5;
    const double satVariation = state.value(Param::SatVariation, state.clock) * .01;
    const double trailLength = state.value(Param::TrailLength, state.clock) / 1080.0;
    const double trailFalloff = state.value(Param::TrailFalloff, state.clock);
    const double currentSpeed = state.value(Param::Speed, state.clock);
    const double dx = state.value(Param::DriftX, state.clock);
    const double dy = state.value(Param::DriftY, state.clock);
    const bool flowing = state.settings.motionMode == 1;
    std::vector<RenderParticle> result;
    result.reserve(state.particles.size());
    for (const auto &p : state.particles) {
        const double progress = clamp((state.motionClock - p.born) / p.lifetime, 0.0, 1.0);
        const double depth = farDepth * std::pow(nearDepth / farDepth, progress);
        const double scale = farDepth / depth;
        const double fade = smooth(progress / .28) * smooth((1.0 - progress) / .04) * opacity;
        const double radius = flowing ? .5 * p.size :
            .5 * p.size * (p.drawing ? 4.0 / depth : .25 + 4.0 / depth);
        // The largest supported trail is 160px; 64px also contains contour glow.
        // Wrap only beyond the complete primitive, including very large drawings.
        const double marginY = (radius + 224.0) / 1080.0;
        const double marginX = marginY / aspect;
        Point center;
        if (flowing) {
            const double spanX = 1.0 + 2.0 * marginX;
            const double spanY = 1.0 + 2.0 * marginY;
            center.x = -marginX + fract(p.flowX + offsetX / spanX) * spanX;
            center.y = -marginY + fract(progress + offsetY / spanY) * spanY;
        } else {
            center = {.5 + p.x * scale / aspect + offsetX, .5 + p.y * scale + offsetY};
        }
        const double mixAmount = p.colorMix * colorVariation;
        Color color;
        if (state.settings.colorMode == 0) {
            color = lerp(hsv(hueA + p.hueJitter * hueVariation, satA + p.satJitter * satVariation, valA),
                         hsv(hueB + p.hueJitter * hueVariation, satB + p.satJitter * satVariation, valB), mixAmount);
        } else {
            // Achromatic endpoints inherit the hue of the chromatic endpoint.
            const double firstHue = satA < 1e-6 ? hueB : hueA;
            const double secondHue = satB < 1e-6 ? firstHue : hueB;
            const double distance = fract((secondHue - firstHue) / 360.0 + .5) * 360.0 - 180.0;
            color = hsv(firstHue + distance * mixAmount + p.hueJitter * hueVariation,
                satA + (satB - satA) * mixAmount + p.satJitter * satVariation,
                valA + (valB - valA) * mixAmount);
        }
        color.r *= brightness; color.g *= brightness; color.b *= brightness;
        double vx = flowing ? dx * aspect :
            p.x * scale * std::log(farDepth / nearDepth) / p.lifetime * currentSpeed + dx * aspect;
        double vy = flowing ? (1.0 + 2.0 * marginY) / p.lifetime * currentSpeed + dy :
            p.y * scale * std::log(farDepth / nearDepth) / p.lifetime * currentSpeed + dy;
        double norm = std::hypot(vx, vy);
        if (norm < 1e-12) {
            vx = flowing ? 0.0 : p.x;
            vy = flowing ? 1.0 : p.y;
            norm = std::hypot(vx, vy);
        }
        const Point end{center.x - trailLength * vx / norm / aspect,
                        center.y - trailLength * vy / norm};
        result.push_back({center, radius, fade, color, end, p.trail ? 1.0 : 0.0,
                          trailFalloff, p.drawing, p.id});
    }
    return result;
}
double Simulation::time() const { return impl_->clock; }
bool Simulation::emitting() const { return impl_->emittingAt(impl_->clock); }
std::size_t Simulation::particleCount() const { return impl_->particles.size(); }

} // namespace sf
