#include "ThemeManager.h"

#include <QApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>

namespace {
theme::Theme g_current   = theme::System;
QPalette     g_initPalette;
QString      g_initStyle;
bool         g_inited    = false;

QPalette lightPalette()
{
    QPalette p;
    p.setColor(QPalette::Window,          QColor(0xf4, 0xf4, 0xf4));
    p.setColor(QPalette::WindowText,      QColor(0x20, 0x20, 0x20));
    p.setColor(QPalette::Base,            QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::AlternateBase,   QColor(0xf6, 0xf6, 0xf6));
    p.setColor(QPalette::Text,            QColor(0x1a, 0x1a, 0x1a));
    p.setColor(QPalette::Button,          QColor(0xf0, 0xf0, 0xf0));
    p.setColor(QPalette::ButtonText,      QColor(0x1a, 0x1a, 0x1a));
    p.setColor(QPalette::ToolTipBase,     QColor(0xff, 0xff, 0xdc));
    p.setColor(QPalette::ToolTipText,     QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::Highlight,       QColor(0x30, 0x8c, 0xc6));
    p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::PlaceholderText, QColor(0x88, 0x88, 0x88));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0xa0, 0xa0, 0xa0));
    return p;
}

QPalette darkPalette()
{
    QPalette p;
    p.setColor(QPalette::Window,          QColor(0x2b, 0x2e, 0x32));
    p.setColor(QPalette::WindowText,      QColor(0xe6, 0xe6, 0xe6));
    p.setColor(QPalette::Base,            QColor(0x1e, 0x21, 0x25));
    p.setColor(QPalette::AlternateBase,   QColor(0x26, 0x2a, 0x2f));
    p.setColor(QPalette::Text,            QColor(0xe6, 0xe6, 0xe6));
    p.setColor(QPalette::Button,          QColor(0x2f, 0x33, 0x38));
    p.setColor(QPalette::ButtonText,      QColor(0xe6, 0xe6, 0xe6));
    p.setColor(QPalette::ToolTipBase,     QColor(0x3a, 0x3f, 0x45));
    p.setColor(QPalette::ToolTipText,     QColor(0xf0, 0xf0, 0xf0));
    p.setColor(QPalette::Highlight,       QColor(0x2d, 0x74, 0xb5));
    p.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8a, 0x8a));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x77, 0x77, 0x77));
    return p;
}

// Visual clássico ao estilo do Process Hacker no Windows: cinza-claro, base branca,
// seleção azul-clara com texto preto (deixa as cores de categoria aparecerem).
QPalette classicPalette()
{
    QPalette p;
    p.setColor(QPalette::Window,          QColor(0xf0, 0xf0, 0xf0));
    p.setColor(QPalette::WindowText,      QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::Base,            QColor(0xff, 0xff, 0xff));
    p.setColor(QPalette::AlternateBase,   QColor(0xf5, 0xf5, 0xf5));
    p.setColor(QPalette::Text,            QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::Button,          QColor(0xf0, 0xf0, 0xf0));
    p.setColor(QPalette::ButtonText,      QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::ToolTipBase,     QColor(0xff, 0xff, 0xe1));
    p.setColor(QPalette::ToolTipText,     QColor(0x00, 0x00, 0x00));
    p.setColor(QPalette::Highlight,       QColor(0x99, 0xc9, 0xef)); // azul-claro
    p.setColor(QPalette::HighlightedText, QColor(0x00, 0x00, 0x00)); // texto preto
    p.setColor(QPalette::PlaceholderText, QColor(0x80, 0x80, 0x80));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x9a, 0x9a, 0x9a));
    return p;
}

