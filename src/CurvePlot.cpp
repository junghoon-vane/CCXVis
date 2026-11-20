// SPDX-License-Identifier: GPL-2.0-only
#include "CurvePlot.h"
#include "CurveData.h"

#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace {
QColor seriesColor(int index)
{
    return QColor::fromHsv((215 + (index % 360) * 137) % 360, 205, 220);
}

QIcon legendKey(const QColor& color)
{
    QPixmap icon(22, 12);
    icon.fill(Qt::transparent);
    QPainter painter(&icon);
    painter.setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(2, 6, 19, 6);
    return QIcon(icon);
}

struct PlotCurve {
    QColor color;
    ccxvis::PairedCurve curve;
};

struct Axis {
    long double minimum = 0;
    long double maximum = 1;

    void pad()
    {
        const auto delta = minimum == maximum ? std::max(std::abs(minimum) * .05L, 1.L) :
                                                (maximum - minimum) * .05L;
        minimum -= delta;
        maximum += delta;
    }

    long double at(long double fraction) const { return minimum + (maximum - minimum) * fraction; }
    double fraction(long double value) const { return static_cast<double>((value - minimum) / (maximum - minimum)); }

    void zoom(long double anchor, long double factor)
    {
        const auto center = at(anchor);
        const auto low = center + (minimum - center) * factor;
        const auto high = center + (maximum - center) * factor;
        if (std::isfinite(low) && std::isfinite(high) && low < high) {
            minimum = low;
            maximum = high;
        }
    }

    void pan(long double fraction)
    {
        const auto delta = (maximum - minimum) * fraction;
        const auto low = minimum + delta;
        const auto high = maximum + delta;
        if (std::isfinite(low) && std::isfinite(high) && low < high) {
            minimum = low;
            maximum = high;
        }
    }
};

class PlotCanvas : public QWidget {
public:
    explicit PlotCanvas(QWidget* parent) : QWidget(parent)
    {
        setMinimumSize(480, 300);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAccessibleName("XY chart");
        setToolTip("Scroll to zoom. Drag to pan.");
    }

    void setCurves(QString xLabel, QString yLabel, std::vector<PlotCurve> curves)
    {
        xLabel_ = std::move(xLabel);
        yLabel_ = std::move(yLabel);
        curves_ = std::move(curves);
        hasPoints_ = false;
        for (const auto& curve : curves_) {
            for (const auto& sample : curve.curve.points) {
                if (!sample) continue;
                const long double x = sample->x(), y = sample->y();
                if (!hasPoints_) {
                    fullX_ = {x, x};
                    fullY_ = {y, y};
                    hasPoints_ = true;
                } else {
                    fullX_.minimum = std::min(fullX_.minimum, x);
                    fullX_.maximum = std::max(fullX_.maximum, x);
                    fullY_.minimum = std::min(fullY_.minimum, y);
                    fullY_.maximum = std::max(fullY_.maximum, y);
                }
            }
        }
        if (hasPoints_) {
            fullX_.pad();
            fullY_.pad();
        } else {
            fullX_ = fullY_ = {};
        }
        reset();
    }

