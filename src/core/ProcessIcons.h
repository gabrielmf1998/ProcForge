#pragma once
#include "ProcInfo.h"
#include <QIcon>

// Resolve um ícone para cada processo da lista (como o Process Hacker mostra o
// ícone do .exe). Estratégia, com cache por chave:
//   1) kernel thread            -> engrenagem (applications-system)
//   2) app do usuário           -> ícone do .desktop cujo Exec bate com o binário
//   3) tema de ícones           -> QIcon::fromTheme(nome-do-binário)
//   4) shells/terminais/serviço -> ícones de sistema adequados
//   5) fallback                 -> application-x-executable
namespace proc_icons {

QIcon iconFor(const ProcInfo &p);

} // namespace proc_icons
