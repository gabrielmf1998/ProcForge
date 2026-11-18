#pragma once
#include <QColor>
#include <QList>
#include <QWidget>

class QLabel;
class QTimer;

// Gráfico de histórico leve (QPainter puro, sem QtCharts). Até 2 séries; a
// escala é fixa (percentual) ou automática (taxas em bytes/s).
class HistoryGraph : public QWidget {
    Q_OBJECT
public:
    explicit HistoryGraph(const QString &title, QWidget *parent = nullptr);
    void setFixedMax(double max) { m_fixedMax = max; }
    void setSeries(int i, const QString &name, const QColor &color);
    void addSample(double a, double b = 0.0);
    void setValueText(const QString &t) { m_valueText = t; update(); }
    void setCompact(bool c) { m_compact = c; }
    QSize sizeHint() const override { return m_compact ? QSize(120, 60) : QSize(360, 140); }
    QSize minimumSizeHint() const override { return m_compact ? QSize(60, 40) : QSize(160, 90); }
    static constexpr int kHistory = 120;   // 2 minutos a 1 amostra/s
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QString       m_title, m_valueText;
    QString       m_names[2];
    QColor        m_colors[2];
    QList<double> m_data[2];
    int           m_series = 1;
    double        m_fixedMax = 0.0;   // 0 = automático
    bool          m_compact = false;
};

// Janela "Informação do sistema" no espírito do Process Hacker: CPU (total e
// por núcleo), memória e swap, disco e rede, com histórico de 2 minutos.
// Só amostra enquanto está aberta.
class SysInfoWindow : public QWidget {
    Q_OBJECT
public:
    static void showWindow(QWidget *anchor);
protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
private:
    explicit SysInfoWindow(QWidget *parent = nullptr);
    void sample();

    QTimer       *m_timer = nullptr;
    HistoryGraph *m_cpu = nullptr, *m_mem = nullptr, *m_disk = nullptr, *m_net = nullptr;
    QList<HistoryGraph*> m_cores;
    QLabel       *m_info = nullptr;
    QList<quint64> m_prevTotal, m_prevIdle;
    quint64       m_prevDiskR = 0, m_prevDiskW = 0, m_prevRx = 0, m_prevTx = 0;
    qint64        m_prevMs = 0;
};