    void reset()
    {
        x_ = fullX_;
        y_ = fullY_;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto theme = palette();
        painter.fillRect(rect(), theme.base());
        const auto plot = graph();
        const auto fontHeight = fontMetrics().height();
        constexpr int ticks = 5;
        for (int i = 0; i <= ticks; ++i) {
            const auto fraction = static_cast<double>(i) / ticks;
            const auto x = plot.left() + plot.width() * fraction;
            const auto y = plot.bottom() - plot.height() * fraction;
            painter.setPen(theme.midlight().color());
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            painter.setPen(theme.text().color());
            painter.drawText(QRectF(x - 44, plot.bottom() + 3, 88, fontHeight),
                             Qt::AlignHCenter, tickLabel(x_, fraction));
            painter.drawText(QRectF(25, y - fontHeight / 2.0, plot.left() - 32, fontHeight),
                             Qt::AlignRight | Qt::AlignVCenter, tickLabel(y_, fraction));
        }
        painter.drawRect(plot);
        painter.drawText(QRectF(plot.left(), height() - fontHeight - 5, plot.width(), fontHeight),
                         Qt::AlignCenter, fontMetrics().elidedText(xLabel_, Qt::ElideRight, plot.width()));
        painter.save();
        painter.translate(6, plot.center().y());
        painter.rotate(-90);
        painter.drawText(QRectF(-plot.height() / 2, 0, plot.height(), fontHeight), Qt::AlignCenter,
                         fontMetrics().elidedText(yLabel_, Qt::ElideRight, plot.height()));
        painter.restore();

        if (!hasPoints_) {
            painter.drawText(plot, Qt::AlignCenter, curves_.empty() ?
                "Select one or more Y series." : "No paired finite samples.");
            return;
        }
        painter.setClipRect(plot.adjusted(-1, -1, 1, 1));
        for (const auto& curve : curves_) {
            painter.setPen(QPen(curve.color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath path;
            std::optional<QPointF> previous;
            bool single = false;
            const auto drawSingle = [&] {
                if (single && plot.contains(*previous)) painter.drawEllipse(*previous, 2.1, 2.1);
            };
            for (const auto& sample : curve.curve.points) {
                if (!sample) {
                    drawSingle();
                    previous.reset();
                    single = false;
                    continue;
                }
                const QPointF mapped(plot.left() + plot.width() * x_.fraction(sample->x()),
                                     plot.bottom() - plot.height() * y_.fraction(sample->y()));
                if (previous) {
                    if (mapped != *previous) {
                        path.lineTo(mapped);
                        single = false;
                    }
                } else {
                    path.moveTo(mapped);
                    single = true;
                }
                previous = mapped;
            }
            drawSingle();
            painter.drawPath(path);
        }
    }

    void wheelEvent(QWheelEvent* event) override
    {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        const auto position = event->position();
#else
        const auto position = event->posF();
#endif
        const auto plot = graph();
        if (!plot.contains(position)) return;
        const auto factor = std::pow(1.2, -event->angleDelta().y() / 120.);
        x_.zoom((position.x() - plot.left()) / plot.width(), factor);
        y_.zoom((plot.bottom() - position.y()) / plot.height(), factor);
        update();
        event->accept();
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton || !graph().contains(event->pos())) return;
        drag_ = event->pos();
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (!drag_) return;
        const auto delta = event->pos() - *drag_;
        x_.pan(-delta.x() / graph().width());
        y_.pan(delta.y() / graph().height());
        drag_ = event->pos();
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() != Qt::LeftButton) return;
        drag_.reset();
        unsetCursor();
    }

private:
    static QString tickLabel(const Axis& axis, double fraction)
    {
        auto value = axis.at(fraction);
        if (std::abs(value) < (axis.maximum - axis.minimum) * 1.e-12L) value = 0;
        return QString::number(static_cast<double>(value), 'g', 5);
    }

    QRectF graph() const
    {
        const auto margin = fontMetrics().horizontalAdvance("-1.2345e+99") + 33;
        return QRectF(rect()).adjusted(margin, fontMetrics().height(), -49, -2 * fontMetrics().height() - 12);
    }

    Axis fullX_, fullY_, x_, y_;
    bool hasPoints_ = false;
    QString xLabel_, yLabel_;
    std::vector<PlotCurve> curves_;
    std::optional<QPoint> drag_;
};

