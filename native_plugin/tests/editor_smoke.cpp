#include "codec.hpp"
#include "core.hpp"
#include "editor.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QDir>
#include <QFont>
#include <QFile>
#include <QInputDialog>
#include <QKeyEvent>
#include <QListWidget>
#include <QMouseEvent>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QLabel>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

#include <cassert>
#include <cmath>
#include <iostream>
#include <memory>

namespace {
void mouse(QWidget *widget, QEvent::Type type, QPointF point,
           Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent event(type, point, QPointF(widget->mapToGlobal(point.toPoint())), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
void click(QWidget *widget, QPointF point, Qt::MouseButton button = Qt::LeftButton)
{
    mouse(widget, QEvent::MouseButtonPress, point, button, button);
    mouse(widget, QEvent::MouseButtonRelease, point, button, Qt::NoButton);
}
void key(QWidget *widget, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, code, modifiers); QApplication::sendEvent(widget, &event);
}
QPushButton *buttonNamed(QWidget *parent, const QString &name)
{
    for (auto *button : parent->findChildren<QPushButton *>()) if (button->text() == name) return button;
    return nullptr;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setFont(QFont(QStringLiteral("Segoe UI"), 9));
    QTemporaryDir preferences; assert(preferences.isValid());
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, preferences.path());
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("SternenflugTests"));
    QCoreApplication::setApplicationName(QStringLiteral("SternenflugEditorSmoke"));
    int settingChanges = 0, draftChanges = 0, sends = 0, pauseChanges = 0, stops = 0, restarts = 0;
    int driftStops = 0, driftResets = 0, motionApplies = 0;
    sf::Settings latest;
    std::shared_ptr<sf::Drawing> latestDraft;
    sf::EditorCallbacks callbacks;
    callbacks.settingsChanged = [&](const sf::Settings &value) { latest = value; ++settingChanges; };
    callbacks.draftChanged = [&](std::shared_ptr<sf::Drawing> value) { latestDraft = std::move(value); ++draftChanges; };
    bool acceptDrawing = true;
    callbacks.submitDrawing = [&](std::shared_ptr<sf::Drawing> value) { assert(value && !value->strokes.empty()); if (!acceptDrawing) return false; ++sends; return true; };
    callbacks.pauseChanged = [&](bool) { ++pauseChanges; };
    callbacks.stopEmission = [&] { ++stops; };
    callbacks.restart = [&] { ++restarts; };
    callbacks.stopDrift = [&](const sf::Settings &value) { latest = value; ++driftStops; };
    callbacks.resetDrift = [&](const sf::Settings &value) { latest = value; ++driftResets; };
    callbacks.applyMotionPreset = [&](const sf::Settings &value) { latest = value; ++motionApplies; };
    std::unique_ptr<QWidget> editor(sf::createEditor(sf::Settings{}, sf::Drawing{}, callbacks));
    editor->setAttribute(Qt::WA_DontShowOnScreen, true);
    editor->resize(340, 700); editor->show(); app.processEvents();
    assert(settingChanges == 0 && draftChanges == 0 && sends == 0);
    auto *tabs = editor->findChild<QTabWidget *>(QStringLiteral("editorTabs")); assert(tabs && tabs->count() == 5);
    if (argc > 2 && QString::fromLocal8Bit(argv[2]) == QStringLiteral("--screenshots-only")) {
        const auto output = QString::fromLocal8Bit(argv[1]); QDir().mkpath(output);
        sf::Drawing heart; heart.strokes.emplace_back();
        constexpr double pi = 3.14159265358979323846;
        for (int i = 0; i <= 128; ++i) {
            const double t = 2 * pi * i / 128.;
            const double x = 16 * std::pow(std::sin(t), 3);
            const double y = 13 * std::cos(t) - 5 * std::cos(2 * t) - 2 * std::cos(3 * t) - std::cos(4 * t);
            heart.strokes.front().push_back({.5 + x * .023, .45 - y * .023});
        }
        sf::Settings preview;
        preview.motionMode = 1; preview.set(sf::Param::Speed, 2.4); preview.set(sf::Param::Density, 650);
        preview.set(sf::Param::TrailDensity, 100); preview.set(sf::Param::TrailLength, 80);
        preview.set(sf::Param::DriftX, .008); preview.set(sf::Param::DriftY, -.006);
        preview.animations[0].mode = 8; preview.animations[0].amount = 35; preview.animations[0].period = 6;
        preview.animations[0].points = {{0, 0}, {.2, .8}, {.55, -.4}, {1, 0}};
        sf::updateEditorSettings(editor.get(), preview, heart);
        sf::updateEditorStatus(editor.get(), QStringLiteral("Quelle: Sternenflug · 350 Partikel · Bereit zum Einmischen"));
        auto capture = [&](int page, int width, int height, const QString &name) {
            editor->resize(width, height); tabs->setCurrentIndex(page);
            app.processEvents(); app.processEvents();
            auto *scroll = qobject_cast<QScrollArea *>(tabs->currentWidget()); assert(scroll);
            if (page == 1 && name.contains(QStringLiteral("Drift"))) {
                auto *drift = editor->findChild<QWidget *>(QStringLiteral("driftSection")); assert(drift);
                scroll->verticalScrollBar()->setValue(drift->mapTo(scroll->widget(), QPoint()).y());
                app.processEvents();
            } else scroll->verticalScrollBar()->setValue(0);
            if (scroll->widget()->width() > scroll->viewport()->width() + 1)
                std::cerr << "horizontal overflow on page " << page << ": " << scroll->widget()->width() << " > " << scroll->viewport()->width() << '\n';
            assert(scroll->widget()->width() <= scroll->viewport()->width() + 1);
            assert(editor->grab().save(output + QChar('/') + name));
        };
        editor->findChild<QAction *>(QStringLiteral("theme_2"))->trigger();
        capture(0, 340, 760, QStringLiteral("Sternenflug-Farbe-Dunkel-340.png"));
        capture(1, 340, 760, QStringLiteral("Sternenflug-Flug-Dunkel-340.png"));
        capture(1, 340, 760, QStringLiteral("Sternenflug-Bewegungsvorlagen-Dunkel-340.png"));
        capture(1, 340, 760, QStringLiteral("Sternenflug-Drift-Dunkel-340.png"));
        capture(2, 340, 760, QStringLiteral("Sternenflug-Groesse-Dunkel-340.png"));
        capture(3, 340, 900, QStringLiteral("Sternenflug-Zeichnen-Dunkel-340.png"));
        capture(4, 340, 900, QStringLiteral("Sternenflug-Zeit-Dunkel-340.png"));
        editor->findChild<QAction *>(QStringLiteral("theme_1"))->trigger();
        app.processEvents();
        for (auto *scroll : editor->findChildren<QScrollArea *>()) {
            assert(scroll->widget()->palette().color(QPalette::Window).lightness() > 220);
            assert(scroll->viewport()->palette().color(QPalette::Window).lightness() > 220);
        }
        assert(editor->findChild<QWidget *>(QStringLiteral("drawingCanvas"))->palette().color(QPalette::Base).lightness() > 220);
        auto *lightBody = qobject_cast<QScrollArea *>(tabs->currentWidget())->widget();
        assert(lightBody->grab().toImage().pixelColor(2, 2).lightness() > 200);
        capture(3, 520, 850, QStringLiteral("Sternenflug-Zeichnen.png"));
        capture(4, 520, 850, QStringLiteral("Sternenflug-Zeit.png"));
        capture(0, 520, 850, QStringLiteral("Sternenflug-Farbe.png"));
        capture(1, 520, 850, QStringLiteral("Sternenflug-Drift-Hell.png"));
        capture(1, 520, 850, QStringLiteral("Sternenflug-Bewegungsvorlagen-Hell.png"));
        editor->findChild<QAction *>(QStringLiteral("theme_2"))->trigger();
        editor->findChild<QAction *>(QStringLiteral("scale_125"))->trigger();
        capture(3, 425, 950, QStringLiteral("Sternenflug-Zeichnen-125.png"));
        capture(4, 425, 950, QStringLiteral("Sternenflug-Zeit-125.png"));
        capture(1, 425, 950, QStringLiteral("Sternenflug-Drift-125.png"));
        std::cout << "native Windows font screenshots: all pages fit 340px; 125 percent scale fits 425px\n";
        return 0;
    }
    for (const auto &def : sf::parameterDefs()) {
        if (def.id == sf::Param::DriftX || def.id == sf::Param::DriftY) continue; // Joint direction/tempo UI.
        const auto name = QStringLiteral("value_") + QString::fromUtf8(def.key);
        auto *value = editor->findChild<QDoubleSpinBox *>(name); assert(value);
        assert(!value->toolTip().isEmpty());
        assert(std::abs(value->minimum() - def.min) < .0001 && std::abs(value->maximum() - def.max) < .0001);
    }
    // Programmatic synchronization, target switches, resize and status are not
    // human changes and must never restart or mutate an effect.
    sf::Settings loaded; loaded.set(sf::Param::SymbolSize, 1024);
    sf::updateEditorSettings(editor.get(), loaded, sf::Drawing{});
    sf::updateEditorStatus(editor.get(), QStringLiteral("Testquelle · läuft"));
    sf::updateEditorPlayback(editor.get(), true);
    assert(settingChanges == 0 && draftChanges == 0 && pauseChanges == 0);
    editor->resize(800, 900); app.processEvents();

