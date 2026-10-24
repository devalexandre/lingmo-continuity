/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "firewall.h"
#include "store.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>

static bool unitActive(const QString &unit)
{
    QProcess p;
    p.start(QStringLiteral("systemctl"), {QStringLiteral("is-active"), QStringLiteral("--quiet"), unit});
    return p.waitForFinished(2000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

static QString activeFirewall()
{
    // Tests: LINGMO_CONTINUITY_FIREWALL=ufw|firewalld|none
    const QString forced = qEnvironmentVariable("LINGMO_CONTINUITY_FIREWALL");
    if (!forced.isEmpty())
        return forced == QLatin1String("none") ? QString() : forced;

    static QElapsedTimer age;
    static QString cached;
    if (age.isValid() && age.elapsed() < 30000)
        return cached;
    age.start();
    cached.clear();
    if (QStandardPaths::findExecutable(QStringLiteral("systemctl")).isEmpty())
        return cached;
    if (unitActive(QStringLiteral("ufw.service")))
        cached = QStringLiteral("ufw");
    else if (unitActive(QStringLiteral("firewalld.service")))
        cached = QStringLiteral("firewalld");
    return cached;
}

QJsonObject Firewall::hint(bool principal)
{
    const QString name = activeFirewall();
    if (name.isEmpty())
        return {{QStringLiteral("name"), QString()}};

    // The principal also takes Deskflow connections; both take pairing,
    // files and discovery
    const int tcpFrom = principal ? Ports::deskflow() : Ports::channel();
    const QString tcp = tcpFrom == Ports::channel() ? QString::number(Ports::channel())
                                                    : QStringLiteral("%1-%2").arg(tcpFrom).arg(Ports::channel());
    const QString udp = QString::number(Ports::discovery());
    QString command;
    if (name == QLatin1String("ufw")) {
        command = QStringLiteral("sudo ufw allow %1/tcp && sudo ufw allow %2/udp")
                      .arg(QString(tcp).replace(QLatin1Char('-'), QLatin1Char(':')), udp);
    } else {
        command = QStringLiteral("sudo firewall-cmd --permanent --add-port=%1/tcp --add-port=%2/udp && "
                                 "sudo firewall-cmd --reload")
                      .arg(tcp, udp);
    }
    return {
        {QStringLiteral("name"), name},
        {QStringLiteral("ports"), QStringLiteral("%1/tcp, %2/udp").arg(tcp, udp)},
        {QStringLiteral("command"), command},
    };
}
