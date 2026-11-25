#pragma once
#include "core.hpp"
#include <QJsonObject>
#include <QString>

namespace sf {
QJsonObject encodeSettings(const Settings &settings);
Settings decodeSettings(const QJsonObject &object);
QJsonObject encodeDrawing(const Drawing &drawing);
std::shared_ptr<Drawing> decodeDrawing(const QJsonObject &object);
QJsonObject encodePreset(const Settings &settings, const Drawing &unused = Drawing{});
bool decodePreset(const QJsonObject &object, Settings &settings,
                  std::shared_ptr<Drawing> &drawing, QString *error = nullptr);
}
