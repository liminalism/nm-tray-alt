#include "ap_steering.h"

#include "../log.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>

#include <ctime>

namespace
{
constexpr const char *kNmService = "org.freedesktop.NetworkManager";
constexpr const char *kDeviceIface = "org.freedesktop.NetworkManager.Device";
constexpr const char *kDeviceWirelessIface = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char *kAccessPointIface = "org.freedesktop.NetworkManager.AccessPoint";
constexpr const char *kActiveConnIface = "org.freedesktop.NetworkManager.Connection.Active";
constexpr const char *kSettingsConnIface = "org.freedesktop.NetworkManager.Settings.Connection";
constexpr const char *kDbusPropsIface = "org.freedesktop.DBus.Properties";

QVariantMap dbusGetAll(const QString &path, const QString &iface)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QString::fromLatin1(kNmService), path, QString::fromLatin1(kDbusPropsIface), QStringLiteral("GetAll"));
    msg << iface;
    const QDBusMessage reply = QDBusConnection::systemBus().call(msg);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        return {};
    }
    return qdbus_cast<QVariantMap>(reply.arguments().at(0));
}

QString toObjectPath(const QVariant &v)
{
    if (v.canConvert<QDBusObjectPath>()) {
        return v.value<QDBusObjectPath>().path();
    }
    if (v.canConvert<QDBusArgument>()) {
        return qdbus_cast<QDBusObjectPath>(v.value<QDBusArgument>()).path();
    }
    return {};
}

QList<QString> toObjectPathList(const QVariant &v)
{
    QList<QString> paths;
    QList<QDBusObjectPath> list;
    if (v.canConvert<QDBusArgument>()) {
        list = qdbus_cast<QList<QDBusObjectPath>>(v.value<QDBusArgument>());
    } else {
        list = qdbus_cast<QList<QDBusObjectPath>>(v);
    }
    paths.reserve(list.size());
    for (const auto &p : list) {
        paths.push_back(p.path());
    }
    return paths;
}

int nowBootSeconds()
{
    struct timespec ts = {};
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0) {
        return 0;
    }
    return static_cast<int>(ts.tv_sec);
}

struct ProfileSteeringInfo
{
    QString bssid;
    QByteArray ssidBytes;
    bool hidden = false;
    bool valid = false;
};

ProfileSteeringInfo readProfileInfo(const QString &settingsPath)
{
    ProfileSteeringInfo info;
    QDBusMessage get = QDBusMessage::createMethodCall(QString::fromLatin1(kNmService),
                                                      settingsPath,
                                                      QString::fromLatin1(kSettingsConnIface),
                                                      QStringLiteral("GetSettings"));
    const QDBusMessage reply = QDBusConnection::systemBus().call(get);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty()) {
        return info;
    }
    const QVariantMap settings = qdbus_cast<QVariantMap>(reply.arguments().at(0));
    const QVariantMap wifi = settings.value(QStringLiteral("802-11-wireless")).toMap();
    info.bssid = wifi.value(QStringLiteral("bssid")).toString();
    info.ssidBytes = wifi.value(QStringLiteral("ssid")).toByteArray();
    info.hidden = wifi.value(QStringLiteral("hidden")).toBool();
    info.valid = true;
    return info;
}

} // namespace