    // Themes are local: the OBS/application palette is untouched.
    const auto appPalette = QApplication::palette();
    auto *dark = editor->findChild<QAction *>(QStringLiteral("theme_2")); assert(dark); dark->trigger();
    assert(editor->palette().color(QPalette::Window).lightness() < 60);
    auto *light = editor->findChild<QAction *>(QStringLiteral("theme_1")); assert(light); light->trigger();
    assert(editor->palette().color(QPalette::Window).lightness() > 220);
    for (auto *scroll : editor->findChildren<QScrollArea *>()) {
        assert(scroll->widget()->palette().color(QPalette::Window).lightness() > 220);
        assert(scroll->viewport()->palette().color(QPalette::Window).lightness() > 220);
    }
    assert(editor->findChild<QWidget *>(QStringLiteral("drawingCanvas"))->palette().color(QPalette::Base).lightness() > 220);
    auto *lightBody = qobject_cast<QScrollArea *>(tabs->currentWidget())->widget();
    assert(lightBody->grab().toImage().pixelColor(2, 2).lightness() > 200);
    assert(QApplication::palette() == appPalette && settingChanges == 0);
    auto *scale = editor->findChild<QAction *>(QStringLiteral("scale_125")); assert(scale); scale->trigger();
    assert(editor->minimumWidth() >= 425);
    editor->findChild<QAction *>(QStringLiteral("scale_100"))->trigger();

