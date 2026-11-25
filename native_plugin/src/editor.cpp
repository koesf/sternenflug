#include "editor.hpp"
#include "codec.hpp"
#include "core.hpp"

#include <QAbstractButton>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QButtonGroup>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTabletEvent>
#include <QToolButton>
#include <QTouchEvent>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace sf {
namespace {

constexpr double pi = 3.14159265358979323846;
double bounded(double value, double low, double high)
{
    return std::max(low, std::min(high, value));
}
QPointF mousePosition(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position();
#else
    return event->localPos();
#endif
}
QString text(const char *value) { return QString::fromUtf8(value); }
bool redoKey(const QKeyEvent *event)
{
    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    return event->matches(QKeySequence::Redo) || (control && event->key() == Qt::Key_Y)
        || (control && event->modifiers().testFlag(Qt::ShiftModifier) && event->key() == Qt::Key_Z);
}
QColor visibleAccent(const QWidget *widget)
{
    auto luminance = [](const QColor &c) {
        auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
        return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
    };
    const QColor selected = widget->palette().color(QPalette::Highlight);
    const double base = luminance(widget->palette().color(QPalette::Base));
    const double accent = luminance(selected);
    const double contrast = (std::max(base, accent) + .05) / (std::min(base, accent) + .05);
    // A host stylesheet can resolve the Highlight role to black on a custom
    // painted child. Never let the interactive graph/focus marker disappear.
    return contrast >= 3.0 ? selected : (base < .3 ? QColor(91, 183, 220) : QColor(0, 102, 158));
}

QLabel *note(const QString &value, QWidget *parent = nullptr)
{
    auto *label = new QLabel(value, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setObjectName(QStringLiteral("sfHint"));
    label->setForegroundRole(QPalette::PlaceholderText);
    QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    policy.setHeightForWidth(true); label->setSizePolicy(policy);
    return label;
}

QToolButton *toolButton(QWidget *parent, const QString &label,
                        QStyle::StandardPixmap icon, const QString &tooltip)
{
    auto *button = new QToolButton(parent);
    button->setText(label);
    button->setIcon(parent->style()->standardIcon(icon));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setToolTip(tooltip);
    button->setAccessibleName(label);
    return button;
}

QString suffix(Param id)
{
    switch (id) {
    case Param::Opacity: case Param::SizeJitter: case Param::SymbolShare:
    case Param::TimeScatter: case Param::TrailDensity: case Param::ColorVariation:
    case Param::SatVariation: case Param::Brightness: case Param::SatA:
    case Param::ValA: case Param::SatB: case Param::ValB: return QStringLiteral(" %");
    case Param::StarSize: case Param::SymbolSize: case Param::TrailLength:
        return QStringLiteral(" px");
    case Param::HueVariation: case Param::HueA: case Param::HueB:
        return QStringLiteral("°");
    case Param::EmissionDuration: return QStringLiteral(" s");
    case Param::DriftX: case Param::DriftY: return QStringLiteral(" /s");
    case Param::Speed: return QStringLiteral("×");
    default: return {};
    }
}
int decimals(double step)
{
    int result = 0;
    while (step < 0.999999 && result < 4) { step *= 10.0; ++result; }
    return result;
}

void paintDrawing(QPainter &painter, const QRectF &bounds, const Drawing &drawing,
                  const QColor &color, double penWidth)
{
    painter.save();
    painter.setClipRect(bounds);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    for (const auto &stroke : drawing.strokes) {
        if (stroke.empty()) continue;
        QPainterPath path;
        path.moveTo(bounds.left() + stroke.front().x * bounds.width(),
                    bounds.top() + stroke.front().y * bounds.height());
        for (std::size_t i = 1; i < stroke.size(); ++i)
            path.lineTo(bounds.left() + stroke[i].x * bounds.width(),
                        bounds.top() + stroke[i].y * bounds.height());
        if (stroke.size() == 1) painter.drawPoint(path.currentPosition());
        else painter.drawPath(path);
    }
    painter.restore();
}

class ParamRow final : public QWidget {
public:
    ParamRow(const ParamDef &definition, double value,
             std::function<void(double)> changed,
             std::function<void()> animate, QWidget *parent = nullptr)
        : QWidget(parent), def_(definition), changed_(std::move(changed))
    {
        setObjectName(QStringLiteral("row_") + text(def_.key));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 3, 0, 3);
        layout->setSpacing(1);
        auto *line = new QHBoxLayout;
        line->setSpacing(5);
        label_ = new QLabel(text(def_.label), this);
        label_->setWordWrap(true);
        QSizePolicy labelPolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        labelPolicy.setHeightForWidth(true); label_->setSizePolicy(labelPolicy);
        label_->setToolTip(text(def_.tooltip));
        line->addWidget(label_, 1);
        spin_ = new QDoubleSpinBox(this);
        spin_->setObjectName(QStringLiteral("value_") + text(def_.key));
        spin_->setAccessibleName(text(def_.label));
        spin_->setRange(def_.min, def_.max);
        spin_->setSingleStep(def_.step);
        spin_->setDecimals(decimals(def_.step));
        spin_->setSuffix(suffix(def_.id));
        spin_->setKeyboardTracking(false);
        spin_->setAccelerated(true);
        spin_->setToolTip(text(def_.tooltip) + QStringLiteral("\nZahl direkt eingeben; Pfeiltasten für Feinschritte."));
        spin_->setMinimumWidth(93);
        label_->setBuddy(spin_);
        line->addWidget(spin_);
        animation_ = new QToolButton(this);
        animation_->setObjectName(QStringLiteral("animate_") + text(def_.key));
        animation_->setText(QStringLiteral("◇"));
        animation_->setAccessibleName(text(def_.label) + QStringLiteral(" animieren"));
        animation_->setToolTip(QStringLiteral("Zeitsteuerung für diesen Wert öffnen. Der Grundwert bleibt separat einstellbar."));
        animation_->setCheckable(true);
        animation_->setEnabled(def_.animatable);
        animation_->setVisible(def_.animatable);
        line->addWidget(animation_);
        layout->addLayout(line);
        slider_ = new QSlider(Qt::Horizontal, this);
        slider_->setObjectName(QStringLiteral("slider_") + text(def_.key));
        slider_->setAccessibleName(text(def_.label));
        slider_->setRange(0, 1000);
        slider_->setToolTip(text(def_.tooltip));
        slider_->setSingleStep(1);
        slider_->setPageStep(25);
        layout->addWidget(slider_);
        QObject::connect(spin_, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
                         [this](double v) {
            { const QSignalBlocker block(slider_); slider_->setValue(toSlider(v)); }
            if (changed_) changed_(v);
        });
        QObject::connect(slider_, &QSlider::valueChanged, this, [this](int position) {
            spin_->setValue(fromSlider(position));
        });
        QObject::connect(animation_, &QToolButton::clicked, this,
                         [this, animate = std::move(animate)] {
            setAnimated(animated_); if (animate) animate();
        });
        setValue(value);
    }
    void setValue(double value)
    {
        const QSignalBlocker a(spin_), b(slider_);
        spin_->setValue(value);
        slider_->setValue(toSlider(value));
    }
    void setAnimated(bool active)
    {
        animated_ = active;
        const QSignalBlocker blocker(animation_);
        animation_->setChecked(active);
        animation_->setText(active ? QStringLiteral("◆") : QStringLiteral("◇"));
    }
private:
    bool sizeMapping() const { return def_.id == Param::SymbolSize; }
    int toSlider(double value) const
    {
        double t = (value - def_.min) / std::max(0.00001, def_.max - def_.min);
        if (sizeMapping()) t = std::sqrt(std::max(0.0, t));
        return qRound(bounded(t, 0.0, 1.0) * 1000.0);
    }
    double fromSlider(int value) const
    {
        double t = value / 1000.0;
        if (sizeMapping()) t *= t;
        const double raw = def_.min + t * (def_.max - def_.min);
        return bounded(std::round(raw / def_.step) * def_.step, def_.min, def_.max);
    }
    ParamDef def_;
    QLabel *label_ = nullptr;
    QDoubleSpinBox *spin_ = nullptr;
    QSlider *slider_ = nullptr;
    QToolButton *animation_ = nullptr;
    std::function<void(double)> changed_;
    bool animated_ = false;
};

class HueWheel final : public QWidget {
public:
    std::function<void(double, double)> changed;
    explicit HueWheel(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("hsvColorWheel"));
        setMinimumSize(150, 150);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMaximumHeight(300);
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(QStringLiteral("Farbkreis: Farbton und Sättigung"));
        setToolTip(QStringLiteral("Farbton am Kreis wählen, Sättigung über den Abstand zur Mitte. Pfeiltasten: Farbton; Hoch/Runter: Sättigung. Umschalt für Feinschritte."));
    }
    QSize sizeHint() const override { return {220, 220}; }
    void setHsv(double hue, double saturation, double value)
    {
        const bool brightnessChanged = std::abs(value_ - value) > 0.0001;
        hue_ = bounded(hue, 0.0, 360.0);
        saturation_ = bounded(saturation, 0.0, 100.0);
        value_ = bounded(value, 0.0, 100.0);
        if (brightnessChanged) image_ = {};
        update();
    }
protected:
    void resizeEvent(QResizeEvent *event) override { QWidget::resizeEvent(event); image_ = {}; }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = wheelRect();
        const int side = std::max(1, qRound(area.width() * devicePixelRatioF()));
        if (image_.width() != side) {
            image_ = QImage(side, side, QImage::Format_ARGB32_Premultiplied);
            image_.fill(Qt::transparent);
            const double half = side * 0.5;
            // The circle always shows full brightness so even a black selected
            // color remains easy to edit. Value is a separate explicit control.
            for (int y = 0; y < side; ++y) {
                auto *scan = reinterpret_cast<QRgb *>(image_.scanLine(y));
                for (int x = 0; x < side; ++x) {
                    const double dx = (x + 0.5 - half) / half;
                    const double dy = (half - y - 0.5) / half;
                    const double radius = std::sqrt(dx * dx + dy * dy);
                    if (radius > 1.0) continue;
                    double hue = std::atan2(dy, dx) / (2.0 * pi);
                    if (hue < 0.0) hue += 1.0;
                    scan[x] = QColor::fromHsvF(hue, radius, 1.0).rgba();
                }
            }
        }
        painter.drawImage(area, image_);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(area);
        const double angle = hue_ * pi / 180.0;
        const double radius = saturation_ / 100.0 * area.width() * 0.5;
        const QPointF point = area.center() + QPointF(std::cos(angle) * radius, -std::sin(angle) * radius);
        painter.setPen(QPen(Qt::black, 3.5));
        painter.drawEllipse(point, 6.0, 6.0);
        painter.setPen(QPen(Qt::white, 1.8));
        painter.drawEllipse(point, 6.0, 6.0);
        if (hasFocus()) {
            painter.setPen(QPen(visibleAccent(this), 2, Qt::DashLine));
            painter.drawEllipse(area.adjusted(-3, -3, 3, 3));
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        setFocus(Qt::MouseFocusReason); drag_ = true; select(mousePosition(event)); event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override { if (drag_) select(mousePosition(event)); }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) { drag_ = false; event->accept(); }
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 0.2 : 2.0;
        double hue = hue_, sat = saturation_;
        switch (event->key()) {
        case Qt::Key_Left: hue -= step; break;
        case Qt::Key_Right: hue += step; break;
        case Qt::Key_Up: sat += step; break;
        case Qt::Key_Down: sat -= step; break;
        default: QWidget::keyPressEvent(event); return;
        }
        hue_ = std::fmod(hue + 360.0, 360.0);
        saturation_ = bounded(sat, 0.0, 100.0);
        if (changed) changed(hue_, saturation_);
        update(); event->accept();
    }
private:
    QRectF wheelRect() const
    {
        const double side = std::max(1, std::min(width(), height()) - 12);
        return {(width() - side) * 0.5, (height() - side) * 0.5, side, side};
    }
    void select(QPointF point)
    {
        const QRectF area = wheelRect();
        const QPointF d = point - area.center();
        double angle = std::atan2(-d.y(), d.x()) * 180.0 / pi;
        if (angle < 0) angle += 360.0;
        hue_ = angle;
        saturation_ = bounded(std::hypot(d.x(), d.y()) / (area.width() * 0.5) * 100.0, 0.0, 100.0);
        if (changed) changed(hue_, saturation_);
        update();
    }
    QImage image_;
    double hue_ = 0.0, saturation_ = 100.0, value_ = 100.0;
    bool drag_ = false;
};

