#pragma once
#include "FdInjector.h"   // reaproveita inject::Result
#include <QString>

// Injeção de biblioteca: força o processo alvo a executar dlopen(path) via ptrace
// (hijack de uma thread, chamada de função com retorno-sentinela). Análogo à
// "injeção de DLL" do Process Hacker. Requer poder ptrace sobre o alvo
// (mesmo uid com ptrace_scope=0, ou helper com CAP_SYS_PTRACE) e libc compatível.
namespace inject {

Result injectLibrary(int pid, const QString &soPath);

} // namespace inject