namespace nm
{

PinDecision choosePinTarget(const QByteArray &ssidBytes,
                            const QString &failedBssid,
                            const QList<AccessPointRecord> &visibleAps,
                            int nowBootSeconds,
                            bool profileHidden)
{
    PinDecision decision;
    if (profileHidden || ssidBytes.isEmpty()) {
        decision.action = PinDecision::Action::Unsupported;
        decision.reason = QStringLiteral("hidden profiles are not steered");
        return decision;
    }
    int sameSsid = 0;
    int freshAlternatives = 0;
    const AccessPointRecord *best = nullptr;
    for (const auto &ap : visibleAps) {
        if (ap.ssidBytes != ssidBytes) {
            continue;
        }
        ++sameSsid;
        if (ap.bssid.compare(failedBssid, Qt::CaseInsensitive) == 0) {
            continue;
        }
        // Freshness fails closed: unknown or out-of-window sightings never steer.
        if (ap.lastSeen < 0 || nowBootSeconds < ap.lastSeen
            || nowBootSeconds - ap.lastSeen > kApFreshWindowSec) {
            continue;
        }
        ++freshAlternatives;
        if (best == nullptr || ap.strength > best->strength
            || (ap.strength == best->strength && ap.lastSeen > best->lastSeen)) {
            best = &ap;
        }
    }
    if (best == nullptr) {
        decision.action = PinDecision::Action::NoFreshAlternative;
        decision.reason = QStringLiteral("%1 same-SSID AP(s) visible, %2 fresh alternative(s)")
                              .arg(sameSsid)
                              .arg(freshAlternatives);
        return decision;
    }
    decision.action = PinDecision::Action::PinToTarget;
    decision.targetApPath = best->path;
    decision.targetBssid = best->bssid;
    decision.reason = QStringLiteral("strongest fresh alternative %1 (%2%)")
                          .arg(best->bssid)
                          .arg(best->strength);
    return decision;
}

ApSteeringMonitor::ApSteeringMonitor(QObject *parent)
    : QObject(parent)
{
}

void ApSteeringMonitor::reconcileDevices(const QList<QString> &wifiDevicePaths)
{
    const QSet<QString> next(wifiDevicePaths.begin(), wifiDevicePaths.end());
    QDBusConnection bus = QDBusConnection::systemBus();

    for (const QString &path : next) {
        if (mWatchedDevices.contains(path)) {
            continue;
        }
        const bool ok = bus.connect(QString::fromLatin1(kNmService),
                                    path,
                                    QString::fromLatin1(kDeviceIface),
                                    QStringLiteral("StateChanged"),
                                    this,
                                    SLOT(onDeviceStateChanged(uint,uint,uint)));
        if (!ok) {
            qCWarning(NM_TRAY).noquote()
                << QStringLiteral("ap-steering: failed to watch %1").arg(path);
            continue;
        }
        mWatchedDevices.insert(path);
    }

    for (const QString &path : std::as_const(mWatchedDevices)) {
        if (next.contains(path)) {
            continue;
        }
        bus.disconnect(QString::fromLatin1(kNmService),
                       path,
                       QString::fromLatin1(kDeviceIface),
                       QStringLiteral("StateChanged"),
                       this,
                       SLOT(onDeviceStateChanged(uint,uint,uint)));
        mTuples.remove(path);
    }

    mWatchedDevices = next;
}

void ApSteeringMonitor::reset()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    for (const QString &path : std::as_const(mWatchedDevices)) {
        bus.disconnect(QString::fromLatin1(kNmService),
                       path,
                       QString::fromLatin1(kDeviceIface),
                       QStringLiteral("StateChanged"),
                       this,
                       SLOT(onDeviceStateChanged(uint,uint,uint)));
    }
    mWatchedDevices.clear();
    mTuples.clear();
}

void ApSteeringMonitor::suppressProfile(const QString &settingsPath)
{
    if (settingsPath.isEmpty() || mSuppressed.contains(settingsPath)) {
        return;
    }
    mSuppressed.insert(settingsPath);
    qCWarning(NM_TRAY).noquote() << QStringLiteral("ap-steering: steering suppressed for %1 "
                                                   "(settings write denied)").arg(settingsPath);
}

