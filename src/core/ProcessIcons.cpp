#include "ProcessIcons.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>

namespace {

QHash<QString, QIcon>  g_cache;       // chave de resolução -> ícone
QHash<QString, QString> g_execToIcon; // basename do binário do Exec -> nome do ícone
bool g_indexed = false;

// Índice dos .desktop instalados: mapeia o binário (basename do 1º token de Exec)
// para o nome do ícone declarado. Feito uma única vez.
void buildDesktopIndex()
{
    g_indexed = true;
    const QStringList dirs = QStandardPaths::locateAll(
        QStandardPaths::GenericDataLocation, QStringLiteral("applications"),
        QStandardPaths::LocateDirectory);
    for (const QString &dir : dirs) {
        const QFileInfoList files = QDir(dir).entryInfoList(
            {QStringLiteral("*.desktop")}, QDir::Files);
        for (const QFileInfo &fi : files) {
            QFile f(fi.absoluteFilePath());
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;
            QTextStream ts(&f);
            bool inEntry = false;
            QString exec, icon;
            while (!ts.atEnd()) {
                const QString line = ts.readLine();
                if (line.startsWith(QLatin1Char('['))) {
                    inEntry = (line == QLatin1String("[Desktop Entry]"));
                    continue;
                }
                if (!inEntry) continue;
                if (exec.isEmpty() && line.startsWith(QLatin1String("Exec=")))
                    exec = line.mid(5);
                else if (icon.isEmpty() && line.startsWith(QLatin1String("Icon=")))
                    icon = line.mid(5);
            }
            if (exec.isEmpty() || icon.isEmpty())
                continue;
            // 1º token do Exec, ignorando prefixo "env VAR=val" e flags %U/%f.
            const QStringList toks = exec.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            QString bin;
            for (const QString &t : toks) {
                if (t == QLatin1String("env") || t.contains(QLatin1Char('=')))
                    continue;
                bin = t;
                break;
            }
            if (bin.isEmpty()) continue;
            bin = bin.section(QLatin1Char('/'), -1); // basename
            if (!bin.isEmpty() && !g_execToIcon.contains(bin))
                g_execToIcon.insert(bin, icon.trimmed());
        }
    }
}

// Binário "canônico" do processo: basename do argv0 (cmdline), com fallback no comm.
QString binaryOf(const ProcInfo &p)
{
    QString argv0 = p.cmdline.section(QLatin1Char(' '), 0, 0).trimmed();
    if (argv0.isEmpty())
        argv0 = p.name;
    argv0 = argv0.section(QLatin1Char('/'), -1);   // basename
    if (argv0.startsWith(QLatin1Char('-')))         // login shell: -bash
        argv0 = argv0.mid(1);
    return argv0;
}

bool isShellOrTerm(const QString &bin)
{
    static const QSet<QString> s = {
        QStringLiteral("bash"), QStringLiteral("sh"), QStringLiteral("zsh"),
        QStringLiteral("fish"), QStringLiteral("dash"), QStringLiteral("tcsh"),
        QStringLiteral("csh"), QStringLiteral("ksh"), QStringLiteral("login"),
    };
    return s.contains(bin);
}

QIcon firstValid(std::initializer_list<const char *> names, const QIcon &fallback)
{
    for (const char *n : names) {
        QIcon i = QIcon::fromTheme(QString::fromLatin1(n));
        if (!i.isNull())
            return i;
    }
    return fallback;
}

QIcon resolve(const ProcInfo &p)
{
    if (!g_indexed)
        buildDesktopIndex();

    const QIcon generic = QIcon::fromTheme(QStringLiteral("application-x-executable"));

    if (p.kernel)
        return firstValid({"applications-system", "cpu", "system-run"}, generic);

    const QString bin = binaryOf(p);

    // 1) app do usuário via .desktop (dá o ícone "de verdade": firefox, code, vlc…)
    auto it = g_execToIcon.constFind(bin);
    if (it != g_execToIcon.constEnd()) {
        QIcon i = QIcon::fromTheme(*it);
        if (!i.isNull())
            return i;
    }
    // 2) o próprio nome do binário costuma ser um ícone do tema
    if (!bin.isEmpty()) {
        QIcon i = QIcon::fromTheme(bin);
        if (!i.isNull())
            return i;
    }
    // 3) shells/terminais
    if (isShellOrTerm(bin))
        return firstValid({"utilities-terminal", "terminal"}, generic);
    // 4) processo de serviço do systemd
    if (p.service)
        return firstValid({"applications-system", "system-run"}, generic);
    // 5) genérico
    return generic;
}

} // namespace

namespace proc_icons {

QIcon iconFor(const ProcInfo &p)
{
    const QString key = p.kernel ? QStringLiteral("@kernel")
                                 : (binaryOf(p).isEmpty() ? QStringLiteral("@") + p.name
                                                          : binaryOf(p));
    auto it = g_cache.constFind(key);
    if (it != g_cache.constEnd())
        return *it;
    const QIcon ic = resolve(p);
    g_cache.insert(key, ic);
    return ic;
}

} // namespace proc_icons
