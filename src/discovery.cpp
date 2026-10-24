/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "discovery.h"
#include "store.h"

#include <QJsonDocument>
#include <QNetworkInterface>
#include <QUdpSocket>

static constexpr int kProtocol = 1;
static constexpr int kMaxDatagram = 4096;

Discovery::Discovery(QObject *parent)
    : QObject(parent)
{
    // Once a minute is enough: PCs also answer queries right away
    m_announceTimer.setInterval(60000);
    connect(&m_announceTimer, &QTimer::timeout, this, &Discovery::announce);
}

bool Discovery::start()
{
    if (m_socket)
        return true;
    m_socket = new QUdpSocket(this);
    if (!m_socket->bind(QHostAddress::AnyIPv4, quint16(Ports::discovery()),
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        qWarning() << "lingmo-continuity: discovery can't listen on" << Ports::discovery() << m_socket->errorString();
        delete m_socket;
        m_socket = nullptr;
        return false;
    }
    connect(m_socket, &QUdpSocket::readyRead, this, &Discovery::onReadyRead);
    m_announceTimer.start();
    query();
    return true;
}

void Discovery::stop()
{
    m_announceTimer.stop();
    if (m_socket) {
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

// Tests (and odd networks) list the addresses to talk to:
// LINGMO_CONTINUITY_DISCOVERY="127.0.0.1:24811,127.0.0.1:24821"
QList<QPair<QHostAddress, quint16>> Discovery::targets() const
{
    QList<QPair<QHostAddress, quint16>> out;
    const QString env = qEnvironmentVariable("LINGMO_CONTINUITY_DISCOVERY");
    if (!env.isEmpty()) {
        for (const QString &item : env.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const int colon = item.lastIndexOf(QLatin1Char(':'));
            const quint16 port = colon > 0 ? item.mid(colon + 1).toUShort() : quint16(Ports::discovery());
            const QHostAddress addr(colon > 0 ? item.left(colon) : item);
            if (!addr.isNull() && port)
                out.append({addr, port});
        }
        return out;
    }
    // Each LAN's broadcast address: Wi-Fi and cable at once
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack) || !(flags & QNetworkInterface::CanBroadcast))
            continue;
        for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull())
                out.append({entry.broadcast(), quint16(Ports::discovery())});
        }
    }
    if (out.isEmpty())
        out.append({QHostAddress::Broadcast, quint16(Ports::discovery())});
    return out;
}

void Discovery::send(const QByteArray &datagram, const QHostAddress &to, quint16 port)
{
    if (!m_socket)
        return;
    if (!to.isNull()) {
        m_socket->writeDatagram(datagram, to, port);
        return;
    }
    for (const auto &[addr, p] : targets())
        m_socket->writeDatagram(datagram, addr, p);
}

void Discovery::query()
{
    QJsonObject q = m_announcement;
    q.insert(QStringLiteral("lc"), kProtocol);
    q.insert(QStringLiteral("t"), QStringLiteral("query"));
    send(QJsonDocument(q).toJson(QJsonDocument::Compact));
}

void Discovery::queryAt(const QHostAddress &address)
{
    QJsonObject q = m_announcement;
    q.insert(QStringLiteral("lc"), kProtocol);
    q.insert(QStringLiteral("t"), QStringLiteral("query"));
    send(QJsonDocument(q).toJson(QJsonDocument::Compact), address, quint16(Ports::discovery()));
}

void Discovery::announce()
{
    QJsonObject a = m_announcement;
    a.insert(QStringLiteral("lc"), kProtocol);
    a.insert(QStringLiteral("t"), QStringLiteral("announce"));
    send(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

void Discovery::onReadyRead()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const qint64 size = m_socket->pendingDatagramSize();
        QByteArray data(int(qBound<qint64>(qint64(0), size, qint64(kMaxDatagram))), Qt::Uninitialized);
        QHostAddress from;
        quint16 fromPort = 0;
        m_socket->readDatagram(data.data(), data.size(), &from, &fromPort);
        if (size > kMaxDatagram)
            continue;
        const QJsonObject o = QJsonDocument::fromJson(data).object();
        if (o.value(QStringLiteral("lc")).toInt() != kProtocol)
            continue;
        const QString id = o.value(QStringLiteral("id")).toString();
        // Our own broadcast coming back
        if (id.isEmpty() || id == m_announcement.value(QStringLiteral("id")).toString())
            continue;
        if (from.protocol() == QAbstractSocket::IPv6Protocol) {
            bool ok = false;
            const QHostAddress v4(from.toIPv4Address(&ok));
            if (ok)
                from = v4;
        }
        if (o.value(QStringLiteral("t")).toString() == QLatin1String("query")) {
            // Answer straight to whoever asked
            QJsonObject a = m_announcement;
            a.insert(QStringLiteral("lc"), kProtocol);
            a.insert(QStringLiteral("t"), QStringLiteral("announce"));
            send(QJsonDocument(a).toJson(QJsonDocument::Compact), from, fromPort);
        }
        emit found(o, from);
    }
}