class DriftDirectionPad final : public QWidget {
public:
    std::function<void(double, double)> changed;
    explicit DriftDirectionPad(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("driftDirectionPad"));
        setMinimumSize(205, 145); setMaximumHeight(165);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setFocusPolicy(Qt::StrongFocus); setCursor(Qt::CrossCursor);
        setAccessibleName(QStringLiteral("Driftrichtung"));
        setToolTip(QStringLiteral("Pfeil ziehen oder eine Richtung anklicken. Pfeiltasten wählen Oben, Unten, Links oder Rechts. Umschalt+Links/Rechts dreht um 1°; Bildauf/Bildab um 15°. Die Entfernung zur Mitte verändert das Tempo nicht. Bei Tempo 0 startet eine Richtungswahl sanft mit 1% Bild/s."));
    }
    QSize sizeHint() const override { return {260, 155}; }
    void setDirection(double x, double y, bool moving)
    {
        const double length = std::hypot(x, y);
        if (std::isfinite(length) && length > 1.e-12) { x_ = x / length; y_ = y / length; }
        moving_ = moving; update();
        const double degrees = std::fmod(std::atan2(x_, -y_) * 180. / pi + 360., 360.);
        setAccessibleDescription(QStringLiteral("Richtung %1 Grad, %2").arg(degrees, 0, 'f', 1).arg(moving ? QStringLiteral("Drift aktiv") : QStringLiteral("gestoppt")));
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        const QRectF circle = circleRect(); const QPointF center = circle.center();
        QColor grid = palette().color(QPalette::Text); grid.setAlpha(35);
        painter.setPen(QPen(grid, 1, Qt::DashLine));
        painter.drawLine(QPointF(circle.left(), center.y()), QPointF(circle.right(), center.y()));
        painter.drawLine(QPointF(center.x(), circle.top()), QPointF(center.x(), circle.bottom()));
        painter.setPen(QPen(palette().color(QPalette::Mid), 1)); painter.setBrush(palette().brush(QPalette::Base));
        painter.drawEllipse(circle);
        painter.setPen(QPen(grid, 1, Qt::DashLine));
        painter.drawLine(QPointF(circle.left(), center.y()), QPointF(circle.right(), center.y()));
        painter.drawLine(QPointF(center.x(), circle.top()), QPointF(center.x(), circle.bottom()));
        for (int i = 0; i < 8; ++i) {
            const double angle = i * pi / 4.; const QPointF delta(std::cos(angle), std::sin(angle));
            painter.setPen(QPen(palette().color(QPalette::Mid), 1.4));
            painter.drawLine(center + delta * (circle.width() * .5 - 5), center + delta * (circle.width() * .5));
        }
        const QColor ink = moving_ ? visibleAccent(this) : palette().color(QPalette::PlaceholderText);
        const QPointF direction(x_, y_), normal(-y_, x_);
        const QPointF tip = center + direction * (circle.width() * .37);
        painter.setPen(QPen(ink, 3, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(center - direction * 8, tip);
        painter.setPen(Qt::NoPen); painter.setBrush(ink);
        QPolygonF head; head << tip + direction * 3 << tip - direction * 11 + normal * 6 << tip - direction * 11 - normal * 6;
        painter.drawPolygon(head); painter.drawEllipse(center, 3.5, 3.5);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(center.x() - 35, 0, 70, 20), Qt::AlignCenter, QStringLiteral("Oben"));
        painter.drawText(QRectF(center.x() - 35, height() - 20, 70, 20), Qt::AlignCenter, QStringLiteral("Unten"));
        painter.drawText(QRectF(0, center.y() - 10, circle.left() - 5, 20), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Links"));
        painter.drawText(QRectF(circle.right() + 5, center.y() - 10, width() - circle.right() - 5, 20), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Rechts"));
        if (hasFocus()) {
            painter.setPen(QPen(visibleAccent(this), 1, Qt::DashLine)); painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(rect().adjusted(1, 1, -2, -2), 5, 5);
        }
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        setFocus(Qt::MouseFocusReason); dragging_ = true; select(mousePosition(event)); event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override { if (dragging_) { select(mousePosition(event)); event->accept(); } }
    void mouseReleaseEvent(QMouseEvent *event) override { if (event->button() == Qt::LeftButton) { dragging_ = false; event->accept(); } }
    void keyPressEvent(QKeyEvent *event) override
    {
        const bool fine = event->modifiers().testFlag(Qt::ShiftModifier);
        double x = x_, y = y_, rotation = 0;
        switch (event->key()) {
        case Qt::Key_Up: x = 0; y = -1; break;
        case Qt::Key_Down: x = 0; y = 1; break;
        case Qt::Key_Left: if (fine) rotation = -pi / 180.; else { x = -1; y = 0; } break;
        case Qt::Key_Right: if (fine) rotation = pi / 180.; else { x = 1; y = 0; } break;
        case Qt::Key_PageUp: rotation = -pi / 12.; break;
        case Qt::Key_PageDown: rotation = pi / 12.; break;
        default: QWidget::keyPressEvent(event); return;
        }
        if (rotation != 0) { x = x_ * std::cos(rotation) - y_ * std::sin(rotation); y = x_ * std::sin(rotation) + y_ * std::cos(rotation); }
        if (changed) changed(x, y); event->accept();
    }
private:
    QRectF circleRect() const
    {
        const double diameter = std::max(50., std::min(width() - 100., height() - 44.));
        return {(width() - diameter) * .5, (height() - diameter) * .5, diameter, diameter};
    }
    void select(const QPointF &point)
    {
        const QPointF delta = point - circleRect().center(); const double length = std::hypot(delta.x(), delta.y());
        if (length >= 6 && changed) changed(delta.x() / length, delta.y() / length);
    }
    double x_ = 1., y_ = 0.;
    bool moving_ = false, dragging_ = false;
};

class ColorRamp final : public QWidget {
public:
    QColor first{Qt::white}, second{Qt::cyan};
    int mode = 1;
    explicit ColorRamp(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedHeight(22);
        setAccessibleName(QStringLiteral("Vorschau des Farbverlaufs von A nach B"));
        setToolTip(QStringLiteral("Farbverlauf zwischen A und B. Die Farbvariation bestimmt, wie weit die Partikel diesen Verlauf nutzen."));
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        for (int x = 0; x < width(); ++x) {
            const double t = x / double(std::max(1, width() - 1));
            QColor color;
            if (mode == 1) {
                double ha = first.hsvHueF(), hb = second.hsvHueF();
                if (ha < 0) ha = hb;
                if (hb < 0) hb = ha;
                if (ha < 0) ha = hb = 0;
                double delta = hb - ha;
                if (delta > .5) delta -= 1.;
                if (delta < -.5) delta += 1.;
                const double h = std::fmod(ha + delta * t + 1., 1.);
                color = QColor::fromHsvF(h, first.hsvSaturationF() * (1 - t) + second.hsvSaturationF() * t,
                                         first.valueF() * (1 - t) + second.valueF() * t);
            } else {
                auto mixLinear = [t](double a, double b) {
                    auto decode = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
                    const double v = decode(a) * (1 - t) + decode(b) * t;
                    return v <= .0031308 ? 12.92 * v : 1.055 * std::pow(v, 1. / 2.4) - .055;
                };
                color = QColor::fromRgbF(mixLinear(first.redF(), second.redF()),
                                         mixLinear(first.greenF(), second.greenF()),
                                         mixLinear(first.blueF(), second.blueF()));
            }
            painter.setPen(color);
            painter.drawLine(x, 1, x, height() - 2);
        }
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
};

class DrawingPreview final : public QWidget {
public:
    Drawing drawing;
    explicit DrawingPreview(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(78, 44);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setAccessibleName(QStringLiteral("Zuletzt gesendete Kontur"));
    }
    QSize sizeHint() const override { return {128, 72}; }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().brush(QPalette::Base));
        const double w = std::min(double(width() - 4), (height() - 4) * 16. / 9.);
        const QRectF area((width() - w) * .5, (height() - w * 9. / 16.) * .5, w, w * 9. / 16.);
        paintDrawing(painter, area, drawing, palette().color(QPalette::Text), 1.3);
        painter.setPen(palette().color(QPalette::Mid));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
};

class DrawingCanvas final : public QWidget {
public:
    std::function<void(const Drawing &)> changed;
    std::function<void()> submit;
    std::function<void()> historyChanged;
    explicit DrawingCanvas(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("drawingCanvas"));
        setMinimumSize(240, 143);
        QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        policy.setHeightForWidth(true); setSizePolicy(policy);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_AcceptTouchEvents);
        setCursor(Qt::CrossCursor);
        setAccessibleName(QStringLiteral("Zeichenbrett 1920 mal 1080"));
        setToolTip(QStringLiteral("Links zeichnen, rechts senden. Mehrere Striche bleiben erhalten. Strg+Z: Rückgängig, Strg+Umschalt+Z: Wiederholen, Eingabe: Senden. Die weiße Fläche bleibt unabhängig von der Fenstergröße im Format 16:9."));
    }
    QSize sizeHint() const override { return {480, 270}; }
    int heightForWidth(int width) const override { return qRound(std::max(1, width - 8) * 9. / 16.) + 8; }
    const Drawing &drawing() const { return drawing_; }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool hasDrawing() const
    {
        for (const auto &s : drawing_.strokes) if (s.size() > 1) return true;
        return false;
    }
    void setDrawing(const Drawing &drawing)
    {
        drawing_ = drawing; undo_.clear(); redo_.clear(); drawingNow_ = false;
        if (historyChanged) historyChanged();
        update();
    }
    void undo()
    {
        if (undo_.empty()) return;
        redo_.push_back(drawing_); drawing_ = undo_.back(); undo_.pop_back();
        completeChange();
    }
    void redo()
    {
        if (redo_.empty()) return;
        undo_.push_back(drawing_); drawing_ = redo_.back(); redo_.pop_back();
        completeChange();
    }
    void clear()
    {
        if (drawing_.strokes.empty()) return;
        remember(); drawing_.strokes.clear(); drawing_.id = 0; completeChange();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().brush(QPalette::Window));
        const QRectF area = boardRect();
        painter.fillRect(area, palette().brush(QPalette::Base));
        painter.setClipRect(area);
        QColor grid = palette().color(QPalette::Text); grid.setAlpha(20);
        painter.setPen(QPen(grid, 1));
        for (int x = 1; x < 8; ++x)
            painter.drawLine(QPointF(area.left() + area.width() * x / 8., area.top()),
                             QPointF(area.left() + area.width() * x / 8., area.bottom()));
        for (int y = 1; y < 4; ++y)
            painter.drawLine(QPointF(area.left(), area.top() + area.height() * y / 4.),
                             QPointF(area.right(), area.top() + area.height() * y / 4.));
        QColor accent = visibleAccent(this); accent.setAlpha(80);
        painter.setPen(QPen(accent, 1, Qt::DashLine));
        painter.drawLine(QPointF(area.center().x() - 6, area.center().y()), QPointF(area.center().x() + 6, area.center().y()));
        painter.drawLine(QPointF(area.center().x(), area.center().y() - 6), QPointF(area.center().x(), area.center().y() + 6));
        paintDrawing(painter, area, drawing_, palette().color(QPalette::Text), 2.3);
        if (drawing_.strokes.empty()) {
            painter.setPen(palette().color(QPalette::PlaceholderText));
            painter.drawText(area.adjusted(14, 14, -14, -14), Qt::AlignCenter,
                             QStringLiteral("Hier zeichnen\nLinks zeichnen · Rechts senden"));
        }
        painter.setClipping(false);
        painter.setPen(QPen(hasFocus() ? visibleAccent(this) : palette().color(QPalette::Mid), hasFocus() ? 2 : 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(area);
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton) {
            if (drawingNow_) finishStroke();
            if (submit) submit(); event->accept(); return;
        }
        if (event->button() == Qt::LeftButton && boardRect().contains(mousePosition(event))) {
            setFocus(Qt::MouseFocusReason); beginStroke(mousePosition(event)); event->accept();
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (drawingNow_) { appendPoint(mousePosition(event)); event->accept(); }
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (drawingNow_ && event->button() == Qt::LeftButton) {
            appendPoint(mousePosition(event)); finishStroke(); event->accept();
        }
    }
    void contextMenuEvent(QContextMenuEvent *event) override { event->accept(); }
    void tabletEvent(QTabletEvent *event) override
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const QPointF position = event->position();
#else
        const QPointF position = event->posF();
#endif
        if (event->type() == QEvent::TabletPress && boardRect().contains(position)) beginStroke(position);
        else if (event->type() == QEvent::TabletMove && drawingNow_) appendPoint(position);
        else if (event->type() == QEvent::TabletRelease && drawingNow_) finishStroke();
        event->accept();
    }
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::TouchBegin || event->type() == QEvent::TouchUpdate || event->type() == QEvent::TouchEnd) {
            auto *touch = static_cast<QTouchEvent *>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (!touch->points().empty()) {
                const QPointF point = touch->points().front().position();
#else
            if (!touch->touchPoints().empty()) {
                const QPointF point = touch->touchPoints().front().pos();
#endif
                if (event->type() == QEvent::TouchBegin && boardRect().contains(point)) beginStroke(point);
                else if (drawingNow_) appendPoint(point);
            }
            if (event->type() == QEvent::TouchEnd && drawingNow_) finishStroke();
            event->accept(); return true;
        }
        if (event->type() == QEvent::TouchCancel && drawingNow_) { finishStroke(); event->accept(); return true; }
        return QWidget::event(event);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->matches(QKeySequence::Undo)) { undo(); event->accept(); }
        else if (redoKey(event)) { redo(); event->accept(); }
        else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) { if (submit) submit(); event->accept(); }
        else QWidget::keyPressEvent(event);
    }
private:
    QRectF boardRect() const
    {
        const double w = std::max(1., std::min(double(width() - 8), (height() - 8) * 16. / 9.));
        return {(width() - w) * .5, (height() - w * 9. / 16.) * .5, w, w * 9. / 16.};
    }
    void remember()
    {
        undo_.push_back(drawing_);
        if (undo_.size() > 64) undo_.erase(undo_.begin());
        redo_.clear();
    }
    void beginStroke(const QPointF &point)
    {
        if (drawing_.strokes.size() >= 128) return;
        remember(); drawing_.id = 0; drawing_.strokes.emplace_back(); drawingNow_ = true; appendPoint(point);
    }
    void appendPoint(const QPointF &point)
    {
        if (!drawingNow_ || drawing_.strokes.empty()) return;
        const QRectF area = boardRect();
        const Point value{bounded((point.x() - area.left()) / area.width(), 0, 1),
                          bounded((point.y() - area.top()) / area.height(), 0, 1)};
        auto &stroke = drawing_.strokes.back();
        std::size_t total = 0;
        for (const auto &existing : drawing_.strokes) total += existing.size();
        if (total >= 4096) return;
        if (stroke.empty() || std::hypot((value.x - stroke.back().x) * area.width(),
                                        (value.y - stroke.back().y) * area.height()) >= .7) {
            stroke.push_back(value); update();
        }
    }
    void finishStroke()
    {
        drawingNow_ = false;
        if (!drawing_.strokes.empty() && drawing_.strokes.back().size() < 2) {
            drawing_.strokes.pop_back();
            if (!undo_.empty()) undo_.pop_back();
        }
        completeChange();
    }
    void completeChange()
    {
        update(); if (changed) changed(drawing_); if (historyChanged) historyChanged();
    }
    Drawing drawing_;
    std::vector<Drawing> undo_, redo_;
    bool drawingNow_ = false;
};

