#ifndef AP_STEERING_H
#define AP_STEERING_H

// Automatic steering away from access points that authenticate but never
// deliver DHCP (fixture: docs/fixtures/surabaya-dhcp-dead-ap.md).
//
// NetworkManager ranks same-SSID APs with no memory of L3 outcome: a dead AP
// wins every retry by signal strength while wpa_supplicant only blacklists
// L2 failures. This monitor closes the gap. It watches wifi-device
// StateChanged signals, attributes an L3 failure (FAILED from IP_CONFIG) to
// the AP that was in use, and — only when a freshly-seen alternative AP for
// the same SSID exists — asks (via SteerRequest) that the profile's bssid be
// re-pinned to the healthy AP. Pins that point at vanished APs are reported
// via stalePinDetected so they can be cleared instead of bricking the profile.

#include "nm_types.h"

#include <QDBusContext>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QMetaType>
#include <QObject>
#include <QSet>
#include <QString>

namespace nm
{

struct SteerRequest
{
    QString devicePath;
    QString settingsPath;
    QString ssid;
    QString failedBssid;
    QString targetApPath;
    QString targetBssid;
};

struct PinDecision
{
    enum class Action
    {
        PinToTarget,
        NoFreshAlternative,
        Unsupported,
    };

    Action action = Action::NoFreshAlternative;
    QString targetApPath;
    QString targetBssid;
    QString reason;
};

// Pure guard decision over one same-SSID AP set. nowBootSeconds is
// CLOCK_BOOTTIME seconds; an AP counts as fresh when seen within
// kApFreshWindowSec. Returns PinToTarget with the strongest fresh AP whose
// BSSID differs from failedBssid, or the reason steering is withheld.
PinDecision choosePinTarget(const QByteArray &ssidBytes,
                            const QString &failedBssid,
                            const QList<AccessPointRecord> &visibleAps,
                            int nowBootSeconds,
                            bool profileHidden);

inline constexpr int kApFreshWindowSec = 120;
inline constexpr int kMaxRepinsPerHour = 3;

struct SteeringTuple
{
    QString settingsPath;
    QString activeConnectionPath;
    QString apPath;
    QString bssid;
    QByteArray ssidBytes;
    uint maxStateSeen = 0;
    bool haveAttribution = false;
};

class ApSteeringMonitor : public QObject, protected QDBusContext
{
    Q_OBJECT

public:
    explicit ApSteeringMonitor(QObject *parent = nullptr);

    // Reconcile per-device StateChanged matches with the current wifi device
    // set; drops match rules and tuples for devices that are gone. Must run on
    // the monitor's thread (same as NmDbusClient).
    void reconcileDevices(const QList<QString> &wifiDevicePaths);
    void reset();

Q_SIGNALS:
    void steerRequested(const nm::SteerRequest &request);
    void stalePinDetected(const QString &devicePath, const QString &settingsPath, const QString &currentPin);
    void steeringBlocked(const QString &settingsPath, const QString &ssid, const QString &reason);

public Q_SLOTS:
    // Permanently (for this session) stop steering a profile, used when a
    // settings write is denied so a denied write can never re-arm.
    void suppressProfile(const QString &settingsPath);

private Q_SLOTS:
    void onDeviceStateChanged(uint newState, uint oldState, uint reason);

private:
    void updateTuple(const QString &devicePath, uint newState);
    void evaluateFailure(const QString &devicePath, uint reason);
    bool breakerTripped(const QString &settingsPath);
    void recordRepin(const QString &settingsPath);

    QSet<QString> mWatchedDevices;
    QMap<QString, SteeringTuple> mTuples;
    QMap<QString, QList<QDateTime>> mRepinLog;
    QSet<QString> mSuppressed;
};

} // namespace nm

Q_DECLARE_METATYPE(nm::SteerRequest)

#endif
