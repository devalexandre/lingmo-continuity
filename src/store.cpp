/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "store.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

#include <unistd.h>

QString Paths::configDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/lingmo-continuity");
}

QString Paths::deskflowDir()
{
    return configDir() + QStringLiteral("/deskflow");
}

QString Paths::runtimeDir()
{
    QString base = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (base.isEmpty())
        base = QDir::tempPath() + QStringLiteral("/lingmo-continuity-") + QString::number(getuid());
    const QString dir = base + QStringLiteral("/lingmo-continuity");
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return dir;
}

int Ports::base()
{
    bool ok = false;
    const int port = qEnvironmentVariableIntValue("LINGMO_CONTINUITY_PORT", &ok);
    return ok && port > 1024 && port < 65530 ? port : 24800;
}

QString Peer::key() const
{
    return id.isEmpty() ? QStringLiteral("host:") + host : id;
}

QSize Peer::desktopSize() const
{
    QRect all;
    for (const QRect &r : screens)
        all = all.united(r);
    return all.isValid() ? all.size() : QSize(1920, 1080);
}

QJsonObject Peer::toJson() const
{
    QJsonArray s;
    for (const QRect &r : screens)
        s.append(QJsonArray{r.x(), r.y(), r.width(), r.height()});
    return QJsonObject{
        {QStringLiteral("id"), id},
        {QStringLiteral("name"), name},
        {QStringLiteral("label"), label},
        {QStringLiteral("host"), host},
        {QStringLiteral("port"), port},
        {QStringLiteral("df"), deskflowPort},
        {QStringLiteral("os"), os},
        {QStringLiteral("paired"), paired},
        {QStringLiteral("role"), role},
        {QStringLiteral("screens"), s},
        {QStringLiteral("x"), pos.x()},
        {QStringLiteral("y"), pos.y()},
        {QStringLiteral("placed"), placed},
    };
}

static QList<QRect> screensFromJson(const QJsonArray &a)
{
    QList<QRect> out;
    for (const QJsonValue &v : a) {
        const QJsonArray r = v.toArray();
        if (r.size() == 4 && r.at(2).toInt() > 0 && r.at(3).toInt() > 0 && out.size() < 16)
            out.append(QRect(r.at(0).toInt(), r.at(1).toInt(), qMin(r.at(2).toInt(), 32768),
                             qMin(r.at(3).toInt(), 32768)));
    }
    return out;
}

Peer Peer::fromJson(const QJsonObject &o)
{
    Peer p;
    p.id = o.value(QStringLiteral("id")).toString().toUpper();
    p.name = Store::cleanName(o.value(QStringLiteral("name")).toString());
    p.label = o.value(QStringLiteral("label")).toString().left(64);
    p.host = o.value(QStringLiteral("host")).toString().left(255);
    p.port = o.value(QStringLiteral("port")).toInt();
    p.deskflowPort = o.value(QStringLiteral("df")).toInt();
    p.os = o.value(QStringLiteral("os")).toString(QStringLiteral("lingmo"));
    p.paired = o.value(QStringLiteral("paired")).toBool();
    p.role = o.value(QStringLiteral("role")).toString();
    p.screens = screensFromJson(o.value(QStringLiteral("screens")).toArray());
    p.pos = QPoint(o.value(QStringLiteral("x")).toInt(), o.value(QStringLiteral("y")).toInt());
    p.placed = o.value(QStringLiteral("placed")).toBool();
    if (p.label.isEmpty())
        p.label = p.name.isEmpty() ? p.host : p.name;
    return p;
}

QString Store::path()
{
    return Paths::configDir() + QStringLiteral("/continuity.json");
}

