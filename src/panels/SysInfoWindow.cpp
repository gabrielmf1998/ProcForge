#include "SysInfoWindow.h"
#include "core/Procfs.h"

#include <KFormat>
#include <KLocalizedString>

#include <QDateTime>
#include <QFile>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScrollArea>
#include <QSysInfo>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

// ===================== HistoryGraph =====================

HistoryGraph::HistoryGraph(const QString &title, QWidget *parent)
    : QWidget(parent), m_title(title)
{
    m_colors[0] = QColor(0x3d, 0xae, 0xe9);
    m_colors[1] = QColor(0xe9, 0x6a, 0x3d);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void HistoryGraph::setSeries(int i, const QString &name, const QColor &color)
{
    if (i < 0 || i > 1) return;
    m_names[i] = name;
    m_colors[i] = color;
    m_series = qMax(m_series, i + 1);
}

void HistoryGraph::addSample(double a, double b)
{
    const double v[2] = {a, b};
    for (int i = 0; i < m_series; ++i) {
        m_data[i].append(qMax(0.0, v[i]));
        if (m_data[i].size() > kHistory)
            m_data[i].removeFirst();
    }
    update();
}

void HistoryGraph::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QPalette pal = palette();
    const bool dark = pal.color(QPalette::Base).lightness() < 128;
    const QRectF area = QRectF(rect()).adjusted(1, m_compact ? 14 : 18, -1, -1);

    p.fillRect(area, dark ? QColor(0x12, 0x16, 0x1a) : QColor(0xfa, 0xfb, 0xfc));
    p.setPen(QPen(dark ? QColor(0x2a, 0x33, 0x3a) : QColor(0xe0, 0xe4, 0xe8), 1));
    for (int i = 1; i < 4; ++i) {
        const double y = area.top() + area.height() * i / 4.0;
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
    p.setPen(QPen(pal.color(QPalette::Mid), 1));
    p.drawRect(area);

    double max = m_fixedMax;
    if (max <= 0.0) {
        for (int s = 0; s < m_series; ++s)
            for (double v : m_data[s]) max = qMax(max, v);
        max = qMax(max * 1.15, 1024.0);
    }

    for (int s = m_series - 1; s >= 0; --s) {
        const QList<double> &d = m_data[s];
        if (d.size() < 2) continue;
        const double step = area.width() / double(kHistory - 1);
        const double x0 = area.right() - step * (d.size() - 1);
        QPainterPath line;
        for (int i = 0; i < d.size(); ++i) {
            const double x = x0 + step * i;
            const double y = area.bottom() - area.height() * qMin(d.at(i) / max, 1.0);
            if (i == 0) line.moveTo(x, y); else line.lineTo(x, y);
        }
        QPainterPath fill = line;
        fill.lineTo(area.right(), area.bottom());
        fill.lineTo(x0, area.bottom());
        fill.closeSubpath();
        QColor fc = m_colors[s];
        fc.setAlpha(dark ? 70 : 55);
        p.fillPath(fill, fc);
        p.setPen(QPen(m_colors[s], 1.5));
        p.drawPath(line);
    }

    QFont f = font();
    if (m_compact) f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    p.setPen(pal.color(QPalette::WindowText));
    const QRectF top(2, 0, width() - 4, area.top() - 1);
    p.drawText(top, Qt::AlignLeft | Qt::AlignVCenter, m_title);
    p.drawText(top, Qt::AlignRight | Qt::AlignVCenter, m_valueText);
}

// ===================== SysInfoWindow =====================

namespace {
QPointer<SysInfoWindow> g_window;

QString readFirstLine(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readLine()).trimmed();
}
} // namespace

void SysInfoWindow::showWindow(QWidget *anchor)
{
    if (!g_window) {
        g_window = new SysInfoWindow;
        g_window->setAttribute(Qt::WA_DeleteOnClose);
        if (anchor)
            g_window->move(anchor->window()->geometry().center() - QPoint(400, 320));
    }
    g_window->show();
    g_window->raise();
    g_window->activateWindow();
}