    // The actual HSV wheel changes color by keyboard as well as pointer input.
    auto *wheel = editor->findChild<QWidget *>(QStringLiteral("hsvColorWheel")); assert(wheel);
    key(wheel, Qt::Key_Right); assert(settingChanges == 1);
    assert(std::abs(latest.get(sf::Param::HueA) - 202) < .01);

    tabs->setCurrentIndex(3); app.processEvents();
    auto *canvas = editor->findChild<QWidget *>(QStringLiteral("drawingCanvas")); assert(canvas);
    const QPointF middle(canvas->width() * .5, canvas->height() * .5);
    mouse(canvas, QEvent::MouseButtonPress, middle - QPointF(45, 20), Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, middle + QPointF(10, -35), Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, middle + QPointF(45, 20), Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, middle + QPointF(45, 20), Qt::LeftButton, Qt::NoButton);
    assert(draftChanges == 1 && latestDraft && latestDraft->strokes.size() == 1);
    const auto firstPoint = latestDraft->strokes.front().front();
    assert(firstPoint.x >= 0 && firstPoint.x <= 1 && firstPoint.y >= 0 && firstPoint.y <= 1);
    click(canvas, middle, Qt::RightButton); click(canvas, middle, Qt::RightButton);
    assert(sends == 2); // Repeated triggering must not depend on shader reload.
    acceptDrawing = false; click(canvas, middle, Qt::RightButton); assert(sends == 2);
    acceptDrawing = true;
    key(canvas, Qt::Key_Z, Qt::ControlModifier); assert(latestDraft->strokes.empty());
    key(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); assert(latestDraft->strokes.size() == 1);
    auto savedDraft = *latestDraft;
    tabs->setCurrentIndex(0); tabs->setCurrentIndex(3);
    click(canvas, middle, Qt::RightButton); assert(sends == 3); // Tab switches preserve draft.