void ApSteeringMonitor::onDeviceStateChanged(uint newState, uint oldState, uint reason)
{
    const QString devicePath = message().path();
    if (!mWatchedDevices.contains(devicePath)) {
        return;
    }
    const auto state = static_cast<DeviceState>(newState);

    if (state == DeviceState::Failed) {
        // Log every FAILED transition raw: a wrong digit in the matcher below
        // would otherwise compile, run, and never trigger, silently.
        qCWarning(NM_TRAY).noquote()
            << QStringLiteral("ap-steering: %1 FAILED (%2 -> %3, reason %4)")
                   .arg(devicePath)
                   .arg(oldState)
                   .arg(newState)
                   .arg(reason);
        evaluateFailure(devicePath, reason);
        mTuples.remove(devicePath);
        return;
    }
    if (state == DeviceState::Deactivating || state == DeviceState::Disconnected
        || state == DeviceState::Activated) {
        // Terminal states end the activation: drop the tuple so the next
        // activation is attributed from a clean slate.
        mTuples.remove(devicePath);
        return;
    }
    updateTuple(devicePath, newState);
}

void ApSteeringMonitor::updateTuple(const QString &devicePath, uint newState)
{
    SteeringTuple &tuple = mTuples[devicePath];
    tuple.maxStateSeen = qMax(tuple.maxStateSeen, newState);
    if (newState < static_cast<uint>(DeviceState::Prepare)
        || newState > static_cast<uint>(DeviceState::IpConfig)) {
        return;
    }

    const QVariantMap devProps = dbusGetAll(devicePath, QString::fromLatin1(kDeviceIface));
    const QString acPath = toObjectPath(devProps.value(QStringLiteral("ActiveConnection")));
    if (acPath.isEmpty() || acPath == QStringLiteral("/")) {
        return;
    }
    tuple.activeConnectionPath = acPath;
    const QVariantMap acProps = dbusGetAll(acPath, QString::fromLatin1(kActiveConnIface));
    if (acProps.isEmpty()) {
        return;
    }
    const QString connPath = toObjectPath(acProps.value(QStringLiteral("Connection")));
    if (!connPath.isEmpty() && connPath != QStringLiteral("/")) {
        tuple.settingsPath = connPath;
    }
    if (newState != static_cast<uint>(DeviceState::IpConfig)) {
        return;
    }
    // At IP_CONFIG the AP is known: prefer the activation's SpecificObject
    // (set at activation creation), fall back to ActiveAccessPoint.
    QString apPath = toObjectPath(acProps.value(QStringLiteral("SpecificObject")));
    if (apPath.isEmpty() || apPath == QStringLiteral("/")) {
        const QVariantMap wifiProps = dbusGetAll(devicePath, QString::fromLatin1(kDeviceWirelessIface));
        apPath = toObjectPath(wifiProps.value(QStringLiteral("ActiveAccessPoint")));
    }
    if (apPath.isEmpty() || apPath == QStringLiteral("/")) {
        return;
    }
    tuple.apPath = apPath;
    const QVariantMap apProps = dbusGetAll(apPath, QString::fromLatin1(kAccessPointIface));
    tuple.bssid = apProps.value(QStringLiteral("HwAddress")).toString();
    tuple.ssidBytes = apProps.value(QStringLiteral("Ssid")).toByteArray();
    tuple.haveAttribution = !tuple.bssid.isEmpty();
}

