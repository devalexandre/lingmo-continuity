/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "channel.h"
#include "deskflowconfig.h"
#include "dragwatcher.h"
#include "identity.h"

#include <QFile>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using DeskflowConfig::Computer;
using DeskflowConfig::links;
using DeskflowConfig::serverConfig;
using DeskflowConfig::neighbourAt;
using DeskflowConfig::isValidHotkey;
using DeskflowConfig::hotkeyLabel;
using DeskflowConfig::writeSettings;
using DeskflowConfig::writeTrustFile;
using DeskflowConfig::readTrustFile;

class TestContinuity : public QObject
{
    Q_OBJECT

private slots:
    void linksSideBySide()
    {
        // laptop | desk, same height
        const QList<Computer> c{{"desk", QRect(1920, 0, 1920, 1080)}, {"laptop", QRect(0, 0, 1920, 1080)}};
        const QList<DeskflowConfig::Link> l = links(c);
        QCOMPARE(l.size(), 2);
        bool deskLeft = false, laptopRight = false;
        for (const DeskflowConfig::Link &x : l) {
            if (x.from == "desk" && x.dir == "left" && x.to == "laptop" && x.s0 == 0 && x.s1 == 100)
                deskLeft = true;
            if (x.from == "laptop" && x.dir == "right" && x.to == "desk" && x.d0 == 0 && x.d1 == 100)
                laptopRight = true;
        }
        QVERIFY(deskLeft);
        QVERIFY(laptopRight);
    }

    void linksPartialEdge()
    {
        // A 1080p laptop to the right of the lower half of a 4K-tall desk
        const QList<Computer> c{{"desk", QRect(0, 0, 1000, 2000)}, {"laptop", QRect(1000, 1000, 500, 1000)}};
        const QList<DeskflowConfig::Link> l = links(c);
        QCOMPARE(l.size(), 2);
        for (const DeskflowConfig::Link &x : l) {
            if (x.from == "desk") {
                QCOMPARE(x.dir, QString("right"));
                QCOMPARE(x.s0, 50);
                QCOMPARE(x.s1, 100);
                QCOMPARE(x.d0, 0);
                QCOMPARE(x.d1, 100);
            } else {
                QCOMPARE(x.dir, QString("left"));
                QCOMPARE(x.s0, 0);
                QCOMPARE(x.s1, 100);
                QCOMPARE(x.d0, 50);
                QCOMPARE(x.d1, 100);
            }
        }
        const QString conf = serverConfig(c, Options());
        QVERIFY(conf.contains("\t\tright(50,100) = laptop\n"));
        QVERIFY(conf.contains("\t\tleft = desk(50,100)\n"));
        QVERIFY(conf.contains("keystroke(ScrollLock) = lockCursorToScreen(toggle)"));
        QVERIFY(conf.contains("keystroke(control+super+Right) = switchToNextScreen\n"));
    }

    void linksAboveAndCorner()
    {
        // tv above desk; tablet only touching desk's corner: no link
        const QList<Computer> c{{"desk", QRect(0, 1080, 1920, 1080)},
                                {"tv", QRect(0, 0, 1920, 1080)},
                                {"tablet", QRect(1920, 2160, 800, 600)}};
        const QList<DeskflowConfig::Link> l = links(c);
        QCOMPARE(l.size(), 2);
        QCOMPARE(neighbourAt(c, "desk", "up", 0.5), QString("tv"));
        QCOMPARE(neighbourAt(c, "tv", "down", 0.1), QString("desk"));
        QCOMPARE(neighbourAt(c, "desk", "right", 0.5), QString());
    }

    void hotkeys()
    {
        QVERIFY(isValidHotkey("ScrollLock"));
        QVERIFY(isValidHotkey("control+alt+L"));
        QVERIFY(isValidHotkey("super+shift+Right"));
        QVERIFY(!isValidHotkey(""));
        QVERIFY(!isValidHotkey("control+"));
        QVERIFY(!isValidHotkey("evil) = switchToScreen(x"));
        QVERIFY(!isValidHotkey("bogus+L"));
        QCOMPARE(hotkeyLabel("control+super+Right"), QString("Ctrl+Super+→"));
        QCOMPARE(hotkeyLabel("ScrollLock"), QString("Scroll Lock"));
        Options o;
        o.lockHotkey.clear();
        o.switchHotkey = "x) = keystroke(a";
        const QString conf = serverConfig({{"a", QRect(0, 0, 10, 10)}}, o);
        QVERIFY(!conf.contains("keystroke"));
    }