SysInfoWindow::SysInfoWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(i18n("Informação do sistema"));
    resize(820, 680);

    auto *v = new QVBoxLayout(this);
    auto *grid = new QGridLayout;
    m_cpu  = new HistoryGraph(i18n("CPU"), this);
    m_cpu->setFixedMax(100.0);
    m_cpu->setSeries(0, i18n("Uso"), QColor(0x4c, 0xaf, 0x50));
    m_mem  = new HistoryGraph(i18n("Memória"), this);
    m_mem->setFixedMax(100.0);
    m_mem->setSeries(0, i18n("RAM"), QColor(0x3d, 0xae, 0xe9));
    m_mem->setSeries(1, i18n("Swap"), QColor(0xc0, 0x7a, 0xe0));
    m_disk = new HistoryGraph(i18n("Disco"), this);
    m_disk->setSeries(0, i18n("Leitura"), QColor(0x3d, 0xae, 0xe9));
    m_disk->setSeries(1, i18n("Escrita"), QColor(0xe9, 0x6a, 0x3d));
    m_net  = new HistoryGraph(i18n("Rede"), this);
    m_net->setSeries(0, i18n("Recebido"), QColor(0x4c, 0xaf, 0x50));
    m_net->setSeries(1, i18n("Enviado"), QColor(0xe9, 0xb4, 0x3d));
    grid->addWidget(m_cpu, 0, 0);
    grid->addWidget(m_mem, 0, 1);
    grid->addWidget(m_disk, 1, 0);
    grid->addWidget(m_net, 1, 1);
    v->addLayout(grid, 3);

    const int ncores = qMax(1, int(procfs::cpuTimes().size()) - 1);
    auto *coresBox = new QGroupBox(i18n("Núcleos (%1)", ncores), this);
    auto *coresGrid = new QGridLayout(coresBox);
    coresGrid->setSpacing(4);
    const int cols = ncores <= 4 ? ncores : (ncores <= 16 ? 8 : 12);
    for (int i = 0; i < ncores; ++i) {
        auto *g = new HistoryGraph(i18n("CPU %1", i), coresBox);
        g->setCompact(true);
        g->setFixedMax(100.0);
        g->setSeries(0, QString(), QColor(0x4c, 0xaf, 0x50));
        coresGrid->addWidget(g, i / cols, i % cols);
        m_cores.append(g);
    }
    v->addWidget(coresBox, 2);

    m_info = new QLabel(this);
    m_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_info->setWordWrap(true);
    v->addWidget(m_info);

    m_timer = new QTimer(this);
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &SysInfoWindow::sample);
}

void SysInfoWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (!m_timer->isActive()) {
        sample();
        m_timer->start();
    }
}

void SysInfoWindow::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_timer->stop();
}

