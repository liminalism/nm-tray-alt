#include "nm_actions.h"

#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QtEndian>

namespace
{
constexpr const char *kNmService = "org.freedesktop.NetworkManager";
constexpr const char *kNmPath = "/org/freedesktop/NetworkManager";
constexpr const char *kNmIface = "org.freedesktop.NetworkManager";
constexpr const char *kSettingsConnIface = "org.freedesktop.NetworkManager.Settings.Connection";
constexpr const char *kDeviceIface = "org.freedesktop.NetworkManager.Device";
constexpr const char *kDeviceWirelessIface = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char *kDbusPropsIface = "org.freedesktop.DBus.Properties";

QDBusObjectPath objectPathOrRoot(const QString &path)
{
    return QDBusObjectPath(path.isEmpty() ? QStringLiteral("/") : path);
}

nm::ConnectionSettings settingsFromReply(const QDBusMessage &reply)
{
    if (reply.arguments().isEmpty()) {
        return {};
    }
    return qdbus_cast<nm::ConnectionSettings>(reply.arguments().at(0));
}

bool isHex(const QString &s)
{
    for (const QChar ch : s) {
        if (!ch.isDigit()
            && (ch.toLower() < QLatin1Char('a') || ch.toLower() > QLatin1Char('f'))) {
            return false;
        }
    }
    return true;
}

} // namespace

namespace nm
{

QString humanActionError(const QString &raw)
{
    const QString lower = raw.toLower();
    if (lower.contains(QStringLiteral("nosecrets")) || lower.contains(QStringLiteral("no secrets"))) {
        return QObject::tr("A password is needed for this network.");
    }
    if (lower.contains(QStringLiteral("notallowed"))
        || lower.contains(QStringLiteral("not allowed"))
        || lower.contains(QStringLiteral("unavailable"))) {
        return QObject::tr("The Wi-Fi adapter is unavailable. Check Wi-Fi or airplane-mode switches.");
    }
    if (lower.contains(QStringLiteral("timeout"))) {
        return QObject::tr("NetworkManager did not answer in time.");
    }
    return raw;
}

QString keyMgmtForAp(uint32_t wpaFlags, uint32_t rsnFlags, bool privacy)
{
    const uint32_t all = wpaFlags | rsnFlags;
    if ((all & 0x400U) != 0U && (all & 0x100U) == 0U) {
        return QStringLiteral("sae");
    }
    if ((all & 0x100U) != 0U) {
        return QStringLiteral("wpa-psk");
    }
    if ((all & 0x200U) != 0U) {
        return QStringLiteral("wpa-eap");
    }
    if ((all & (0x800U | 0x1000U)) != 0U) {
        return QStringLiteral("owe");
    }
    if (privacy) {
        return QStringLiteral("wpa-psk");
    }
    return {};
}

bool isWpaPskValid(const QString &password)
{
    if (password.size() >= 8 && password.size() <= 63) {
        return true;
    }
    return password.size() == 64 && isHex(password);
}

void NmActions::callAsync(QDBusMessage &&msg, QObject *ctx, AsyncResult done, int timeoutMs)
{
    auto pending = QDBusConnection::systemBus().asyncCall(std::move(msg), timeoutMs);
    auto *watcher = new QDBusPendingCallWatcher(pending, ctx);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, ctx,
                     [done = std::move(done)](QDBusPendingCallWatcher *w) mutable {
                         w->deleteLater();
                         const QDBusMessage reply = w->reply();
                         const bool ok = !w->isError();
                         const QString error = ok ? QString{} : humanActionError(w->error().message());
                         if (done) {
                             done(ok, error, reply);
                         }
                     });
}

void NmActions::activateConnection(const QString &connectionPath,
                                   const QString &devicePath,
                                   const QString &specificObject,
                                   QObject *ctx,
                                   AsyncResult done)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      QString::fromLatin1(kNmPath),
                                                      QString::fromLatin1(kNmIface),
                                                      QStringLiteral("ActivateConnection"));
    msg << QDBusObjectPath(connectionPath)
        << objectPathOrRoot(devicePath)
        << objectPathOrRoot(specificObject);
    callAsync(std::move(msg), ctx, std::move(done));
}

void NmActions::deactivateConnection(const QString &activeConnectionPath, QObject *ctx, AsyncResult done)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      QString::fromLatin1(kNmPath),
                                                      QString::fromLatin1(kNmIface),
                                                      QStringLiteral("DeactivateConnection"));
    msg << QDBusObjectPath(activeConnectionPath);
    callAsync(std::move(msg), ctx, std::move(done));
}

