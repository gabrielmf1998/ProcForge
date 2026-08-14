#pragma once
#include "core/MemoryScanner.h"
#include <QWidget>
#include <QList>

class QComboBox;
class QLineEdit;
class QLabel;
class QSpinBox;
class QTableWidget;
class QPlainTextEdit;
class QTimer;

// Janela independente por processo-alvo (estilo Cheat Engine): busca de valor
// com refinamento, edição, freeze por reescrita em loop, e uma aba de hex
// com leitura/escrita de bytes em endereço arbitrário.
class ScannerWindow : public QWidget {
    Q_OBJECT
public:
    ScannerWindow(int pid, quint64 starttime, const QString &name, QWidget *parent = nullptr);

private Q_SLOTS:
    void firstScan();
    void refineScan();
    void newScan();
    void editSelected();
    void freezeSelected();
    void unfreezeSelected();
    void refreshValues();   // timer lento: atualiza valores exibidos
    void applyFreezes();    // timer rápido: reescreve valores congelados
    void hexRead();
    void hexWrite();
    void onTypeChanged();
    // Manipulação de páginas (mmap/mprotect/munmap) via helper.
    void memAlloc();
    void memProtect();
    void memFree();

private:
    QByteArray readBytes(quint64 addr, int len);          // direto + fallback helper
    bool       writeBytes(quint64 addr, const QByteArray &raw);
    void       reloadResults();
    ScanType   currentType() const;
    int        protFromChecks() const;   // R/W/X marcados -> PROT_*

    int      m_pid;
    quint64  m_starttime;
    QString  m_name;
    MemoryScanner m_scanner;

    QComboBox   *m_type = nullptr;
    QLineEdit   *m_value = nullptr;
    QComboBox   *m_refineMode = nullptr;
    QLabel      *m_count = nullptr;
    QTableWidget*m_results = nullptr;
    QTableWidget*m_frozen = nullptr;

    struct Frozen { quint64 addr; QByteArray raw; QString shown; };
    QList<Frozen> m_freezeList;

    QTimer *m_displayTimer = nullptr;
    QTimer *m_freezeTimer = nullptr;

    QLineEdit     *m_hexAddr = nullptr;
    QSpinBox      *m_hexLen = nullptr;
    QPlainTextEdit*m_hexView = nullptr;
    QLineEdit     *m_hexWriteAddr = nullptr;
    QLineEdit     *m_hexWriteBytes = nullptr;

    // Controles de manipulação de páginas
    QLineEdit *m_pageAddr = nullptr;
    QLineEdit *m_pageLen  = nullptr;
    class QCheckBox *m_protR = nullptr;
    class QCheckBox *m_protW = nullptr;
    class QCheckBox *m_protX = nullptr;

    static constexpr int kDisplayCap = 2000;
};
