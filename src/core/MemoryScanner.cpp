#include "MemoryScanner.h"
#include "MemoryIO.h"
#include "Procfs.h"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {
constexpr quint64 kChunk = 256 * 1024; // múltiplo de 8: mantém o alinhamento
}

int scanTypeSize(ScanType t)
{
    switch (t) {
    case ScanType::Int8:   return 1;
    case ScanType::Int16:  return 2;
    case ScanType::Int32:  return 4;
    case ScanType::Int64:  return 8;
    case ScanType::Float:  return 4;
    case ScanType::Double: return 8;
    }
    return 4;
}

QString scanTypeName(ScanType t)
{
    switch (t) {
    case ScanType::Int8:   return QStringLiteral("Int8");
    case ScanType::Int16:  return QStringLiteral("Int16");
    case ScanType::Int32:  return QStringLiteral("Int32");
    case ScanType::Int64:  return QStringLiteral("Int64");
    case ScanType::Float:  return QStringLiteral("Float");
    case ScanType::Double: return QStringLiteral("Double");
    }
    return {};
}

QList<QPair<quint64, quint64>> MemoryScanner::writableRegions() const
{
    QList<QPair<quint64, quint64>> out;
    for (const procfs::MapEntry &m : procfs::listMaps(m_pid)) {
        if (!m.perms.contains(QLatin1Char('w')) || !m.perms.contains(QLatin1Char('r')))
            continue;
        if (m.path == QLatin1String("[vvar]") || m.path == QLatin1String("[vsyscall]"))
            continue;
        if (m.end > m.start)
            out.append({m.start, m.end});
    }
    return out;
}

double MemoryScanner::asNumber(const char *p) const
{
    switch (m_type) {
    case ScanType::Int8:  { int8_t  v; std::memcpy(&v, p, 1); return v; }
    case ScanType::Int16: { int16_t v; std::memcpy(&v, p, 2); return v; }
    case ScanType::Int32: { int32_t v; std::memcpy(&v, p, 4); return v; }
    case ScanType::Int64: { int64_t v; std::memcpy(&v, p, 8); return static_cast<double>(v); }
    case ScanType::Float: { float   v; std::memcpy(&v, p, 4); return v; }
    case ScanType::Double:{ double  v; std::memcpy(&v, p, 8); return v; }
    }
    return 0.0;
}

bool MemoryScanner::matchExactSlot(const char *p) const
{
    switch (m_type) {
    case ScanType::Int8:  { int8_t  v; std::memcpy(&v, p, 1); return v == static_cast<int8_t>(m_targetI); }
    case ScanType::Int16: { int16_t v; std::memcpy(&v, p, 2); return v == static_cast<int16_t>(m_targetI); }
    case ScanType::Int32: { int32_t v; std::memcpy(&v, p, 4); return v == static_cast<int32_t>(m_targetI); }
    case ScanType::Int64: { int64_t v; std::memcpy(&v, p, 8); return v == static_cast<int64_t>(m_targetI); }
    case ScanType::Float: { float   v; std::memcpy(&v, p, 4);
        return std::fabs(static_cast<double>(v) - m_targetD) <= 0.001 * std::max(1.0, std::fabs(m_targetD)); }
    case ScanType::Double:{ double  v; std::memcpy(&v, p, 8);
        return std::fabs(v - m_targetD) <= 0.001 * std::max(1.0, std::fabs(m_targetD)); }
    }
    return false;
}

long MemoryScanner::firstScanExact(const QString &valueText, int *err)
{
    m_matches.clear();
    m_truncated = false;
    m_targetI = valueText.toLongLong();
    m_targetD = valueText.toDouble();
    const int ts = scanTypeSize(m_type);

    QByteArray buf;
    buf.resize(static_cast<int>(kChunk));

    bool anyReadOk = false;
    int  lastErr   = 0;
    bool hadRegions = false;

    for (const auto &region : writableRegions()) {
        hadRegions = true;
        const quint64 start = region.first, end = region.second;
        for (quint64 off = start; off < end;) {
            const quint64 want = std::min<quint64>(kChunk, end - off);
            const ssize_t n = mem::readv(m_pid, off, buf.data(), want);
            if (n <= 0) {
                lastErr = errno;
                off += want;              // pula chunk ilegível (guard page etc.)
                continue;
            }
            anyReadOk = true;
            const char *p = buf.constData();
            for (quint64 i = 0; i + static_cast<quint64>(ts) <= static_cast<quint64>(n); i += ts) {
                if (matchExactSlot(p + i)) {
                    m_matches.append(Match{off + i, QByteArray(p + i, ts)});
                    if (m_matches.size() >= kMaxMatches) {
                        m_truncated = true;
                        if (err) *err = 0;
                        return m_matches.size();
                    }
                }
            }
            off += static_cast<quint64>(n);
        }
    }

    if (err)
        *err = (hadRegions && !anyReadOk) ? (lastErr ? lastErr : EPERM) : 0;
    return m_matches.size();
}

