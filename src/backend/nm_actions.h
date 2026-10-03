#ifndef NM_ACTIONS_H
#define NM_ACTIONS_H

#include "nm_types.h"

#include <QDBusMessage>
#include <QMap>
#include <QObject>
#include <QVariantMap>
#include <functional>

namespace nm
{

using ConnectionSettings = QMap<QString, QVariantMap>;
using AsyncResult = std::function<void(bool ok, const QString &error, const QDBusMessage &reply)>;
// Mutates fetched settings in place. Return false when no write is needed;
// updateConnectionSettings then reports success with a default (skipped) reply.
using SettingsMutator = std::function<bool(ConnectionSettings &)>;

QString humanActionError(const QString &raw);
QString keyMgmtForAp(uint32_t wpaFlags, uint32_t rsnFlags, bool privacy);
bool isWpaPskValid(const QString &password);

class NmActions
{
public:
    static void callAsync(QDBusMessage &&msg, QObject *ctx, AsyncResult done, int timeoutMs = 15000);

    static void activateConnection(const QString &connectionPath,
                                   const QString &devicePath,
                                   const QString &specificObject,
                                   QObject *ctx,
                                   AsyncResult done);
    static void deactivateConnection(const QString &activeConnectionPath, QObject *ctx, AsyncResult done);
    static void disconnectDevice(const QString &devicePath, QObject *ctx, AsyncResult done);
    static void requestScan(const QString &devicePath, QObject *ctx, AsyncResult done);
    static void setNetworkingEnabled(bool enabled, QObject *ctx, AsyncResult done);
    static void setWirelessEnabled(bool enabled, QObject *ctx, AsyncResult done);
    static void setConnectionAutoconnect(const QString &connectionPath, bool enabled, QObject *ctx, AsyncResult done);
    // Serialized GetSettings -> mutate -> Update round-trip. Writes to the same
    // connection queue behind each other so overlapping callers cannot silently
    // clobber each other's Update (last-writer-wins). Thread-safe; completions
    // arrive on the calling thread via ctx.
    static void updateConnectionSettings(const QString &connectionPath,
                                         QObject *ctx,
                                         SettingsMutator mutate,
                                         AsyncResult done);
    static void setBssidPin(const QString &connectionPath,
                            const QString &bssid,
                            QObject *ctx,
                            AsyncResult done);
    static void clearBssidPin(const QString &connectionPath, QObject *ctx, AsyncResult done);
    static void addAndActivateWifi(const AccessPointRecord &ap,
                                   const QString &devicePath,
                                   const QString &password,
                                   QObject *ctx,
                                   AsyncResult done,
                                   bool hidden = false);
    static void updateSavedPsk(const QString &connectionPath,
                               const QString &password,
                               QObject *ctx,
                               AsyncResult done);
    static void applyPublicDns(const QString &connectionPath,
                               const QString &activeConnectionPath,
                               QObject *ctx,
                               AsyncResult done);
};

} // namespace nm

Q_DECLARE_METATYPE(nm::ConnectionSettings)
Q_DECLARE_METATYPE(QList<uint>)

#endif