    void settingsFile()
    {
        QTemporaryDir dir;
        DeskflowConfig::Settings st;
        st.mode = "server";
        st.name = "desk";
        st.port = 24810;
        st.certificate = dir.filePath("tls/deskflow.pem");
        st.serverConfig = dir.filePath("server.conf");
        st.computers = {"desk", "laptop"};
        st.options.switchMode = "doubletap";
        st.options.clipboard = false;
        QVERIFY(writeSettings(dir.filePath("Deskflow.conf"), st));
        QSettings ini(dir.filePath("Deskflow.conf"), QSettings::IniFormat);
        QCOMPARE(ini.value("security/tlsEnabled").toBool(), true);
        QCOMPARE(ini.value("security/checkPeerFingerprints").toBool(), true);
        QCOMPARE(ini.value("core/computerName").toString(), QString("desk"));
        QCOMPARE(ini.value("server/enableSwitchDoubleTap").toBool(), true);
        QCOMPARE(ini.value("server/enableSwitchDelay").toBool(), false);
        QCOMPARE(ini.value("server/enableClipboard").toBool(), false);
        QCOMPARE(ini.value("computer_laptop/name").toString(), QString("laptop"));
        QCOMPARE(QFile(dir.filePath("Deskflow.conf")).permissions() & (QFile::ReadOther | QFile::ReadGroup),
                 QFile::Permissions());
    }

    void trustFile()
    {
        QTemporaryDir dir;
        const QString fp = QString("AB").repeated(32);
        QVERIFY(writeTrustFile(dir.filePath("tls/trusted-clients"), {fp}));
        QFile f(dir.filePath("tls/trusted-clients"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), QByteArray("v2:sha256:" + QByteArray("ab").repeated(32) + "\n"));
        QCOMPARE(readTrustFile(dir.filePath("tls/trusted-clients")), QStringList{fp});
    }

    void pairingCodeIsSymmetric()
    {
        const QString a = QString("1F").repeated(32), b = QString("C4").repeated(32);
        const QString code = Identity::pairingCode(a, b);
        QCOMPARE(code, Identity::pairingCode(b, a));
        QCOMPARE(code.size(), 7);
        QVERIFY(code != Identity::pairingCode(a, QString("C5").repeated(32)));
        QCOMPARE(Identity::formatFingerprint("ABCDEF"), QString("AB:CD:EF"));
    }

    void fileNames()
    {
        QCOMPARE(Channel::safeFileName("../../.bashrc"), QString("bashrc"));
        QCOMPARE(Channel::safeFileName("C:\\Users\\x\\foto.jpg"), QString("foto.jpg"));
        QCOMPARE(Channel::safeFileName(".."), QString("file"));
        QTemporaryDir dir;
        QFile(dir.filePath("a.txt")).open(QIODevice::WriteOnly);
        QCOMPARE(Channel::uniquePath(dir.path(), "a.txt"), dir.filePath("a (2).txt"));
        QCOMPARE(Channel::uniquePath(dir.path(), "b.txt"), dir.filePath("b.txt"));
    }

    void uriList()
    {
        const QByteArray data = "# comment\r\nfile:///home/u/Relat%C3%B3rio.pdf\r\nhttps://example.com/x\r\nfile:///tmp/a%20b\r\n";
        QCOMPARE(DragWatcher::parseUriList(data), (QStringList{"/home/u/Relatório.pdf", "/tmp/a b"}));
    }

