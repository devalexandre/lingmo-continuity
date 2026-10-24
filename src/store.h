/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QJsonObject>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

// Where lingmo-continuity keeps its files
namespace Paths {
// ~/.config/lingmo-continuity
QString configDir();
// ~/.config/lingmo-continuity/deskflow: Deskflow.conf, server.conf, tls/
QString deskflowDir();
// $XDG_RUNTIME_DIR/lingmo-continuity (0700): Deskflow's IPC socket
QString runtimeDir();
}

// Ports, all from one base so tests can run two daemons on one machine
// (LINGMO_CONTINUITY_PORT, default 24800):
//   base     TCP  Deskflow (only on the principal PC)
//   base + 1 UDP  discovery of other Lingmo PCs
//   base + 2 TCP  pairing, layout and files (TLS, paired PCs only)
struct Ports {
    static int base();
    static int deskflow() { return base(); }
    static int discovery() { return base() + 1; }
    static int channel() { return base() + 2; }
};

// Another computer: a Lingmo PC found on the network or paired, or a PC with
// Deskflow/Synergy/Input Leap (Windows, macOS, other Linux) added by hand
struct Peer {
    // SHA-256 of its TLS certificate (hex, upper case). Empty for a PC added
    // by hand until its fingerprint is confirmed
    QString id;
    // Name in Deskflow (the "screen name"): a host name
    QString name;
    // Name people see ("Notebook da Ana")
    QString label;
    // Last known address
    QString host;
    // Its pairing/files port and its Deskflow port (Lingmo PCs)
    int port = 0;
    int deskflowPort = 0;
    // "lingmo" or "other"
    QString os = QStringLiteral("lingmo");
    bool paired = false;
    // Its role as it last told us: "server" (principal) or "client"
    QString role;
    // Its monitors, in its own desktop coordinates
    QList<QRect> screens;
    // Top-left in the shared layout, and whether the user placed it
    QPoint pos;
    bool placed = false;

    // Not saved
    qint64 lastSeen = 0;
    bool connected = false;

    // Stable key for the UI and D-Bus: the fingerprint, or "host:<address>"
    QString key() const;
    // Size of its whole desktop (1920x1080 when unknown)
    QSize desktopSize() const;

    QJsonObject toJson() const;
    static Peer fromJson(const QJsonObject &o);
};

struct Options {
    bool clipboard = true;
    // "immediate", "delay" or "doubletap"
    QString switchMode = QStringLiteral("immediate");
    int switchDelay = 250;
    int doubleTap = 250;
    // Deskflow key names ("ScrollLock", "control+alt+L"); empty: off
    QString lockHotkey = QStringLiteral("ScrollLock");
    QString switchHotkey = QStringLiteral("control+super+Right");
};

// The saved state: ~/.config/lingmo-continuity/continuity.json
class Store
{
public:
    bool enabled = false;
    // "server" (principal: shares its keyboard and mouse) or "client"
    QString role = QStringLiteral("server");
    QString label;
    Options options;
    // On a secundário: the principal it uses (peer key)
    QString serverKey;
    QPoint selfPos;
    QList<Peer> peers;

    static QString path();
    void load();
    bool save() const;

    Peer *find(const QString &key);
    Peer *findById(const QString &id);
    Peer *findByName(const QString &name);

    // The Deskflow name of this PC: its host name, cleaned up
    static QString computerName();
    // A host name usable as a Deskflow screen name
    static QString cleanName(const QString &name);
    // "Notebook" when nothing better is known: the pretty host name
    static QString defaultLabel();
};