void Store::load()
{
    QFile f(path());
    if (!f.open(QIODevice::ReadOnly)) {
        label = defaultLabel();
        return;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    enabled = o.value(QStringLiteral("enabled")).toBool();
    role = o.value(QStringLiteral("role")).toString() == QLatin1String("client") ? QStringLiteral("client")
                                                                               : QStringLiteral("server");
    label = o.value(QStringLiteral("label")).toString();
    if (label.isEmpty())
        label = defaultLabel();
    serverKey = o.value(QStringLiteral("server")).toString();
    selfPos = QPoint(o.value(QStringLiteral("selfX")).toInt(), o.value(QStringLiteral("selfY")).toInt());

    const QJsonObject opt = o.value(QStringLiteral("options")).toObject();
    Options d;
    options.clipboard = opt.value(QStringLiteral("clipboard")).toBool(d.clipboard);
    options.switchMode = opt.value(QStringLiteral("switchMode")).toString(d.switchMode);
    options.switchDelay = qBound(50, opt.value(QStringLiteral("switchDelay")).toInt(d.switchDelay), 3000);
    options.doubleTap = qBound(50, opt.value(QStringLiteral("doubleTap")).toInt(d.doubleTap), 3000);
    options.lockHotkey = opt.value(QStringLiteral("lockHotkey")).toString(d.lockHotkey);
    options.switchHotkey = opt.value(QStringLiteral("switchHotkey")).toString(d.switchHotkey);

    peers.clear();
    for (const QJsonValue &v : o.value(QStringLiteral("peers")).toArray())
        peers.append(Peer::fromJson(v.toObject()));
}

bool Store::save() const
{
    QDir().mkpath(Paths::configDir());
    QJsonArray p;
    for (const Peer &peer : peers)
        p.append(peer.toJson());
    const QJsonObject o{
        {QStringLiteral("enabled"), enabled},
        {QStringLiteral("role"), role},
        {QStringLiteral("label"), label},
        {QStringLiteral("server"), serverKey},
        {QStringLiteral("selfX"), selfPos.x()},
        {QStringLiteral("selfY"), selfPos.y()},
        {QStringLiteral("options"),
         QJsonObject{
             {QStringLiteral("clipboard"), options.clipboard},
             {QStringLiteral("switchMode"), options.switchMode},
             {QStringLiteral("switchDelay"), options.switchDelay},
             {QStringLiteral("doubleTap"), options.doubleTap},
             {QStringLiteral("lockHotkey"), options.lockHotkey},
             {QStringLiteral("switchHotkey"), options.switchHotkey},
         }},
        {QStringLiteral("peers"), p},
    };
    QSaveFile f(path());
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    f.write(QJsonDocument(o).toJson());
    return f.commit();
}

Peer *Store::find(const QString &key)
{
    for (Peer &p : peers) {
        if (p.key() == key)
            return &p;
    }
    return nullptr;
}

Peer *Store::findById(const QString &id)
{
    if (id.isEmpty())
        return nullptr;
    for (Peer &p : peers) {
        if (p.id.compare(id, Qt::CaseInsensitive) == 0)
            return &p;
    }
    return nullptr;
}

Peer *Store::findByName(const QString &name)
{
    for (Peer &p : peers) {
        if (!p.name.isEmpty() && p.name.compare(name, Qt::CaseInsensitive) == 0)
            return &p;
    }
    return nullptr;
}

QString Store::cleanName(const QString &name)
{
    // Deskflow accepts host-name-like screen names
    QString out = name.trimmed();
    out.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")), QStringLiteral("-"));
    out.remove(QRegularExpression(QStringLiteral("^[-.]+|[-.]+$")));
    return out.left(63);
}

QString Store::computerName()
{
    const QString env = qEnvironmentVariable("LINGMO_CONTINUITY_NAME");
    const QString name = cleanName(env.isEmpty() ? QSysInfo::machineHostName() : env);
    return name.isEmpty() ? QStringLiteral("lingmo") : name;
}

QString Store::defaultLabel()
{
    const QString env = qEnvironmentVariable("LINGMO_CONTINUITY_LABEL");
    if (!env.isEmpty())
        return env;
    // systemd's pretty host name, when set
    QSettings machineInfo(QStringLiteral("/etc/machine-info"), QSettings::IniFormat);
    const QString pretty = machineInfo.value(QStringLiteral("PRETTY_HOSTNAME")).toString();
    return pretty.isEmpty() ? QSysInfo::machineHostName() : pretty;
}