void NmActions::disconnectDevice(const QString &devicePath, QObject *ctx, AsyncResult done)
{
    if (devicePath.isEmpty() || devicePath == QStringLiteral("/")) {
        if (done) {
            done(false, QObject::tr("Invalid device path."), {});
        }
        return;
    }

    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      devicePath,
                                                      QString::fromLatin1(kDeviceIface),
                                                      QStringLiteral("Disconnect"));
    callAsync(std::move(msg), ctx, std::move(done));
}

void NmActions::requestScan(const QString &devicePath, QObject *ctx, AsyncResult done)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      devicePath,
                                                      QString::fromLatin1(kDeviceWirelessIface),
                                                      QStringLiteral("RequestScan"));
    msg << QVariantMap{};
    callAsync(std::move(msg), ctx, std::move(done), 8000);
}

void NmActions::setNetworkingEnabled(bool enabled, QObject *ctx, AsyncResult done)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      QString::fromLatin1(kNmPath),
                                                      QString::fromLatin1(kNmIface),
                                                      QStringLiteral("Enable"));
    msg << enabled;
    callAsync(std::move(msg), ctx, std::move(done));
}

void NmActions::setWirelessEnabled(bool enabled, QObject *ctx, AsyncResult done)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      QString::fromLatin1(kNmPath),
                                                      QString::fromLatin1(kDbusPropsIface),
                                                      QStringLiteral("Set"));
    msg << QString::fromLatin1(kNmIface)
        << QStringLiteral("WirelessEnabled")
        << QVariant::fromValue(QDBusVariant(enabled));
    callAsync(std::move(msg), ctx, std::move(done));
}

namespace
{

struct PendingSettingsWrite
{
    QObject *ctx = nullptr;
    nm::SettingsMutator mutate;
    nm::AsyncResult done;
};

// Serializes GetSettings -> mutate -> Update round-trips per connection path.
// Completions hop threads via callAsync/ctx, so all queue state is mutex
// guarded; the pump runs from completion handlers (event-loop delivered, no
// stack growth).
class SettingsWriteQueue
{
public:
    void enqueue(const QString &path, QObject *ctx, nm::SettingsMutator mutate, nm::AsyncResult done)
    {
        {
            QMutexLocker lock(&mMutex);
            if (mBusy.contains(path)) {
                PendingSettingsWrite pending;
                pending.ctx = ctx;
                pending.mutate = std::move(mutate);
                pending.done = std::move(done);
                mQueues[path].push_back(std::move(pending));
                return;
            }
            mBusy.insert(path);
        }
        runHead(path, ctx, std::move(mutate), std::move(done));
    }

private:
    void runHead(const QString &path, QObject *ctx, nm::SettingsMutator mutate, nm::AsyncResult done)
    {
        // The watchers below are deliberately unparented (null ctx): a caller
        // that dies mid-round-trip must not take the completion (and the pump)
        // with it, or this path would stay busy forever. done-callback safety
        // stays the caller's contract, as with every other NmActions call.
        Q_UNUSED(ctx);
        QDBusMessage get = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                          path,
                                                          QString::fromLatin1(kSettingsConnIface),
                                                          QStringLiteral("GetSettings"));
        nm::NmActions::callAsync(std::move(get),
                                 nullptr,
                                 [this, path, mutate = std::move(mutate), done = std::move(done)](
                                     bool ok, const QString &err, const QDBusMessage &reply) mutable {
                                     if (!ok) {
                                         if (done) {
                                             done(false, err, reply);
                                         }
                                         pump(path);
                                         return;
                                     }
                                     nm::ConnectionSettings settings = settingsFromReply(reply);
                                     if (!mutate(settings)) {
                                         if (done) {
                                             done(true, {}, QDBusMessage{});
                                         }
                                         pump(path);
                                         return;
                                     }
                                     QDBusMessage update =
                                         QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                                        path,
                                                                        QString::fromLatin1(kSettingsConnIface),
                                                                        QStringLiteral("Update"));
                                     update << QVariant::fromValue(settings);
                                     nm::NmActions::callAsync(std::move(update),
                                                              nullptr,
                                                              [this, path, done = std::move(done)](
                                                                  bool ok2,
                                                                  const QString &err2,
                                                                  const QDBusMessage &reply2) mutable {
                                                                  if (done) {
                                                                      done(ok2, err2, reply2);
                                                                  }
                                                                  pump(path);
                                                              });
                                 });
    }

    void pump(const QString &path)
    {
        PendingSettingsWrite next;
        bool haveNext = false;
        {
            QMutexLocker lock(&mMutex);
            auto it = mQueues.find(path);
            if (it != mQueues.end() && !it->isEmpty()) {
                next = std::move(it->first());
                it->removeFirst();
                if (it->isEmpty()) {
                    mQueues.erase(it);
                }
                haveNext = true;
            } else {
                mBusy.remove(path);
            }
        }
        if (haveNext) {
            runHead(path, next.ctx, std::move(next.mutate), std::move(next.done));
        }
    }

    QMutex mMutex;
    QSet<QString> mBusy;
    QMap<QString, QList<PendingSettingsWrite>> mQueues;
};

