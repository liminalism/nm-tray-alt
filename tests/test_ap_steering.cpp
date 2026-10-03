// Unit tests for the AP-steering guard decision (pure function only: no D-Bus,
// no threads, deterministic). Run via ctest; safe in minimal build environments.
#include "../src/backend/ap_steering.h"

#include <QtTest/QtTest>

namespace
{
nm::AccessPointRecord makeAp(const char *path,
                             const char *ssid,
                             const char *bssid,
                             int strength,
                             int lastSeen)
{
    nm::AccessPointRecord ap;
    ap.path = QString::fromLatin1(path);
    ap.ssidBytes = QByteArray(ssid);
    ap.ssid = QString::fromUtf8(ssid);
    ap.bssid = QString::fromLatin1(bssid);
    ap.strength = strength;
    ap.lastSeen = lastSeen;
    return ap;
}

constexpr int kNow = 100000;
constexpr int kFresh = kNow - 10;

using Action = nm::PinDecision::Action;

} // namespace

class TestApSteering : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void picksStrongestFreshAlternative()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/dead", "Hotel", "24:A4:3C:8F:8E:5D", 90, kFresh),
            makeAp("/ap/weak", "Hotel", "06:2B:A6:5F:C0:6B", 30, kFresh),
            makeAp("/ap/best", "Hotel", "06:2B:A6:5F:C0:70", 55, kFresh),
        };
        const nm::PinDecision d = nm::choosePinTarget("Hotel", "24:A4:3C:8F:8E:5D", aps, kNow, false);
        QVERIFY(d.action == Action::PinToTarget);
        QCOMPARE(d.targetBssid, QStringLiteral("06:2B:A6:5F:C0:70"));
        QCOMPARE(d.targetApPath, QStringLiteral("/ap/best"));
    }

    void skipsFailedBssidCaseInsensitively()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/dead", "Hotel", "24:a4:3c:8f:8e:5d", 90, kFresh),
            makeAp("/ap/ok", "Hotel", "06:2B:A6:5F:C0:6B", 30, kFresh),
        };
        const nm::PinDecision d = nm::choosePinTarget("Hotel", "24:A4:3C:8F:8E:5D", aps, kNow, false);
        QVERIFY(d.action == Action::PinToTarget);
        QCOMPARE(d.targetBssid, QStringLiteral("06:2B:A6:5F:C0:6B"));
    }

    void soleApYieldsNoFreshAlternative()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/dead", "Hotel", "24:A4:3C:8F:8E:5D", 90, kFresh),
        };
        const nm::PinDecision d = nm::choosePinTarget("Hotel", "24:A4:3C:8F:8E:5D", aps, kNow, false);
        QVERIFY(d.action == Action::NoFreshAlternative);
        QVERIFY(d.targetBssid.isEmpty());
    }

    void staleSightingsNeverSteer()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/dead", "Hotel", "24:A4:3C:8F:8E:5D", 90, kFresh),
            makeAp("/ap/ghost", "Hotel", "06:2B:A6:5F:C0:6B", 80, kNow - nm::kApFreshWindowSec - 1),
        };
        const nm::PinDecision d = nm::choosePinTarget("Hotel", "24:A4:3C:8F:8E:5D", aps, kNow, false);
        QVERIFY(d.action == Action::NoFreshAlternative);
    }

    void unknownLastSeenNeverSteers()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/dead", "Hotel", "24:A4:3C:8F:8E:5D", 90, kFresh),
            makeAp("/ap/unknown", "Hotel", "06:2B:A6:5F:C0:6B", 80, -1),
        };
        const nm::PinDecision d = nm::choosePinTarget("Hotel", "24:A4:3C:8F:8E:5D", aps, kNow, false);
        QVERIFY(d.action == Action::NoFreshAlternative);
    }

    void futureLastSeenNeverSteers()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/dead", "Hotel", "24:A4:3C:8F:8E:5D", 90, kFresh),
            makeAp("/ap/future", "Hotel", "06:2B:A6:5F:C0:6B", 80, kNow + 60),
        };
        const nm::PinDecision d = nm::choosePinTarget("Hotel", "24:A4:3C:8F:8E:5D", aps, kNow, false);
        QVERIFY(d.action == Action::NoFreshAlternative);
    }

    void freshnessBoundaryIsInclusive()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/dead", "Hotel", "24:A4:3C:8F:8E:5D", 90, kFresh),
            makeAp("/ap/edge", "Hotel", "06:2B:A6:5F:C0:6B", 80, kNow - nm::kApFreshWindowSec),
        };
        const nm::PinDecision d = nm::choosePinTarget("Hotel", "24:A4:3C:8F:8E:5D", aps, kNow, false);
        QVERIFY(d.action == Action::PinToTarget);
        QCOMPARE(d.targetBssid, QStringLiteral("06:2B:A6:5F:C0:6B"));
    }

    void otherSsidsAreIgnored()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/dead", "Hotel", "24:A4:3C:8F:8E:5D", 90, kFresh),
            makeAp("/ap/cafe", "Cafe", "AA:BB:CC:DD:EE:FF", 95, kFresh),
        };
        const nm::PinDecision d = nm::choosePinTarget("Hotel", "24:A4:3C:8F:8E:5D", aps, kNow, false);
        QVERIFY(d.action == Action::NoFreshAlternative);
    }

    void strengthTiePrefersFresher()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/dead", "Hotel", "24:A4:3C:8F:8E:5D", 90, kFresh),
            makeAp("/ap/older", "Hotel", "06:2B:A6:5F:C0:6B", 55, kNow - 100),
            makeAp("/ap/newer", "Hotel", "06:2B:A6:5F:C0:70", 55, kNow - 5),
        };
        const nm::PinDecision d = nm::choosePinTarget("Hotel", "24:A4:3C:8F:8E:5D", aps, kNow, false);
        QVERIFY(d.action == Action::PinToTarget);
        QCOMPARE(d.targetApPath, QStringLiteral("/ap/newer"));
    }

    void hiddenProfilesAreUnsupported()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/ok", "Hotel", "06:2B:A6:5F:C0:6B", 80, kFresh),
        };
        const nm::PinDecision d = nm::choosePinTarget("Hotel", "24:A4:3C:8F:8E:5D", aps, kNow, true);
        QVERIFY(d.action == Action::Unsupported);
    }

    void emptySsidIsUnsupported()
    {
        const QList<nm::AccessPointRecord> aps{
            makeAp("/ap/ok", "Hotel", "06:2B:A6:5F:C0:6B", 80, kFresh),
        };
        const nm::PinDecision d = nm::choosePinTarget("", "24:A4:3C:8F:8E:5D", aps, kNow, false);
        QVERIFY(d.action == Action::Unsupported);
    }
};

QTEST_MAIN(TestApSteering)
#include "test_ap_steering.moc"
