/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <functional>

class Identity;
class QSslServer;
class QSslSocket;
class Link;

// The Lingmo-to-Lingmo channel (TLS over TCP, port base+2), used for:
//  - pairing: both PCs see each other's certificate and show the same code;
//    each user confirms on their own PC;
//  - the layout, which the principal sends to its secundários;
//  - files dragged across the edge, when lingmo-share isn't there.
// Both sides present their certificate (the same one Deskflow uses). Apart
// from a pairing request, only paired certificates are listened to.
class Channel : public QObject
{
    Q_OBJECT
public:
    explicit Channel(Identity *identity, QObject *parent = nullptr);
    ~Channel() override;

    bool listen(int port = -1);
    void close();
    bool isListening() const { return m_server != nullptr; }

    // What we tell the other side about us (name, label, role, screens, port)
    void setHello(const QJsonObject &hello) { m_hello = hello; }
    // Is this certificate paired?
    void setTrust(std::function<bool(const QString &)> trusted) { m_trusted = std::move(trusted); }
    // Where received files go (default: Downloads)
    void setDownloadDir(const QString &dir) { m_downloadDir = dir; }

    // Pairing (one at a time)
    void pair(const QString &host, int port);
    void answerPairing(bool accept);
    void cancelPairing();
    bool isPairing() const { return !m_pairing.isNull(); }

    // To a paired PC; `id` is its certificate fingerprint, checked before
    // anything is sent
    void sendMessage(const QString &host, int port, const QString &id, const QJsonObject &message);
    void sendFiles(const QString &host, int port, const QString &id, const QStringList &paths);

    // Frame helpers, public for the tests
    static QByteArray frame(char type, const QByteArray &payload);
    static QString safeFileName(const QString &name);
    static QString uniquePath(const QString &dir, const QString &name);

signals:
    // `incoming`: they asked; `code`: what both screens show
    void pairingStarted(const QJsonObject &peer, const QString &peerId, const QString &code, bool incoming,
                        const QString &host);
    // reason: "", "rejected-here", "rejected-there", "busy", "lost", "timeout",
    // "unreachable", "off"
    void pairingFinished(bool ok, const QJsonObject &peer, const QString &peerId, const QString &host,
                         const QString &reason);
    // A paired PC said something (layout, unpair, hello)
    void message(const QString &peerId, const QJsonObject &message, const QString &host);
    void filesReceived(const QString &peerId, const QJsonObject &peer, const QStringList &paths);
    void sendFinished(const QString &peerId, bool ok, int count);

private:
    friend class Link;
    Link *newLink(QSslSocket *socket, bool outgoing);
    void onLinkHello(Link *link);
    void onLinkMessage(Link *link, const QJsonObject &message);
    void onLinkClosed(Link *link);
    void checkPairingDone();
    void finishPairing(bool ok, const QString &reason);

    Identity *m_identity;
    QSslServer *m_server = nullptr;
    QJsonObject m_hello;
    std::function<bool(const QString &)> m_trusted;
    QString m_downloadDir;

    QPointer<Link> m_pairing;
    bool m_localAccepted = false;
    bool m_remoteAccepted = false;
    QTimer m_pairingTimeout;
};

// One TLS connection and its framing: [type: 1 byte][length: 4 bytes BE][payload]
// type 'J': a JSON object; 'D': file data
class Link : public QObject
{
    Q_OBJECT
    friend class Channel;
public:
    enum class Purpose { Incoming, Pair, Message, Files };

    Link(Channel *channel, QSslSocket *socket, bool outgoing);
    ~Link() override;

    QSslSocket *socket() const { return m_socket; }
    QString host() const;
    void send(const QJsonObject &message);
    void sendData(const QByteArray &data);
    void close();

    Purpose purpose = Purpose::Incoming;
    QString expectedId;
    QString peerId;
    QJsonObject peerHello;
    bool helloSeen = false;
    QJsonObject pendingMessage;
    QStringList pendingFiles;
    bool closing = false;

signals:
    void closed();

private:
    void emitClosed();
    void onEncrypted();
    void onReadyRead();
    void parseBuffer();
    void handleFrame(char type, const QByteArray &payload);
    void pumpFiles();
    void startReceiving(const QJsonObject &message);
    void receiveFileHeader(const QJsonObject &message);
    void receiveData(const QByteArray &data);
    void finishReceiving();
    void abortReceiving();

    Channel *m_channel;
    QSslSocket *m_socket;
    QByteArray m_buffer;
    QTimer m_idle;

    // Sending files
    int m_sendIndex = -1;
    QFile m_sendFile;
    int m_sentCount = 0;
    qint64 m_sendLeft = 0;
    bool m_closedEmitted = false;

    // Receiving files
    bool m_receiving = false;
    int m_expectFiles = 0;
    QFile m_recvFile;
    qint64 m_recvLeft = 0;
    QString m_recvFinalName;
    QStringList m_received;
};
