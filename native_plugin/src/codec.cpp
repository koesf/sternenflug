#include "codec.hpp"
#include <QJsonArray>
#include <algorithm>
#include <cmath>

namespace sf {
QJsonObject encodeSettings(const Settings &input)
{
    const auto settings = sanitizeSettings(input);
    QJsonObject values, animations;
    for (const auto &def : parameterDefs()) {
        values[def.key] = settings.get(def.id);
        const auto &a = settings.animations[static_cast<size_t>(def.id)];
        QJsonArray points;
        for (const auto &p : a.points) points.append(QJsonArray{p.time, p.value});
        animations[def.key] = QJsonObject{{"mode", a.mode}, {"amount", a.amount},
            {"period", a.period}, {"phase", a.phase}, {"pingPong", a.pingPong}, {"points", points}};
    }
    return {{"values", values}, {"animations", animations},
            {"seed", static_cast<double>(settings.seed)}, {"colorMode", settings.colorMode},
            {"motionMode", settings.motionMode},
            {"emissionForever", settings.emissionForever}};
}

Settings decodeSettings(const QJsonObject &object)
{
    Settings settings;
    const auto values = object["values"].toObject();
    const auto animations = object["animations"].toObject();
    for (const auto &def : parameterDefs()) {
        if (values[def.key].isDouble()) settings.set(def.id, values[def.key].toDouble());
        auto &a = settings.animations[static_cast<size_t>(def.id)];
        const auto entry = animations[def.key].toObject();
        a.mode = entry["mode"].toInt();
        a.amount = entry["amount"].toDouble();
        a.period = entry["period"].toDouble(4.0);
        a.phase = entry["phase"].toDouble();
        a.pingPong = entry["pingPong"].toBool();
        if (entry["points"].isArray()) {
            a.points.clear();
            const auto points = entry["points"].toArray();
            for (int i = 0; i < points.size() && i < 128; ++i) {
                const auto pair = points[i].toArray();
                if (pair.size() == 2 && pair[0].isDouble() && pair[1].isDouble())
                    a.points.push_back({pair[0].toDouble(), pair[1].toDouble()});
            }
        }
    }
    const double seed = object["seed"].toDouble(1);
    settings.seed = static_cast<uint32_t>(std::clamp(seed, 0.0, 4294967295.0));
    settings.colorMode = object["colorMode"].toInt(1);
    // Older scenes and presets retain the original perspective flight.
    settings.motionMode = object["motionMode"].toInt(0);
    settings.emissionForever = object["emissionForever"].toBool();
    return sanitizeSettings(settings);
}

QJsonObject encodeDrawing(const Drawing &drawing)
{
    QJsonArray strokes;
    for (const auto &stroke : drawing.strokes) {
        QJsonArray points;
        for (const auto &point : stroke) points.append(QJsonArray{point.x, point.y});
        strokes.append(points);
    }
    return {{"id", QString::number(drawing.id)}, {"strokes", strokes}};
}

std::shared_ptr<Drawing> decodeDrawing(const QJsonObject &object)
{
    auto drawing = std::make_shared<Drawing>();
    drawing->id = object["id"].toString().toULongLong();
    const auto strokes = object["strokes"].toArray();
    size_t total = 0;
    for (int s = 0; s < strokes.size() && s < 128 && total < 4096; ++s) {
        std::vector<Point> stroke;
        const auto points = strokes[s].toArray();
        for (int p = 0; p < points.size() && total < 4096; ++p) {
            const auto pair = points[p].toArray();
            if (pair.size() != 2 || !pair[0].isDouble() || !pair[1].isDouble()) continue;
            const double x = pair[0].toDouble(), y = pair[1].toDouble();
            if (!std::isfinite(x) || !std::isfinite(y)) continue;
            stroke.push_back({std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)});
            ++total;
        }
        if (!stroke.empty()) drawing->strokes.push_back(std::move(stroke));
    }
    return drawing;
}

QJsonObject encodePreset(const Settings &settings, const Drawing &)
{
    // Effect presets deliberately contain no transient handwritten trigger.
    return {{"format", "sternenflug-effect-preset"}, {"schemaVersion", 1},
            {"settings", encodeSettings(settings)}};
}

bool decodePreset(const QJsonObject &object, Settings &settings,
                  std::shared_ptr<Drawing> &drawing, QString *error)
{
    if (object["schemaVersion"].toInt() != 1 ||
        object["format"].toString() != "sternenflug-effect-preset" ||
        !object["settings"].isObject() ||
        !object["settings"].toObject()["values"].isObject()) {
        if (error) *error = QStringLiteral("Dies ist kein unterstütztes Sternenflug-Effekt-Preset.");
        return false;
    }
    settings = decodeSettings(object["settings"].toObject());
    drawing.reset();
    return true;
}
}