class CurvePlot : public QDialog {
public:
    CurvePlot(QWidget* parent, const QFileInfo& file, ccxvis::CurveData data)
        : QDialog(parent), data_(std::move(data))
    {
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowFlags(Qt::Window);
        setObjectName("xyPlotWindow");
        setProperty("curveFile", file.canonicalFilePath());
        setWindowTitle(file.fileName() + " — XY Plot");
        resize(1050, 610);

        auto* layout = new QVBoxLayout(this);
        auto* controls = new QHBoxLayout;
        auto* selector = new QFormLayout;
        x_ = new QComboBox(this);
        x_->setObjectName("xyPlotX");
        x_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        x_->setMinimumContentsLength(22);
        selector->addRow("&X axis", x_);
        controls->addLayout(selector, 1);
        auto* reset = new QPushButton("Reset View", this);
        controls->addWidget(reset);
        layout->addLayout(controls);

        auto* body = new QHBoxLayout;
        chart_ = new PlotCanvas(this);
        body->addWidget(chart_, 1);
        auto* legend = new QVBoxLayout;
        auto* label = new QLabel("&Y series", this);
        legend->addWidget(label);
        y_ = new QListWidget(this);
        y_->setObjectName("xyPlotY");
        y_->setMinimumWidth(240);
        y_->setMaximumWidth(310);
        y_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        label->setBuddy(y_);
        legend->addWidget(y_);
        body->addLayout(legend);
        layout->addLayout(body, 1);
        notice_ = new QLabel(this);
        notice_->setTextFormat(Qt::PlainText);
        notice_->setWordWrap(true);
        layout->addWidget(notice_);

        if (data_.native) x_->addItem("Time", -1);
        for (std::size_t i = 0; i < data_.series.size(); ++i) {
            const auto& name = data_.series[i].name;
            x_->addItem(name, static_cast<int>(i));
            auto* item = new QListWidgetItem(legendKey(seriesColor(i)), name, y_);
            item->setToolTip(name);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(i == (data_.native ? 0u : 1u) ? Qt::Checked : Qt::Unchecked);
        }
        connect(x_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { refresh(); });
        connect(y_, &QListWidget::itemChanged, this, [this] { refresh(); });
        connect(reset, &QPushButton::clicked, chart_, &PlotCanvas::reset);
        refresh();
    }

private:
    void refresh()
    {
        std::vector<PlotCurve> curves;
        QStringList labels, notices;
        const int x = x_->currentData().toInt();
        for (int i = 0; i < y_->count(); ++i) {
            if (y_->item(i)->checkState() != Qt::Checked) continue;
            auto curve = ccxvis::pairCurve(data_, x, i);
            const auto& name = data_.series[i].name;
            labels << name;
            if (curve.unmatchedTimes || curve.ambiguousTimes)
                notices << QString("%1 — %2 unmatched times, %3 ambiguous duplicate times")
                    .arg(name).arg(curve.unmatchedTimes).arg(curve.ambiguousTimes);
            curves.push_back({seriesColor(i), std::move(curve)});
        }
        notice_->setText(notices.join('\n'));
        notice_->setVisible(!notices.isEmpty());
        chart_->setCurves(x_->currentText(), labels.size() == 1 ? labels.front() : QString("Value"), std::move(curves));
    }

    ccxvis::CurveData data_;
    QComboBox* x_;
    QListWidget* y_;
    PlotCanvas* chart_;
    QLabel* notice_;
};
}

void openCurvePlot(QWidget* parent, const QString& path)
{
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    for (auto* window : parent->findChildren<QDialog*>("xyPlotWindow", Qt::FindDirectChildrenOnly)) {
        if (!canonical.isEmpty() && window->property("curveFile").toString() == canonical && window->isVisible()) {
            window->showNormal();
            window->raise();
            window->activateWindow();
            return;
        }
    }
    try {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error(file.errorString().toStdString());
        const auto bytes = file.readAll();
        if (file.error() != QFileDevice::NoError) throw std::runtime_error(file.errorString().toStdString());
        auto data = ccxvis::parseCurveData(QString::fromUtf8(bytes), info.suffix().compare("csv", Qt::CaseInsensitive) == 0);
        (new CurvePlot(parent, info, std::move(data)))->show();
    } catch (const std::exception& error) {
        QMessageBox::warning(parent, "Could not open XY Plot", QString::fromUtf8(error.what()));
    }
}
