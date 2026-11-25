#include "core.hpp"
#include "codec.hpp"
#include "editor.hpp"
#include "renderer.hpp"
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QApplication>
#include <QAction>
#include <QComboBox>
#include <QDockWidget>
#include <QJsonDocument>
#include <QLabel>
#include <QMainWindow>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QSignalBlocker>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <atomic>
#include <mutex>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Sternenflug")

namespace {
constexpr const char *sourceId = "sternenflug_native_source";
constexpr const char *dockId = "sternenflug.native.editor";
std::atomic<uint64_t> nextModelId{1};
std::atomic<bool> unloading{false};

struct Model {
    uint64_t id = nextModelId++;
    std::mutex mutex;
    sf::Settings settings;
    sf::Simulation simulation;
    std::shared_ptr<sf::Drawing> draft = std::make_shared<sf::Drawing>();
    obs_weak_source_t *source = nullptr;
    ~Model() { if (source) obs_weak_source_release(source); }
};
struct Source {
    std::shared_ptr<Model> model = std::make_shared<Model>();
    std::atomic<uint32_t> width{1920}, height{1080};
    std::unique_ptr<sf::Renderer> renderer;
    bool renderErrorLogged = false;
    std::array<obs_hotkey_id,5> hotkeys{OBS_INVALID_HOTKEY_ID,OBS_INVALID_HOTKEY_ID,OBS_INVALID_HOTKEY_ID,OBS_INVALID_HOTKEY_ID,OBS_INVALID_HOTKEY_ID};
};
std::mutex registryMutex;
std::vector<std::weak_ptr<Model>> registry;
QPointer<QWidget> dockContent;
QPointer<QWidget> editorWidget;
QPointer<QComboBox> sourcePicker;
QVBoxLayout *editorLayout = nullptr;
std::weak_ptr<Model> selectedModel;
std::mutex dispatcherMutex;
QObject *guiDispatcher = nullptr;
QPointer<QAction> toolsAction;

void onGui(std::function<void()> fn)
{
    auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app || unloading) return;
    std::unique_lock lock(dispatcherMutex);
    if (!guiDispatcher || unloading) return;
    if (QThread::currentThread() == guiDispatcher->thread()) { lock.unlock(); fn(); }
    else QMetaObject::invokeMethod(guiDispatcher, [fn = std::move(fn)] { if (!unloading) fn(); }, Qt::QueuedConnection);
}