long MemoryScanner::refine(RefineMode mode, const QString &valueText, int *err)
{
    const int ts = scanTypeSize(m_type);
    m_targetI = valueText.toLongLong();
    m_targetD = valueText.toDouble();

    QList<Match> keep;
    keep.reserve(m_matches.size());

    char cur[8];
    for (const Match &mt : m_matches) {
        const ssize_t n = mem::readv(m_pid, mt.addr, cur, ts);
        if (n != ts)
            continue; // sumiu/ilegível -> descarta candidato

        const double curN  = asNumber(cur);
        const double lastN = mt.last.size() == ts ? asNumber(mt.last.constData()) : curN;
        bool ok = false;
        switch (mode) {
        case RefineMode::Exact:       ok = matchExactSlot(cur); break;
        case RefineMode::Changed:     ok = std::memcmp(cur, mt.last.constData(), ts) != 0; break;
        case RefineMode::Unchanged:   ok = std::memcmp(cur, mt.last.constData(), ts) == 0; break;
        case RefineMode::Increased:   ok = curN > lastN; break;
        case RefineMode::Decreased:   ok = curN < lastN; break;
        case RefineMode::GreaterThan: ok = curN > m_targetD; break;
        case RefineMode::LessThan:    ok = curN < m_targetD; break;
        }
        if (ok)
            keep.append(Match{mt.addr, QByteArray(cur, ts)});
    }
    m_matches = std::move(keep);
    if (err) *err = 0;
    return m_matches.size();
}

QByteArray MemoryScanner::readRaw(quint64 addr)
{
    const int ts = scanTypeSize(m_type);
    QByteArray b(ts, 0);
    if (mem::readv(m_pid, addr, b.data(), ts) != ts)
        return {};
    return b;
}

QString MemoryScanner::formatValue(const QByteArray &raw) const
{
    const int ts = scanTypeSize(m_type);
    if (raw.size() < ts)
        return QStringLiteral("?");
    const char *p = raw.constData();
    switch (m_type) {
    case ScanType::Int8:  { int8_t  v; std::memcpy(&v, p, 1); return QString::number(v); }
    case ScanType::Int16: { int16_t v; std::memcpy(&v, p, 2); return QString::number(v); }
    case ScanType::Int32: { int32_t v; std::memcpy(&v, p, 4); return QString::number(v); }
    case ScanType::Int64: { int64_t v; std::memcpy(&v, p, 8); return QString::number(qlonglong(v)); }
    case ScanType::Float: { float   v; std::memcpy(&v, p, 4); return QString::number(v, 'g', 7); }
    case ScanType::Double:{ double  v; std::memcpy(&v, p, 8); return QString::number(v, 'g', 15); }
    }
    return {};
}

bool MemoryScanner::parseValue(const QString &text, QByteArray &out) const
{
    const int ts = scanTypeSize(m_type);
    out.resize(ts);
    bool ok = false;
    switch (m_type) {
    case ScanType::Int8:  { int8_t  x = static_cast<int8_t>(text.toInt(&ok));   std::memcpy(out.data(), &x, 1); break; }
    case ScanType::Int16: { int16_t x = static_cast<int16_t>(text.toInt(&ok));  std::memcpy(out.data(), &x, 2); break; }
    case ScanType::Int32: { int32_t x = static_cast<int32_t>(text.toInt(&ok));  std::memcpy(out.data(), &x, 4); break; }
    case ScanType::Int64: { int64_t x = static_cast<int64_t>(text.toLongLong(&ok)); std::memcpy(out.data(), &x, 8); break; }
    case ScanType::Float: { float   x = text.toFloat(&ok);   std::memcpy(out.data(), &x, 4); break; }
    case ScanType::Double:{ double  x = text.toDouble(&ok);  std::memcpy(out.data(), &x, 8); break; }
    }
    return ok;
}

bool MemoryScanner::contains(quint64 addr) const
{
    for (const Match &m : m_matches)
        if (m.addr == addr)
            return true;
    return false;
}
