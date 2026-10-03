/*COPYRIGHT_HEADER

This file is a part of nm-tray.

Copyright (c)
    2015~now Palo Kisa <palo.kisa@gmail.com>

nm-tray is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

COPYRIGHT_HEADER*/
#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QLockFile>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>

#include "tray.h"

#include "backend/nm_actions.h"
#include "icons.h"

int main(int argc, char * argv[])
{
    QApplication app{argc, argv};
    app.setOrganizationName(QStringLiteral("nm-tray-alt"));
    app.setApplicationName(QStringLiteral("nm-tray-alt"));
    app.setWindowIcon(icons::getIcon(icons::PREFERENCES_NETWORK, true));
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QObject::tr("NetworkManager control for LegeOS and tray-based desktops"));
    parser.addHelpOption();
    QCommandLineOption popupOption(QStringList{QStringLiteral("p"), QStringLiteral("popup")},
                                   QObject::tr("Open the network control menu immediately."));
    parser.addOption(popupOption);
    parser.process(app);
    const bool openPopup = parser.isSet(popupOption);

    qDBusRegisterMetaType<nm::ConnectionSettings>();
    qDBusRegisterMetaType<QList<uint>>();

    QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimeDir.isEmpty()) {
        runtimeDir = QDir::tempPath();
    }
    QLockFile lock(QDir(runtimeDir).filePath(QStringLiteral("nm-tray-alt.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(100)) {
        if (openPopup) {
            QDBusInterface running(QStringLiteral("org.legeos.NetworkTray"),
                                   QStringLiteral("/org/legeos/NetworkTray"),
                                   QString(),
                                   QDBusConnection::sessionBus());
            running.call(QDBus::NoBlock, QStringLiteral("showNetworkMenu"));
            return 0;
        }
        QMessageBox::information(nullptr,
                                 QObject::tr("nm-tray-alt"),
                                 QObject::tr("nm-tray-alt is already running."));
        return 0;
    }

    Tray tray;
    auto bus = QDBusConnection::sessionBus();
    bus.registerService(QStringLiteral("org.legeos.NetworkTray"));
    bus.registerObject(QStringLiteral("/org/legeos/NetworkTray"),
                       &tray,
                       QDBusConnection::ExportAllSlots);
    if (openPopup) {
        QTimer::singleShot(0, &tray, &Tray::showNetworkMenu);
    }
    
    return app.exec();
}