    // The large canvas is a modeless window and is the very same widget, so
    // neither drawing nor undo history can disappear when it closes.
    auto *expand = editor->findChild<QToolButton *>(QStringLiteral("expandDrawing")); assert(expand); expand->click(); app.processEvents();
    assert(canvas->window() != editor.get());
    auto *largeWindow = qobject_cast<QDialog *>(canvas->window()); assert(largeWindow && !largeWindow->isModal());
    largeWindow->close(); app.processEvents();
    assert(canvas->window() == editor.get());
    key(canvas, Qt::Key_Z, Qt::ControlModifier); assert(latestDraft->strokes.empty());
    key(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); assert(latestDraft->strokes.size() == 1);

    auto *pause = editor->findChild<QToolButton *>(QStringLiteral("pausePlayback")); assert(pause);
    pause->click(); assert(pauseChanges == 1);
    editor->findChild<QToolButton *>(QStringLiteral("stopEmission"))->click(); assert(stops == 1);
    editor->findChild<QToolButton *>(QStringLiteral("restartPlayback"))->click(); assert(restarts == 1);

    // A real editable curve: add, drag, delete and undo a node. Targets retain
    // their independent animations when a different parameter is selected.
    tabs->setCurrentIndex(4); app.processEvents();
    auto *target = editor->findChild<QComboBox *>(QStringLiteral("animationTarget"));
    auto *mode = editor->findChild<QComboBox *>(QStringLiteral("animationMode")); assert(target && mode);
    target->setCurrentIndex(target->findData(static_cast<int>(sf::Param::Opacity))); mode->setCurrentIndex(mode->findData(8));
    auto *curve = editor->findChild<QWidget *>(QStringLiteral("curveCanvas")); assert(curve); app.processEvents();
    auto graphPoint = [curve](double t, double v) { return QPointF(33 + t * (curve->width() - 48), 14 + (1. - v) * .5 * (curve->height() - 48)); };
    const auto initialCount = latest.animations[0].points.size();
    const QPointF newNode = graphPoint(.24, .35);
    mouse(curve, QEvent::MouseButtonDblClick, newNode, Qt::LeftButton, Qt::LeftButton);
    mouse(curve, QEvent::MouseButtonRelease, newNode, Qt::LeftButton, Qt::NoButton);
    assert(latest.animations[0].points.size() == initialCount + 1);
    mouse(curve, QEvent::MouseButtonPress, newNode, Qt::LeftButton, Qt::LeftButton);
    mouse(curve, QEvent::MouseMove, graphPoint(.30, -.25), Qt::NoButton, Qt::LeftButton);
    mouse(curve, QEvent::MouseButtonRelease, graphPoint(.30, -.25), Qt::LeftButton, Qt::NoButton);
    assert(std::abs(latest.animations[0].points[1].time - .30) < .01);
    key(curve, Qt::Key_Delete); assert(latest.animations[0].points.size() == initialCount);
    key(curve, Qt::Key_Z, Qt::ControlModifier); assert(latest.animations[0].points.size() == initialCount + 1);
    target->setCurrentIndex(target->findData(static_cast<int>(sf::Param::UVX))); mode->setCurrentIndex(mode->findData(6));
    assert(latest.animations[0].mode == 8 && latest.animations[static_cast<int>(sf::Param::UVX)].mode == 6);

