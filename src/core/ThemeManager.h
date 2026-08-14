#pragma once
#include <QString>

// Temas aplicáveis ao vivo. "Classic" reproduz o visual do Process Hacker clássico
// (estilo Fusion, fundo claro, seleção azul, listras das categorias em tons pálidos).
namespace theme {

enum Theme { System, Light, Dark, Classic };

void    init();          // captura o tema inicial do Plasma; chamar 1x após criar o QApplication
void    apply(Theme t);  // aplica imediatamente (paleta + estilo)
Theme   current();
QString label(Theme t);

} // namespace theme