void SysInfoWindow::sample()
{
    static const KFormat fmt;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double dt = (m_prevMs && now > m_prevMs) ? (now - m_prevMs) / 1000.0 : 0.0;

    // ---- CPU (agregado + núcleos) ----
    const QList<procfs::CpuTimes> cpus = procfs::cpuTimes();
    if (m_prevTotal.size() == cpus.size()) {
        for (int i = 0; i < cpus.size(); ++i) {
            const quint64 dT = cpus[i].total > m_prevTotal[i] ? cpus[i].total - m_prevTotal[i] : 0;
            const quint64 dI = cpus[i].idle >= m_prevIdle[i] ? cpus[i].idle - m_prevIdle[i] : 0;
            const double pct = dT ? qBound(0.0, 100.0 * double(dT - qMin(dI, dT)) / double(dT), 100.0) : 0.0;
            if (i == 0) {
                m_cpu->addSample(pct);
                m_cpu->setValueText(QStringLiteral("%1%").arg(pct, 0, 'f', 1));
            } else if (i - 1 < m_cores.size()) {
                m_cores[i - 1]->addSample(pct);
                m_cores[i - 1]->setValueText(QStringLiteral("%1%").arg(pct, 0, 'f', 0));
            }
        }
    }
    m_prevTotal.clear();
    m_prevIdle.clear();
    for (const auto &c : cpus) { m_prevTotal << c.total; m_prevIdle << c.idle; }

    // ---- Memória ----
    const procfs::MemInfo mi = procfs::memInfo();
    if (mi.total) {
        const double ram = 100.0 * double(mi.used()) / double(mi.total);
        const double swp = mi.swapTotal ? 100.0 * double(mi.swapUsed()) / double(mi.swapTotal) : 0.0;
        m_mem->addSample(ram, swp);
        m_mem->setValueText(mi.swapTotal
            ? i18n("RAM %1 de %2 · swap %3", fmt.formatByteSize(mi.used(), 1), fmt.formatByteSize(mi.total, 1),
                   fmt.formatByteSize(mi.swapUsed(), 1))
            : i18n("RAM %1 de %2", fmt.formatByteSize(mi.used(), 1), fmt.formatByteSize(mi.total, 1)));
    }

    // ---- Disco e rede (taxas) ----
    quint64 dr = 0, dw = 0, rx = 0, tx = 0;
    const bool haveDisk = procfs::diskTotals(&dr, &dw);
    const bool haveNet  = procfs::netTotals(&rx, &tx);
    if (dt > 0) {
        if (haveDisk) {
            const double r = dr >= m_prevDiskR ? (dr - m_prevDiskR) / dt : 0;
            const double w = dw >= m_prevDiskW ? (dw - m_prevDiskW) / dt : 0;
            m_disk->addSample(r, w);
            m_disk->setValueText(i18n("L %1/s · E %2/s", fmt.formatByteSize(r, 1), fmt.formatByteSize(w, 1)));
        }
        if (haveNet) {
            const double r = rx >= m_prevRx ? (rx - m_prevRx) / dt : 0;
            const double t = tx >= m_prevTx ? (tx - m_prevTx) / dt : 0;
            m_net->addSample(r, t);
            m_net->setValueText(i18n("↓ %1/s · ↑ %2/s", fmt.formatByteSize(r, 1), fmt.formatByteSize(t, 1)));
        }
    }
    m_prevDiskR = dr; m_prevDiskW = dw; m_prevRx = rx; m_prevTx = tx;
    m_prevMs = now;

    // ---- Texto de resumo ----
    QString model;
    {
        QFile f(QStringLiteral("/proc/cpuinfo"));
        if (f.open(QIODevice::ReadOnly))
            while (!f.atEnd()) {
                const QByteArray line = f.readLine();
                if (line.startsWith("model name")) { model = QString::fromUtf8(line).section(QLatin1Char(':'), 1).trimmed(); break; }
            }
    }
    const QString load = readFirstLine(QStringLiteral("/proc/loadavg")).section(QLatin1Char(' '), 0, 2);
    const double up = readFirstLine(QStringLiteral("/proc/uptime")).section(QLatin1Char(' '), 0, 0).toDouble();
    m_info->setText(i18n("<b>Kernel:</b> %1 &nbsp; <b>Sistema:</b> %2<br><b>CPU:</b> %3 (%4 núcleos lógicos)"
                         " &nbsp; <b>Carga (1/5/15 min):</b> %5 &nbsp; <b>Ligado há:</b> %6"
                         " &nbsp; <b>Processos:</b> %7",
                         QSysInfo::kernelVersion().toHtmlEscaped(), QSysInfo::prettyProductName().toHtmlEscaped(),
                         (model.isEmpty() ? QSysInfo::currentCpuArchitecture() : model).toHtmlEscaped(),
                         qMax(1, int(cpus.size()) - 1), load,
                         fmt.formatDuration(qint64(up) * 1000), int(procfs::listPids().size())));
}