    // Drift uses one physical direction/tempo control without changing the
    // persisted UV-axis units. Programmatic hotkey synchronization touches only
    // those four axes and preserves drawing undo history and unrelated state.
    auto *pad = editor->findChild<QWidget *>(QStringLiteral("driftDirectionPad"));
    auto *driftTempo = editor->findChild<QDoubleSpinBox *>(QStringLiteral("driftSpeedPercent"));
    auto *driftSlider = editor->findChild<QSlider *>(QStringLiteral("driftSpeedSlider"));
    auto *fine = editor->findChild<QToolButton *>(QStringLiteral("driftFineStep"));
    auto *stopDrift = editor->findChild<QPushButton *>(QStringLiteral("stopDrift"));
    auto *resetDrift = editor->findChild<QPushButton *>(QStringLiteral("resetDrift"));
    auto *driftAnimationHint = editor->findChild<QLabel *>(QStringLiteral("driftAnimationHint"));
    assert(pad && driftTempo && driftSlider && fine && stopDrift && resetDrift && driftAnimationHint);
    const auto axes = {sf::Param::UVX, sf::Param::UVY, sf::Param::DriftX, sf::Param::DriftY};
    auto assertUnchangedExcept = [](const sf::Settings &before, const sf::Settings &after,
                                   std::initializer_list<sf::Param> allowed, bool allowMotionMode = false) {
        auto a = sf::encodeSettings(before), b = sf::encodeSettings(after);
        if (allowMotionMode) { a.remove(QStringLiteral("motionMode")); b.remove(QStringLiteral("motionMode")); }
        auto av = a["values"].toObject(), bv = b["values"].toObject();
        auto aa = a["animations"].toObject(), ba = b["animations"].toObject();
        for (auto id : allowed) {
            const auto key = QString::fromUtf8(sf::parameterDef(id).key);
            av.remove(key); bv.remove(key); aa.remove(key); ba.remove(key);
        }
        a["values"] = av; b["values"] = bv; a["animations"] = aa; b["animations"] = ba;
        assert(a == b);
    };
    const auto beforeDriftLoad = latest;
    auto high = latest;
    high.set(sf::Param::DriftX, 1); high.set(sf::Param::DriftY, -1);
    high.set(sf::Param::UVX, .25); high.set(sf::Param::UVY, -.5);
    high.animations[static_cast<int>(sf::Param::DriftX)].mode = 1;
    high.animations[static_cast<int>(sf::Param::DriftY)].mode = 7;
    high.animations[static_cast<int>(sf::Param::UVX)].mode = 2;
    high.set(sf::Param::Opacity, 17); high.set(sf::Param::SymbolSize, 999); high.seed = 333;
    const int changesBeforeDriftLoad = settingChanges, draftsBeforeDriftLoad = draftChanges;
    sf::updateEditorDrift(editor.get(), high);
    assert(settingChanges == changesBeforeDriftLoad && draftChanges == draftsBeforeDriftLoad);
    assert(std::abs(driftTempo->value() - 141.42) < .01 && !driftAnimationHint->isHidden());
    assert(mode->currentData().toInt() == 2); // Active UVX target also synchronized.
    auto *brightness = editor->findChild<QDoubleSpinBox *>(QStringLiteral("value_brightness")); assert(brightness);
    brightness->setValue(101); // Reveals the internal settings without touching drift.
    assert(latest.get(sf::Param::DriftX) == 1 && latest.get(sf::Param::DriftY) == -1);
    assert(latest.get(sf::Param::SymbolSize) == beforeDriftLoad.get(sf::Param::SymbolSize));
    assert(latest.get(sf::Param::Opacity) == beforeDriftLoad.get(sf::Param::Opacity) && latest.seed == beforeDriftLoad.seed);
    sf::Settings persistedHigh; std::shared_ptr<sf::Drawing> noDrawing; QString driftError;
    assert(sf::decodePreset(sf::encodePreset(latest), persistedHigh, noDrawing, &driftError));
    assert(persistedHigh.get(sf::Param::DriftX) == 1 && persistedHigh.get(sf::Param::DriftY) == -1);
    const auto beforeStopDrift = latest;
    const int changesBeforeStopDrift = settingChanges, sendsBeforeDrift = sends, pauseBeforeDrift = pauseChanges;
    const int stopsBeforeDrift = stops, restartsBeforeDrift = restarts;
    stopDrift->click();
    assert(driftStops == 1 && driftResets == 0 && settingChanges == changesBeforeStopDrift);
    assert(latest.get(sf::Param::DriftX) == 0 && latest.get(sf::Param::DriftY) == 0);
    assert(latest.animations[static_cast<int>(sf::Param::DriftX)].mode == 0 && latest.animations[static_cast<int>(sf::Param::DriftY)].mode == 0);
    assertUnchangedExcept(beforeStopDrift, latest, {sf::Param::DriftX, sf::Param::DriftY});
    assert(driftAnimationHint->isHidden());
    key(pad, Qt::Key_Left); // A stopped direction edit starts gently at1%/s.
    assert(std::abs(latest.get(sf::Param::DriftX) + .01) < 1.e-12 && std::abs(latest.get(sf::Param::DriftY)) < 1.e-12);
    assert(std::abs(driftTempo->value() - 1) < .001);
    driftTempo->setValue(.01);
    assert(std::abs(latest.get(sf::Param::DriftX) + .0001) < 1.e-12);
    const int changesBeforeFine = settingChanges;
    fine->click(); assert(std::abs(driftTempo->singleStep() - .1) < 1.e-12);
    fine->click(); assert(std::abs(driftTempo->singleStep() - .01) < 1.e-12 && settingChanges == changesBeforeFine);
    driftTempo->setValue(1);
    click(pad, QPointF(pad->width() * .5 + 40, pad->height() * .5 - 40));
    assert(latest.get(sf::Param::DriftX) > 0 && latest.get(sf::Param::DriftY) < 0);
    assert(std::abs(std::hypot(latest.get(sf::Param::DriftX), latest.get(sf::Param::DriftY)) - .01) < 1.e-12);
    driftSlider->setValue(100);
    const double slowPercent = std::hypot(latest.get(sf::Param::DriftX), latest.get(sf::Param::DriftY)) * 100;
    assert(slowPercent > 0 && slowPercent < .2); // Cubic low-speed range, not a raw huge linear slider.
    key(pad, Qt::Key_Right, Qt::ShiftModifier);
    assert(std::hypot(latest.get(sf::Param::DriftX), latest.get(sf::Param::DriftY)) > 0);