SettingsWriteQueue *writeQueue()
{
    static SettingsWriteQueue queue;
    return &queue;
}

} // namespace

void NmActions::updateConnectionSettings(const QString &connectionPath,
                                         QObject *ctx,
                                         SettingsMutator mutate,
                                         AsyncResult done)
{
    if (connectionPath.isEmpty() || connectionPath == QStringLiteral("/")) {
        if (done) {
            done(false, QObject::tr("Invalid connection path."), {});
        }
        return;
    }
    writeQueue()->enqueue(connectionPath, ctx, std::move(mutate), std::move(done));
}

void NmActions::setBssidPin(const QString &connectionPath, const QString &bssid, QObject *ctx, AsyncResult done)
{
    updateConnectionSettings(connectionPath,
                             ctx,
                             [bssid](ConnectionSettings &settings) {
                                 QVariantMap wifi = settings.value(QStringLiteral("802-11-wireless"));
                                 if (wifi.value(QStringLiteral("bssid")).toString().compare(
                                         bssid, Qt::CaseInsensitive)
                                     == 0) {
                                     return false;
                                 }
                                 wifi.insert(QStringLiteral("bssid"), bssid);
                                 settings.insert(QStringLiteral("802-11-wireless"), wifi);
                                 return true;
                             },
                             std::move(done));
}

void NmActions::clearBssidPin(const QString &connectionPath, QObject *ctx, AsyncResult done)
{
    updateConnectionSettings(connectionPath,
                             ctx,
                             [](ConnectionSettings &settings) {
                                 QVariantMap wifi = settings.value(QStringLiteral("802-11-wireless"));
                                 if (!wifi.contains(QStringLiteral("bssid"))) {
                                     return false;
                                 }
                                 wifi.remove(QStringLiteral("bssid"));
                                 settings.insert(QStringLiteral("802-11-wireless"), wifi);
                                 return true;
                             },
                             std::move(done));
}

void NmActions::setConnectionAutoconnect(const QString &connectionPath, bool enabled, QObject *ctx, AsyncResult done)
{
    updateConnectionSettings(connectionPath,
                             ctx,
                             [enabled](ConnectionSettings &settings) {
                                 QVariantMap connection = settings.value(QStringLiteral("connection"));
                                 connection.insert(QStringLiteral("autoconnect"), enabled);
                                 settings.insert(QStringLiteral("connection"), connection);
                                 return true;
                             },
                             std::move(done));
}

void NmActions::addAndActivateWifi(const AccessPointRecord &ap,
                                   const QString &devicePath,
                                   const QString &password,
                                   QObject *ctx,
                                   AsyncResult done,
                                   bool hidden)
{
    if (ap.ssidBytes.isEmpty() || devicePath.isEmpty() || devicePath == QStringLiteral("/")) {
        if (done) {
            done(false, QObject::tr("SSID and a usable Wi-Fi adapter are required."), {});
        }
        return;
    }

    const QString keyMgmt = keyMgmtForAp(ap.wpaFlags, ap.rsnFlags, ap.privacy);
    if (keyMgmt == QLatin1String("wpa-eap")) {
        if (done) {
            done(false,
                 QObject::tr("Enterprise Wi-Fi needs more settings than a password. Open the connection editor to create this profile."),
                 {});
        }
        return;
    }
    if ((keyMgmt == QLatin1String("wpa-psk") || keyMgmt == QLatin1String("sae")) && !isWpaPskValid(password)) {
        if (done) {
            done(false, QObject::tr("WPA/WPA2/WPA3 passwords must be 8–63 characters, or exactly 64 hex digits."), {});
        }
        return;
    }

    QVariantMap connection{
        { QStringLiteral("id"), QString::fromUtf8(ap.ssidBytes) },
        { QStringLiteral("type"), QStringLiteral("802-11-wireless") },
        { QStringLiteral("autoconnect"), true },
    };
    QVariantMap wireless{
        { QStringLiteral("ssid"), ap.ssidBytes },
        { QStringLiteral("mode"), QStringLiteral("infrastructure") },
    };
    if (hidden) {
        wireless.insert(QStringLiteral("hidden"), true);
    }

    ConnectionSettings settings;
    settings.insert(QStringLiteral("connection"), connection);
    settings.insert(QStringLiteral("802-11-wireless"), wireless);

    if (!keyMgmt.isEmpty()) {
        QVariantMap security{ { QStringLiteral("key-mgmt"), keyMgmt } };
        if (keyMgmt != QLatin1String("owe")) {
            security.insert(QStringLiteral("psk"), password);
            security.insert(QStringLiteral("psk-flags"), 0U);
        }
        settings.insert(QStringLiteral("802-11-wireless-security"), security);
    }

    QDBusMessage msg = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      QString::fromLatin1(kNmPath),
                                                      QString::fromLatin1(kNmIface),
                                                      QStringLiteral("AddAndActivateConnection"));
    msg << QVariant::fromValue(settings)
        << QDBusObjectPath(devicePath)
        << objectPathOrRoot(ap.path);
    callAsync(std::move(msg), ctx, std::move(done), 30000);
}

