#pragma once
#include <QString>

// Verificação de autorização polkit via D-Bus cru (sem polkit-qt-devel).
// Chama org.freedesktop.PolicyKit1.Authority.CheckAuthorization com o
// subject "system-bus-name" do chamador, permitindo interação (diálogo do KDE).
namespace Polkit {

enum Result { No, Yes, Error };

Result check(const QString &callerBusName, const QString &actionId, QString *errorOut);

} // namespace Polkit