    auto beforeReset = latest;
    beforeReset.set(sf::Param::UVX, .7); beforeReset.set(sf::Param::UVY, -.4);
    beforeReset.set(sf::Param::DriftX, .02); beforeReset.set(sf::Param::DriftY, .03);
    for (auto id : axes) beforeReset.animations[static_cast<int>(id)].mode = 3;
    sf::updateEditorDrift(editor.get(), beforeReset);
    brightness->setValue(102); beforeReset = latest;
    const int changesBeforeResetDrift = settingChanges;
    resetDrift->click();
    assert(driftResets == 1 && driftStops == 1 && settingChanges == changesBeforeResetDrift);
    for (auto id : axes) { assert(latest.get(id) == 0); assert(latest.animations[static_cast<int>(id)].mode == 0); }
    assertUnchangedExcept(beforeReset, latest, axes);
    assert(editor->findChild<QLabel *>(QStringLiteral("driftActionHint"))->text().contains(QStringLiteral("sanft")));
    assert(sends == sendsBeforeDrift && pauseChanges == pauseBeforeDrift && stops == stopsBeforeDrift && restarts == restartsBeforeDrift);
    assert(draftChanges == draftsBeforeDriftLoad); // No reset, synchronization or stop touched the draft.
    key(canvas, Qt::Key_Z, Qt::ControlModifier); assert(latestDraft->strokes.empty());
    key(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); assert(!latestDraft->strokes.empty());
    const auto driftSnapshot = sf::encodeSettings(latest); const int beforeThemeDrift = settingChanges;
    dark->trigger(); light->trigger();
    assert(sf::encodeSettings(latest) == driftSnapshot && settingChanges == beforeThemeDrift);