class CurveCanvas final : public QWidget {
public:
    std::function<void(const std::vector<KeyPoint> &)> changed;
    std::function<void(int)> selectionChanged;
    std::function<void()> historyChanged;
    explicit CurveCanvas(QWidget *parent = nullptr) : QWidget(parent)
    {
        setObjectName(QStringLiteral("curveCanvas"));
        setMinimumSize(240, 170);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip(QStringLiteral("Eigene Kurve: Knoten ziehen, freie Stelle doppelt anklicken für einen neuen Knoten. Entf oder Rechtsklick entfernt einen inneren Knoten. Strg+Z macht Änderungen rückgängig. Endpunkte bleiben bei 0 und 100 % Zeit."));
        setAccessibleName(QStringLiteral("Grafischer Animationskurveneditor"));
    }
    QSize sizeHint() const override { return {440, 210}; }
    int selection() const { return selected_; }
    const std::vector<KeyPoint> &points() const { return animation_.points; }
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    void setAnimation(const Animation &animation, bool resetHistory)
    {
        animation_ = animation;
        if (animation_.points.size() < 2) animation_.points = {{0, 0}, {.5, 1}, {1, 0}};
        if (resetHistory) { undo_.clear(); redo_.clear(); selected_ = 0; }
        selected_ = std::min(selected_, int(animation_.points.size()) - 1);
        if (selectionChanged) selectionChanged(selected_);
        if (historyChanged) historyChanged(); update();
    }
    void editSelected(double time, double value)
    {
        if (animation_.mode != 8 || selected_ < 0 || selected_ >= int(animation_.points.size())) return;
        remember(); assignSelected(time, value); notify();
    }
    void resetCurve()
    {
        if (animation_.mode != 8) return;
        remember(); animation_.points = {{0, 0}, {.5, 1}, {1, 0}}; selected_ = 1; notify();
    }
    void closeLoop()
    {
        if (animation_.mode != 8 || animation_.points.size() < 2) return;
        remember(); animation_.points.back().value = animation_.points.front().value; notify();
    }
    void undo()
    {
        if (undo_.empty() || animation_.mode != 8) return;
        redo_.push_back(animation_.points); animation_.points = undo_.back(); undo_.pop_back();
        selected_ = std::min(selected_, int(animation_.points.size()) - 1); notify();
    }
    void redo()
    {
        if (redo_.empty() || animation_.mode != 8) return;
        undo_.push_back(animation_.points); animation_.points = redo_.back(); redo_.pop_back();
        selected_ = std::min(selected_, int(animation_.points.size()) - 1); notify();
    }
    void deleteSelected()
    {
        if (animation_.mode != 8 || selected_ <= 0 || selected_ >= int(animation_.points.size()) - 1) return;
        remember(); animation_.points.erase(animation_.points.begin() + selected_);
        selected_ = std::min(selected_, int(animation_.points.size()) - 2); notify();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), palette().brush(QPalette::Base));
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF graph = graphRect();
        QColor grid = palette().color(QPalette::Text); grid.setAlpha(30);
        painter.setPen(QPen(grid, 1));
        for (int i = 0; i <= 4; ++i) {
            const double x = graph.left() + graph.width() * i / 4.;
            const double y = graph.top() + graph.height() * i / 4.;
            painter.drawLine(QPointF(x, graph.top()), QPointF(x, graph.bottom()));
            painter.drawLine(QPointF(graph.left(), y), QPointF(graph.right(), y));
        }
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(QRectF(0, graph.top() - 8, graph.left() - 5, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("+1"));
        painter.drawText(QRectF(0, graph.center().y() - 8, graph.left() - 5, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("0"));
        painter.drawText(QRectF(0, graph.bottom() - 8, graph.left() - 5, 16), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("−1"));
        painter.drawText(QRectF(graph.left(), graph.bottom() + 5, 65, 20), Qt::AlignLeft, QStringLiteral("0 %"));
        painter.drawText(QRectF(graph.right() - 65, graph.bottom() + 5, 65, 20), Qt::AlignRight, QStringLiteral("100 % Zeit"));
        painter.save(); painter.setClipRect(graph.adjusted(-5, -5, 5, 5));
        QPainterPath path;
        Animation normalized = animation_; normalized.amount = 1.0; normalized.period = 1.0; normalized.phase = 0;
        // Use the actual core evaluator rather than a UI-only approximation.
        for (int i = 0; i <= 400; ++i) {
            const double t = i / 400.;
            const double v = animation_.mode == 8 ? curveAt(t) : animationValue(normalized, t);
            const QPointF point = graphPoint(t, bounded(v, -1.2, 1.2));
            if (i == 0) path.moveTo(point); else path.lineTo(point);
        }
        QColor fill = visibleAccent(this); fill.setAlpha(25);
        QPainterPath filled = path;
        filled.lineTo(graphPoint(1, 0)); filled.lineTo(graphPoint(0, 0)); filled.closeSubpath();
        painter.fillPath(filled, fill);
        painter.setPen(QPen(visibleAccent(this), 2.2)); painter.drawPath(path);
        if (animation_.mode == 8) {
            for (int i = 0; i < int(animation_.points.size()); ++i) {
                const auto &node = animation_.points[std::size_t(i)];
                painter.setPen(QPen(palette().color(QPalette::Base), 2));
                painter.setBrush(i == selected_ ? palette().color(QPalette::Text) : visibleAccent(this));
                painter.drawEllipse(graphPoint(node.time, node.value), i == selected_ ? 6 : 4.5, i == selected_ ? 6 : 4.5);
            }
        }
        painter.restore();
        painter.setPen(QPen(hasFocus() ? visibleAccent(this) : palette().color(QPalette::Mid), 1));
        painter.setBrush(Qt::NoBrush); painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (animation_.mode != 8) return;
        setFocus(Qt::MouseFocusReason);
        const int index = nodeAt(mousePosition(event));
        if (index < 0) return;
        selected_ = index;
        if (selectionChanged) selectionChanged(selected_);
        if (event->button() == Qt::RightButton) { deleteSelected(); event->accept(); return; }
        if (event->button() == Qt::LeftButton) { dragging_ = true; remember(); update(); event->accept(); }
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!dragging_) return;
        const auto point = normalizedPoint(mousePosition(event));
        assignSelected(point.time, point.value); notify(); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && dragging_) { dragging_ = false; event->accept(); }
    }
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (animation_.mode != 8 || event->button() != Qt::LeftButton || !graphRect().contains(mousePosition(event))) return;
        if (nodeAt(mousePosition(event)) >= 0 || animation_.points.size() >= 64) return;
        const auto point = normalizedPoint(mousePosition(event));
        for (const auto &existing : animation_.points) if (std::abs(existing.time - point.time) < .01) return;
        remember();
        const auto it = std::lower_bound(animation_.points.begin(), animation_.points.end(), point.time,
                                         [](const KeyPoint &p, double t) { return p.time < t; });
        selected_ = int(it - animation_.points.begin()); animation_.points.insert(it, point); notify(); event->accept();
    }
    void contextMenuEvent(QContextMenuEvent *event) override { event->accept(); }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->matches(QKeySequence::Undo)) { undo(); event->accept(); }
        else if (redoKey(event)) { redo(); event->accept(); }
        else if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) { deleteSelected(); event->accept(); }
        else if (animation_.mode == 8 && selected_ >= 0 && selected_ < int(animation_.points.size())) {
            const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? .001 : .01;
            auto point = animation_.points[std::size_t(selected_)];
            switch (event->key()) {
            case Qt::Key_Left: point.time -= step; break;
            case Qt::Key_Right: point.time += step; break;
            case Qt::Key_Up: point.value += step; break;
            case Qt::Key_Down: point.value -= step; break;
            default: QWidget::keyPressEvent(event); return;
            }
            editSelected(point.time, point.value); event->accept();
        } else QWidget::keyPressEvent(event);
    }
private:
    QRectF graphRect() const { return {33, 14, double(std::max(1, width() - 48)), double(std::max(1, height() - 48))}; }
    QPointF graphPoint(double time, double value) const
    {
        const auto rect = graphRect();
        return {rect.left() + time * rect.width(), rect.center().y() - value * rect.height() * .5};
    }
    KeyPoint normalizedPoint(QPointF point) const
    {
        const auto area = graphRect();
        return {bounded((point.x() - area.left()) / area.width(), 0, 1),
                bounded((area.center().y() - point.y()) / (area.height() * .5), -1, 1)};
    }
    int nodeAt(QPointF point) const
    {
        for (int i = int(animation_.points.size()) - 1; i >= 0; --i) {
            const auto &p = animation_.points[std::size_t(i)];
            const QPointF distance = graphPoint(p.time, p.value) - point;
            if (std::hypot(distance.x(), distance.y()) < 11) return i;
        }
        return -1;
    }
    double curveAt(double t) const
    {
        // The core evaluator includes the same Hermite interpolation and loop
        // behavior; use a tiny inset at t=1 so the final knot remains visible.
        Animation curve = animation_; curve.amount = 1.; curve.period = 1.; curve.phase = 0; curve.pingPong = false;
        return animationValue(curve, std::min(t, std::nextafter(1.0, 0.0)));
    }
    void remember()
    {
        undo_.push_back(animation_.points); if (undo_.size() > 64) undo_.erase(undo_.begin()); redo_.clear();
    }
    void assignSelected(double time, double value)
    {
        if (selected_ < 0 || selected_ >= int(animation_.points.size())) return;
        auto &point = animation_.points[std::size_t(selected_)];
        if (selected_ == 0) point.time = 0;
        else if (selected_ == int(animation_.points.size()) - 1) point.time = 1;
        else point.time = bounded(time, animation_.points[std::size_t(selected_ - 1)].time + .005,
                                 animation_.points[std::size_t(selected_ + 1)].time - .005);
        point.value = bounded(value, -1, 1);
    }
    void notify()
    {
        if (changed) changed(animation_.points);
        if (selectionChanged) selectionChanged(selected_);
        if (historyChanged) historyChanged(); update();
    }
    Animation animation_;
    int selected_ = 0;
    bool dragging_ = false;
    std::vector<std::vector<KeyPoint>> undo_, redo_;
};

struct PresetEntry {
    QString id, name, category, modified;
    QJsonObject data;
};

class PresetDialog final : public QDialog {
public:
    explicit PresetDialog(const Settings &current, QWidget *parent = nullptr)
        : QDialog(parent), current_(current)
    {
        setWindowTitle(QStringLiteral("Sternenflug · Effekt-Presets"));
        resize(620, 500); setMinimumSize(400, 380);
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(note(QStringLiteral("Ein Preset speichert den ganzen Effekt: Farben, Bewegung, Größen, Schweif und Animationen. Deine Handskizze und bereits fliegende Symbole bleiben beim Laden erhalten."), this));
        auto *filters = new QHBoxLayout;
        search_ = new QLineEdit(this);
        search_->setPlaceholderText(QStringLiteral("Preset suchen …"));
        search_->setAccessibleName(QStringLiteral("Presets durchsuchen"));
        filter_ = new QComboBox(this);
        filter_->setAccessibleName(QStringLiteral("Preset-Kategorie filtern"));
        filters->addWidget(search_, 1); filters->addWidget(filter_);
        layout->addLayout(filters);
        list_ = new QListWidget(this);
        list_->setObjectName(QStringLiteral("effectPresetList"));
        list_->setSelectionMode(QAbstractItemView::SingleSelection);
        list_->setTextElideMode(Qt::ElideMiddle);
        layout->addWidget(list_, 1);
        detail_ = note({}, this); detail_->setMinimumHeight(36); layout->addWidget(detail_);

        auto *actions = new QGridLayout;
        auto addAction = [&](int row, int column, const QString &name, const QString &tip,
                             std::function<void()> action) {
            auto *button = new QPushButton(name, this); button->setToolTip(tip);
            QObject::connect(button, &QPushButton::clicked, this, std::move(action));
            actions->addWidget(button, row, column); return button;
        };
        addAction(0, 0, QStringLiteral("Neu …"), QStringLiteral("Den aktuellen Effekt als neues Preset speichern."), [this] { create(); });
        save_ = addAction(0, 1, QStringLiteral("Speichern"), QStringLiteral("Das ausgewählte Preset mit dem aktuellen Effekt überschreiben."), [this] { saveCurrent(); });
        rename_ = addAction(0, 2, QStringLiteral("Umbenennen …"), QStringLiteral("Den Namen des ausgewählten Presets ändern."), [this] { rename(); });
        duplicate_ = addAction(1, 0, QStringLiteral("Duplizieren …"), QStringLiteral("Eine unabhängige Kopie des ausgewählten Presets anlegen."), [this] { duplicate(); });
        category_ = addAction(1, 1, QStringLiteral("Kategorie …"), QStringLiteral("Das ausgewählte Preset einer Kategorie zuordnen. Eine leere Eingabe entfernt die Kategorie."), [this] { changeCategory(); });
        remove_ = addAction(1, 2, QStringLiteral("Löschen …"), QStringLiteral("Nur das ausgewählte Preset aus der Bibliothek entfernen."), [this] { remove(); });
        layout->addLayout(actions);
        auto *bottom = new QHBoxLayout;
        auto *importButton = new QPushButton(QStringLiteral("Importieren …"), this);
        auto *exportButton = new QToolButton(this);
        exportButton->setText(QStringLiteral("Exportieren"));
        exportButton->setPopupMode(QToolButton::InstantPopup);
        auto *exportMenu = new QMenu(exportButton);
        exportMenu->addAction(QStringLiteral("Ausgewähltes Preset …"), this, [this] { exportSelected(); });
        exportMenu->addAction(QStringLiteral("Ganze Bibliothek …"), this, [this] { exportAll(); });
        exportButton->setMenu(exportMenu);
        bottom->addWidget(importButton); bottom->addWidget(exportButton); bottom->addStretch();
        load_ = new QPushButton(QStringLiteral("Laden"), this); load_->setDefault(true);
        auto *close = new QPushButton(QStringLiteral("Schließen"), this);
        bottom->addWidget(load_); bottom->addWidget(close); layout->addLayout(bottom);
        QObject::connect(importButton, &QPushButton::clicked, this, [this] { importFile(); });
        QObject::connect(load_, &QPushButton::clicked, this, [this] { loadSelected(); });
        QObject::connect(close, &QPushButton::clicked, this, &QDialog::reject);
        QObject::connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) { loadSelected(); });
        QObject::connect(list_, &QListWidget::currentRowChanged, this, [this](int) { updateSelection(); });
        QObject::connect(search_, &QLineEdit::textChanged, this, [this] { refreshList(); });
        QObject::connect(filter_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { refreshList(); });
        read(); refreshCategories(); refreshList();
    }
    const Settings &loadedSettings() const { return loaded_; }
