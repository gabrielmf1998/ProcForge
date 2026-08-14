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