    // Movement templates are explicit, scoped operations. Selecting a name is
    // inert; applying it affects only movement/trails and their animations.
    // Colors, all sizes, emission, the sketch, and live generations are not
    // restarted or triggered, and the user's effect-preset library is untouched.
    auto *motionPreset = editor->findChild<QComboBox *>(QStringLiteral("motionPreset"));
    auto *applyMotion = editor->findChild<QPushButton *>(QStringLiteral("applyMotionPreset"));
    assert(motionPreset && applyMotion && motionPreset->count() == 3);
    const auto motionAxes = {sf::Param::Speed, sf::Param::Density, sf::Param::TrailLength, sf::Param::TrailDensity,
        sf::Param::TrailFalloff, sf::Param::UVX, sf::Param::UVY, sf::Param::DriftX, sf::Param::DriftY};
    // Make several affected animations nonzero before applying a template.
    for (auto id : {sf::Param::Speed, sf::Param::Density, sf::Param::TrailLength}) {
        target->setCurrentIndex(target->findData(static_cast<int>(id))); mode->setCurrentIndex(mode->findData(1));
    }
    const auto beforeMotion = latest;
    const auto userLibraryPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/sternenflug/presets.json");
    QFile userLibraryBefore(userLibraryPath);
    const bool libraryExistedBefore = userLibraryBefore.exists();
    QByteArray userLibraryBytes;
    if (userLibraryBefore.open(QIODevice::ReadOnly)) userLibraryBytes = userLibraryBefore.readAll();
    userLibraryBefore.close();
    const int settingsBeforeMotion = settingChanges, draftsBeforeMotion = draftChanges;
    const int sendsBeforeMotion = sends, stopsBeforeMotion = stops, pausesBeforeMotion = pauseChanges, restartsBeforeMotion = restarts;
    motionPreset->setCurrentIndex(motionPreset->findData(QStringLiteral("soft")));
    motionPreset->setCurrentIndex(motionPreset->findData(QStringLiteral("rain")));
    assert(settingChanges == settingsBeforeMotion && motionApplies == 0);
    applyMotion->click();
    assert(motionApplies == 1 && settingChanges == settingsBeforeMotion && latest.motionMode == 1);
    assert(std::abs(latest.get(sf::Param::Speed) - 2.4) < 1.e-12 && latest.get(sf::Param::Density) == 650);
    assert(latest.get(sf::Param::TrailLength) == 80 && latest.get(sf::Param::TrailDensity) == 100);
    assert(std::abs(latest.get(sf::Param::TrailFalloff) - 1.8) < 1.e-12);
    for (auto id : motionAxes) assert(latest.animations[static_cast<int>(id)].mode == 0);
    for (auto id : axes) assert(latest.get(id) == 0);
    assertUnchangedExcept(beforeMotion, latest, motionAxes, true);
    assert(editor->findChild<QLabel *>(QStringLiteral("motionModeHint"))->text().contains(QStringLiteral("Von oben nach unten")));
    sf::Settings modeDecoded; std::shared_ptr<sf::Drawing> modeDrawing; QString modeError;
    const auto modeJson = sf::encodeSettings(latest);
    assert(sf::decodeSettings(modeJson).motionMode == 1);
    assert(sf::decodePreset(sf::encodePreset(latest), modeDecoded, modeDrawing, &modeError) && modeDecoded.motionMode == 1 && !modeDrawing);
    auto legacySettings = modeJson; legacySettings.remove(QStringLiteral("motionMode"));
    assert(sf::decodeSettings(legacySettings).motionMode == 0);
    auto invalidMode = modeJson; invalidMode[QStringLiteral("motionMode")] = 77;
    assert(sf::decodeSettings(invalidMode).motionMode == 0);
    auto legacyPreset = sf::encodePreset(latest); legacyPreset[QStringLiteral("settings")] = legacySettings;
    assert(sf::decodePreset(legacyPreset, modeDecoded, modeDrawing, &modeError) && modeDecoded.motionMode == 0);
    motionPreset->setCurrentIndex(motionPreset->findData(QStringLiteral("soft"))); applyMotion->click();
    assert(motionApplies == 2 && latest.motionMode == 1 && latest.get(sf::Param::Speed) == .5 && latest.get(sf::Param::Density) == 220);
    assert(latest.get(sf::Param::TrailLength) == 0 && latest.get(sf::Param::TrailDensity) == 0);
    assertUnchangedExcept(beforeMotion, latest, motionAxes, true);
    motionPreset->setCurrentIndex(motionPreset->findData(QStringLiteral("starflight"))); applyMotion->click();
    assert(motionApplies == 3 && latest.motionMode == 0 && latest.get(sf::Param::Speed) == 1 && latest.get(sf::Param::Density) == 350);
    assert(latest.get(sf::Param::TrailLength) == 80 && latest.get(sf::Param::TrailDensity) == 35 && latest.get(sf::Param::TrailFalloff) == 1.5);
    assertUnchangedExcept(beforeMotion, latest, motionAxes, true);
    assert(settingChanges == settingsBeforeMotion && draftChanges == draftsBeforeMotion && sends == sendsBeforeMotion);
    assert(stops == stopsBeforeMotion && pauseChanges == pausesBeforeMotion && restarts == restartsBeforeMotion && driftStops == 1 && driftResets == 1);
    QFile userLibraryAfter(userLibraryPath);
    assert(userLibraryAfter.exists() == libraryExistedBefore);
    if (libraryExistedBefore) { assert(userLibraryAfter.open(QIODevice::ReadOnly)); assert(userLibraryAfter.readAll() == userLibraryBytes); }
    userLibraryAfter.close();
    key(canvas, Qt::Key_Z, Qt::ControlModifier); assert(latestDraft->strokes.empty());
    key(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier); assert(!latestDraft->strokes.empty());