// Folha de estilo "Aero" do Windows 7 (SEM transparência): gradientes suaves,
// bordas beveladas, seleção azul degradê, cantos levemente arredondados. Dá ao
// ProcForge a "sensação" do Process Hacker rodando no Win7. Só toca cromo
// (headers/abas/botões/frames/seleção) — NÃO pinta o fundo dos itens, para
// preservar as cores por categoria do modelo.
QString classicAeroStyleSheet()
{
    return QStringLiteral(R"QSS(
QMenuBar {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #f6f9fd, stop:1 #e7eef8);
    border-bottom: 1px solid #c4d0e2;
}
QMenuBar::item { background: transparent; padding: 4px 10px; }
QMenuBar::item:selected { background: #cde4fb; border: 1px solid #a7cbee; border-radius: 3px; }
QMenu { background: #f7fafe; border: 1px solid #b7c6dc; }
QMenu::item { padding: 4px 24px 4px 22px; }
QMenu::item:selected { background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #e3f0ff, stop:1 #bfdcf7); }
QMenu::separator { height: 1px; background: #dbe2ee; margin: 4px 6px; }

QToolBar {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #f5f8fc, stop:1 #e2e9f3);
    border: 0px; border-bottom: 1px solid #c4d0e2; padding: 2px; spacing: 2px;
}
QToolButton { border: 1px solid transparent; border-radius: 3px; padding: 3px 6px; }
QToolButton:hover { background: #e8f2fe; border: 1px solid #a7cbee; }
QToolButton:pressed { background: #cfe4fb; border: 1px solid #7da8dc; }

QStatusBar {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #eef3fa, stop:1 #dde6f2);
    border-top: 1px solid #c4d0e2;
}
QStatusBar::item { border: 0px; }

QTabWidget::pane { border: 1px solid #a9bad2; top: -1px; background: #ffffff; }
QTabBar::tab {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #f4f7fb, stop:1 #dde5f0);
    border: 1px solid #b3c2d8; border-bottom: 0px;
    border-top-left-radius: 4px; border-top-right-radius: 4px;
    padding: 5px 14px; margin-right: 2px; color: #1b1b1b;
}
QTabBar::tab:selected {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #ffffff, stop:1 #eaf2fc);
    border-color: #7da8dc;
}
QTabBar::tab:!selected { margin-top: 2px; }
QTabBar::tab:hover { background: #eaf3ff; }

QHeaderView::section {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #fbfdff, stop:0.5 #eef4fb, stop:1 #e3ebf6);
    border: 0px; border-right: 1px solid #d3dcea; border-bottom: 1px solid #c4d0e2;
    padding: 4px 6px; color: #23374d;
}
QHeaderView::section:hover { background: #e8f2fe; }

QTreeView, QTableView {
    background: #ffffff; alternate-background-color: #f4f8fd;
    gridline-color: #e3e8f0; border: 1px solid #9db4d0;
    selection-color: #000000;
}
QTreeView::item:hover, QTableView::item:hover {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #f5fbff, stop:1 #e2f1ff);
}
QTreeView::item:selected, QTableView::item:selected {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #d3e9ff, stop:1 #aed4f6);
    border-top: 1px solid #7fb0e6; border-bottom: 1px solid #7fb0e6; color: #000000;
}

QPushButton {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #f7fbff, stop:0.5 #eaf2fc, stop:1 #d8e6f8);
    border: 1px solid #a9c2e0; border-radius: 3px; padding: 4px 12px; color: #14314f;
}
QPushButton:hover {
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #ffffff, stop:1 #dcecfd);
    border: 1px solid #6fa3da;
}
QPushButton:pressed { background: #c9e0f7; border: 1px solid #5b93d0; }
QPushButton:disabled { color: #9aa4b1; border: 1px solid #cdd6e2;
    background: qlineargradient(x1:0,y1:0,x2:0,y2:1, stop:0 #f4f6f9, stop:1 #e8ecf2); }

QLineEdit, QComboBox, QSpinBox, QPlainTextEdit {
    background: #ffffff;
    border: 1px solid #a9bdd6; border-radius: 2px; padding: 2px 4px;
    selection-background-color: #3399ff; selection-color: #ffffff;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid #5b93d0; }

QGroupBox {
    border: 1px solid #bcccdf; border-radius: 4px; margin-top: 8px;
    background: #fbfcfe;
}
QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #274b6d; }

QScrollBar:vertical { background: #eef2f8; width: 15px; margin: 0px; }
QScrollBar::handle:vertical {
    background: qlineargradient(x1:0,y1:0,x2:1,y2:0, stop:0 #cdd9ea, stop:1 #b7c8e0);
    border: 1px solid #a4b6d0; border-radius: 3px; min-height: 24px;
}
QScrollBar::handle:vertical:hover { background: #a9c4e6; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0px; }
)QSS");
}
} // namespace

namespace theme {

void init()
{
    if (g_inited)
        return;
    g_initPalette = qApp->palette();
    g_initStyle   = qApp->style() ? qApp->style()->objectName() : QStringLiteral("Breeze");
    g_inited      = true;
}

void apply(Theme t)
{
    if (!g_inited)
        init();
    g_current = t;
    // A folha Aero só vale no Clássico; nos demais temas ela é limpa.
    if (t != Classic)
        qApp->setStyleSheet(QString());
    switch (t) {
    case System:
        if (!g_initStyle.isEmpty())
            if (QStyle *s = QStyleFactory::create(g_initStyle))
                qApp->setStyle(s);
        qApp->setPalette(g_initPalette);
        break;
    case Light:
        if (QStyle *s = QStyleFactory::create(QStringLiteral("Breeze")))
            qApp->setStyle(s);
        qApp->setPalette(lightPalette());
        break;
    case Dark:
        if (QStyle *s = QStyleFactory::create(QStringLiteral("Breeze")))
            qApp->setStyle(s);
        qApp->setPalette(darkPalette());
        break;
    case Classic:
        if (QStyle *s = QStyleFactory::create(QStringLiteral("Fusion")))
            qApp->setStyle(s);
        qApp->setPalette(classicPalette());
        qApp->setStyleSheet(classicAeroStyleSheet());
        break;
    }
}

Theme current() { return g_current; }

QString label(Theme t)
{
    switch (t) {
    case System:  return QStringLiteral("Sistema (Breeze)");
    case Light:   return QStringLiteral("Claro");
    case Dark:    return QStringLiteral("Escuro");
    case Classic: return QStringLiteral("Clássico (Process Hacker)");
    }
    return {};
}

} // namespace theme
