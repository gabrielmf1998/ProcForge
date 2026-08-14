#include "ScannerWindow.h"
#include "core/MemoryIO.h"
#include "HelperClient.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QSpinBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QSplitter>
#include <QInputDialog>
#include <QFontDatabase>
#include <QCheckBox>

#include <cerrno>
#include <sys/mman.h>

namespace {
quint64 parseAddr(const QString &t, bool *ok)
{
    QString s = t.trimmed();
    if (s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        s = s.mid(2);
    return s.toULongLong(ok, 16);
}
} // namespace

ScannerWindow::ScannerWindow(int pid, quint64 starttime, const QString &name, QWidget *parent)
    : QWidget(parent), m_pid(pid), m_starttime(starttime), m_name(name), m_scanner(pid)
{
    setWindowTitle(i18n("Scanner de memória — %1 (PID %2)", name, pid));
    setWindowFlag(Qt::Window, true);
    resize(720, 560);

    auto *outer = new QVBoxLayout(this);
    auto *tabs = new QTabWidget(this);
    outer->addWidget(tabs);

    // ===================== Aba Scanner =====================
    {
        auto *page = new QWidget(tabs);
        auto *v = new QVBoxLayout(page);

        auto *top = new QHBoxLayout;
        top->addWidget(new QLabel(i18n("Tipo:"), page));
        m_type = new QComboBox(page);
        m_type->addItem(QStringLiteral("Int8"),   int(ScanType::Int8));
        m_type->addItem(QStringLiteral("Int16"),  int(ScanType::Int16));
        m_type->addItem(QStringLiteral("Int32"),  int(ScanType::Int32));
        m_type->addItem(QStringLiteral("Int64"),  int(ScanType::Int64));
        m_type->addItem(QStringLiteral("Float"),  int(ScanType::Float));
        m_type->addItem(QStringLiteral("Double"), int(ScanType::Double));
        m_type->setCurrentIndex(2); // Int32
        connect(m_type, &QComboBox::currentIndexChanged, this, &ScannerWindow::onTypeChanged);
        top->addWidget(m_type);
        top->addWidget(new QLabel(i18n("Valor:"), page));
        m_value = new QLineEdit(page);
        top->addWidget(m_value, 1);
        v->addLayout(top);

        auto *btns = new QHBoxLayout;
        auto *bFirst = new QPushButton(i18n("Primeira busca"), page);
        connect(bFirst, &QPushButton::clicked, this, &ScannerWindow::firstScan);
        btns->addWidget(bFirst);
        m_refineMode = new QComboBox(page);
        m_refineMode->addItem(i18n("= valor"),   int(RefineMode::Exact));
        m_refineMode->addItem(i18n("mudou"),     int(RefineMode::Changed));
        m_refineMode->addItem(i18n("não mudou"), int(RefineMode::Unchanged));
        m_refineMode->addItem(i18n("aumentou"),  int(RefineMode::Increased));
        m_refineMode->addItem(i18n("diminuiu"),  int(RefineMode::Decreased));
        m_refineMode->addItem(i18n("> valor"),   int(RefineMode::GreaterThan));
        m_refineMode->addItem(i18n("< valor"),   int(RefineMode::LessThan));
        btns->addWidget(m_refineMode);
        auto *bRefine = new QPushButton(i18n("Refinar"), page);
        connect(bRefine, &QPushButton::clicked, this, &ScannerWindow::refineScan);
        btns->addWidget(bRefine);
        auto *bNew = new QPushButton(i18n("Nova busca"), page);
        connect(bNew, &QPushButton::clicked, this, &ScannerWindow::newScan);
        btns->addWidget(bNew);
        btns->addStretch();
        m_count = new QLabel(i18n("0 resultados"), page);
        btns->addWidget(m_count);
        v->addLayout(btns);

        auto *split = new QSplitter(Qt::Horizontal, page);

        auto *left = new QWidget(split);
        auto *lv = new QVBoxLayout(left);
        lv->setContentsMargins(0, 0, 0, 0);
        m_results = new QTableWidget(0, 2, left);
        m_results->setHorizontalHeaderLabels({i18n("Endereço"), i18n("Valor")});
        m_results->verticalHeader()->setVisible(false);
        m_results->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_results->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_results->horizontalHeader()->setStretchLastSection(true);
        connect(m_results, &QTableWidget::cellDoubleClicked, this, &ScannerWindow::editSelected);
        lv->addWidget(m_results, 1);
        auto *lb = new QHBoxLayout;
        auto *bEdit = new QPushButton(i18n("Editar valor..."), left);
        connect(bEdit, &QPushButton::clicked, this, &ScannerWindow::editSelected);
        auto *bFreeze = new QPushButton(i18n("Congelar →"), left);
        connect(bFreeze, &QPushButton::clicked, this, &ScannerWindow::freezeSelected);
        lb->addWidget(bEdit);
        lb->addWidget(bFreeze);
        lv->addLayout(lb);

        auto *right = new QWidget(split);
        auto *rv = new QVBoxLayout(right);
        rv->setContentsMargins(0, 0, 0, 0);
        rv->addWidget(new QLabel(i18n("Congelados (reescritos em loop):"), right));
        m_frozen = new QTableWidget(0, 2, right);
        m_frozen->setHorizontalHeaderLabels({i18n("Endereço"), i18n("Valor travado")});
        m_frozen->verticalHeader()->setVisible(false);
        m_frozen->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_frozen->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_frozen->horizontalHeader()->setStretchLastSection(true);
        rv->addWidget(m_frozen, 1);
        auto *bUnfreeze = new QPushButton(i18n("Descongelar"), right);
        connect(bUnfreeze, &QPushButton::clicked, this, &ScannerWindow::unfreezeSelected);
        rv->addWidget(bUnfreeze);

        split->addWidget(left);
        split->addWidget(right);
        split->setStretchFactor(0, 3);
        split->setStretchFactor(1, 2);
        v->addWidget(split, 1);

        tabs->addTab(page, i18n("Scanner"));
    }

    // ===================== Aba Hex =====================
    {
        auto *page = new QWidget(tabs);
        auto *v = new QVBoxLayout(page);

        auto *readBox = new QGroupBox(i18n("Ler memória"), page);
        auto *rl = new QHBoxLayout(readBox);
        rl->addWidget(new QLabel(i18n("Endereço (hex):"), readBox));
        m_hexAddr = new QLineEdit(readBox);
        m_hexAddr->setPlaceholderText(QStringLiteral("0x..."));
        rl->addWidget(m_hexAddr, 1);
        rl->addWidget(new QLabel(i18n("Bytes:"), readBox));
        m_hexLen = new QSpinBox(readBox);
        m_hexLen->setRange(1, 4096);
        m_hexLen->setValue(256);
        rl->addWidget(m_hexLen);
        auto *bRead = new QPushButton(i18n("Ler"), readBox);
        connect(bRead, &QPushButton::clicked, this, &ScannerWindow::hexRead);
        rl->addWidget(bRead);
        v->addWidget(readBox);

        m_hexView = new QPlainTextEdit(page);
        m_hexView->setReadOnly(true);
        m_hexView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_hexView->setLineWrapMode(QPlainTextEdit::NoWrap);
        v->addWidget(m_hexView, 1);

        auto *writeBox = new QGroupBox(i18n("Escrever bytes"), page);
        auto *wl = new QHBoxLayout(writeBox);
        wl->addWidget(new QLabel(i18n("Endereço (hex):"), writeBox));
        m_hexWriteAddr = new QLineEdit(writeBox);
        m_hexWriteAddr->setPlaceholderText(QStringLiteral("0x..."));
        wl->addWidget(m_hexWriteAddr, 1);
        wl->addWidget(new QLabel(i18n("Bytes (hex):"), writeBox));
        m_hexWriteBytes = new QLineEdit(writeBox);
        m_hexWriteBytes->setPlaceholderText(QStringLiteral("de ad be ef"));
        wl->addWidget(m_hexWriteBytes, 1);
        auto *bWrite = new QPushButton(i18n("Gravar"), writeBox);
        connect(bWrite, &QPushButton::clicked, this, &ScannerWindow::hexWrite);
        wl->addWidget(bWrite);
        v->addWidget(writeBox);

        // --- Manipulação de páginas: mmap / mprotect / munmap (via helper) ---
        auto *pageBox = new QGroupBox(i18n("Manipular páginas (mmap / mprotect / munmap)"), page);
        auto *pv = new QVBoxLayout(pageBox);
        auto *pr1 = new QHBoxLayout;
        pr1->addWidget(new QLabel(i18n("Endereço (hex):"), pageBox));
        m_pageAddr = new QLineEdit(pageBox);
        m_pageAddr->setPlaceholderText(QStringLiteral("0x... (alvo de proteger/liberar)"));
        pr1->addWidget(m_pageAddr, 1);
        pr1->addWidget(new QLabel(i18n("Tamanho (bytes):"), pageBox));
        m_pageLen = new QLineEdit(pageBox);
        m_pageLen->setText(QStringLiteral("4096"));
        m_pageLen->setMaximumWidth(110);
        pr1->addWidget(m_pageLen);
        pv->addLayout(pr1);

        auto *pr2 = new QHBoxLayout;
        pr2->addWidget(new QLabel(i18n("Proteção:"), pageBox));
        m_protR = new QCheckBox(QStringLiteral("R"), pageBox); m_protR->setChecked(true);
        m_protW = new QCheckBox(QStringLiteral("W"), pageBox); m_protW->setChecked(true);
        m_protX = new QCheckBox(QStringLiteral("X"), pageBox);
        pr2->addWidget(m_protR); pr2->addWidget(m_protW); pr2->addWidget(m_protX);
        pr2->addStretch();
        auto *bAlloc = new QPushButton(i18n("Alocar (mmap)"), pageBox);
        connect(bAlloc, &QPushButton::clicked, this, &ScannerWindow::memAlloc);
        auto *bProtect = new QPushButton(i18n("Proteger (mprotect)"), pageBox);
        connect(bProtect, &QPushButton::clicked, this, &ScannerWindow::memProtect);
        auto *bFree = new QPushButton(i18n("Liberar (munmap)"), pageBox);
        connect(bFree, &QPushButton::clicked, this, &ScannerWindow::memFree);
        pr2->addWidget(bAlloc); pr2->addWidget(bProtect); pr2->addWidget(bFree);
        pv->addLayout(pr2);
        v->addWidget(pageBox);

        tabs->addTab(page, i18n("Memória (hex)"));
    }

    // Timers: exibição (500 ms) e reescrita de congelados (100 ms).
    m_displayTimer = new QTimer(this);
    m_displayTimer->setInterval(500);
    connect(m_displayTimer, &QTimer::timeout, this, &ScannerWindow::refreshValues);
    m_displayTimer->start();

    m_freezeTimer = new QTimer(this);
    m_freezeTimer->setInterval(100);
    connect(m_freezeTimer, &QTimer::timeout, this, &ScannerWindow::applyFreezes);
    m_freezeTimer->start();
}

ScanType ScannerWindow::currentType() const
{
    return static_cast<ScanType>(m_type->currentData().toInt());
}

void ScannerWindow::onTypeChanged()
{
    m_scanner.setType(currentType());
    m_scanner.reset();
    reloadResults();
}

QByteArray ScannerWindow::readBytes(quint64 addr, int len)
{
    QByteArray b(len, 0);
    const ssize_t n = mem::readv(m_pid, addr, b.data(), len);
    if (n == len)
        return b;
    if (errno == EPERM || errno == EACCES) {
        bool ok = false;
        QByteArray r = helper::readMem(m_pid, addr, len, &ok);
        if (ok) return r;
    }
    return {};
}

bool ScannerWindow::writeBytes(quint64 addr, const QByteArray &raw)
{
    const ssize_t n = mem::writev(m_pid, addr, raw.constData(), raw.size());
    if (n == raw.size())
        return true;
    if (errno == EPERM || errno == EACCES)
        return helper::writeMem(m_pid, addr, raw).ok;
    return false;
}

void ScannerWindow::firstScan()
{
    m_scanner.setType(currentType());
    if (m_value->text().trimmed().isEmpty()) {
        KMessageBox::information(this, i18n("Digite um valor para buscar."));
        return;
    }
    int err = 0;
    const long n = m_scanner.firstScanExact(m_value->text(), &err);
    if (n == 0 && (err == EPERM || err == EACCES)) {
        KMessageBox::error(this,
            i18n("Sem permissão para ler a memória deste processo (uid diferente). "
                 "A busca direta funciona para processos do seu usuário; varredura "
                 "cross-uid pelo helper é um passo futuro."));
        return;
    }
    reloadResults();
}

void ScannerWindow::refineScan()
{
    if (m_scanner.count() == 0) {
        KMessageBox::information(this, i18n("Faça a primeira busca antes de refinar."));
        return;
    }
    const auto mode = static_cast<RefineMode>(m_refineMode->currentData().toInt());
    int err = 0;
    m_scanner.refine(mode, m_value->text(), &err);
    reloadResults();
}

void ScannerWindow::newScan()
{
    m_scanner.reset();
    reloadResults();
}

void ScannerWindow::reloadResults()
{
    const auto &matches = m_scanner.matches();
    const int show = int(qMin<long>(matches.size(), kDisplayCap));
    m_results->setRowCount(show);
    for (int i = 0; i < show; ++i) {
        const Match &mt = matches.at(i);
        auto *a = new QTableWidgetItem(QStringLiteral("0x%1").arg(mt.addr, 0, 16));
        a->setData(Qt::UserRole, qulonglong(mt.addr));
        a->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        auto *val = new QTableWidgetItem(m_scanner.formatValue(mt.last));
        m_results->setItem(i, 0, a);
        m_results->setItem(i, 1, val);
    }
    m_results->resizeColumnToContents(0);

    if (matches.size() > kDisplayCap)
        m_count->setText(i18n("%1 resultados (mostrando %2)", matches.size(), kDisplayCap));
    else if (m_scanner.truncated())
        m_count->setText(i18n("%1+ resultados (truncado — refine mais)", matches.size()));
    else
        m_count->setText(i18n("%1 resultados", matches.size()));
}

void ScannerWindow::refreshValues()
{
    // Atualiza os valores exibidos (releitura leve das linhas visíveis).
    for (int r = 0; r < m_results->rowCount(); ++r) {
        auto *a = m_results->item(r, 0);
        if (!a) continue;
        const quint64 addr = a->data(Qt::UserRole).toULongLong();
        const QByteArray raw = m_scanner.readRaw(addr);
        if (!raw.isEmpty())
            if (auto *v = m_results->item(r, 1))
                v->setText(m_scanner.formatValue(raw));
    }
    for (int r = 0; r < m_frozen->rowCount(); ++r) {
        auto *a = m_frozen->item(r, 0);
        if (!a) continue;
        const quint64 addr = a->data(Qt::UserRole).toULongLong();
        const QByteArray raw = readBytes(addr, m_freezeList.value(r).raw.size());
        if (!raw.isEmpty())
            if (auto *v = m_frozen->item(r, 1))
                v->setText(m_scanner.formatValue(raw));
    }
}

void ScannerWindow::applyFreezes()
{
    for (const Frozen &f : m_freezeList)
        writeBytes(f.addr, f.raw);
}

void ScannerWindow::editSelected()
{
    const int row = m_results->currentRow();
    if (row < 0) {
        KMessageBox::information(this, i18n("Selecione um resultado."));
        return;
    }
    const quint64 addr = m_results->item(row, 0)->data(Qt::UserRole).toULongLong();
    bool ok = false;
    const QString cur = m_results->item(row, 1) ? m_results->item(row, 1)->text() : QString();
    const QString text = QInputDialog::getText(
        this, i18n("Editar valor em 0x%1", QString::number(addr, 16)),
        i18n("Novo valor (%1):", scanTypeName(currentType())),
        QLineEdit::Normal, cur, &ok);
    if (!ok)
        return;
    QByteArray raw;
    if (!m_scanner.parseValue(text, raw)) {
        KMessageBox::error(this, i18n("Valor inválido para o tipo %1.", scanTypeName(currentType())));
        return;
    }
    if (!writeBytes(addr, raw))
        KMessageBox::error(this, i18n("Falha ao escrever na memória (permissão?)."));
}

void ScannerWindow::freezeSelected()
{
    const int row = m_results->currentRow();
    if (row < 0) {
        KMessageBox::information(this, i18n("Selecione um resultado para congelar."));
        return;
    }
    const quint64 addr = m_results->item(row, 0)->data(Qt::UserRole).toULongLong();
    const QByteArray raw = m_scanner.readRaw(addr);
    if (raw.isEmpty()) {
        KMessageBox::error(this, i18n("Não consegui ler o valor atual."));
        return;
    }
    const QString shown = m_scanner.formatValue(raw);
    m_freezeList.append(Frozen{addr, raw, shown});

    const int r = m_frozen->rowCount();
    m_frozen->insertRow(r);
    auto *a = new QTableWidgetItem(QStringLiteral("0x%1").arg(addr, 0, 16));
    a->setData(Qt::UserRole, qulonglong(addr));
    a->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_frozen->setItem(r, 0, a);
    m_frozen->setItem(r, 1, new QTableWidgetItem(shown));
}

void ScannerWindow::unfreezeSelected()
{
    const int row = m_frozen->currentRow();
    if (row < 0)
        return;
    m_frozen->removeRow(row);
    if (row < m_freezeList.size())
        m_freezeList.removeAt(row);
}

void ScannerWindow::hexRead()
{
    bool ok = false;
    const quint64 addr = parseAddr(m_hexAddr->text(), &ok);
    if (!ok) {
        KMessageBox::error(this, i18n("Endereço inválido."));
        return;
    }
    const int len = m_hexLen->value();
    const QByteArray data = readBytes(addr, len);
    if (data.isEmpty()) {
        m_hexView->setPlainText(i18n("<falha ao ler — endereço não mapeado ou sem permissão>"));
        return;
    }
    QString out;
    for (int i = 0; i < data.size(); i += 16) {
        out += QStringLiteral("%1  ").arg(addr + i, 12, 16, QLatin1Char('0'));
        QString ascii;
        for (int j = 0; j < 16; ++j) {
            if (i + j < data.size()) {
                const uchar b = uchar(data.at(i + j));
                out += QStringLiteral("%1 ").arg(b, 2, 16, QLatin1Char('0'));
                ascii += (b >= 32 && b < 127) ? QChar(b) : QChar('.');
            } else {
                out += QStringLiteral("   ");
            }
        }
        out += QStringLiteral(" |") + ascii + QStringLiteral("|\n");
    }
    m_hexView->setPlainText(out);
}

void ScannerWindow::hexWrite()
{
    bool ok = false;
    const quint64 addr = parseAddr(m_hexWriteAddr->text(), &ok);
    if (!ok) {
        KMessageBox::error(this, i18n("Endereço inválido."));
        return;
    }
    QString hex = m_hexWriteBytes->text();
    hex.remove(QLatin1Char(' '));
    const QByteArray raw = QByteArray::fromHex(hex.toLatin1());
    if (raw.isEmpty()) {
        KMessageBox::error(this, i18n("Bytes hex inválidos (ex.: de ad be ef)."));
        return;
    }
    const auto btn = KMessageBox::warningContinueCancel(
        this, i18n("Gravar %1 byte(s) em 0x%2 do processo %3?",
                   raw.size(), QString::number(addr, 16), m_pid),
        i18n("Escrever na memória"));
    if (btn != KMessageBox::Continue)
        return;
    if (writeBytes(addr, raw))
        KMessageBox::information(this, i18n("%1 byte(s) gravado(s).", raw.size()));
    else
        KMessageBox::error(this, i18n("Falha ao gravar (permissão?)."));
}

// ---- manipulação de páginas (mmap/mprotect/munmap) via helper ----

int ScannerWindow::protFromChecks() const
{
    int prot = 0;
    if (m_protR->isChecked()) prot |= PROT_READ;
    if (m_protW->isChecked()) prot |= PROT_WRITE;
    if (m_protX->isChecked()) prot |= PROT_EXEC;
    return prot;
}

void ScannerWindow::memAlloc()
{
    bool ok = false;
    const quint64 len = m_pageLen->text().trimmed().toULongLong(&ok);
    if (!ok || len == 0) {
        KMessageBox::error(this, i18n("Tamanho inválido."));
        return;
    }
    QString err;
    const quint64 addr = helper::allocMem(m_pid, m_starttime, len, protFromChecks(), &err);
    if (addr == 0) {
        KMessageBox::error(this, i18n("mmap falhou: %1", err.isEmpty() ? i18n("erro") : err));
        return;
    }
    const QString hex = QStringLiteral("0x%1").arg(addr, 0, 16);
    m_pageAddr->setText(hex);
    m_hexAddr->setText(hex);
    KMessageBox::information(this, i18n("Região alocada em %1 (%2 bytes).", hex, len));
}

void ScannerWindow::memProtect()
{
    bool ok = false;
    const quint64 addr = parseAddr(m_pageAddr->text(), &ok);
    if (!ok) { KMessageBox::error(this, i18n("Endereço inválido.")); return; }
    const quint64 len = m_pageLen->text().trimmed().toULongLong();
    if (len == 0) { KMessageBox::error(this, i18n("Tamanho inválido.")); return; }
    const actions::Result r = helper::protectMem(m_pid, m_starttime, addr, len, protFromChecks());
    if (r.ok)
        KMessageBox::information(this, i18n("Proteção alterada em 0x%1.", QString::number(addr, 16)));
    else
        KMessageBox::error(this, i18n("mprotect falhou: %1", r.error));
}

void ScannerWindow::memFree()
{
    bool ok = false;
    const quint64 addr = parseAddr(m_pageAddr->text(), &ok);
    if (!ok) { KMessageBox::error(this, i18n("Endereço inválido.")); return; }
    const quint64 len = m_pageLen->text().trimmed().toULongLong();
    if (len == 0) { KMessageBox::error(this, i18n("Tamanho inválido.")); return; }
    const auto btn = KMessageBox::warningContinueCancel(
        this, i18n("Liberar (munmap) %1 byte(s) em 0x%2 do processo %3?\n"
                   "Se a região estiver em uso, o processo pode falhar.",
                   len, QString::number(addr, 16), m_pid),
        i18n("Liberar memória"));
    if (btn != KMessageBox::Continue)
        return;
    const actions::Result r = helper::freeMem(m_pid, m_starttime, addr, len);
    if (r.ok)
        KMessageBox::information(this, i18n("Região liberada."));
    else
        KMessageBox::error(this, i18n("munmap falhou: %1", r.error));
}
