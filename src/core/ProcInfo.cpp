#include "ProcInfo.h"
#include <KLocalizedString>

QString ProcInfo::stateText(char s)
{
    switch (s) {
    case 'R': return i18n("Executando");
    case 'S': return i18n("Dormindo");
    case 'D': return i18n("Espera ininterrupta (D)");
    case 'Z': return i18n("Zumbi");
    case 'T': return i18n("Parado (stopped)");
    case 't': return i18n("Rastreado (traced)");
    case 'X': return i18n("Morto");
    case 'I': return i18n("Ocioso (kernel)");
    default:  return QString(QChar(s));
    }
}