    // Presets are effect-wide and intentionally contain no hand sketch.
    const auto encoded = sf::encodePreset(latest, savedDraft);
    assert(!encoded.contains(QStringLiteral("drawing")));
    sf::Settings decoded; std::shared_ptr<sf::Drawing> unused; QString error;
    assert(sf::decodePreset(encoded, decoded, unused, &error)); assert(!unused);
    assert(decoded.animations[0].points.size() == latest.animations[0].points.size());
    assert(decoded.animations[static_cast<int>(sf::Param::UVX)].mode == latest.animations[static_cast<int>(sf::Param::UVX)].mode);
    assert(std::abs(decoded.get(sf::Param::SymbolSize) - 1024) < .01);

    // Open and close the actual manager. Creating a named preset writes only
    // its library; loading one emits one effect change and no drawing trigger.
    const int sendsBeforePreset = sends, draftsBeforePreset = draftChanges;
    auto *manage = editor->findChild<QPushButton *>(QStringLiteral("managePresets")); assert(manage);
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); assert(dialog);
        auto *create = buttonNamed(dialog, QStringLiteral("Neu …")); assert(create);
        QTimer::singleShot(0, [&] {
            auto *name = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()); assert(name);
            name->setTextValue(QStringLiteral("Smoke-Test Effekt")); name->accept();
        });
        create->click();
        auto *list = dialog->findChild<QListWidget *>(QStringLiteral("effectPresetList")); assert(list && list->count() > 0);
        auto *load = buttonNamed(dialog, QStringLiteral("Laden")); assert(load); load->click();
    });
    const int changesBeforePreset = settingChanges; manage->click();
    assert(settingChanges == changesBeforePreset + 1 && sends == sendsBeforePreset && draftChanges == draftsBeforePreset);
    assert(latestDraft && !latestDraft->strokes.empty());

    // OBS may delete a source asynchronously while its preset manager is open.
    // Heap-owned dialogs must close safely with the editor, including exec().
    auto *transient = sf::createEditor(sf::Settings{}, sf::Drawing{}, {});
    transient->setAttribute(Qt::WA_DontShowOnScreen, true); transient->show();
    auto *transientManager = transient->findChild<QPushButton *>(QStringLiteral("managePresets")); assert(transientManager);
    QTimer::singleShot(0, [&] { delete transient; transient = nullptr; });
    transientManager->click(); assert(!transient);

    // Older editor owners can omit the dedicated stop callback. They still
    // receive one settings update, while the position remains untouched.
    int fallbackUpdates = 0; sf::Settings fallbackSettings;
    fallbackSettings.set(sf::Param::UVX, .3); fallbackSettings.set(sf::Param::DriftX, .01);
    sf::EditorCallbacks fallbackCallbacks;
    fallbackCallbacks.settingsChanged = [&](const sf::Settings &value) { fallbackSettings = value; ++fallbackUpdates; };
    std::unique_ptr<QWidget> fallback(sf::createEditor(fallbackSettings, {}, fallbackCallbacks));
    fallback->findChild<QPushButton *>(QStringLiteral("stopDrift"))->click();
    assert(fallbackUpdates == 1 && fallbackSettings.get(sf::Param::UVX) == .3 && fallbackSettings.get(sf::Param::DriftX) == 0);
    fallback->findChild<QPushButton *>(QStringLiteral("applyMotionPreset"))->click();
    assert(fallbackUpdates == 2 && fallbackSettings.motionMode == 1 && std::abs(fallbackSettings.get(sf::Param::Speed) - 2.4) < 1.e-12);

    if (argc > 1) {
        // Optional review screenshots of the actual QWidget rendering.
        editor->resize(520, 850); tabs->setCurrentIndex(3); app.processEvents();
        editor->grab().save(QString::fromLocal8Bit(argv[1]) + QStringLiteral("/Sternenflug-Zeichnen.png"));
        tabs->setCurrentIndex(4); app.processEvents();
        editor->grab().save(QString::fromLocal8Bit(argv[1]) + QStringLiteral("/Sternenflug-Zeit.png"));
        tabs->setCurrentIndex(0); app.processEvents();
        editor->grab().save(QString::fromLocal8Bit(argv[1]) + QStringLiteral("/Sternenflug-Farbe.png"));
    }
    editor.reset(); app.processEvents();
    std::cout << "editor smoke: parameters, callbacks, local themes, resize, HSV, drawing, replay, undo, modeless canvas, curves, drift direction/tempo/persistence/stop/smooth-reset/hotkey synchronization and effect presets passed\n";
    return 0;
}