QJsonObject readConfiguration(obs_data_t *settings)
{
    const char *text = obs_data_get_string(settings, "configuration");
    if (!text || !*text) return {};
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

template<typename Change>
void changeAndPersist(const std::shared_ptr<Model> &model, Change change)
{
    obs_source_t *source = obs_weak_source_get_source(model->source);
    if (!source) return;
    QJsonObject configuration;
    {
        std::lock_guard lock(model->mutex);
        change();
        configuration = {{"schemaVersion", 1}, {"settings", sf::encodeSettings(model->settings)},
                         {"draft", sf::encodeDrawing(*model->draft)}};
        const auto json = QJsonDocument(configuration).toJson(QJsonDocument::Compact);
        // Store the already-applied UI state without scheduling an older OBS
        // update to run back through the model. Serialize access with update/save.
        obs_data_t *stored = obs_source_get_settings(source);
        obs_data_set_string(stored,"configuration",json.constData());
        obs_data_release(stored);
    }
    obs_source_release(source);
}

void refreshPicker();
void updateModelDrift(std::shared_ptr<Model> model,bool center)
{
    changeAndPersist(model,[&] {
        // Capture the visible offset before clearing any UI base values.
        if(center) model->simulation.resetDrift();
        else model->simulation.stopDrift();
        model->settings=model->simulation.settings();
    });
    std::weak_ptr<Model> weak=model;
    onGui([weak] {
        auto target=weak.lock();
        if(!target || selectedModel.lock()!=target || !editorWidget) return;
        sf::Settings settings;
        { std::lock_guard lock(target->mutex); settings=target->settings; }
        sf::updateEditorDrift(editorWidget,settings);
    });
}

void selectModel(const std::shared_ptr<Model> &model)
{
    selectedModel = model;
    if (!editorLayout) return;
    if (editorWidget) { editorLayout->removeWidget(editorWidget); editorWidget->deleteLater(); editorWidget = nullptr; }
    if (!model) {
        auto *empty = new QLabel(QStringLiteral("In OBS eine Quelle vom Typ „Sternenflug“ hinzufügen."), dockContent);
        empty->setWordWrap(true); editorWidget = empty; editorLayout->addWidget(empty);
        return;
    }
    sf::Settings settings;
    sf::Drawing draft;
    bool paused = false;
    {
        std::lock_guard lock(model->mutex);
        settings = model->settings; draft = *model->draft; paused = model->simulation.paused();
    }
    std::weak_ptr<Model> weak = model;
    sf::EditorCallbacks callbacks;
    callbacks.settingsChanged = [weak](const sf::Settings &settings) {
        if (auto model = weak.lock()) {
            changeAndPersist(model,[&] { model->settings=sf::sanitizeSettings(settings); model->simulation.setSettings(model->settings); });
        }
    };
    callbacks.draftChanged = [weak](std::shared_ptr<sf::Drawing> draft) {
        if (auto model = weak.lock()) {
            changeAndPersist(model,[&] { model->draft=std::make_shared<sf::Drawing>(*draft); });
        }
    };
    callbacks.submitDrawing = [weak](std::shared_ptr<sf::Drawing> drawing) {
        if (auto model = weak.lock()) {
            bool accepted;
            { std::lock_guard lock(model->mutex); accepted = model->simulation.submit(drawing); }
            if (!accepted) onGui([weak] {
                if (selectedModel.lock() == weak.lock() && editorWidget)
                    sf::updateEditorStatus(editorWidget, QStringLiteral("Zeichnung nicht gesendet: leer oder Kapazitätsgrenze erreicht. Laufende Symbole bleiben erhalten."));
            });
            return accepted;
        }
        return false;
    };
    callbacks.stopEmission = [weak] { if (auto model = weak.lock()) { std::lock_guard lock(model->mutex); model->simulation.stopEmission(); } };
    callbacks.pauseChanged = [weak](bool paused) { if (auto model = weak.lock()) { std::lock_guard lock(model->mutex); model->simulation.setPaused(paused); } };
    callbacks.restart = [weak] { if (auto model = weak.lock()) { std::lock_guard lock(model->mutex); model->simulation.restart(); } };
    callbacks.resetDrift = [weak](const sf::Settings &) { if(auto model=weak.lock()) updateModelDrift(model,true); };
    callbacks.stopDrift = [weak](const sf::Settings &) { if(auto model=weak.lock()) updateModelDrift(model,false); };
    callbacks.applyMotionPreset = [weak](const sf::Settings &settings) {
        if (auto model = weak.lock()) {
            changeAndPersist(model,[&] {
                // Preserve the current offset for a soft return before applying
                // neutral axes from the explicitly selected movement template.
                model->simulation.resetDrift();
                model->settings = sf::sanitizeSettings(settings);
                model->simulation.setSettings(model->settings);
            });
        }
    };
    editorWidget = sf::createEditor(settings, draft, std::move(callbacks), dockContent);
    sf::updateEditorPlayback(editorWidget, paused);
    editorLayout->addWidget(editorWidget);
}

void refreshPicker()
{
    if (!sourcePicker) return;
    std::vector<std::shared_ptr<Model>> models;
    {
        std::lock_guard lock(registryMutex);
        for (auto it = registry.begin(); it != registry.end();) {
            if (auto model = it->lock()) { models.push_back(model); ++it; }
            else it = registry.erase(it);
        }
    }
    auto selected = selectedModel.lock();
    QSignalBlocker block(sourcePicker);
    sourcePicker->clear();
    int index = -1;
    for (const auto &model : models) {
        obs_source_t *source = obs_weak_source_get_source(model->source);
        if (!source) continue;
        sourcePicker->addItem(QString::fromUtf8(obs_source_get_name(source)), QVariant::fromValue<qulonglong>(model->id));
        if (model == selected) index = sourcePicker->count() - 1;
        obs_source_release(source);
    }
    if (index < 0 && sourcePicker->count()) index = 0;
    sourcePicker->setCurrentIndex(index);
    if (!selected && index >= 0) {
        const auto id = sourcePicker->currentData().toULongLong();
        for (auto &model : models) if (model->id == id) { selectModel(model); break; }
    } else if (selected && index < 0) selectModel(nullptr);
    else if (!selected && index < 0 && editorWidget &&
             editorWidget->objectName()==QStringLiteral("sternenflugEditor")) selectModel(nullptr);
}

void ensureDock()
{
    if (dockContent) return;
    auto *main = static_cast<QWidget *>(obs_frontend_get_main_window());
    if (!main) return;
    auto *content = new QWidget(main);
    content->setObjectName(QStringLiteral("sternenflugDockContent"));
    content->setMinimumWidth(340);
    auto *layout = new QVBoxLayout(content);
    layout->setContentsMargins(6,6,6,6); layout->setSpacing(6);
    auto *picker = new QComboBox(content);
    picker->setObjectName(QStringLiteral("sternenflugSourcePicker"));
    picker->setToolTip(QStringLiteral("Diese Quelle erhält Regleränderungen und neue Zeichnungen."));
    layout->addWidget(new QLabel(QStringLiteral("Sternenflug · Zielquelle"), content));
    layout->addWidget(picker);
    dockContent = content; sourcePicker = picker; editorLayout = layout;
    QObject::connect(picker, &QComboBox::currentIndexChanged, content, [](int) {
        if (!sourcePicker) return;
        const auto id = sourcePicker->currentData().toULongLong();
        std::shared_ptr<Model> selection;
        { std::lock_guard lock(registryMutex); for (auto &weak : registry) if (auto model = weak.lock(); model && model->id == id) selection = model; }
        if (selectedModel.lock() != selection) selectModel(selection);
    });
    if (!obs_frontend_add_dock_by_id(dockId, "Sternenflug", content)) {
        editorLayout = nullptr; content->deleteLater(); return;
    }
    auto *timer = new QTimer(content); timer->setInterval(300);
    QObject::connect(timer, &QTimer::timeout, content, [] {
        refreshPicker();
        auto model = selectedModel.lock();
        if (!model || !editorWidget) return;
        bool paused, emitting; size_t particles; double time;
        { std::lock_guard lock(model->mutex); paused = model->simulation.paused(); emitting = model->simulation.emitting();
          particles = model->simulation.particleCount(); time = model->simulation.time(); }
        sf::updateEditorPlayback(editorWidget, paused);
        sf::updateEditorStatus(editorWidget, QStringLiteral("%1 · %2 · %3 Partikel · %4 s")
            .arg(paused ? QStringLiteral("Pausiert") : QStringLiteral("Läuft"))
            .arg(emitting ? QStringLiteral("Symbole entstehen") : QStringLiteral("Sterne entstehen"))
            .arg(particles).arg(time,0,'f',1));
    });
    timer->start(); refreshPicker();
    if (!selectedModel.lock()) selectModel(nullptr);
}

void openDock(const std::shared_ptr<Model> &target = {})
{
    onGui([target] {
        ensureDock(); if (!dockContent) return;
        if (target) { selectModel(target); refreshPicker(); }
        QWidget *parent = dockContent->parentWidget();
        if (auto *dock = qobject_cast<QDockWidget *>(parent)) { dock->show(); dock->raise(); }
        else { dockContent->show(); dockContent->raise(); }
    });
}

void sourceUpdate(void *data, obs_data_t *settings)
{
    auto *source = static_cast<Source *>(data);
    std::lock_guard lock(source->model->mutex);
    source->width = static_cast<uint32_t>(std::clamp<int64_t>(obs_data_get_int(settings,"width"),320,7680));
    source->height = static_cast<uint32_t>(std::clamp<int64_t>(obs_data_get_int(settings,"height"),180,4320));
    const auto config = readConfiguration(settings);
    if (!config.isEmpty()) {
        source->model->settings = sf::decodeSettings(config["settings"].toObject());
        source->model->simulation.setSettings(source->model->settings);
        if (config["draft"].isObject()) source->model->draft = sf::decodeDrawing(config["draft"].toObject());
    }
}

void *sourceCreate(obs_data_t *settings, obs_source_t *obsSource)
{
    auto *source = new Source;
    source->model->source = obs_source_get_weak_source(obsSource);
    sourceUpdate(source, settings);
    source->model->simulation.restart();
    source->hotkeys[0]=obs_hotkey_register_source(obsSource,"Sternenflug.Send","Sternenflug: Zeichnung auslösen",
        [](void *data,obs_hotkey_id,obs_hotkey_t *,bool pressed) {
            if(!pressed) return; auto model=static_cast<Source *>(data)->model;
            std::lock_guard lock(model->mutex); model->simulation.submit(model->draft);
        },source);
    source->hotkeys[1]=obs_hotkey_register_source(obsSource,"Sternenflug.Stop","Sternenflug: Einmischen beenden",
        [](void *data,obs_hotkey_id,obs_hotkey_t *,bool pressed) {
            if(!pressed) return; auto model=static_cast<Source *>(data)->model;
            std::lock_guard lock(model->mutex); model->simulation.stopEmission();
        },source);
    source->hotkeys[2]=obs_hotkey_register_source(obsSource,"Sternenflug.Pause","Sternenflug: Pause / Fortsetzen",
        [](void *data,obs_hotkey_id,obs_hotkey_t *,bool pressed) {
            if(!pressed) return; auto model=static_cast<Source *>(data)->model;
            std::lock_guard lock(model->mutex); model->simulation.setPaused(!model->simulation.paused());
        },source);
    source->hotkeys[3]=obs_hotkey_register_source(obsSource,"Sternenflug.Restart","Sternenflug: Effekt neu starten",
        [](void *data,obs_hotkey_id,obs_hotkey_t *,bool pressed) {
            if(!pressed) return; auto model=static_cast<Source *>(data)->model;
            std::lock_guard lock(model->mutex); model->simulation.restart();
        },source);
    source->hotkeys[4]=obs_hotkey_register_source(obsSource,"Sternenflug.ResetDrift","Sternenflug: Drift sanft zurücksetzen",
        [](void *data,obs_hotkey_id,obs_hotkey_t *,bool pressed) {
            if(pressed) updateModelDrift(static_cast<Source *>(data)->model,true);
        },source);
    auto *procedures=obs_source_get_proc_handler(obsSource);
    proc_handler_add(procedures,"void trigger()",[](void *data,calldata_t *) {
        auto model=static_cast<Source *>(data)->model;
        std::lock_guard lock(model->mutex); model->simulation.submit(model->draft);
    },source);
    proc_handler_add(procedures,"void stop_emission()",[](void *data,calldata_t *) {
        auto model=static_cast<Source *>(data)->model;
        std::lock_guard lock(model->mutex); model->simulation.stopEmission();
    },source);
    proc_handler_add(procedures,"void reset_drift()",[](void *data,calldata_t *) {
        updateModelDrift(static_cast<Source *>(data)->model,true);
    },source);
    proc_handler_add(procedures,"void stop_drift()",[](void *data,calldata_t *) {
        updateModelDrift(static_cast<Source *>(data)->model,false);
    },source);
    proc_handler_add(procedures,"void get_status(out int particles, out bool emitting, out bool paused, out float time)",[](void *data,calldata_t *args) {
        auto model=static_cast<Source *>(data)->model;
        std::lock_guard lock(model->mutex);
        calldata_set_int(args,"particles",static_cast<long long>(model->simulation.particleCount()));
        calldata_set_bool(args,"emitting",model->simulation.emitting());
        calldata_set_bool(args,"paused",model->simulation.paused());
        calldata_set_float(args,"time",model->simulation.time());
    },source);
    { std::lock_guard lock(registryMutex); registry.push_back(source->model); }
    onGui([] { refreshPicker(); });
    return source;
}
void sourceDestroy(void *data)
{
    auto *source = static_cast<Source *>(data);
    for(auto hotkey:source->hotkeys) if(hotkey!=OBS_INVALID_HOTKEY_ID) obs_hotkey_unregister(hotkey);
    obs_enter_graphics(); source->renderer.reset(); obs_leave_graphics();
    delete source;
    onGui([] { refreshPicker(); });
}
void sourceTick(void *data, float seconds)
{
    auto model = static_cast<Source *>(data)->model;
    std::lock_guard lock(model->mutex); model->simulation.tick(seconds);
}
void sourceRender(void *data, gs_effect_t *)
{
    auto *source = static_cast<Source *>(data);
    const auto width = source->width.load(), height = source->height.load();
    std::vector<sf::RenderParticle> frame;
    { std::lock_guard lock(source->model->mutex); frame = source->model->simulation.renderFrame(width,height); }
    if (!source->renderer) source->renderer = std::make_unique<sf::Renderer>();
    if (!source->renderer->render(frame,width,height) && !source->renderErrorLogged) {
        blog(LOG_ERROR,"[Sternenflug] GPU renderer: %s",source->renderer->error().c_str()); source->renderErrorLogged=true;
    }
}
void sourceSave(void *data, obs_data_t *settings)
{
    auto model = static_cast<Source *>(data)->model;
    std::lock_guard lock(model->mutex);
    QJsonObject config{{"schemaVersion",1},{"settings",sf::encodeSettings(model->settings)}, {"draft",sf::encodeDrawing(*model->draft)}};
    const auto json = QJsonDocument(config).toJson(QJsonDocument::Compact);
    obs_data_set_string(settings,"configuration",json.constData());
}
bool openButton(obs_properties_t *, obs_property_t *, void *data)
{
    if (auto *source = static_cast<Source *>(data)) openDock(source->model); else openDock();
    return false;
}
obs_properties_t *sourceProperties(void *data)
{
    auto *properties = obs_properties_create();
    obs_properties_add_button2(properties,"editor","Sternenflug-Steuerung öffnen",openButton,data);
    obs_properties_add_int(properties,"width","Ausgabebreite",320,7680,1);
    obs_properties_add_int(properties,"height","Ausgabehöhe",180,4320,1);
    obs_properties_add_text(properties,"notice","Farben, Bewegung, Größe und Zeichnen befinden sich im Sternenflug-Dock.",OBS_TEXT_INFO);
    return properties;
}
void sourceDefaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings,"width",1920); obs_data_set_default_int(settings,"height",1080);
    QJsonObject config{{"schemaVersion",1},{"settings",sf::encodeSettings(sf::Settings{})}};
    const auto json = QJsonDocument(config).toJson(QJsonDocument::Compact);
    obs_data_set_default_string(settings,"configuration",json.constData());
}
void frontendEvent(enum obs_frontend_event event,void *)
{
    if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) onGui([]{ensureDock();});
    else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED) onGui([]{refreshPicker();});
}
}