void NmActions::updateSavedPsk(const QString &connectionPath,
                               const QString &password,
                               QObject *ctx,
                               AsyncResult done)
{
    if (!isWpaPskValid(password)) {
        if (done) {
            done(false, QObject::tr("WPA/WPA2/WPA3 passwords must be 8–63 characters, or exactly 64 hex digits."), {});
        }
        return;
    }

    QDBusMessage get = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      connectionPath,
                                                      QString::fromLatin1(kSettingsConnIface),
                                                      QStringLiteral("GetSettings"));
    callAsync(std::move(get), ctx,
              [connectionPath, password, ctx, done = std::move(done)](bool ok, const QString &err, const QDBusMessage &reply) mutable {
                  if (!ok) {
                      if (done) {
                          done(false, err, reply);
                      }
                      return;
                  }

                  ConnectionSettings settings = settingsFromReply(reply);
                  QVariantMap security = settings.value(QStringLiteral("802-11-wireless-security"));
                  security.insert(QStringLiteral("psk"), password);
                  security.insert(QStringLiteral("psk-flags"), 0U);
                  if (!security.contains(QStringLiteral("key-mgmt"))) {
                      security.insert(QStringLiteral("key-mgmt"), QStringLiteral("wpa-psk"));
                  }
                  settings.insert(QStringLiteral("802-11-wireless-security"), security);

                  QDBusMessage update = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                                       connectionPath,
                                                                       QString::fromLatin1(kSettingsConnIface),
                                                                       QStringLiteral("Update"));
                  update << QVariant::fromValue(settings);
                  callAsync(std::move(update), ctx, std::move(done));
              });
}

void NmActions::applyPublicDns(const QString &connectionPath,
                               const QString &activeConnectionPath,
                               QObject *ctx,
                               AsyncResult done)
{
    QDBusMessage get = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      connectionPath,
                                                      QString::fromLatin1(kSettingsConnIface),
                                                      QStringLiteral("GetSettings"));
    callAsync(std::move(get), ctx,
              [connectionPath, activeConnectionPath, ctx, done = std::move(done)](bool ok, const QString &err, const QDBusMessage &reply) mutable {
                  if (!ok) {
                      if (done) {
                          done(false, err, reply);
                      }
                      return;
                  }

                  ConnectionSettings settings = settingsFromReply(reply);
                  QVariantMap ipv4 = settings.value(QStringLiteral("ipv4"));
                  ipv4.insert(QStringLiteral("dns"),
                              QVariant::fromValue(QList<uint>{ qToBigEndian(0x01010101U),
                                                               qToBigEndian(0x09090909U) }));
                  ipv4.insert(QStringLiteral("ignore-auto-dns"), true);
                  if (!ipv4.contains(QStringLiteral("method"))) {
                      ipv4.insert(QStringLiteral("method"), QStringLiteral("auto"));
                  }
                  settings.insert(QStringLiteral("ipv4"), ipv4);

                  QDBusMessage update = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                                       connectionPath,
                                                                       QString::fromLatin1(kSettingsConnIface),
                                                                       QStringLiteral("Update"));
                  update << QVariant::fromValue(settings);
                  callAsync(std::move(update), ctx,
                            [connectionPath, activeConnectionPath, ctx, done = std::move(done)](bool ok2,
                                                                                                 const QString &err2,
                                                                                                 const QDBusMessage &reply2) mutable {
                                if (!ok2 || activeConnectionPath.isEmpty()) {
                                    if (done) {
                                        done(ok2, err2, reply2);
                                    }
                                    return;
                                }
                                // Deactivation lets NM re-activate with the changed DNS; the user can also
                                // reconnect manually if policy blocks autoconnect.
                                QDBusMessage deactivate = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                                                         QString::fromLatin1(kNmPath),
                                                                                         QString::fromLatin1(kNmIface),
                                                                                         QStringLiteral("DeactivateConnection"));
                                deactivate << QDBusObjectPath(activeConnectionPath);
                                callAsync(std::move(deactivate), ctx, std::move(done));
                            });
              });
}

} // namespace nm
