/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class QUdpSocket;

// Finds other Lingmo PCs on the local network: a small UDP announcement
// (name, role, monitors, certificate fingerprint) on port base+1, only while
// the feature is on. Nothing here is trusted: it only says where a PC can be
// reached; pairing and every connection check its TLS certificate.
class Discovery : public QObject
{
    Q_OBJECT
public:
    explicit Discovery(QObject *parent = nullptr);

    bool start();
    void stop();
    bool isRunning() const { return m_socket != nullptr; }

    // What we announce about ourselves
    void setAnnouncement(const QJsonObject &announcement) { m_announcement = announcement; }
    // Asks every Lingmo PC to answer now
    void query();
    // Asks one address (a PC typed in by hand, maybe on another subnet)
    void queryAt(const QHostAddress &address);
    void announce();

signals:
    // Another PC said hello (`info`: id, name, label, role, port, screens)
    void found(const QJsonObject &info, const QHostAddress &from);

private:
    void send(const QByteArray &datagram, const QHostAddress &to = {}, quint16 port = 0);
    void onReadyRead();
    QList<QPair<QHostAddress, quint16>> targets() const;

    QUdpSocket *m_socket = nullptr;
    QTimer m_announceTimer;
    QJsonObject m_announcement;
};
