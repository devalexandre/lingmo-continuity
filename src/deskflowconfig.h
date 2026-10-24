/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "store.h"

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

// Turns the layout people arrange in Settings into Deskflow's files.
namespace DeskflowConfig {

// A computer in the shared layout: its whole desktop, in layout pixels
struct Computer {
    QString name;
    QRect rect;
};

// One Deskflow link: leaving `from` through `dir` ("left", "right", "up",
// "down") between `s0`..`s1` % of that edge enters `to` between `d0`..`d1` %
struct Link {
    QString from;
    QString dir;
    int s0 = 0, s1 = 100;
    QString to;
    int d0 = 0, d1 = 100;
};

// Computers whose edges touch (and overlap) are linked both ways
QList<Link> links(const QList<Computer> &computers);

// "section: links … section: options …" for deskflow-core server
QString serverConfig(const QList<Computer> &computers, const Options &options);

// The computer across `dir` from `from` at `fraction` (0..1) of that edge,
// or "" when that part of the edge leads nowhere
QString neighbourAt(const QList<Computer> &computers, const QString &from, const QString &dir, double fraction);

// Settings for deskflow-core (Deskflow.conf, read with -s)
struct Settings {
    QString mode;          // "server" or "client"
    QString name;          // this computer's Deskflow name
    int port = 24800;
    QString remoteHost;    // client: the principal's address(es), comma separated
    QString certificate;   // key + certificate (PEM)
    QString serverConfig;  // server: path of the links file
    QStringList computers; // server: every computer in the layout
    Options options;
    QString logLevel = QStringLiteral("INFO");
};

bool writeSettings(const QString &path, const Settings &settings);

// Deskflow's trust files (tls/trusted-servers, tls/trusted-clients): one
// "v2:sha256:<hex>" line per fingerprint
bool writeTrustFile(const QString &path, const QStringList &fingerprints);
QStringList readTrustFile(const QString &path);

// A hot key as Settings shows it ("Ctrl+Alt+L") from Deskflow's spelling
QString hotkeyLabel(const QString &deskflowKey);
// Whether a hot key spelling is one we accept (modifiers + key name)
bool isValidHotkey(const QString &deskflowKey);

}
