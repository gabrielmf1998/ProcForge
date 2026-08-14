#pragma once
#include <QList>
#include <QPair>
#include <QString>
#include <QByteArray>
#include <cstdint>

enum class ScanType { Int8, Int16, Int32, Int64, Float, Double };
int     scanTypeSize(ScanType t);
QString scanTypeName(ScanType t);

enum class RefineMode { Exact, Changed, Unchanged, Increased, Decreased, GreaterThan, LessThan };

// Um candidato: endereço + último valor cru conhecido (do tamanho do tipo).
struct Match {
    quint64    addr = 0;
    QByteArray last;
};

// Scanner estilo scanmem/Cheat Engine sobre a memória viva de um processo.
// Fluxo: firstScanExact(valor) -> refine(modo[, valor]) -> refine... até poucos
// candidatos; então editar/congelar. Varre só regiões graváveis (rw), alinhado
// ao tamanho do tipo (fast scan).
class MemoryScanner {
public:
    explicit MemoryScanner(int pid) : m_pid(pid) {}

    void     setType(ScanType t) { m_type = t; }
    ScanType type() const        { return m_type; }
    bool     truncated() const   { return m_truncated; }

    long firstScanExact(const QString &valueText, int *err);
    long refine(RefineMode mode, const QString &valueText, int *err);

    const QList<Match> &matches() const { return m_matches; }
    long count() const                  { return m_matches.size(); }
    void reset()                        { m_matches.clear(); m_truncated = false; }

    QByteArray readRaw(quint64 addr);
    QString    formatValue(const QByteArray &raw) const;
    bool       parseValue(const QString &text, QByteArray &out) const;
    bool       contains(quint64 addr) const;

    static constexpr long kMaxMatches = 2'000'000;

private:
    QList<QPair<quint64, quint64>> writableRegions() const;
    bool   matchExactSlot(const char *p) const;
    double asNumber(const char *p) const;

    int          m_pid;
    ScanType     m_type = ScanType::Int32;
    QList<Match> m_matches;
    qint64       m_targetI = 0;
    double       m_targetD = 0.0;
    bool         m_truncated = false;
};