MODULE_EXPORT const char *obs_module_name(void) { return "Sternenflug"; }
MODULE_EXPORT const char *obs_module_description(void) { return "Native transparente Sternenflug-Quelle mit Zeichenfläche, Kurveneditor und Effekt-Presets."; }

bool obs_module_load(void)
{
    unloading=false;
    obs_source_info info{};
    info.id=sourceId; info.type=OBS_SOURCE_TYPE_INPUT;
    info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW|OBS_SOURCE_SRGB;
    info.get_name=[](void *){return "Sternenflug";};
    info.create=sourceCreate; info.destroy=sourceDestroy; info.update=sourceUpdate;
    info.get_defaults=sourceDefaults; info.get_properties=sourceProperties;
    info.get_width=[](void *data){return static_cast<Source *>(data)->width.load();};
    info.get_height=[](void *data){return static_cast<Source *>(data)->height.load();};
    info.video_tick=sourceTick; info.video_render=sourceRender; info.save=sourceSave;
    info.icon_type=OBS_ICON_TYPE_CUSTOM;
    obs_register_source(&info);
    blog(LOG_INFO,"[Sternenflug] Native source %s loaded",STERNENFLUG_VERSION);
    return true;
}
void obs_module_post_load(void)
{
    auto *app=qobject_cast<QApplication *>(QCoreApplication::instance());
    if (!app) return;
    {
        std::lock_guard lock(dispatcherMutex);
        guiDispatcher=new QObject;
        guiDispatcher->moveToThread(app->thread());
    }
    obs_frontend_add_event_callback(frontendEvent,nullptr);
    onGui([] {
        toolsAction=static_cast<QAction *>(obs_frontend_add_tools_menu_qaction("Sternenflug-Steuerung"));
        if(toolsAction) QObject::connect(toolsAction,&QAction::triggered,guiDispatcher,[]{openDock();});
        ensureDock();
    });
}
void obs_module_unload(void)
{
    unloading=true;
    obs_frontend_remove_event_callback(frontendEvent,nullptr);
    QObject *dispatcher;
    {
        std::lock_guard lock(dispatcherMutex);
        dispatcher=guiDispatcher; guiDispatcher=nullptr;
    }
    if(dispatcher) {
        auto cleanup=[dispatcher] {
            if(toolsAction) { delete toolsAction.data(); toolsAction=nullptr; }
            if(dockContent) {
                // Destroy plugin widgets synchronously; OBS may delete its dock later.
                delete dockContent.data(); dockContent=nullptr;
                obs_frontend_remove_dock(dockId);
            }
            editorWidget=nullptr; sourcePicker=nullptr; editorLayout=nullptr;
            // Removing the receiver also cancels every queued plugin functor.
            delete dispatcher;
        };
        if(QThread::currentThread()==dispatcher->thread()) cleanup();
        else QMetaObject::invokeMethod(dispatcher,cleanup,Qt::BlockingQueuedConnection);
    }
    selectedModel.reset();
}