private:
    QString libraryPath() const
    {
        return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/sternenflug/presets.json");
    }
    void error(const QString &message) { QMessageBox::warning(this, QStringLiteral("Effekt-Presets"), message); }
    QJsonObject snapshot() const { return encodePreset(current_, Drawing{}); }
    static QJsonObject serialize(const PresetEntry &entry)
    {
        return {{QStringLiteral("id"), entry.id}, {QStringLiteral("name"), entry.name},
                {QStringLiteral("category"), entry.category}, {QStringLiteral("modified"), entry.modified},
                {QStringLiteral("preset"), entry.data}};
    }
    QJsonObject library() const
    {
        QJsonArray array;
        for (const auto &entry : entries_) array.append(serialize(entry));
        return {{QStringLiteral("libraryVersion"), 1}, {QStringLiteral("presets"), array}};
    }
    static bool validPreset(const QJsonObject &object, QString *message)
    {
        Settings settings; std::shared_ptr<Drawing> unused;
        return decodePreset(object, settings, unused, message);
    }
    void read()
    {
        QFile file(libraryPath());
        if (!file.exists()) return;
        if (!file.open(QIODevice::ReadOnly)) { error(QStringLiteral("Die Preset-Bibliothek konnte nicht geöffnet werden.\n") + file.errorString()); writable_ = false; return; }
        if (file.size() > 8 * 1024 * 1024) { error(QStringLiteral("Die Preset-Bibliothek ist zu groß. Sie wurde unverändert gelassen.")); writable_ = false; return; }
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()
            || doc.object().value(QStringLiteral("libraryVersion")).toInt() != 1
            || !doc.object().value(QStringLiteral("presets")).isArray()) {
            error(QStringLiteral("Die Preset-Bibliothek ist ungültig und wurde unverändert gelassen. Du kannst weiterhin einzelne Presets importieren und exportieren."));
            writable_ = false; return;
        }
        const auto array = doc.object().value(QStringLiteral("presets")).toArray();
        if (array.size() > 2000) { error(QStringLiteral("Die Preset-Bibliothek enthält zu viele Einträge und bleibt unverändert.")); writable_ = false; return; }
        for (const auto &value : array) {
            const auto obj = value.toObject(); QString message;
            const auto data = obj.value(QStringLiteral("preset")).toObject();
            if (!validPreset(data, &message)) { writable_ = false; error(QStringLiteral("Ein Eintrag der Bibliothek ist ungültig. Die Datei bleibt unverändert.\n") + message); continue; }
            PresetEntry entry{obj.value(QStringLiteral("id")).toString(), obj.value(QStringLiteral("name")).toString().trimmed().left(120),
                              obj.value(QStringLiteral("category")).toString().trimmed().left(80), obj.value(QStringLiteral("modified")).toString(), data};
            if (entry.id.isEmpty()) entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (entry.name.isEmpty()) entry.name = QStringLiteral("Unbenannt");
            entries_.push_back(std::move(entry));
            if (entries_.size() >= 2000) break;
        }
    }
    bool write()
    {
        if (!writable_) { error(QStringLiteral("Die vorhandene Bibliothek ist beschädigt und wird nicht überschrieben. Exportiere deinen aktuellen Stand zuerst in eine neue Datei.")); return false; }
        const QFileInfo info(libraryPath());
        if (!QDir().mkpath(info.absolutePath())) { error(QStringLiteral("Der Ordner für Presets konnte nicht erstellt werden.")); return false; }
        QSaveFile file(libraryPath());
        if (!file.open(QIODevice::WriteOnly)) { error(QStringLiteral("Die Preset-Bibliothek konnte nicht gespeichert werden.\n") + file.errorString()); return false; }
        const auto data = QJsonDocument(library()).toJson(QJsonDocument::Indented);
        if (file.write(data) != data.size() || !file.commit()) { error(QStringLiteral("Die Preset-Bibliothek konnte nicht vollständig gespeichert werden.\n") + file.errorString()); return false; }
        return true;
    }
    QString selectedId() const { return list_->currentItem() ? list_->currentItem()->data(Qt::UserRole).toString() : QString{}; }
    int selectedIndex() const
    {
        const auto id = selectedId();
        for (int i = 0; i < int(entries_.size()); ++i) if (entries_[std::size_t(i)].id == id) return i;
        return -1;
    }
    void refreshCategories()
    {
        const auto selected = filter_->count() ? filter_->currentData().toString() : QStringLiteral("*");
        const QSignalBlocker blocker(filter_); filter_->clear();
        filter_->addItem(QStringLiteral("Alle Kategorien"), QStringLiteral("*"));
        filter_->addItem(QStringLiteral("Ohne Kategorie"), QString{});
        QStringList categories;
        for (const auto &entry : entries_) if (!entry.category.isEmpty() && !categories.contains(entry.category)) categories.append(entry.category);
        categories.sort(Qt::CaseInsensitive);
        for (const auto &category : categories) filter_->addItem(category, category);
        filter_->setCurrentIndex(std::max(0, filter_->findData(selected)));
    }
    void refreshList(const QString &select = {})
    {
        const QString wanted = select.isEmpty() ? selectedId() : select;
        const QSignalBlocker blocker(list_); list_->clear();
        const auto category = filter_->currentData().toString();
        const auto query = search_->text().trimmed();
        for (const auto &entry : entries_) {
            if (category != QStringLiteral("*") && entry.category != category) continue;
            if (!query.isEmpty() && !(entry.name + QStringLiteral(" ") + entry.category).contains(query, Qt::CaseInsensitive)) continue;
            auto *item = new QListWidgetItem(entry.name + (entry.category.isEmpty() ? QString{} : QStringLiteral("  ·  ") + entry.category), list_);
            item->setData(Qt::UserRole, entry.id);
            item->setToolTip(entry.name + QStringLiteral("\n") + (entry.category.isEmpty() ? QStringLiteral("Ohne Kategorie") : entry.category));
            if (entry.id == wanted) list_->setCurrentItem(item);
        }
        if (!list_->currentItem() && list_->count()) list_->setCurrentRow(0);
        updateSelection();
    }
    void updateSelection()
    {
        const int index = selectedIndex(); const bool selected = index >= 0;
        for (auto *button : {save_, rename_, duplicate_, category_, remove_, load_}) button->setEnabled(selected);
        if (!selected) { detail_->setText(QStringLiteral("Noch kein Preset gewählt. „Neu“ speichert den aktuellen Effekt als Preset.")); return; }
        const auto &entry = entries_[std::size_t(index)];
        detail_->setText(entry.name + QStringLiteral("\n") + (entry.category.isEmpty() ? QStringLiteral("Ohne Kategorie") : entry.category));
    }
    QString askName(const QString &title, const QString &initial, bool *ok)
    {
        const auto value = QInputDialog::getText(this, title, QStringLiteral("Name"), QLineEdit::Normal, initial, ok).trimmed().left(120);
        if (*ok && value.isEmpty()) { error(QStringLiteral("Bitte einen Namen eingeben.")); *ok = false; }
        return value;
    }
    void create()
    {
        bool ok; const auto name = askName(QStringLiteral("Neues Effekt-Preset"), QStringLiteral("Mein Sternenflug"), &ok); if (!ok) return;
        PresetEntry entry{QUuid::createUuid().toString(QUuid::WithoutBraces), name, {}, QDateTime::currentDateTimeUtc().toString(Qt::ISODate), snapshot()};
        const auto id = entry.id; entries_.push_back(std::move(entry));
        if (!write()) { entries_.pop_back(); return; }
        { const QSignalBlocker blocker(filter_); filter_->setCurrentIndex(0); }
        search_->clear(); refreshCategories(); refreshList(id);
    }
    void saveCurrent()
    {
        const int index = selectedIndex(); if (index < 0) return;
        auto before = entries_[std::size_t(index)]; auto &entry = entries_[std::size_t(index)];
        entry.data = snapshot(); entry.modified = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        if (!write()) entry = before;
        refreshList(entry.id);
    }
    void rename()
    {
        const int index = selectedIndex(); if (index < 0) return;
        auto &entry = entries_[std::size_t(index)]; bool ok;
        const auto name = askName(QStringLiteral("Preset umbenennen"), entry.name, &ok); if (!ok) return;
        const auto before = entry.name; entry.name = name; if (!write()) entry.name = before; refreshList(entry.id);
    }
    void duplicate()
    {
        const int index = selectedIndex(); if (index < 0) return;
        auto entry = entries_[std::size_t(index)]; bool ok;
        const auto name = askName(QStringLiteral("Preset duplizieren"), entry.name + QStringLiteral(" Kopie"), &ok); if (!ok) return;
        entry.id = QUuid::createUuid().toString(QUuid::WithoutBraces); entry.name = name;
        entry.modified = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        const auto id = entry.id; entries_.push_back(std::move(entry)); if (!write()) { entries_.pop_back(); return; } refreshList(id);
    }
    void changeCategory()
    {
        const int index = selectedIndex(); if (index < 0) return;
        QStringList categories{QString{}};
        for (const auto &entry : entries_) if (!categories.contains(entry.category)) categories.append(entry.category);
        auto &entry = entries_[std::size_t(index)]; bool ok;
        const auto category = QInputDialog::getItem(this, QStringLiteral("Preset organisieren"), QStringLiteral("Kategorie (frei eingeben oder auswählen)"),
                                                   categories, std::max(0, int(categories.indexOf(entry.category))), true, &ok).trimmed().left(80);
        if (!ok) return;
        const auto before = entry.category; entry.category = category; if (!write()) entry.category = before;
        const auto id = entry.id; refreshCategories(); { const QSignalBlocker blocker(filter_); filter_->setCurrentIndex(0); } refreshList(id);
    }
    void remove()
    {
        const int index = selectedIndex(); if (index < 0) return;
        const auto before = entries_[std::size_t(index)];
        if (QMessageBox::question(this, QStringLiteral("Preset löschen"), QStringLiteral("„%1“ aus der Preset-Bibliothek löschen?").arg(before.name),
                                   QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) return;
        entries_.erase(entries_.begin() + index);
        if (!write()) entries_.insert(entries_.begin() + index, before);
        refreshCategories(); refreshList();
    }
    bool writeExport(const QString &path, const QJsonObject &object)
    {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { error(QStringLiteral("Die Exportdatei konnte nicht geöffnet werden.\n") + file.errorString()); return false; }
        const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
        if (file.write(bytes) != bytes.size() || !file.commit()) { error(QStringLiteral("Der Export konnte nicht gespeichert werden.\n") + file.errorString()); return false; }
        return true;
    }
    void exportSelected()
    {
        const int index = selectedIndex(); if (index < 0) { error(QStringLiteral("Bitte zuerst ein Preset wählen.")); return; }
        const auto &entry = entries_[std::size_t(index)];
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Effekt-Preset exportieren"), entry.name + QStringLiteral(".sternenflug.json"), QStringLiteral("Sternenflug-Preset (*.json)"));
        if (path.isEmpty()) return;
        auto data = entry.data; data.insert(QStringLiteral("name"), entry.name); data.insert(QStringLiteral("category"), entry.category); writeExport(path, data);
    }
    void exportAll()
    {
        const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("Preset-Bibliothek exportieren"), QStringLiteral("Sternenflug-Presets.json"), QStringLiteral("Sternenflug-Bibliothek (*.json)"));
        if (!path.isEmpty()) writeExport(path, library());
    }
    void importFile()
    {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Sternenflug-Presets importieren"), {}, QStringLiteral("Sternenflug-Presets (*.json)"));
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) { error(QStringLiteral("Die Datei konnte nicht geöffnet werden.\n") + file.errorString()); return; }
        if (file.size() > 8 * 1024 * 1024) { error(QStringLiteral("Die Datei ist für eine Preset-Bibliothek zu groß.")); return; }
        QJsonParseError parse; const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) { error(QStringLiteral("Die Datei enthält kein gültiges Sternenflug-Preset.")); return; }
        const auto object = doc.object(); QJsonArray candidates;
        if (object.contains(QStringLiteral("libraryVersion"))) {
            if (object.value(QStringLiteral("libraryVersion")).toInt() != 1 || !object.value(QStringLiteral("presets")).isArray()) { error(QStringLiteral("Diese Bibliotheksversion wird nicht unterstützt.")); return; }
            candidates = object.value(QStringLiteral("presets")).toArray();
        } else {
            candidates.append(QJsonObject{{QStringLiteral("name"), object.value(QStringLiteral("name")).toString(QFileInfo(path).completeBaseName())},
                                           {QStringLiteral("category"), object.value(QStringLiteral("category"))},
                                           {QStringLiteral("preset"), object}});
        }
        if (candidates.size() > 2000 || entries_.size() + std::size_t(candidates.size()) > 2000) { error(QStringLiteral("Die Bibliothek darf höchstens 2000 Presets enthalten.")); return; }
        std::vector<PresetEntry> imported;
        for (const auto &candidate : candidates) {
            const auto item = candidate.toObject(); const auto preset = item.value(QStringLiteral("preset")).toObject(); QString message;
            if (!validPreset(preset, &message)) { error(QStringLiteral("Der Import wurde nicht übernommen: Ein Preset ist ungültig.\n") + message); return; }
            auto name = item.value(QStringLiteral("name")).toString().trimmed().left(120); if (name.isEmpty()) name = QStringLiteral("Importiertes Preset");
            imported.push_back({QUuid::createUuid().toString(QUuid::WithoutBraces), name,
                                item.value(QStringLiteral("category")).toString().trimmed().left(80), QDateTime::currentDateTimeUtc().toString(Qt::ISODate), preset});
        }
        if (imported.empty()) { error(QStringLiteral("Die Datei enthält keine Presets.")); return; }
        const auto oldSize = entries_.size(); entries_.insert(entries_.end(), imported.begin(), imported.end());
        if (!write()) { entries_.resize(oldSize); return; }
        refreshCategories(); { const QSignalBlocker blocker(filter_); filter_->setCurrentIndex(0); } search_->clear(); refreshList(imported.front().id);
    }
    void loadSelected()
    {
        const int index = selectedIndex(); if (index < 0) return;
        std::shared_ptr<Drawing> ignored; QString message;
        if (!decodePreset(entries_[std::size_t(index)].data, loaded_, ignored, &message)) { error(QStringLiteral("Das Preset konnte nicht geladen werden.\n") + message); return; }
        accept();
    }
    Settings current_, loaded_;
    std::vector<PresetEntry> entries_;
    bool writable_ = true;
    QLineEdit *search_ = nullptr;
    QComboBox *filter_ = nullptr;
    QListWidget *list_ = nullptr;
    QLabel *detail_ = nullptr;
    QPushButton *save_ = nullptr, *rename_ = nullptr, *duplicate_ = nullptr,
                *category_ = nullptr, *remove_ = nullptr, *load_ = nullptr;
};