void ApSteeringMonitor::evaluateFailure(const QString &devicePath, uint reason)
{
    const auto r = static_cast<DeviceStateReason>(reason);
    if (r != DeviceStateReason::IpConfigUnavailable && r != DeviceStateReason::IpConfigExpired
        && r != DeviceStateReason::DhcpFailed) {
        return;
    }
    const SteeringTuple tuple = mTuples.value(devicePath);
    if (tuple.settingsPath.isEmpty() || tuple.settingsPath == QStringLiteral("/")) {
        qCWarning(NM_TRAY).noquote() << QStringLiteral("ap-steering: %1 failed (reason %2) "
                                                       "with no attributed profile; ignoring")
                                           .arg(devicePath)
                                           .arg(reason);
        return;
    }
    if (mSuppressed.contains(tuple.settingsPath)) {
        return;
    }
    const ProfileSteeringInfo profile = readProfileInfo(tuple.settingsPath);

    if (tuple.maxStateSeen < static_cast<uint>(DeviceState::IpConfig)) {
        // L2 never came up: only actionable when a pin points at a vanished AP.
        if (profile.valid && !profile.bssid.isEmpty()) {
            qCWarning(NM_TRAY).noquote()
                << QStringLiteral("ap-steering: %1 failed before IP config with pin %2; "
                                   "pin looks stale")
                       .arg(tuple.settingsPath, profile.bssid);
            emit stalePinDetected(devicePath, tuple.settingsPath, profile.bssid);
        }
        return;
    }
    if (!tuple.haveAttribution || tuple.bssid.isEmpty()) {
        qCWarning(NM_TRAY).noquote()
            << QStringLiteral("ap-steering: %1 failed after IP config but the AP "
                               "is unattributed; ignoring")
                   .arg(tuple.settingsPath);
        return;
    }
    const QString ssid = QString::fromUtf8(tuple.ssidBytes);
    if (breakerTripped(tuple.settingsPath)) {
        qCWarning(NM_TRAY).noquote()
            << QStringLiteral("ap-steering: %1 hit the re-pin limit; stopping").arg(tuple.settingsPath);
        emit steeringBlocked(tuple.settingsPath,
                             ssid,
                             tr("Re-pinned %1 times in the last hour; stopping automatic steering.").arg(kMaxRepinsPerHour));
        return;
    }

    QList<AccessPointRecord> visible;
    const QVariantMap wifiProps = dbusGetAll(devicePath, QString::fromLatin1(kDeviceWirelessIface));
    for (const QString &apPath : toObjectPathList(wifiProps.value(QStringLiteral("AccessPoints")))) {
        const QVariantMap apProps = dbusGetAll(apPath, QString::fromLatin1(kAccessPointIface));
        if (apProps.isEmpty()) {
            continue;
        }
        AccessPointRecord ap;
        ap.path = apPath;
        ap.devicePath = devicePath;
        ap.ssidBytes = apProps.value(QStringLiteral("Ssid")).toByteArray();
        ap.ssid = QString::fromUtf8(ap.ssidBytes);
        ap.bssid = apProps.value(QStringLiteral("HwAddress")).toString();
        ap.strength = apProps.value(QStringLiteral("Strength")).toInt();
        ap.lastSeen = apProps.contains(QStringLiteral("LastSeen"))
            ? apProps.value(QStringLiteral("LastSeen")).toInt()
            : -1;
        visible.push_back(ap);
    }

    const PinDecision decision = choosePinTarget(tuple.ssidBytes,
                                                 tuple.bssid,
                                                 visible,
                                                 nowBootSeconds(),
                                                 profile.valid && profile.hidden);
    if (decision.action != PinDecision::Action::PinToTarget) {
        qCWarning(NM_TRAY).noquote()
            << QStringLiteral("ap-steering: not steering %1: %2").arg(tuple.settingsPath, decision.reason);
        return;
    }
    recordRepin(tuple.settingsPath);
    SteerRequest request;
    request.devicePath = devicePath;
    request.settingsPath = tuple.settingsPath;
    request.ssid = ssid;
    request.failedBssid = tuple.bssid;
    request.targetApPath = decision.targetApPath;
    request.targetBssid = decision.targetBssid;
    qCWarning(NM_TRAY).noquote()
        << QStringLiteral("ap-steering: %1: %2 failed DHCP, steering to %3 (%4)")
               .arg(request.settingsPath, request.failedBssid, request.targetBssid, decision.reason);
    emit steerRequested(request);
}

bool ApSteeringMonitor::breakerTripped(const QString &settingsPath)
{
    const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-3600);
    QList<QDateTime> &log = mRepinLog[settingsPath];
    while (!log.isEmpty() && log.first() < cutoff) {
        log.pop_front();
    }
    return log.size() >= kMaxRepinsPerHour;
}

void ApSteeringMonitor::recordRepin(const QString &settingsPath)
{
    mRepinLog[settingsPath].push_back(QDateTime::currentDateTimeUtc());
}

} // namespace nm
