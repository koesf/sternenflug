#pragma once

#include <functional>
#include <memory>

#include <QString>
#include <QWidget>

namespace sf {

struct Settings;
struct Drawing;

// All callbacks run on the GUI thread. The owner chooses the target source and
// transfers immutable copies to the renderer; the editor never calls libobs.
struct EditorCallbacks {
    std::function<void(const Settings &)> settingsChanged;
    std::function<bool(std::shared_ptr<Drawing>)> submitDrawing;
    std::function<void()> stopEmission;
    std::function<void(bool)> pauseChanged;
    std::function<void()> restart;
    std::function<void(std::shared_ptr<Drawing>)> draftChanged;
    // Atomically applies these already-reset settings and starts the renderer's
    // smooth return of accumulated drift. This replaces settingsChanged for
    // this one action and must not restart or trigger particles.
    std::function<void(const Settings &)> resetDrift;
    // Stops drift, including an in-progress smooth return, while preserving
    // the visible position. Falls back to settingsChanged when not provided.
    std::function<void(const Settings &)> stopDrift;
    // Explicitly applies a movement template. The owner captures the old
    // offset, starts a smooth return, then applies these new settings atomically.
    std::function<void(const Settings &)> applyMotionPreset;
};

QWidget *createEditor(const Settings &settings, const Drawing &draft,
                      EditorCallbacks callbacks, QWidget *parent = nullptr);
void updateEditorStatus(QWidget *editor, QString status);

// Programmatic updates never emit user-change callbacks. This also makes the
// same dock safe to reuse when the selected source changes.
void updateEditorSettings(QWidget *editor, const Settings &settings,
                          const Drawing &draft);
void updateEditorPlayback(QWidget *editor, bool paused);
// Synchronizes only position/drift axes and their animation controls. It keeps
// the current draft, drawing history, and all other effect settings intact.
void updateEditorDrift(QWidget *editor, const Settings &settings);

} // namespace sf