class Editor final : public QWidget {
public:
    Editor(const Settings &settings, const Drawing &draft, EditorCallbacks callbacks,
           QWidget *parent)
        : QWidget(parent), settings_(sanitizeSettings(settings)), callbacks_(std::move(callbacks)),
          preferences_(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("Sternenflug"), QStringLiteral("Editor"))
    {
        setObjectName(QStringLiteral("sternenflugEditor"));
        setMinimumSize(340, 340);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAutoFillBackground(true);
        baseFont_ = font();
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(8, 8, 8, 8); layout->setSpacing(7);
        auto *header = new QHBoxLayout;
        auto *title = new QLabel(QStringLiteral("Sternenflug"), this);
        QFont titleFont = title->font(); titleFont.setBold(true); title->setFont(titleFont);
        header->addWidget(title, 1);
        auto *presets = new QPushButton(QStringLiteral("Presets …"), this);
        presets->setObjectName(QStringLiteral("managePresets"));
        presets->setToolTip(QStringLiteral("Den ganzen Effekt speichern, laden und in Kategorien organisieren. Handskizzen bleiben davon unabhängig."));
        header->addWidget(presets);
        auto *view = new QToolButton(this);
        view->setText(QStringLiteral("Ansicht")); view->setPopupMode(QToolButton::InstantPopup);
        view->setAccessibleName(QStringLiteral("Ansicht und Oberflächengröße"));
        auto *viewMenu = new QMenu(view);
        auto *themeMenu = viewMenu->addMenu(QStringLiteral("Farbschema"));
        auto *themeActions = new QActionGroup(themeMenu); themeActions->setExclusive(true);
        const QStringList themeNames{QStringLiteral("OBS folgen"), QStringLiteral("Hell"), QStringLiteral("Dunkel")};
        theme_ = std::clamp(preferences_.value(QStringLiteral("theme"), 0).toInt(), 0, 2);
        for (int i = 0; i < themeNames.size(); ++i) {
            auto *action = themeMenu->addAction(themeNames[i]); action->setCheckable(true); action->setChecked(i == theme_);
            action->setObjectName(QStringLiteral("theme_%1").arg(i)); themeActions->addAction(action);
            QObject::connect(action, &QAction::triggered, this, [this, i] { applyTheme(i); preferences_.setValue(QStringLiteral("theme"), i); });
        }
        auto *scaleMenu = viewMenu->addMenu(QStringLiteral("Oberfläche skalieren"));
        auto *scaleActions = new QActionGroup(scaleMenu); scaleActions->setExclusive(true);
        scale_ = preferences_.value(QStringLiteral("scale"), 100).toInt();
        if (scale_ != 85 && scale_ != 100 && scale_ != 115 && scale_ != 125 && scale_ != 150) scale_ = 100;
        for (const int factor : {85, 100, 115, 125, 150}) {
            auto *action = scaleMenu->addAction(QStringLiteral("%1 %").arg(factor)); action->setCheckable(true); action->setChecked(scale_ == factor);
            action->setObjectName(QStringLiteral("scale_%1").arg(factor)); scaleActions->addAction(action);
            QObject::connect(action, &QAction::triggered, this, [this, factor] { applyScale(factor); preferences_.setValue(QStringLiteral("scale"), factor); });
        }
        view->setMenu(viewMenu); header->addWidget(view); layout->addLayout(header);

        auto *transport = new QHBoxLayout; transport->setSpacing(4);
        send_ = toolButton(this, QStringLiteral("Senden"), QStyle::SP_MediaPlay,
                           QStringLiteral("Die aktuelle Handskizze einmischen. Erneutes Senden startet ein neues Einmischfenster; bereits fliegende Motive bleiben erhalten."));
        send_->setObjectName(QStringLiteral("sendDrawing"));
        pause_ = toolButton(this, QStringLiteral("Pause"), QStyle::SP_MediaPause,
                            QStringLiteral("Flug, Animation und Einmischzeit gemeinsam anhalten oder fortsetzen. Die Zeichnung bleibt erhalten."));
        pause_->setObjectName(QStringLiteral("pausePlayback")); pause_->setCheckable(true);
        auto *stop = toolButton(this, QStringLiteral("Beenden"), QStyle::SP_MediaStop,
                                QStringLiteral("Einmischen beenden: Es entstehen keine neuen eigenen Motive. Bereits fliegende Motive fliegen zu Ende; Sterne bleiben sichtbar."));
        stop->setObjectName(QStringLiteral("stopEmission"));
        auto *restart = toolButton(this, {}, QStyle::SP_BrowserReload,
                                   QStringLiteral("Effekt neu starten: Flug und Animationszeit auf den Anfang setzen. Die Handskizze bleibt erhalten."));
        restart->setObjectName(QStringLiteral("restartPlayback"));
        restart->setAccessibleName(QStringLiteral("Effekt neu starten"));
        restart->setToolButtonStyle(Qt::ToolButtonIconOnly);
        transport->addWidget(send_, 1); transport->addWidget(pause_, 1);
        transport->addWidget(stop, 1); transport->addWidget(restart); layout->addLayout(transport);
        status_ = new QLabel(QStringLiteral("Quelle auswählen, zeichnen und senden."), this);
        status_->setObjectName(QStringLiteral("sourceStatus"));
        status_->setWordWrap(true); status_->setTextFormat(Qt::PlainText);
        QSizePolicy statusPolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
        statusPolicy.setHeightForWidth(true); status_->setSizePolicy(statusPolicy);
        status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        status_->setToolTip(QStringLiteral("Zielquelle und aktueller Ausgabezustand. Alle Regler gelten für die oben gewählte Sternenflug-Quelle."));
        layout->addWidget(status_);

        tabs_ = new QTabWidget(this); tabs_->setObjectName(QStringLiteral("editorTabs"));
        tabs_->setDocumentMode(true); tabs_->setUsesScrollButtons(true);
        tabs_->tabBar()->setElideMode(Qt::ElideRight);
        layout->addWidget(tabs_, 1);
        createColors(); createMovement(); createSizes(); createDrawing(); createAnimation();
        QObject::connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (!loading_ && tabs_->widget(index) == animationPage_) refreshAnimation(false);
        });

        QObject::connect(send_, &QToolButton::clicked, this, [this] { submit(); });
        QObject::connect(pause_, &QToolButton::toggled, this, [this](bool paused) {
            setPlayback(paused); if (!loading_ && callbacks_.pauseChanged) callbacks_.pauseChanged(paused);
        });
        QObject::connect(stop, &QToolButton::clicked, this, [this] { if (callbacks_.stopEmission) callbacks_.stopEmission(); });
        QObject::connect(restart, &QToolButton::clicked, this, [this] {
            if (callbacks_.restart) callbacks_.restart();
        });
        QObject::connect(presets, &QPushButton::clicked, this, [this] {
            const QPointer<Editor> safeThis(this);
            const QPointer<PresetDialog> dialog = new PresetDialog(settings_, this);
            const int result = dialog->exec();
            if (!safeThis || !dialog) return;
            if (result != QDialog::Accepted) { dialog->deleteLater(); return; }
            // Loading an effect preset changes neither the current drawing nor
            // any running immutable drawing generation, and never triggers it.
            settings_ = sanitizeSettings(dialog->loadedSettings()); dialog->deleteLater(); refreshSettings(false);
            notifySettings();
        });
        load(settings_, draft);
        applyTheme(theme_); applyScale(scale_);
    }

    void setStatus(const QString &value) { status_->setText(value); }
    void setPlayback(bool paused)
    {
        const QSignalBlocker blocker(pause_);
        pause_->setChecked(paused);
        pause_->setText(paused ? QStringLiteral("Weiter") : QStringLiteral("Pause"));
        pause_->setIcon(style()->standardIcon(paused ? QStyle::SP_MediaPlay : QStyle::SP_MediaPause));
        pause_->setAccessibleName(paused ? QStringLiteral("Wiedergabe fortsetzen") : QStringLiteral("Wiedergabe pausieren"));
    }
    void load(const Settings &settings, const Drawing &draft)
    {
        loading_ = true; settings_ = sanitizeSettings(settings);
        refreshSettings(true); canvas_->setDrawing(draft);
        submitted_->drawing = {}; submitted_->update(); submitted_->hide(); sentLabel_->setText(QStringLiteral("Noch nichts gesendet"));
        drawingHint_->setText(QStringLiteral("Links zeichnen · Rechts oder „Senden“ zum Einmischen. Die Skizze bleibt zum erneuten Senden erhalten."));
        loading_ = false; refreshDrawingHistory();
    }
    void loadDrift(const Settings &settings)
    {
        const bool before = loading_; loading_ = true;
        for (auto id : {Param::UVX, Param::UVY, Param::DriftX, Param::DriftY}) {
            const auto index = static_cast<std::size_t>(id);
            settings_.set(id, settings.get(id)); settings_.animations[index] = settings.animations[index];
            if (rows_[index]) { rows_[index]->setValue(settings_.get(id)); rows_[index]->setAnimated(settings_.animations[index].mode != 0); }
        }
        refreshDrift(false); refreshDriftAnimation();
        if (currentTarget() == Param::UVX || currentTarget() == Param::UVY || currentTarget() == Param::DriftX || currentTarget() == Param::DriftY)
            refreshAnimation(false);
        loading_ = before;
    }
protected:
    void changeEvent(QEvent *event) override
    {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::PaletteChange) {
            // Native custom widgets read palette() while painting; no global
            // palette mutation is required for the host's theme to propagate.
            if (colorWheel_) colorWheel_->update();
            if (ramp_) ramp_->update();
            if (canvas_) canvas_->update();
            if (curve_) curve_->update();
        }
    }
