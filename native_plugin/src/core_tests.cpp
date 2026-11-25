#include "core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {
void require(bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error(message);
}
bool near(double a, double b, double tolerance = 1e-10)
{
    return std::abs(a - b) <= tolerance;
}
std::shared_ptr<sf::Drawing> drawing(std::uint64_t id)
{
    auto d = std::make_shared<sf::Drawing>();
    d->id = id;
    d->strokes = {{{.2, .5}, {.5, .2}, {.8, .5}, {.5, .8}, {.2, .5}}};
    return d;
}
void sameFrame(const std::vector<sf::RenderParticle> &a,
               const std::vector<sf::RenderParticle> &b)
{
    require(a.size() == b.size(), "frame sizes differ");
    for (std::size_t i = 0; i < a.size(); ++i) {
        require(a[i].id == b[i].id, "particle identities differ");
        require(near(a[i].center.x, b[i].center.x), "x differs");
        require(near(a[i].center.y, b[i].center.y), "y differs");
        require(near(a[i].radiusPixels, b[i].radiusPixels), "radius differs");
        require(near(a[i].fade, b[i].fade), "fade differs");
        require(near(a[i].color.r, b[i].color.r), "red differs");
        require(near(a[i].color.g, b[i].color.g), "green differs");
        require(near(a[i].color.b, b[i].color.b), "blue differs");
    }
}
void deterministicAndFrameIndependent()
{
    sf::Settings settings;
    settings.seed = 928451;
    auto &animation = settings.animations[static_cast<std::size_t>(sf::Param::Speed)];
    animation.mode = 1; animation.amount = .7; animation.period = 2.7;
    settings.set(sf::Param::DriftX, .01);
    sf::Simulation fine(settings), coarse(settings), identical(settings);
    for (int i = 0; i < 600; ++i) fine.tick(1.0 / 60.0);
    for (int i = 0; i < 40; ++i) coarse.tick(.25);
    for (int i = 0; i < 100; ++i) identical.tick(.1);
    sameFrame(fine.frame(), coarse.frame());
    sameFrame(fine.frame(), identical.frame());
    require(near(fine.time(), 10.0), "effect clock does not match elapsed time");
    settings.seed++;
    sf::Simulation other(settings);
    require(!near(other.frame().front().center.x, sf::Simulation{}.frame().front().center.x),
            "seed should change the initial field");
}
void speedChangesAndPauseAreContinuous()
{
    sf::Simulation sim;
    sim.tick(2.0);
    const auto before = sim.frame();
    auto settings = sim.settings();
    settings.set(sf::Param::Speed, 3.0);
    sim.setSettings(settings);
    sameFrame(before, sim.frame());
    sim.setPaused(true);
    const double time = sim.time();
    sim.tick(30.0);
    require(near(sim.time(), time), "pause advanced the clock");
    sameFrame(before, sim.frame());
    sim.setPaused(false);
    sim.tick(1.0 / 120.0);
    require(sim.time() > time, "resume did not advance the clock");
    require(sim.frame().front().center.x != before.front().center.x, "resume did not move particles");
    settings.set(sf::Param::Speed, 0.0);
    sim.setSettings(settings);
    const auto stopped = sim.frame();
    for (int i = 0; i < 10; ++i) sim.tick(60.0);
    sameFrame(stopped, sim.frame());
    require(sim.particleCount() <= sf::Simulation::maxParticles, "zero speed leaked particles");
}
void shapesReplaceBirthsAndOldShapesFinish()
{
    sf::Settings s;
    s.set(sf::Param::SymbolShare, 100);
    s.set(sf::Param::EmissionDuration, 30);
    sf::Simulation symbols(s), stars(s);
    auto first = drawing(71);
    require(symbols.submit(first), "valid first shape rejected");
    // Submission takes an immutable copy, not the caller's mutable storage.
    first->strokes.clear();
    symbols.tick(2.0); stars.tick(2.0);
    auto firstFrame = symbols.frame();
    const auto starFrame = stars.frame();
    require(firstFrame.size() == starFrame.size(), "symbol drawing doubled the emission rate");
    std::unordered_map<std::uint64_t, double> oldSizes;
    std::weak_ptr<const sf::Drawing> oldGeneration;
    for (std::size_t i = 0; i < firstFrame.size(); ++i) {
        require(firstFrame[i].id == starFrame[i].id, "symbol submission disturbed birth identities");
        require(near(firstFrame[i].center.x, starFrame[i].center.x), "symbols use a separate trajectory stream");
        if (firstFrame[i].drawing) {
            require(!firstFrame[i].drawing->strokes.empty(), "caller mutation altered a live generation");
            oldSizes[firstFrame[i].id] = firstFrame[i].radiusPixels;
            oldGeneration = firstFrame[i].drawing;
        }
    }
    require(!oldSizes.empty(), "no first-generation symbols were born");
    s.set(sf::Param::SymbolSize, 2000);
    symbols.setSettings(s);
    for (const auto &p : symbols.frame()) {
        const auto previous = oldSizes.find(p.id);
        if (previous != oldSizes.end())
            require(near(p.radiusPixels, previous->second), "size edit resized a live symbol");
    }
    require(symbols.submit(drawing(72)), "valid second shape rejected");
    symbols.tick(2.0);
    bool oldFound = false, newFound = false;
    for (const auto &p : symbols.frame()) {
        if (p.drawing && p.drawing->id == 71) oldFound = true;
        if (p.drawing && p.drawing->id == 72) newFound = true;
    }
    require(oldFound && newFound, "new submission deleted old flying shapes");
    const auto stopFrame = symbols.frame();
    std::uint64_t lastId = 0;
    for (const auto &p : stopFrame) lastId = std::max(lastId, p.id);
    symbols.stopEmission();
    require(!symbols.emitting(), "stop did not stop future emission");
    symbols.tick(2.0);
    oldFound = false;
    for (const auto &p : symbols.frame()) {
        if (p.id > lastId) require(!p.drawing, "a symbol was born after stop");
        if (p.drawing && p.drawing->id == 71) oldFound = true;
    }
    require(oldFound, "stop erased live symbols");
    firstFrame.clear();
    // stopFrame still intentionally owns old drawings; only test release after that scope.
    symbols.tick(20.0);
    for (const auto &p : symbols.frame()) require(!p.drawing, "old shape failed to finish");
}
void durationAndInvalidValues()
{
    sf::Settings s;
    s.values[static_cast<std::size_t>(sf::Param::EmissionDuration)] = std::numeric_limits<double>::quiet_NaN();
    s.values[static_cast<std::size_t>(sf::Param::Speed)] = std::numeric_limits<double>::infinity();
    s.animations[0].period = 0;
    sf::Simulation sim(s);
    require(std::isfinite(sim.settings().get(sf::Param::EmissionDuration)), "NaN duration survived sanitization");
    require(sim.settings().animations[0].period > 0, "zero animation period survived sanitization");
    require(!sim.submit(std::make_shared<sf::Drawing>()), "empty drawing accepted");
    s = sim.settings();
    s.set(sf::Param::EmissionDuration, .1);
    s.set(sf::Param::SymbolShare, 100);
    sim.setSettings(s);
    require(sim.submit(drawing(2)), "short valid duration rejected");
    sim.tick(.2);
    require(!sim.emitting(), "finite duration did not end");
    s.set(sf::Param::EmissionDuration, 5.125);
    sim.setSettings(s);
    require(sim.submit(drawing(3)), "decimal duration prevented retriggering");
    sim.tick(.5);
    require(sim.emitting(), "decimal-duration event stopped too early");
    s.set(sf::Param::EmissionDuration, .1);
    sim.setSettings(s);
    sim.tick(.5);
    require(sim.emitting(), "duration edit changed an already running event");
    sim.tick(std::numeric_limits<double>::infinity());
    sim.tick(-1.0);
    require(std::isfinite(sim.time()), "invalid host delta poisoned the clock");
}
void curvesAndPresets()
{
    sf::Animation a;
    a.period = 2.5;
    for (int mode = 1; mode <= 8; ++mode) {
        if (mode == 6) continue;
        a.mode = mode;
        require(near(sf::animationValue(a, .217, 42), sf::animationValue(a, 2.717, 42)),
                "preset does not repeat after its period");
    }
    a.mode = 8; a.period = 2; a.pingPong = true;
    a.points = {{1, 1}, {0, -1}, {.5, 0}};
    require(near(sf::animationValue(a, 0), -1), "custom curve start is wrong");
    require(near(sf::animationValue(a, 1), 1), "ping-pong curve end is wrong");
    require(near(sf::animationValue(a, 2), -1), "ping-pong curve did not return");
    a.pingPong = false;
    a.points = {{0, 0}, {.5, 1}, {1, 0}};
    require(near(sf::animationValue(a, .5), .5), "custom curve interpolation is wrong");
    require(near(sf::animationValue(a, 1), 1), "custom curve key point is wrong");
    a.mode = 6; a.amount = 1;
    sf::Settings s;
    s.animations[static_cast<std::size_t>(sf::Param::UVX)] = a;
    require(near(sf::parameterValue(s, sf::Param::UVX, 10), 5), "linear UV movement clamped or looped");
    require(near(s.get(sf::Param::UVX), 0), "animation overwrote the base parameter");
}
void independentInstancesAndLiveOutput()
{
    sf::Simulation a, b;
    const auto before = b.frame();
    a.submit(drawing(100));
    a.tick(3);
    sameFrame(before, b.frame());
    auto s = b.settings();
    s.set(sf::Param::Opacity, 0);
    b.setSettings(s);
    for (const auto &p : b.frame()) require(p.fade == 0, "opacity zero did not remove every particle");
    s.set(sf::Param::Opacity, 50);
    b.setSettings(s);
    const auto half = b.frame();
    for (std::size_t i = 0; i < half.size(); ++i)
        require(near(half[i].fade, before[i].fade * .5), "live opacity is not linear");
    b.restart();
    require(near(b.time(), 0), "restart did not reset time");
    require(!b.emitting(), "restart retained emission");
}
void drawingGenerationsAreReleased()
{
    sf::Settings s;
    s.set(sf::Param::SymbolShare, 100);
    sf::Simulation sim(s);
    sim.submit(drawing(1));
    sim.tick(1);
    std::weak_ptr<const sf::Drawing> weak;
    {
        const auto frame = sim.frame();
        for (const auto &p : frame) if (p.drawing) weak = p.drawing;
    }
    require(!weak.expired(), "test did not create a drawing generation");
    sim.stopEmission();
    sim.tick(20);
    require(weak.expired(), "finished drawing generation retained memory");
}
void jitterPercentIsTheActualDeviation()
{
    sf::Settings plain, scattered;
    plain.set(sf::Param::SizeJitter, 0);
    scattered.set(sf::Param::SizeJitter, 100);
    const auto baseline = sf::Simulation(plain).frame();
    const auto full = sf::Simulation(scattered).frame();
    bool belowQuarter = false, aboveOldMaximum = false;
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        const double factor = full[i].radiusPixels / baseline[i].radiusPixels;
        require(factor >= .01 - 1e-12 && factor <= 2.0, "100% jitter exceeds documented limits");
        belowQuarter = belowQuarter || factor < .25;
        aboveOldMaximum = aboveOldMaximum || factor > 1.75;
    }
    require(belowQuarter && aboveOldMaximum, "jitter still scales the percent by an undocumented factor");
    scattered.set(sf::Param::SizeJitter, 35);
    const auto partial = sf::Simulation(scattered).frame();
    for (std::size_t i = 0; i < baseline.size(); ++i) {
        const double factor = partial[i].radiusPixels / baseline[i].radiusPixels;
        require(factor >= .65 && factor <= 1.35, "35% jitter is not plus/minus 35%");
    }
}
void driftResetRecentersWithoutRestarting()
{
    sf::Settings drifting;
    drifting.seed = 7654321;
    drifting.set(sf::Param::SymbolShare, 100);
    drifting.set(sf::Param::EmissionDuration, 5.125);
    drifting.set(sf::Param::SymbolSize, 400);
    drifting.set(sf::Param::SizeJitter, 70);
    drifting.set(sf::Param::Speed, 1.25);
    drifting.set(sf::Param::UVX, .2);
    drifting.set(sf::Param::UVY, -.15);
    drifting.set(sf::Param::DriftX, .03);
    drifting.set(sf::Param::DriftY, -.01);
    const sf::Param resetTargets[]{sf::Param::UVX, sf::Param::UVY, sf::Param::DriftX, sf::Param::DriftY};
    const int modes[]{1, 2, 6, 5};
    for (std::size_t i = 0; i < 4; ++i) {
        auto &a = drifting.animations[static_cast<std::size_t>(resetTargets[i])];
        a.mode = modes[i]; a.amount = i < 2 ? .2 : .007;
        a.period = 2.7; a.phase = .13 + double(i) * .01;
        a.pingPong = i == 1;
        a.points = {{0, -.2}, {.4, 1}, {1, -.2}};
    }
    auto &unrelated = drifting.animations[static_cast<std::size_t>(sf::Param::Opacity)];
    unrelated.mode = 1; unrelated.amount = 20; unrelated.period = 3; unrelated.phase = .2;
    sf::Settings centered = drifting;
    for (const auto target : resetTargets) {
        centered.set(target, 0);
        auto &a = centered.animations[static_cast<std::size_t>(target)];
        a.mode = 0; a.amount = 0;
    }
    sf::Simulation moving(drifting), reference(centered);
    require(moving.submit(drawing(301)) && reference.submit(drawing(301)), "drift test drawing rejected");
    moving.tick(2.503); reference.tick(2.503); // Includes a fixed-step remainder that reset must retain.
    const auto before = moving.frame();
    require(!near(before.front().center.x, reference.frame().front().center.x), "drift reset test never accumulated an offset");
    const sf::Point capturedOffset{before.front().center.x - reference.frame().front().center.x,
                                   before.front().center.y - reference.frame().front().center.y};
    const auto original = moving.settings();
    const double originalTime = moving.time();
    const auto originalCount = moving.particleCount();
    moving.setPaused(true); reference.setPaused(true);
    moving.resetDrift();
    require(moving.paused() && moving.emitting(), "drift reset changed pause or emission state");
    require(near(moving.time(), originalTime) && moving.particleCount() == originalCount,
            "drift reset restarted time or particles");
    sameFrame(before, moving.frame()); // Reset starts at the visible position, without a jump.
    const auto after = moving.frame();
    bool retainedSymbol = false;
    for (std::size_t i = 0; i < before.size(); ++i) {
        require(before[i].id == after[i].id && near(before[i].radiusPixels, after[i].radiusPixels),
                "drift reset replaced or resized live particles");
        require(before[i].drawing.get() == after[i].drawing.get(), "drift reset changed a live drawing generation");
        retainedSymbol = retainedSymbol || bool(after[i].drawing);
    }
    require(retainedSymbol, "drift reset test did not retain a flying symbol");
    for (std::size_t i = 0; i < sf::parameterCount; ++i) {
        const auto id = static_cast<sf::Param>(i);
        const bool reset = id == sf::Param::UVX || id == sf::Param::UVY ||
                           id == sf::Param::DriftX || id == sf::Param::DriftY;
        const auto &a = moving.settings().animations[i];
        const auto &b = original.animations[i];
        require(moving.settings().values[i] == (reset ? 0 : original.values[i]),
                "drift reset changed an unrelated parameter or retained an offset");
        require(a.mode == (reset ? 0 : b.mode) && a.amount == (reset ? 0 : b.amount),
                "drift reset disabled the wrong animation or left drift animation active");
        require(a.period == b.period && a.phase == b.phase && a.pingPong == b.pingPong && a.points.size() == b.points.size(),
                "drift reset changed animation timing or curve data");
        for (std::size_t p = 0; p < a.points.size(); ++p)
            require(a.points[p].time == b.points[p].time && a.points[p].value == b.points[p].value,
                    "drift reset changed curve points");
    }
    require(moving.settings().seed == original.seed && moving.settings().colorMode == original.colorMode &&
            moving.settings().emissionForever == original.emissionForever, "drift reset changed global settings");
    moving.tick(3);
    require(near(moving.time(), originalTime), "paused drift reset resumed the effect");
    sameFrame(before, moving.frame());
    moving.setPaused(false); reference.setPaused(false);
    moving.tick(.6); reference.tick(.6);
    const auto half = moving.frame();
    const auto halfReference = reference.frame();
    require(near(half.front().center.x - halfReference.front().center.x, capturedOffset.x * .5) &&
            near(half.front().center.y - halfReference.front().center.y, capturedOffset.y * .5),
            "smooth drift reset is not halfway home after 0.6 seconds");
    moving.setPaused(true); reference.setPaused(true);
    moving.tick(5);
    sameFrame(half, moving.frame());
    moving.setPaused(false); reference.setPaused(false);
    moving.resetDrift(); // A second click must restart from the interpolated offset.
    sameFrame(half, moving.frame());
    auto changed = moving.settings();
    auto changedReference = reference.settings();
    changed.set(sf::Param::Brightness, 127);
    changedReference.set(sf::Param::Brightness, 127);
    moving.setSettings(changed); reference.setSettings(changedReference);
    sf::Point previous{capturedOffset.x * .5, capturedOffset.y * .5};
    for (int step = 1; step <= 12; ++step) {
        moving.tick(.1); reference.tick(.1);
        const auto currentFrame = moving.frame();
        const auto referenceFrame = reference.frame();
        const sf::Point offset{currentFrame.front().center.x - referenceFrame.front().center.x,
                               currentFrame.front().center.y - referenceFrame.front().center.y};
        require(std::abs(offset.x) <= std::abs(previous.x) + 1e-10 &&
                std::abs(offset.y) <= std::abs(previous.y) + 1e-10,
                "smooth drift reset moved away from center");
        if (step == 6)
            require(near(offset.x, capturedOffset.x * .25) && near(offset.y, capturedOffset.y * .25),
                    "repeated reset or unrelated settings edit interrupted the easing");
        previous = offset;
    }
    sameFrame(moving.frame(), reference.frame());
    require(moving.emitting(), "drift reset ended the active emission early");
    moving.tick(.9); reference.tick(.9);
    sameFrame(moving.frame(), reference.frame());
    require(!moving.emitting() && !reference.emitting(), "drift reset restarted the emission window");

    sf::Settings offsetSettings;
    offsetSettings.set(sf::Param::UVX, .4);
    sf::Simulation overlaid(offsetSettings), overlaidReference;
    overlaid.resetDrift();
    overlaid.tick(.3); overlaidReference.tick(.3);
    const auto beforeNewControl = overlaid.frame();
    auto newControl = overlaid.settings();
    newControl.set(sf::Param::DriftX, .1);
    auto newControlReference = overlaidReference.settings();
    newControlReference.set(sf::Param::DriftX, .1);
    overlaid.setSettings(newControl); overlaidReference.setSettings(newControlReference);
    sameFrame(beforeNewControl, overlaid.frame());
    overlaid.tick(.9); overlaidReference.tick(.9);
    sameFrame(overlaid.frame(), overlaidReference.frame());
}
void stopDriftHoldsTheCurrentReturnPosition()
{
    sf::Settings drifting;
    drifting.set(sf::Param::UVX, .4);
    drifting.set(sf::Param::UVY, -.2);
    drifting.set(sf::Param::DriftX, .03);
    drifting.set(sf::Param::DriftY, -.02);
    drifting.set(sf::Param::SymbolShare, 100);
    sf::Settings centered = drifting;
    for (const auto id : {sf::Param::UVX, sf::Param::UVY, sf::Param::DriftX, sf::Param::DriftY})
        centered.set(id, 0);
    sf::Simulation moving(drifting), reference(centered);
    require(moving.submit(drawing(401)) && reference.submit(drawing(401)), "stop drift test drawing rejected");
    moving.tick(1); reference.tick(1);
    moving.resetDrift();
    moving.tick(.3); reference.tick(.3);
    // New UV controls remain authoritative, even while an earlier return is active.
    auto changes = moving.settings();
    changes.set(sf::Param::UVX, .11); changes.set(sf::Param::UVY, -.06);
    changes.set(sf::Param::DriftX, .08); changes.set(sf::Param::DriftY, -.02);
    auto &driftAnimation = changes.animations[static_cast<std::size_t>(sf::Param::DriftX)];
    driftAnimation.mode = 2; driftAnimation.amount = .02;
    moving.setSettings(changes);
    moving.setPaused(true); reference.setPaused(true);
    const auto before = moving.frame();
    const auto referenceBefore = reference.frame();
    const sf::Point offset{before.front().center.x - referenceBefore.front().center.x,
                           before.front().center.y - referenceBefore.front().center.y};
    const auto original = moving.settings();
    const double originalTime = moving.time();
    moving.stopDrift();
    sameFrame(before, moving.frame());
    require(moving.paused() && moving.emitting() && near(moving.time(), originalTime),
            "stop drift changed the clock, pause, or emission state");
    const auto after = moving.frame();
    bool symbol = false;
    for (std::size_t i = 0; i < before.size(); ++i) {
        require(before[i].drawing.get() == after[i].drawing.get(), "stop drift replaced a live drawing");
        symbol = symbol || bool(after[i].drawing);
    }
    require(symbol, "stop drift test did not create live symbols");
    for (std::size_t i = 0; i < sf::parameterCount; ++i) {
        const auto id = static_cast<sf::Param>(i);
        const bool stopped = id == sf::Param::DriftX || id == sf::Param::DriftY;
        const auto &a = moving.settings().animations[i];
        const auto &b = original.animations[i];
        require(moving.settings().values[i] == (stopped ? 0 : original.values[i]) &&
                a.mode == (stopped ? 0 : b.mode) && a.amount == (stopped ? 0 : b.amount) &&
                a.period == b.period && a.phase == b.phase,
                "stop drift changed UV/unrelated controls or retained drift velocity");
    }
    moving.tick(2);
    sameFrame(before, moving.frame());
    moving.setPaused(false); reference.setPaused(false);
    moving.tick(1.5); reference.tick(1.5);
    const auto held = moving.frame();
    const auto heldReference = reference.frame();
    require(held.size() == heldReference.size(), "stop drift altered particle births");
    for (std::size_t i = 0; i < held.size(); ++i) {
        require(held[i].id == heldReference[i].id &&
                near(held[i].center.x - heldReference[i].center.x, offset.x) &&
                near(held[i].center.y - heldReference[i].center.y, offset.y),
                "stop drift allowed the return or drift velocity to continue");
    }

    sf::Settings animatedUV;
    auto &uv = animatedUV.animations[static_cast<std::size_t>(sf::Param::UVX)];
    uv.mode = 1; uv.amount = .15; uv.period = 1.5; uv.phase = .33;
    sf::Simulation uvStopped(animatedUV), uvReference(animatedUV);
    uvStopped.tick(.4); uvReference.tick(.4);
    uvStopped.stopDrift();
    uvStopped.tick(.7); uvReference.tick(.7);
    sameFrame(uvStopped.frame(), uvReference.frame());
}
void downwardFlowIsContinuousAndReplenished()
{
    sf::Settings rain;
    rain.motionMode = 1;
    rain.set(sf::Param::Speed, 2.4);
    rain.set(sf::Param::Density, 650);
    rain.set(sf::Param::StarSize, 1.5);
    rain.set(sf::Param::TrailLength, 80);
    rain.set(sf::Param::TrailDensity, 100);
    sf::Simulation sim(rain);
    const auto before = sim.frame();
    std::unordered_map<std::uint64_t, sf::RenderParticle> previous;
    int bins[10]{};
    for (const auto &p : before) {
        previous.emplace(p.id, p);
        if (p.center.x >= 0 && p.center.x < 1)
            ++bins[static_cast<int>(p.center.x * 10)];
    }
    for (int count : bins) require(count > 5, "downward flow does not cover the complete width");
    sim.tick(1.0 / 60.0);
    int checked = 0;
    for (const auto &p : sim.frame()) {
        const auto found = previous.find(p.id);
        if (found == previous.end()) continue;
        const auto &old = found->second;
        if (old.center.y > .1 && old.center.y < .8) {
            require(p.center.y > old.center.y, "rain particle does not move downwards");
            require(near(p.center.x, old.center.x), "rain retains radial horizontal expansion");
            require(near(p.radiusPixels, old.radiusPixels), "rain changes particle size during the flight");
            require(p.trailEnd.y < p.center.y && near(p.trailEnd.x, p.center.x),
                    "rain trail does not point upwards");
            ++checked;
        }
    }
    require(checked > 20, "not enough visible rain trajectories were checked");
    const auto speedBefore = sim.frame();
    auto changed = sim.settings(); changed.set(sf::Param::Speed, .5);
    sim.setSettings(changed);
    sameFrame(speedBefore, sim.frame());
    const double time = sim.time();
    sim.setPaused(true); sim.tick(30);
    require(near(sim.time(), time), "rain pause advanced time");
    sameFrame(speedBefore, sim.frame());

    sf::Simulation fine(rain), coarse(rain);
    for (int i = 0; i < 600; ++i) fine.tick(1.0 / 60.0);
    for (int i = 0; i < 40; ++i) coarse.tick(.25);
    sameFrame(fine.frame(), coarse.frame());

    rain.set(sf::Param::DriftX, .7); rain.set(sf::Param::DriftY, .8);
    sf::Simulation endless(rain);
    for (int second = 0; second < 120; ++second) {
        endless.tick(1);
        std::size_t visible = 0;
        for (const auto &p : endless.frame())
            if (p.center.x >= 0 && p.center.x <= 1 && p.center.y >= 0 && p.center.y <= 1 && p.fade > .01)
                ++visible;
        require(visible > 40, "long-running drift emptied the rain field");
        require(endless.particleCount() <= sf::Simulation::maxParticles, "rain exceeded its particle budget");
    }
    std::size_t wraps = 0;
    for (int frame = 0; frame < 180; ++frame) {
        previous.clear();
        for (const auto &p : endless.frame()) previous.emplace(p.id, p);
        endless.tick(1.0 / 60.0);
        for (const auto &p : endless.frame()) {
            const auto found = previous.find(p.id);
            if (found == previous.end()) continue;
            const auto &old = found->second;
            if (p.center.x - old.center.x < -.5) {
                const double radiusX = p.radiusPixels / 1920.0;
                require(old.center.x > 1 + radiusX && p.center.x < -radiusX,
                        "horizontal rain wrap teleported a visible particle");
                ++wraps;
            }
            if (p.center.y - old.center.y < -.5) {
                const double radiusY = p.radiusPixels / 1080.0;
                require(old.center.y > 1 + radiusY && p.center.y < -radiusY,
                        "vertical rain wrap teleported a visible particle");
                ++wraps;
            }
        }
    }
    require(wraps > 0, "rain wrapping was not exercised");
}
void downwardFlowPreservesDrawingGenerationsAndLargeShapes()
{
    sf::Settings s;
    s.motionMode = 1; s.emissionForever = true;
    s.set(sf::Param::SymbolShare, 100);
    s.set(sf::Param::SymbolSize, 4096);
    s.set(sf::Param::SizeJitter, 100);
    sf::Simulation sim(s);
    require(sim.submit(drawing(501)), "large rain drawing rejected");
    sim.tick(7);
    std::unordered_map<std::uint64_t, sf::RenderParticle> old;
    bool large = false;
    for (const auto &p : sim.frame()) {
        if (p.drawing) old.emplace(p.id, p);
        large = large || (p.drawing && p.radiusPixels > 1024);
    }
    require(large && !old.empty(), "large drawing did not enter the downward flow");
    require(sim.submit(drawing(502)), "second rain drawing rejected");
    sim.tick(1);
    bool retained = false, replacement = false;
    for (const auto &p : sim.frame()) {
        const auto found = old.find(p.id);
        if (found != old.end()) {
            require(p.drawing.get() == found->second.drawing.get() &&
                    near(p.radiusPixels, found->second.radiusPixels) &&
                    near(p.center.x, found->second.center.x),
                    "new rain drawing replaced a live form, size or lane");
            retained = true;
        }
        replacement = replacement || (p.drawing && p.drawing->id == 502);
    }
    require(retained && replacement, "rain does not mix old and new drawing generations");
    sameFrame(sim.renderFrame(1920, 1080), sim.renderFrame(3840, 2160));
    for (const auto &p : sim.renderFrame(1080, 1920))
        require(std::isfinite(p.center.x) && std::isfinite(p.center.y) && std::isfinite(p.radiusPixels),
                "large rain shape has invalid portrait projection");
    sim.stopEmission(); sim.tick(20);
    for (const auto &p : sim.frame()) require(!p.drawing, "old rain drawing generation never retired");
    s.motionMode = 99;
    require(sf::sanitizeSettings(s).motionMode == 0, "invalid motion mode was not sanitized");
}
} // namespace

int main()
{
    try {
        deterministicAndFrameIndependent();
        speedChangesAndPauseAreContinuous();
        shapesReplaceBirthsAndOldShapesFinish();
        durationAndInvalidValues();
        curvesAndPresets();
        independentInstancesAndLiveOutput();
        drawingGenerationsAreReleased();
        jitterPercentIsTheActualDeviation();
        driftResetRecentersWithoutRestarting();
        stopDriftHoldsTheCurrentReturnPosition();
        downwardFlowIsContinuousAndReplenished();
        downwardFlowPreservesDrawingGenerationsAndLargeShapes();
        std::cout << "Sternenflug core: 12 test groups passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        std::cerr << "Sternenflug core test failed: " << e.what() << '\n';
        return EXIT_FAILURE;
    }
}