    // Two channels on loopback: pairing needs both yes; files only once paired
    void channelPairAndFiles()
    {
        QTemporaryDir dir;
        Identity ida(dir.filePath("a/deskflow.pem")), idb(dir.filePath("b/deskflow.pem"));
        QVERIFY(ida.ensure());
        QVERIFY(idb.ensure());
        QVERIFY(ida.fingerprint() != idb.fingerprint());

        Channel a(&ida), b(&idb);
        QStringList trustedByA, trustedByB;
        a.setTrust([&](const QString &id) { return trustedByA.contains(id); });
        b.setTrust([&](const QString &id) { return trustedByB.contains(id); });
        a.setHello({{"name", "pc-a"}, {"label", "PC A"}});
        b.setHello({{"name", "pc-b"}, {"label", "PC B"}});
        b.setDownloadDir(dir.filePath("downloads-b"));
        const int portB = 24900 + (QCoreApplication::applicationPid() % 50);
        QVERIFY(b.listen(portB));

        // Files from an unpaired PC are refused
        QSignalSpy sentA(&a, &Channel::sendFinished);
        QSignalSpy gotB(&b, &Channel::filesReceived);
        QFile f(dir.filePath("hello.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(700 * 1024, 'x'));
        f.close();
        a.sendFiles("127.0.0.1", portB, idb.fingerprint(), {f.fileName()});
        QVERIFY(sentA.wait(5000));
        QCOMPARE(sentA.last().at(1).toBool(), false);
        QCOMPARE(gotB.size(), 0);

        // Pairing: both get the same code; A says yes, B says no -> no pairing
        QSignalSpy startA(&a, &Channel::pairingStarted), startB(&b, &Channel::pairingStarted);
        QSignalSpy doneA(&a, &Channel::pairingFinished), doneB(&b, &Channel::pairingFinished);
        a.pair("127.0.0.1", portB);
        QTRY_COMPARE_WITH_TIMEOUT(startA.size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(startB.size(), 1, 5000);
        QCOMPARE(startA.at(0).at(1).toString(), idb.fingerprint());
        QCOMPARE(startB.at(0).at(1).toString(), ida.fingerprint());
        QCOMPARE(startA.at(0).at(2).toString(), startB.at(0).at(2).toString());
        QCOMPARE(startB.at(0).at(3).toBool(), true);
        QCOMPARE(startB.at(0).at(0).toJsonObject().value("label").toString(), QString("PC A"));
        a.answerPairing(true);
        b.answerPairing(false);
        QTRY_COMPARE_WITH_TIMEOUT(doneA.size(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(doneB.size(), 1, 5000);
        QCOMPARE(doneA.at(0).at(0).toBool(), false);
        QCOMPARE(doneA.at(0).at(4).toString(), QString("rejected-there"));

        // Again, both say yes
        a.pair("127.0.0.1", portB);
        QTRY_COMPARE_WITH_TIMEOUT(startB.size(), 2, 5000);
        b.answerPairing(true);
        QTest::qWait(200);
        QCOMPARE(doneB.size(), 1); // still waiting for A
        a.answerPairing(true);
        QTRY_COMPARE_WITH_TIMEOUT(doneA.size(), 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(doneB.size(), 2, 5000);
        QCOMPARE(doneA.at(1).at(0).toBool(), true);
        QCOMPARE(doneB.at(1).at(0).toBool(), true);
        trustedByA << idb.fingerprint();
        trustedByB << ida.fingerprint();

        // Now files go through
        a.sendFiles("127.0.0.1", portB, idb.fingerprint(), {f.fileName(), f.fileName()});
        QTRY_COMPARE_WITH_TIMEOUT(gotB.size(), 1, 10000);
        const QStringList paths = gotB.at(0).at(2).toStringList();
        QCOMPARE(paths.size(), 2);
        QCOMPARE(QFileInfo(paths.at(0)).fileName(), QString("hello.txt"));
        QCOMPARE(QFileInfo(paths.at(1)).fileName(), QString("hello (2).txt"));
        QCOMPARE(QFileInfo(paths.at(1)).size(), qint64(700 * 1024));
        QTRY_COMPARE_WITH_TIMEOUT(sentA.size(), 2, 5000);
        QCOMPARE(sentA.last().at(1).toBool(), true);

        // Someone else answering at B's address with another certificate: nothing is sent
        a.sendFiles("127.0.0.1", portB, QString("00").repeated(32), {f.fileName()});
        QTRY_COMPARE_WITH_TIMEOUT(sentA.size(), 3, 5000);
        QCOMPARE(sentA.last().at(1).toBool(), false);
        QTest::qWait(300);
        QCOMPARE(gotB.size(), 1);
    }
};

QTEST_MAIN(TestContinuity)
#include "tst_continuity.moc"