private:
    struct Page { QWidget *widget; QVBoxLayout *layout; };
    Page page(const QString &name, const QString &tooltip)
    {
        auto *scroll = new QScrollArea(this); scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto *content = new QWidget(scroll); content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        auto *layout = new QVBoxLayout(content); layout->setContentsMargins(4, 7, 8, 7); layout->setSpacing(8);
        scroll->setWidget(content); const int index = tabs_->addTab(scroll, name); tabs_->setTabToolTip(index, tooltip);
        return {content, layout};
    }
    QVBoxLayout *section(QVBoxLayout *parent, const QString &title)
    {
        auto *group = new QGroupBox(title, this); auto *layout = new QVBoxLayout(group);
        layout->setContentsMargins(9, 12, 9, 8); layout->setSpacing(4); parent->addWidget(group); return layout;
    }
    ParamRow *addParam(QVBoxLayout *layout, Param id)
    {
        const auto &definition = parameterDef(id);
        auto *row = new ParamRow(definition, settings_.get(id),
            [this, id](double value) {
                if (loading_) return;
                settings_.set(id, value);
                if (id == Param::HueA || id == Param::SatA || id == Param::ValA ||
                    id == Param::HueB || id == Param::SatB || id == Param::ValB) refreshColors();
                notifySettings();
            }, [this, id] { openAnimation(id); }, this);
        rows_[static_cast<std::size_t>(id)] = row;
        layout->addWidget(row); return row;
    }
    void createColors()
    {
        auto colors = page(QStringLiteral("Farbe"), QStringLiteral("Farben, Farbvariation, Helligkeit und Deckkraft"));
        auto *picker = section(colors.layout, QStringLiteral("Farben A und B"));
        auto *colorSlots = new QHBoxLayout; auto *slotGroup = new QButtonGroup(this); slotGroup->setExclusive(true);
        for (int i = 0; i < 2; ++i) {
            colorButtons_[i] = new QPushButton(i == 0 ? QStringLiteral("Farbe A") : QStringLiteral("Farbe B"), this);
            colorButtons_[i]->setObjectName(i == 0 ? QStringLiteral("selectColorA") : QStringLiteral("selectColorB"));
            colorButtons_[i]->setCheckable(true); colorButtons_[i]->setChecked(i == 0);
            colorButtons_[i]->setToolTip(i == 0 ? QStringLiteral("Startfarbe des Farbverlaufs bearbeiten.") : QStringLiteral("Zweite Farbe des Farbverlaufs bearbeiten."));
            slotGroup->addButton(colorButtons_[i], i); colorSlots->addWidget(colorButtons_[i]);
            QObject::connect(colorButtons_[i], &QPushButton::clicked, this, [this, i] { activeColor_ = i; colorValues_->setCurrentIndex(i); refreshColors(); });
        }
        picker->addLayout(colorSlots);
        colorWheel_ = new HueWheel(this); picker->addWidget(colorWheel_);
        colorWheel_->changed = [this](double hue, double saturation) {
            if (loading_) return;
            settings_.set(activeColor_ ? Param::HueB : Param::HueA, hue);
            settings_.set(activeColor_ ? Param::SatB : Param::SatA, saturation);
            for (auto id : {Param::HueA, Param::SatA, Param::HueB, Param::SatB}) rows_[static_cast<std::size_t>(id)]->setValue(settings_.get(id));
            refreshColors(); notifySettings();
        };
        colorValues_ = new QStackedWidget(this);
        for (int i = 0; i < 2; ++i) {
            auto *values = new QWidget(colorValues_); auto *layout = new QVBoxLayout(values);
            layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(2);
            addParam(layout, i ? Param::HueB : Param::HueA);
            addParam(layout, i ? Param::SatB : Param::SatA);
            addParam(layout, i ? Param::ValB : Param::ValA);
            colorValues_->addWidget(values);
        }
        picker->addWidget(colorValues_);
        ramp_ = new ColorRamp(this); picker->addWidget(ramp_);
        auto *modeLine = new QHBoxLayout; modeLine->addWidget(new QLabel(QStringLiteral("Farbverlauf"), this));
        colorMode_ = new QComboBox(this); colorMode_->setObjectName(QStringLiteral("colorInterpolation"));
        colorMode_->addItem(QStringLiteral("Farbkreis (HSV)"), 1); colorMode_->addItem(QStringLiteral("Lichtmischung (RGB)"), 0);
        colorMode_->setToolTip(QStringLiteral("HSV folgt dem kürzesten Weg auf dem Farbkreis. RGB mischt die Lichtfarben im linearen Farbraum."));
        modeLine->addWidget(colorMode_, 1); picker->addLayout(modeLine);
        QObject::connect(colorMode_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            if (loading_) return; settings_.colorMode = colorMode_->currentData().toInt(); refreshColors(); notifySettings();
        });
        auto *variation = section(colors.layout, QStringLiteral("Variation und Licht"));
        for (auto id : {Param::ColorVariation, Param::HueVariation, Param::SatVariation, Param::Brightness, Param::Opacity}) addParam(variation, id);
        variation->addWidget(note(QStringLiteral("Leere Bereiche bleiben transparent. Deckkraft und Helligkeit wirken sofort auf Sterne und Symbole."), this));
        colors.layout->addStretch();
    }
    void createMovement()
    {
        auto movement = page(QStringLiteral("Flug"), QStringLiteral("Bewegung, Sternendichte, Bildverschiebung und Schweif"));
        auto *templates = section(movement.layout, QStringLiteral("Bewegungsvorlagen"));
        auto *templateLine = new QHBoxLayout;
        motionPreset_ = new QComboBox(this); motionPreset_->setObjectName(QStringLiteral("motionPreset"));
        motionPreset_->addItem(QStringLiteral("Regen"), QStringLiteral("rain"));
        motionPreset_->addItem(QStringLiteral("Sanft abwärts"), QStringLiteral("soft"));
        motionPreset_->addItem(QStringLiteral("Sternenflug"), QStringLiteral("starflight"));
        motionPreset_->setAccessibleName(QStringLiteral("Bewegungsvorlage auswählen"));
        motionPreset_->setToolTip(QStringLiteral("Eine Vorlage auswählen und dann Anwenden drücken. Die Auswahl allein ändert den Effekt nicht."));
        auto *apply = new QPushButton(QStringLiteral("Anwenden"), this); apply->setObjectName(QStringLiteral("applyMotionPreset"));
        apply->setToolTip(QStringLiteral("Setzt Bewegungsmodus, Flugtempo, Partikeldichte und Schweifwerte sowie deren Zeitsteuerung. Position und Drift werden sanft neutralisiert. Farben, Größen, Handskizze, Symbolanteil und Einmischdauer bleiben erhalten. Laufende Motive werden nicht gelöscht."));
        templateLine->addWidget(motionPreset_, 1); templateLine->addWidget(apply); templates->addLayout(templateLine);
        motionModeHint_ = note({}, this); motionModeHint_->setObjectName(QStringLiteral("motionModeHint")); templates->addWidget(motionModeHint_);
        QObject::connect(apply, &QPushButton::clicked, this, [this] { applyMovementTemplate(); });
        auto *flight = section(movement.layout, QStringLiteral("Bewegung"));
        addParam(flight, Param::Speed); addParam(flight, Param::Density);
        flight->addWidget(note(QStringLiteral("Das Flugtempo ist unabhängig davon, wie lange neue eigene Motive eingemischt werden."), this));
        createDrift(movement.layout);
        auto *shift = section(movement.layout, QStringLiteral("Feste Bildposition"));
        for (auto id : {Param::UVX, Param::UVY}) addParam(shift, id);
        shift->addWidget(note(QStringLiteral("Verschiebt das Bild an eine feste Position. Die Drift darüber bewegt es fortlaufend."), this));
        auto *trails = section(movement.layout, QStringLiteral("Schweif"));
        for (auto id : {Param::TrailLength, Param::TrailDensity, Param::TrailFalloff}) addParam(trails, id);
        auto *random = section(movement.layout, QStringLiteral("Zufallsvariation"));
        auto *line = new QHBoxLayout; line->addWidget(new QLabel(QStringLiteral("Variante"), this), 1);
        seed_ = new QDoubleSpinBox(this); seed_->setObjectName(QStringLiteral("randomSeed")); seed_->setRange(0, 4294967295.0);
        seed_->setDecimals(0); seed_->setSingleStep(1);
        seed_->setKeyboardTracking(false); seed_->setToolTip(QStringLiteral("Gleicher Seed ergibt die gleiche Zufallsfolge. Ein neuer Seed verändert künftige Zufallsentscheidungen."));
        line->addWidget(seed_);
        auto *shuffle = new QToolButton(this); shuffle->setText(QStringLiteral("Würfeln")); shuffle->setToolTip(QStringLiteral("Eine neue Zufallsvariation wählen."));
        line->addWidget(shuffle); random->addLayout(line);
        QObject::connect(seed_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { if (!loading_) { settings_.seed = static_cast<std::uint32_t>(value); notifySettings(); } });
        QObject::connect(shuffle, &QToolButton::clicked, this, [this] {
            const auto value = QUuid::createUuid().data1 & 0x7fffffffU; seed_->setValue(int(value));
        });
        movement.layout->addStretch();
    }
    void applyMovementTemplate()
    {
        const auto selected = motionPreset_->currentData().toString();
        const bool rain = selected == QStringLiteral("rain"), soft = selected == QStringLiteral("soft");
        settings_.motionMode = rain || soft ? 1 : 0;
        settings_.set(Param::Speed, rain ? 2.4 : (soft ? .5 : 1.));
        settings_.set(Param::Density, rain ? 650 : (soft ? 220 : 350));
        settings_.set(Param::TrailLength, soft ? 0 : 80);
        settings_.set(Param::TrailDensity, rain ? 100 : (soft ? 0 : 35));
        settings_.set(Param::TrailFalloff, rain ? 1.8 : 1.5);
        for (auto id : {Param::UVX, Param::UVY, Param::DriftX, Param::DriftY}) settings_.set(id, 0);
        for (auto id : {Param::Speed, Param::Density, Param::TrailLength, Param::TrailDensity, Param::TrailFalloff,
                        Param::UVX, Param::UVY, Param::DriftX, Param::DriftY})
            settings_.animations[static_cast<std::size_t>(id)].mode = 0;
        refreshSettings(false);
        if (callbacks_.applyMotionPreset) callbacks_.applyMotionPreset(settings_); else notifySettings();
    }
    void createDrift(QVBoxLayout *parent)
    {
        auto *drift = section(parent, QStringLiteral("Drift · Richtung und Tempo"));
        drift->parentWidget()->setObjectName(QStringLiteral("driftSection"));
        driftPad_ = new DriftDirectionPad(this); drift->addWidget(driftPad_);
        auto *tempo = new QHBoxLayout;
        auto *label = new QLabel(QStringLiteral("Tempo"), this); tempo->addWidget(label, 1);
        driftSpeed_ = new QDoubleSpinBox(this); driftSpeed_->setObjectName(QStringLiteral("driftSpeedPercent"));
        driftSpeed_->setDecimals(2); driftSpeed_->setRange(0, std::sqrt(2.) * 100.);
        driftSpeed_->setSingleStep(.01); driftSpeed_->setAccelerated(true); driftSpeed_->setKeyboardTracking(false);
        driftSpeed_->setSuffix(QStringLiteral(" % Bild/s")); label->setBuddy(driftSpeed_);
        driftSpeed_->setAccessibleName(QStringLiteral("Drifttempo in Prozent Bild pro Sekunde"));
        driftSpeed_->setToolTip(QStringLiteral("0 stoppt die Drift. 1% Bild/s ist langsam: 100 Sekunden für eine Bildbreite oder -höhe. Zahl direkt eingeben; Fein schaltet zwischen 0,01 und 0,1 Prozentpunkten. Maximal 100% je Achse, diagonal bis 141,42%."));
        tempo->addWidget(driftSpeed_);
        auto *fine = new QToolButton(this); fine->setObjectName(QStringLiteral("driftFineStep")); fine->setText(QStringLiteral("Fein"));
        fine->setCheckable(true); fine->setChecked(true); fine->setToolTip(QStringLiteral("Aktiv: Schritte von 0,01% Bild/s. Inaktiv: Schritte von 0,1% Bild/s. Direkte Zahleneingabe bleibt möglich."));
        tempo->addWidget(fine); drift->addLayout(tempo);
        driftSlider_ = new QSlider(Qt::Horizontal, this); driftSlider_->setObjectName(QStringLiteral("driftSpeedSlider"));
        driftSlider_->setRange(0, 1000); driftSlider_->setSingleStep(1); driftSlider_->setPageStep(20);
        driftSlider_->setAccessibleName(QStringLiteral("Drifttempo mit feinem Langsambereich"));
        driftSlider_->setToolTip(QStringLiteral("Fein verteiltes langsames Tempo. Der Regler steigt zum rechten Rand schneller an; die Zahl zeigt immer das genaue Tempo."));
        drift->addWidget(driftSlider_);
        driftReadout_ = note({}, this); driftReadout_->setObjectName(QStringLiteral("driftReadout")); drift->addWidget(driftReadout_);
        auto *timeLine = new QHBoxLayout;
        timeLine->addWidget(new QLabel(QStringLiteral("Zeitsteuerung"), this), 1);
        for (int i = 0; i < 2; ++i) {
            auto *button = new QToolButton(this); button->setObjectName(i ? QStringLiteral("animate_drift_y") : QStringLiteral("animate_drift_x"));
            button->setText(i ? QStringLiteral("Y ◇") : QStringLiteral("X ◇")); button->setCheckable(true);
            button->setAccessibleName(i ? QStringLiteral("Vertikale Drift animieren") : QStringLiteral("Horizontale Drift animieren"));
            button->setToolTip(QStringLiteral("Zusätzliche Zeitsteuerung für die entsprechende Driftachse öffnen. Das Richtungspad zeigt weiterhin die Basisbewegung."));
            driftAnimationButtons_[i] = button; timeLine->addWidget(button);
            QObject::connect(button, &QToolButton::clicked, this, [this, i] { refreshDriftAnimation(); openAnimation(i ? Param::DriftY : Param::DriftX); });
        }
        drift->addLayout(timeLine);
        driftAnimationHint_ = note(QStringLiteral("Drift-Animation aktiv: Pad und Tempo zeigen die Basiswerte. Die Zeitsteuerung verändert die Bewegung zusätzlich."), this);
        driftAnimationHint_->setObjectName(QStringLiteral("driftAnimationHint")); drift->addWidget(driftAnimationHint_);
        auto *actions = new QHBoxLayout;
        auto *stop = new QPushButton(QStringLiteral("Drift stoppen"), this); stop->setObjectName(QStringLiteral("stopDrift"));
        stop->setToolTip(QStringLiteral("Stoppt die Drift und ihre X/Y-Zeitsteuerung. Die erreichte Bildposition, feste Position und deren Animationen bleiben erhalten."));
        auto *reset = new QPushButton(QStringLiteral("Drift zurücksetzen"), this); reset->setObjectName(QStringLiteral("resetDrift"));
        reset->setToolTip(QStringLiteral("Stoppt die Drift und fährt die Position sanft in ca. 1,2 Sekunden zurück. Sterne und Symbole laufen weiter. Pause hält auch die Rückfahrt an. Die Zeitsteuerung für Position und Drift wird ausgeschaltet."));
        reset->setEnabled(bool(callbacks_.resetDrift));
        actions->addWidget(stop); actions->addWidget(reset); drift->addLayout(actions);
        driftActionHint_ = note({}, this); driftActionHint_->setObjectName(QStringLiteral("driftActionHint")); driftActionHint_->hide(); drift->addWidget(driftActionHint_);
        drift->addWidget(note(QStringLiteral("Richtung am Pfeil wählen. 1% Bild/s = 100s für eine Bildbreite/-höhe. Tempo 0 bedeutet Stopp. Eine neue Richtung startet bei Stopp sanft mit 1%."), this));
        driftPad_->changed = [this](double x, double y) {
            if (loading_) return;
            const double length = std::hypot(x, y); if (length < 1.e-12) return;
            const double oldTempo = std::hypot(settings_.get(Param::DriftX), settings_.get(Param::DriftY)) * 100.;
            driftDirectionX_ = x / length; driftDirectionY_ = y / length;
            applyDriftTempo(oldTempo > 1.e-10 ? oldTempo : 1.);
        };
        QObject::connect(driftSpeed_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double percent) { if (!loading_) applyDriftTempo(percent); });
        QObject::connect(driftSlider_, &QSlider::valueChanged, this, [this](int position) {
            if (loading_) return;
            const double raw = driftTempoLimit() * std::pow(position / 1000., 3);
            applyDriftTempo(std::round(raw * 100.) / 100.);
        });
        QObject::connect(fine, &QToolButton::toggled, this, [this](bool enabled) { driftSpeed_->setSingleStep(enabled ? .01 : .1); });
        QObject::connect(stop, &QPushButton::clicked, this, [this] {
            for (auto id : {Param::DriftX, Param::DriftY}) { settings_.set(id, 0); settings_.animations[static_cast<std::size_t>(id)].mode = 0; }
            refreshDrift(true); refreshDriftAnimation(); refreshAnimation(false);
            driftActionHint_->setText(QStringLiteral("Drift gestoppt. Die aktuelle Bildposition bleibt erhalten.")); driftActionHint_->show();
            if (callbacks_.stopDrift) callbacks_.stopDrift(settings_); else notifySettings();
        });
        QObject::connect(reset, &QPushButton::clicked, this, [this] {
            if (!callbacks_.resetDrift) return;
            for (auto id : {Param::UVX, Param::UVY, Param::DriftX, Param::DriftY}) {
                settings_.set(id, 0); settings_.animations[static_cast<std::size_t>(id)].mode = 0;
                if (auto *row = rows_[static_cast<std::size_t>(id)]) { row->setValue(0); row->setAnimated(false); }
            }
            refreshDrift(false); refreshDriftAnimation(); refreshAnimation(false);
            driftActionHint_->setText(QStringLiteral("Die Position fährt sanft in ca. 1,2s zurück. Pause hält die Rückfahrt an; Sterne und Symbole laufen weiter.")); driftActionHint_->show();
            callbacks_.resetDrift(settings_);
        });
    }
    double driftTempoLimit() const
    {
        return 100. / std::max(.000001, std::max(std::abs(driftDirectionX_), std::abs(driftDirectionY_)));
    }
    void applyDriftTempo(double percent)
    {
        percent = bounded(percent, 0, driftTempoLimit());
        settings_.set(Param::DriftX, driftDirectionX_ * percent / 100.);
        settings_.set(Param::DriftY, driftDirectionY_ * percent / 100.);
        refreshDrift(true); driftActionHint_->hide(); notifySettings();
    }
    void refreshDrift(bool preserveStoppedDirection)
    {
        if (!driftPad_) return;
        const double x = settings_.get(Param::DriftX), y = settings_.get(Param::DriftY);
        const double speed = std::hypot(x, y), percent = speed * 100.;
        if (speed > 1.e-12) { driftDirectionX_ = x / speed; driftDirectionY_ = y / speed; }
        else if (!preserveStoppedDirection) { driftDirectionX_ = 1.; driftDirectionY_ = 0.; }
        driftPad_->setDirection(driftDirectionX_, driftDirectionY_, speed > 1.e-12);
        { const QSignalBlocker blocker(driftSpeed_); driftSpeed_->setMaximum(driftTempoLimit()); driftSpeed_->setValue(percent); }
        const double normalized = bounded(percent / driftTempoLimit(), 0, 1);
        { const QSignalBlocker blocker(driftSlider_); driftSlider_->setValue(qRound(std::cbrt(normalized) * 1000.)); }
        if (speed < 1.e-12) driftReadout_->setText(QStringLiteral("Gestoppt · Richtung wählen oder Tempo erhöhen."));
        else {
            const QString horizontal = driftDirectionX_ > .2 ? QStringLiteral("rechts") : (driftDirectionX_ < -.2 ? QStringLiteral("links") : QString{});
            const QString vertical = driftDirectionY_ > .2 ? QStringLiteral("unten") : (driftDirectionY_ < -.2 ? QStringLiteral("oben") : QString{});
            const QString direction = horizontal.isEmpty() ? vertical : (vertical.isEmpty() ? horizontal : horizontal + QStringLiteral(" ") + vertical);
            driftReadout_->setText(QStringLiteral("Nach %1 · ca. %2s für eine Bildlänge.").arg(direction).arg(100. / percent, 0, 'f', percent > 10 ? 2 : 0));
        }
    }
    void refreshDriftAnimation()
    {
        if (!driftAnimationHint_) return;
        bool active = false;
        for (int i = 0; i < 2; ++i) {
            const bool animated = settings_.animations[static_cast<std::size_t>(i ? Param::DriftY : Param::DriftX)].mode != 0;
            active = active || animated; const QSignalBlocker blocker(driftAnimationButtons_[i]);
            driftAnimationButtons_[i]->setChecked(animated);
            driftAnimationButtons_[i]->setText((i ? QStringLiteral("Y ") : QStringLiteral("X ")) + (animated ? QStringLiteral("◆") : QStringLiteral("◇")));
        }
        driftAnimationHint_->setVisible(active);
    }
    void createSizes()
    {
        auto sizes = page(QStringLiteral("Größe"), QStringLiteral("Sternengröße, Symbolgröße und Größenstreuung"));
        auto *stars = section(sizes.layout, QStringLiteral("Sterne")); addParam(stars, Param::StarSize);
        auto *symbols = section(sizes.layout, QStringLiteral("Eigene Motive"));
        addParam(symbols, Param::SymbolSize); addParam(symbols, Param::SizeJitter);
        symbols->addWidget(note(QStringLiteral("Pixel beziehen sich auf eine Ausgabehöhe von 1080. Auch sehr große Motive sind möglich. Neue Größen und Streuung gelten für neu entstehende Partikel; bereits fliegende Motive behalten ihre Größe."), this));
        sizes.layout->addStretch();
    }
    void createDrawing()
    {
        auto drawing = page(QStringLiteral("Zeichnen"), QStringLiteral("16:9-Zeichenfläche und Einmischen eigener Motive"));
        drawingCanvasLayout_ = drawing.layout;
        drawingCanvasParent_ = drawing.widget;
        drawingPage_ = tabs_->widget(tabs_->count() - 1);
        auto *boardHeader = new QHBoxLayout;
        auto *label = new QLabel(QStringLiteral("Entwurf · 1920 × 1080"), this); boardHeader->addWidget(label, 1);
        auto *pop = new QToolButton(this); pop->setText(QStringLiteral("Vergrößern"));
        pop->setToolTip(QStringLiteral("Die Zeichenfläche in einem eigenen skalierbaren Fenster öffnen. Die Werkzeuge und derselbe Entwurf bleiben direkt erreichbar."));
        pop->setObjectName(QStringLiteral("expandDrawing")); boardHeader->addWidget(pop); drawing.layout->addLayout(boardHeader);
        canvas_ = new DrawingCanvas(this); drawing.layout->addWidget(canvas_);
        auto *tools = new QHBoxLayout; tools->setSpacing(4);
        undoDrawing_ = toolButton(this, QStringLiteral("Zurück"), QStyle::SP_ArrowBack, QStringLiteral("Letzten Zeichenstrich rückgängig machen (Strg+Z in der Zeichenfläche)."));
        redoDrawing_ = toolButton(this, QStringLiteral("Wiederholen"), QStyle::SP_ArrowForward, QStringLiteral("Rückgängig gemachten Zeichenstrich wiederherstellen (Strg+Umschalt+Z)."));
        undoDrawing_->setToolButtonStyle(Qt::ToolButtonIconOnly); redoDrawing_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        auto *clear = new QToolButton(this); clear->setText(QStringLiteral("Leeren")); clear->setToolTip(QStringLiteral("Nur den Entwurf leeren. Bereits fliegende Motive bleiben sichtbar; Rückgängig ist möglich."));
        auto *send = new QToolButton(this); send->setText(QStringLiteral("Senden")); send->setObjectName(QStringLiteral("sendFromCanvas")); send->setToolTip(QStringLiteral("Den aktuellen Entwurf einmischen. Rechtsklick in die Fläche führt dieselbe Aktion aus."));
        tools->addWidget(undoDrawing_); tools->addWidget(redoDrawing_); tools->addWidget(clear); tools->addStretch(); tools->addWidget(send);
        drawing.layout->addLayout(tools);
        drawingHint_ = note({}, this); drawingHint_->setObjectName(QStringLiteral("drawingHint")); drawing.layout->addWidget(drawingHint_);
        auto *sent = new QHBoxLayout;
        submitted_ = new DrawingPreview(this); submitted_->setMaximumHeight(65); submitted_->setMaximumWidth(120);
        sent->addWidget(submitted_); sentLabel_ = note(QStringLiteral("Noch nichts gesendet"), this); sent->addWidget(sentLabel_, 1); drawing.layout->addLayout(sent);
        auto *mix = section(drawing.layout, QStringLiteral("Einmischen ins Sternenfeld"));
        addParam(mix, Param::SymbolShare); addParam(mix, Param::TimeScatter);
        forever_ = new QCheckBox(QStringLiteral("Einmischen, bis ich es beende"), this);
        forever_->setObjectName(QStringLiteral("emissionForever"));
        forever_->setToolTip(QStringLiteral("Nach dem Senden entstehen fortlaufend eigene Motive. „Beenden“ stoppt nur neue Motive; bereits fliegende laufen aus."));
        mix->addWidget(forever_); duration_ = addParam(mix, Param::EmissionDuration);
        mix->addWidget(note(QStringLiteral("Einmischdauer: Zeitraum, in dem nach dem nächsten Senden neue eigene Motive entstehen. Das ist keine Flugdauer. Neue Skizzen gelten nur für neue Partikel; alte Motive fliegen unverändert zu Ende."), this));
        canvas_->changed = [this](const Drawing &value) {
            if (!loading_ && callbacks_.draftChanged) callbacks_.draftChanged(std::make_shared<Drawing>(value));
            refreshDrawingHistory();
        };
        canvas_->historyChanged = [this] { refreshDrawingHistory(); };
        canvas_->submit = [this] { submit(); };
        QObject::connect(undoDrawing_, &QToolButton::clicked, this, [this] { canvas_->undo(); });
        QObject::connect(redoDrawing_, &QToolButton::clicked, this, [this] { canvas_->redo(); });
        QObject::connect(clear, &QToolButton::clicked, this, [this] { canvas_->clear(); });
        QObject::connect(send, &QToolButton::clicked, this, [this] { submit(); });
        QObject::connect(forever_, &QCheckBox::toggled, this, [this](bool continuous) {
            if (loading_) return;
            settings_.emissionForever = continuous; duration_->setEnabled(!continuous); notifySettings();
        });
        QObject::connect(pop, &QToolButton::clicked, this, [this] { expandDrawing(); });
        drawing.layout->addStretch();
    }
    void createAnimation()
    {
        auto animation = page(QStringLiteral("Zeit"), QStringLiteral("Presets und grafische Kurven für die Animation einzelner Werte"));
        animationPage_ = tabs_->widget(tabs_->count() - 1);
        auto *sectionLayout = section(animation.layout, QStringLiteral("Zeitsteuerung"));
        auto *form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        target_ = new QComboBox(this); target_->setObjectName(QStringLiteral("animationTarget"));
        for (const auto &def : parameterDefs()) if (def.animatable)
            target_->addItem(text(def.group) + QStringLiteral(" · ") + text(def.label), static_cast<int>(def.id));
        target_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        target_->setMinimumContentsLength(12); target_->setToolTip(QStringLiteral("Genau diesen Grundwert animieren. Die Raute neben jedem Regler öffnet direkt dessen Zeitsteuerung."));
        form->addRow(QStringLiteral("Wert"), target_);
        mode_ = new QComboBox(this); mode_->setObjectName(QStringLiteral("animationMode"));
        const QStringList modes{QStringLiteral("Aus"), QStringLiteral("Sinus · weich"), QStringLiteral("Dreieck · gleichmäßig"),
            QStringLiteral("Sägezahn · wiederkehrend"), QStringLiteral("Puls · ein/aus"), QStringLiteral("Zufall · weich"),
            QStringLiteral("Linear · fortlaufend"), QStringLiteral("Zufall · gehalten"), QStringLiteral("Eigene Kurve")};
        for (int i = 0; i < modes.size(); ++i) mode_->addItem(modes[i], i);
        mode_->setToolTip(QStringLiteral("Wellenform für die Veränderung um den Grundwert. Linear läuft ohne Rücksprung weiter; für UV-Verschiebungen eignet sich das als Drift."));
        form->addRow(QStringLiteral("Verlauf"), mode_);
        amount_ = new QDoubleSpinBox(this); amount_->setObjectName(QStringLiteral("animationAmount"));
        amount_->setKeyboardTracking(false); amount_->setAccelerated(true);
        amount_->setToolTip(QStringLiteral("Abweichung vom Grundwert in dessen Einheit. Negative Werte kehren die Richtung um. Der Grundwert selbst bleibt im jeweiligen Tab einstellbar."));
        form->addRow(QStringLiteral("Stärke"), amount_);
        period_ = new QDoubleSpinBox(this); period_->setObjectName(QStringLiteral("animationPeriod"));
        period_->setRange(.05, 3600); period_->setDecimals(2); period_->setSingleStep(.1); period_->setSuffix(QStringLiteral(" s")); period_->setKeyboardTracking(false);
        period_->setToolTip(QStringLiteral("Zeit für eine vollständige Wellenform. Beim linearen Verlauf die Zeit für eine Änderung um die eingestellte Stärke."));
        form->addRow(QStringLiteral("Zykluszeit"), period_);
        phase_ = new QDoubleSpinBox(this); phase_->setObjectName(QStringLiteral("animationPhase"));
        phase_->setRange(0, 99.9); phase_->setDecimals(1); phase_->setSingleStep(1); phase_->setSuffix(QStringLiteral(" %")); phase_->setKeyboardTracking(false);
        phase_->setToolTip(QStringLiteral("Startposition innerhalb der Wellenform: 50 % entspricht einem halben Zyklus."));
        form->addRow(QStringLiteral("Versatz"), phase_);
        pingPong_ = new QCheckBox(QStringLiteral("Hin und zurück"), this); pingPong_->setObjectName(QStringLiteral("animationPingPong"));
        pingPong_->setToolTip(QStringLiteral("Den Verlauf vorwärts und rückwärts abspielen. Verbindet auch unterschiedlich hohe Endpunkte ohne harten Rücksprung."));
        form->addRow(QString{}, pingPong_); sectionLayout->addLayout(form);
        animationInfo_ = note({}, this); sectionLayout->addWidget(animationInfo_);
        curve_ = new CurveCanvas(this); sectionLayout->addWidget(curve_);
        nodeControls_ = new QWidget(this); auto *nodes = new QVBoxLayout(nodeControls_); nodes->setContentsMargins(0, 0, 0, 0); nodes->setSpacing(5);
        auto *nodeLine = new QHBoxLayout;
        nodeLabel_ = new QLabel(QStringLiteral("Knoten"), this); nodeLine->addWidget(nodeLabel_, 1);
        nodeTime_ = new QDoubleSpinBox(this); nodeTime_->setObjectName(QStringLiteral("curveNodeTime")); nodeTime_->setRange(0, 100); nodeTime_->setDecimals(1); nodeTime_->setSingleStep(.5); nodeTime_->setSuffix(QStringLiteral(" %")); nodeTime_->setKeyboardTracking(false);
        nodeTime_->setAccessibleName(QStringLiteral("Zeit des ausgewählten Kurvenknotens")); nodeTime_->setToolTip(QStringLiteral("Zeitposition des ausgewählten Knotens. Start und Ende bleiben fest bei 0 bzw. 100 %."));
        nodeValue_ = new QDoubleSpinBox(this); nodeValue_->setObjectName(QStringLiteral("curveNodeValue")); nodeValue_->setRange(-1, 1); nodeValue_->setDecimals(3); nodeValue_->setSingleStep(.01); nodeValue_->setKeyboardTracking(false);
        nodeValue_->setAccessibleName(QStringLiteral("Wert des ausgewählten Kurvenknotens")); nodeValue_->setToolTip(QStringLiteral("Normierte Abweichung: −1 = Grundwert minus Stärke, 0 = Grundwert, +1 = Grundwert plus Stärke."));
        nodeLine->addWidget(nodeTime_); nodeLine->addWidget(nodeValue_); nodes->addLayout(nodeLine);
        auto *curveTools = new QHBoxLayout;
        curveUndo_ = toolButton(this, QStringLiteral("Zurück"), QStyle::SP_ArrowBack, QStringLiteral("Letzte Kurvenänderung rückgängig machen."));
        curveRedo_ = toolButton(this, QStringLiteral("Wiederholen"), QStyle::SP_ArrowForward, QStringLiteral("Rückgängig gemachte Kurvenänderung wiederholen."));
        curveUndo_->setToolButtonStyle(Qt::ToolButtonIconOnly); curveRedo_->setToolButtonStyle(Qt::ToolButtonIconOnly);
        auto *remove = new QToolButton(this); remove->setText(QStringLiteral("Knoten −")); remove->setToolTip(QStringLiteral("Ausgewählten inneren Knoten entfernen. Endpunkte bleiben erhalten."));
        curveTools->addWidget(curveUndo_); curveTools->addWidget(curveRedo_); curveTools->addWidget(remove); curveTools->addStretch(); nodes->addLayout(curveTools);
        auto *curveDefaults = new QHBoxLayout;
        auto *closeLoop = new QPushButton(QStringLiteral("Loop schließen"), this); closeLoop->setToolTip(QStringLiteral("Den Endwert an den Startwert angleichen, damit sich die Kurve ohne sichtbaren Sprung wiederholen kann."));
        auto *reset = new QPushButton(QStringLiteral("Standardkurve"), this); reset->setToolTip(QStringLiteral("Drei weiche Knoten einsetzen. Rückgängig ist möglich."));
        curveDefaults->addWidget(closeLoop); curveDefaults->addWidget(reset); nodes->addLayout(curveDefaults);
        loopWarning_ = note({}, this); nodes->addWidget(loopWarning_); sectionLayout->addWidget(nodeControls_);
        sectionLayout->addWidget(note(QStringLiteral("Kurve: doppelt klicken zum Hinzufügen, Knoten ziehen, Entf zum Entfernen. Pfeiltasten verändern den ausgewählten Knoten; Umschalt für Feinschritte."), this));
        animation.layout->addStretch();
        QObject::connect(target_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) { if (!loading_) refreshAnimation(true); });
        QObject::connect(mode_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            if (loading_) return;
            auto &a = currentAnimation(); a.mode = mode_->currentData().toInt();
            if (a.mode && std::abs(a.amount) < 1.e-9) { const auto &def = parameterDef(currentTarget()); a.amount = (def.max - def.min) * .1; }
            refreshAnimation(false); notifySettings();
        });
        QObject::connect(amount_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { if (!loading_) { currentAnimation().amount = value; notifySettings(); } });
        QObject::connect(period_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { if (!loading_) { currentAnimation().period = value; notifySettings(); } });
        QObject::connect(phase_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) { if (!loading_) { currentAnimation().phase = value / 100.; notifySettings(); } });
        QObject::connect(pingPong_, &QCheckBox::toggled, this, [this](bool checked) { if (!loading_) { currentAnimation().pingPong = checked; refreshAnimation(false); notifySettings(); } });
        curve_->changed = [this](const std::vector<KeyPoint> &points) {
            if (loading_) return;
            currentAnimation().points = points; refreshLoopWarning(); notifySettings();
        };
        curve_->selectionChanged = [this](int index) { refreshNode(index); };
        curve_->historyChanged = [this] { curveUndo_->setEnabled(curve_->canUndo()); curveRedo_->setEnabled(curve_->canRedo()); };
        auto editNode = [this] { if (!loading_ && !nodeSync_) curve_->editSelected(nodeTime_->value() / 100., nodeValue_->value()); };
        QObject::connect(nodeTime_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [editNode](double) { editNode(); });
        QObject::connect(nodeValue_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [editNode](double) { editNode(); });
        QObject::connect(curveUndo_, &QToolButton::clicked, this, [this] { curve_->undo(); });
        QObject::connect(curveRedo_, &QToolButton::clicked, this, [this] { curve_->redo(); });
        QObject::connect(remove, &QToolButton::clicked, this, [this] { curve_->deleteSelected(); });
        QObject::connect(closeLoop, &QPushButton::clicked, this, [this] { curve_->closeLoop(); });
        QObject::connect(reset, &QPushButton::clicked, this, [this] { curve_->resetCurve(); });
    }
    Param currentTarget() const
    {
        return target_ && target_->currentIndex() >= 0 ? static_cast<Param>(target_->currentData().toInt()) : Param::Opacity;
    }
    Animation &currentAnimation() { return settings_.animations[static_cast<std::size_t>(currentTarget())]; }
    void openAnimation(Param id)
    {
        const int index = target_->findData(static_cast<int>(id)); if (index < 0) return;
        target_->setCurrentIndex(index); tabs_->setCurrentWidget(animationPage_); refreshAnimation(true); mode_->setFocus();
    }
    void notifySettings()
    {
        for (std::size_t i = 0; i < parameterCount; ++i) if (rows_[i]) rows_[i]->setAnimated(settings_.animations[i].mode != 0);
        refreshDriftAnimation();
        if (!loading_ && callbacks_.settingsChanged) callbacks_.settingsChanged(settings_);
    }
    void refreshSettings(bool resetCurveHistory)
    {
        const bool before = loading_; loading_ = true;
        for (std::size_t i = 0; i < parameterCount; ++i) if (rows_[i]) {
            rows_[i]->setValue(settings_.values[i]); rows_[i]->setAnimated(settings_.animations[i].mode != 0);
        }
        if (colorMode_) { const QSignalBlocker blocker(colorMode_); colorMode_->setCurrentIndex(std::max(0, colorMode_->findData(settings_.colorMode))); }
        if (seed_) { const QSignalBlocker blocker(seed_); seed_->setValue(double(settings_.seed)); }
        if (forever_) { const QSignalBlocker blocker(forever_); forever_->setChecked(settings_.emissionForever); duration_->setEnabled(!settings_.emissionForever); }
        refreshDrift(false); refreshDriftAnimation();
        if (motionModeHint_) motionModeHint_->setText(settings_.motionMode == 1
            ? QStringLiteral("Aktuell: Von oben nach unten. Farben, Größen und Skizze bleiben erhalten.")
            : QStringLiteral("Aktuell: Sternenflug. Farben, Größen und Skizze bleiben erhalten."));
        if (driftActionHint_) driftActionHint_->hide();
        refreshColors(); refreshAnimation(resetCurveHistory); loading_ = before;
    }
    QColor color(bool second) const
    {
        const double hue = settings_.get(second ? Param::HueB : Param::HueA);
        return QColor::fromHsvF(std::fmod(hue, 360.) / 360., settings_.get(second ? Param::SatB : Param::SatA) / 100., settings_.get(second ? Param::ValB : Param::ValA) / 100.);
    }
    void refreshColors()
    {
        if (!colorWheel_) return;
        colorWheel_->setHsv(settings_.get(activeColor_ ? Param::HueB : Param::HueA),
                            settings_.get(activeColor_ ? Param::SatB : Param::SatA),
                            settings_.get(activeColor_ ? Param::ValB : Param::ValA));
        for (int i = 0; i < 2; ++i) {
            QPixmap swatch(24, 18); swatch.fill(color(i != 0)); colorButtons_[i]->setIcon(QIcon(swatch));
        }
        if (ramp_) { ramp_->first = color(false); ramp_->second = color(true); ramp_->mode = settings_.colorMode; ramp_->update(); }
    }
    void refreshAnimation(bool resetHistory)
    {
        if (!target_ || !curve_) return;
        const bool before = loading_; loading_ = true;
        const auto &def = parameterDef(currentTarget()); const auto &a = currentAnimation();
        { const QSignalBlocker b(mode_); mode_->setCurrentIndex(std::max(0, mode_->findData(a.mode))); }
        { const QSignalBlocker b(amount_); amount_->setRange(-(def.max - def.min), def.max - def.min); amount_->setDecimals(decimals(def.step)); amount_->setSingleStep(def.step); amount_->setSuffix(suffix(def.id)); amount_->setValue(a.amount); }
        { const QSignalBlocker b(period_); period_->setValue(a.period); }
        { const QSignalBlocker b(phase_); phase_->setValue(a.phase * 100.); }
        { const QSignalBlocker b(pingPong_); pingPong_->setChecked(a.pingPong); }
        for (auto *widget : {static_cast<QWidget *>(amount_), static_cast<QWidget *>(period_), static_cast<QWidget *>(phase_), static_cast<QWidget *>(pingPong_)}) widget->setEnabled(a.mode != 0);
        curve_->setAnimation(a, resetHistory); nodeControls_->setVisible(a.mode == 8);
        if (a.mode == 0) animationInfo_->setText(QStringLiteral("Animation aus. Der Grundwert wird im jeweiligen Tab eingestellt."));
        else if (a.mode == 6 && !a.pingPong) animationInfo_->setText(QStringLiteral("Fortlaufend: Grundwert + Stärke je Zykluszeit. Bei Bildpositionen ohne Rücksprung; andere Werte bleiben im erlaubten Bereich."));
        else animationInfo_->setText(QStringLiteral("Grundwert %1%2 + Kurve × Stärke. Ein Zyklus dauert %3 s.").arg(settings_.get(currentTarget()), 0, 'f', decimals(def.step)).arg(suffix(def.id)).arg(a.period, 0, 'f', 2));
        refreshLoopWarning(); loading_ = before;
    }
    void refreshNode(int index)
    {
        if (!nodeTime_ || !curve_ || index < 0 || index >= int(curve_->points().size())) return;
        const bool before = nodeSync_; nodeSync_ = true;
        const auto &point = curve_->points()[std::size_t(index)];
        nodeLabel_->setText(QStringLiteral("Knoten %1/%2").arg(index + 1).arg(curve_->points().size()));
        { const QSignalBlocker b(nodeTime_); nodeTime_->setValue(point.time * 100.); }
        { const QSignalBlocker b(nodeValue_); nodeValue_->setValue(point.value); }
        nodeTime_->setEnabled(index > 0 && index < int(curve_->points().size()) - 1);
        nodeSync_ = before;
    }
    void refreshLoopWarning()
    {
        if (!loopWarning_) return;
        const auto &a = currentAnimation();
        const bool jump = a.mode == 8 && !a.pingPong && a.points.size() >= 2 && std::abs(a.points.front().value - a.points.back().value) > .001;
        loopWarning_->setText(jump ? QStringLiteral("Start und Ende sind verschieden. „Loop schließen“ gleicht die Endwerte an; „Hin und zurück“ spielt ohne Rücksprung zurück.") : QStringLiteral("Der Verlauf lässt sich weich wiederholen."));
    }
    void refreshDrawingHistory()
    {
        if (!canvas_) return;
        undoDrawing_->setEnabled(canvas_->canUndo()); redoDrawing_->setEnabled(canvas_->canRedo());
        send_->setEnabled(canvas_->hasDrawing());
    }
    void submit()
    {
        if (!canvas_->hasDrawing()) {
            tabs_->setCurrentWidget(drawingPage_); canvas_->setFocus();
            drawingHint_->setText(QStringLiteral("Zeichne zuerst mindestens einen Strich. Das laufende Sternenfeld wird dadurch nicht unterbrochen.")); return;
        }
        auto drawing = std::make_shared<Drawing>(canvas_->drawing()); drawing->id = 0;
        if (!callbacks_.submitDrawing || !callbacks_.submitDrawing(drawing)) {
            drawingHint_->setText(QStringLiteral("Senden wurde nicht übernommen. Lass vorhandene Motive ausfliegen und sende erneut. Der Entwurf und die bisherige Einmischung bleiben erhalten."));
            return;
        }
        submitted_->drawing = *drawing; submitted_->update(); submitted_->show();
        sentLabel_->setText(QStringLiteral("Zuletzt abgeschickt\nNeue Partikel erhalten diese Kontur; laufende Motive behalten ihre eigene Form."));
        drawingHint_->setText(QStringLiteral("Skizze abgeschickt. Du kannst sie erneut senden oder den nächsten Entwurf zeichnen. Bereits fliegende Motive bleiben erhalten."));
    }
    void expandDrawing()
    {
        if (expandedDialog_) { expandedDialog_->show(); expandedDialog_->raise(); expandedDialog_->activateWindow(); return; }
        auto *dialog = new QDialog(this); dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(QStringLiteral("Sternenflug · Zeichenfläche"));
        dialog->resize(900, 650); dialog->setMinimumSize(400, 300);
        auto *layout = new QVBoxLayout(dialog);
        layout->addWidget(note(QStringLiteral("Derselbe Entwurf · 16:9 · Links zeichnen, rechts senden. Schließen erhält die Skizze und den gesamten Zeichenverlauf."), dialog));
        // Move the actual canvas, rather than copying a second editor: this
        // preserves all undo history and leaves the host usable while drawing.
        drawingCanvasLayout_->removeWidget(canvas_); canvas_->setParent(dialog);
        layout->addWidget(canvas_, 1); expandedDialog_ = dialog;
        auto *tools = new QHBoxLayout;
        auto *undo = new QPushButton(QStringLiteral("Rückgängig"), dialog);
        auto *redo = new QPushButton(QStringLiteral("Wiederholen"), dialog);
        auto *clear = new QPushButton(QStringLiteral("Leeren"), dialog);
        auto *send = new QPushButton(QStringLiteral("Senden"), dialog);
        auto *close = new QPushButton(QStringLiteral("Schließen"), dialog);
        tools->addWidget(undo); tools->addWidget(redo); tools->addWidget(clear); tools->addStretch(); tools->addWidget(send); tools->addWidget(close); layout->addLayout(tools);
        const QPointer<QPushButton> safeUndo(undo), safeRedo(redo);
        canvas_->historyChanged = [this, safeUndo, safeRedo] {
            refreshDrawingHistory();
            if (safeUndo) safeUndo->setEnabled(canvas_->canUndo());
            if (safeRedo) safeRedo->setEnabled(canvas_->canRedo());
        };
        canvas_->historyChanged();
        QObject::connect(undo, &QPushButton::clicked, dialog, [this] { canvas_->undo(); });
        QObject::connect(redo, &QPushButton::clicked, dialog, [this] { canvas_->redo(); });
        QObject::connect(clear, &QPushButton::clicked, dialog, [this] { canvas_->clear(); });
        QObject::connect(send, &QPushButton::clicked, dialog, [this] { submit(); });
        QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
        QObject::connect(dialog, &QDialog::finished, this, [this] {
            canvas_->setParent(drawingCanvasParent_); drawingCanvasLayout_->insertWidget(1, canvas_); canvas_->show();
            canvas_->historyChanged = [this] { refreshDrawingHistory(); };
            refreshDrawingHistory(); expandedDialog_ = nullptr;
        });
        dialog->show(); canvas_->show(); canvas_->setFocus();
    }
    void applyTheme(int value)
    {
        theme_ = value;
        if (value == 0) {
            setStyleSheet(QString{}); setPalette(QPalette());
            for (auto *child : findChildren<QWidget *>()) child->setPalette(QPalette());
            update(); return;
        }
        QPalette palette;
        const bool dark = value == 2;
        const QColor window = dark ? QColor(30, 33, 39) : QColor(244, 245, 248);
        const QColor base = dark ? QColor(20, 23, 28) : QColor(255, 255, 255);
        const QColor foreground = dark ? QColor(232, 236, 242) : QColor(34, 40, 50);
        const QColor alternate = dark ? QColor(40, 44, 52) : QColor(234, 237, 243);
        const QColor highlight = dark ? QColor(91, 183, 220) : QColor(0, 102, 158);
        palette.setColor(QPalette::Window, window); palette.setColor(QPalette::WindowText, foreground);
        palette.setColor(QPalette::Base, base); palette.setColor(QPalette::AlternateBase, alternate);
        palette.setColor(QPalette::Text, foreground); palette.setColor(QPalette::Button, alternate);
        palette.setColor(QPalette::ButtonText, foreground); palette.setColor(QPalette::Highlight, highlight);
        palette.setColor(QPalette::HighlightedText, dark ? QColor(10, 24, 32) : QColor(Qt::white));
        palette.setColor(QPalette::ToolTipBase, base); palette.setColor(QPalette::ToolTipText, foreground);
        palette.setColor(QPalette::PlaceholderText, dark ? QColor(162, 173, 190) : QColor(91, 103, 121));
        palette.setColor(QPalette::Mid, dark ? QColor(70, 77, 91) : QColor(174, 183, 198));
        palette.setColor(QPalette::Light, dark ? QColor(78, 85, 99) : QColor(255, 255, 255));
        palette.setColor(QPalette::Dark, dark ? QColor(13, 16, 21) : QColor(150, 160, 176));
        palette.setColor(QPalette::Link, highlight);
        for (auto role : {QPalette::Text, QPalette::WindowText, QPalette::ButtonText})
            palette.setColor(QPalette::Disabled, role, dark ? QColor(116, 124, 140) : QColor(136, 145, 159));
        setPalette(palette);
        // OBS can style its widgets with an application stylesheet. Local
        // palette-based rules let an explicit light/dark choice override those
        // colors for this editor alone, while keeping native layout and DPI.
        QString localStyle = QStringLiteral(
            "QWidget { background-color: palette(window); color: palette(window-text); }"
            "QLabel, QCheckBox, QGroupBox { color: palette(window-text); }"
            "QLabel#sfHint { color: palette(placeholder-text); }"
            "QGroupBox { border: 1px solid palette(mid); border-radius: 5px; margin-top: 8px; padding-top: 3px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
            "QLineEdit, QDoubleSpinBox, QSpinBox, QComboBox, QListWidget { background-color: palette(base); color: palette(text); border: 1px solid palette(mid); border-radius: 4px; padding: 3px; selection-background-color: palette(highlight); selection-color: palette(highlighted-text); }"
            "QPushButton, QToolButton { background-color: palette(button); color: palette(button-text); border: 1px solid palette(mid); border-radius: 4px; padding: 4px 5px; }"
            "QPushButton:hover, QToolButton:hover, QLineEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QComboBox:focus { border-color: palette(highlight); }"
            "QPushButton:checked, QToolButton:checked { background-color: palette(highlight); color: palette(highlighted-text); }"
            "QPushButton:disabled, QToolButton:disabled { color: palette(mid); }"
            "QTabWidget::pane { border: 0; }"
            "QTabBar::tab { background-color: palette(button); color: palette(button-text); border-bottom: 2px solid transparent; padding: 6px; }"
            "QTabBar::tab:selected { border-bottom-color: palette(highlight); color: palette(highlight); }"
            "QScrollArea { border: 0; background-color: palette(window); }"
            "QMenu { background-color: palette(window); color: palette(window-text); }"
            "QMenu::item:selected { background-color: palette(highlight); color: palette(highlighted-text); }");
        // Resolve palette tokens here. Qt's stylesheet proxy can otherwise
        // resolve palette(window) against the OBS application palette during
        // repolishing, undoing an explicit editor-only theme change.
        localStyle.replace(QStringLiteral("palette(window-text)"), foreground.name());
        localStyle.replace(QStringLiteral("palette(button-text)"), foreground.name());
        localStyle.replace(QStringLiteral("palette(placeholder-text)"), palette.color(QPalette::PlaceholderText).name());
        localStyle.replace(QStringLiteral("palette(highlighted-text)"), palette.color(QPalette::HighlightedText).name());
        localStyle.replace(QStringLiteral("palette(highlight)"), highlight.name());
        localStyle.replace(QStringLiteral("palette(window)"), window.name());
        localStyle.replace(QStringLiteral("palette(base)"), base.name());
        localStyle.replace(QStringLiteral("palette(text)"), foreground.name());
        localStyle.replace(QStringLiteral("palette(button)"), alternate.name());
        localStyle.replace(QStringLiteral("palette(mid)"), palette.color(QPalette::Mid).name());
        setStyleSheet(localStyle);
        // A scroll area's content and viewport can retain a resolved palette
        // after a stylesheet theme change. Reset every editor-owned widget,
        // including custom painters, instead of leaving dark child surfaces
        // under light labels. This does not touch any OBS/application widget.
        for (auto *child : findChildren<QWidget *>()) child->setPalette(palette);
        update();
    }
    void applyScale(int value)
    {
        scale_ = value; QFont scaled = baseFont_;
        if (scaled.pointSizeF() > 0) scaled.setPointSizeF(scaled.pointSizeF() * value / 100.);
        else if (scaled.pixelSize() > 0) scaled.setPixelSize(qRound(scaled.pixelSize() * value / 100.));
        setFont(scaled);
        for (auto *button : findChildren<QAbstractButton *>()) button->setIconSize(QSize(qRound(16 * value / 100.), qRound(16 * value / 100.)));
        setMinimumWidth(std::max(300, qRound(340 * value / 100.)));
        updateGeometry();
    }

    Settings settings_;
    EditorCallbacks callbacks_;
    QSettings preferences_;
    QFont baseFont_;
    bool loading_ = false, nodeSync_ = false;
    int theme_ = 0, scale_ = 100, activeColor_ = 0;
    std::array<ParamRow *, parameterCount> rows_{};
    QLabel *status_ = nullptr;
    QToolButton *send_ = nullptr, *pause_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QWidget *drawingPage_ = nullptr, *animationPage_ = nullptr;
    QWidget *drawingCanvasParent_ = nullptr;
    QVBoxLayout *drawingCanvasLayout_ = nullptr;
    QPointer<QDialog> expandedDialog_;
    QPushButton *colorButtons_[2]{};
    HueWheel *colorWheel_ = nullptr;
    ColorRamp *ramp_ = nullptr;
    QStackedWidget *colorValues_ = nullptr;
    QComboBox *colorMode_ = nullptr;
    QDoubleSpinBox *seed_ = nullptr;
    QComboBox *motionPreset_ = nullptr;
    QLabel *motionModeHint_ = nullptr;
    DriftDirectionPad *driftPad_ = nullptr;
    QDoubleSpinBox *driftSpeed_ = nullptr;
    QSlider *driftSlider_ = nullptr;
    QToolButton *driftAnimationButtons_[2]{};
    QLabel *driftReadout_ = nullptr, *driftAnimationHint_ = nullptr, *driftActionHint_ = nullptr;
    double driftDirectionX_ = 1., driftDirectionY_ = 0.;
    DrawingCanvas *canvas_ = nullptr;
    DrawingPreview *submitted_ = nullptr;
    QToolButton *undoDrawing_ = nullptr, *redoDrawing_ = nullptr;
    QLabel *drawingHint_ = nullptr, *sentLabel_ = nullptr;
    QCheckBox *forever_ = nullptr;
    ParamRow *duration_ = nullptr;
    QComboBox *target_ = nullptr, *mode_ = nullptr;
    QDoubleSpinBox *amount_ = nullptr, *period_ = nullptr, *phase_ = nullptr;
    QCheckBox *pingPong_ = nullptr;
    CurveCanvas *curve_ = nullptr;
    QWidget *nodeControls_ = nullptr;
    QLabel *animationInfo_ = nullptr, *nodeLabel_ = nullptr, *loopWarning_ = nullptr;
    QDoubleSpinBox *nodeTime_ = nullptr, *nodeValue_ = nullptr;
    QToolButton *curveUndo_ = nullptr, *curveRedo_ = nullptr;
};

} // namespace

QWidget *createEditor(const Settings &settings, const Drawing &draft,
                      EditorCallbacks callbacks, QWidget *parent)
{
    return new Editor(settings, draft, std::move(callbacks), parent);
}
void updateEditorStatus(QWidget *editor, QString status)
{
    if (auto *widget = dynamic_cast<Editor *>(editor)) widget->setStatus(status);
}
void updateEditorSettings(QWidget *editor, const Settings &settings, const Drawing &draft)
{
    if (auto *widget = dynamic_cast<Editor *>(editor)) widget->load(settings, draft);
}
void updateEditorPlayback(QWidget *editor, bool paused)
{
    if (auto *widget = dynamic_cast<Editor *>(editor)) widget->setPlayback(paused);
}
void updateEditorDrift(QWidget *editor, const Settings &settings)
{
    if (auto *widget = dynamic_cast<Editor *>(editor)) widget->loadDrift(settings);
}

} // namespace sf
