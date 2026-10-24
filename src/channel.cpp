/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "channel.h"
#include "identity.h"
#include "store.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSslConfiguration>
#include <QSslServer>
#include <QSslSocket>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QtEndian>

static constexpr int kMaxJson = 64 * 1024;
static constexpr int kMaxData = 1024 * 1024;
static constexpr int kChunk = 256 * 1024;
static constexpr qint64 kQueue = 2 * 1024 * 1024;
static constexpr int kMaxFiles = 1000;
static constexpr int kIdleMs = 60000;
static constexpr int kPairingMs = 120000;

static QSslConfiguration sslConfiguration(Identity *identity)
{
    QSslConfiguration c = QSslConfiguration::defaultConfiguration();
    c.setLocalCertificate(identity->certificate());
    c.setPrivateKey(identity->key());
    c.setProtocol(QSsl::TlsV1_2OrLater);
    // Self-signed on both sides: ask for the certificate and check its
    // fingerprint ourselves (paired list, or the pairing code)
    c.setPeerVerifyMode(QSslSocket::QueryPeer);
    return c;
}

// ---------------------------------------------------------------- Channel

Channel::Channel(Identity *identity, QObject *parent)
    : QObject(parent)
    , m_identity(identity)
{
    m_pairingTimeout.setSingleShot(true);
    connect(&m_pairingTimeout, &QTimer::timeout, this, [this] {
        if (m_pairing)
            m_pairing->send({{QStringLiteral("t"), QStringLiteral("pair-answer")}, {QStringLiteral("accept"), false}});
        finishPairing(false, QStringLiteral("timeout"));
    });
    m_downloadDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

Channel::~Channel()
{
    close();
}

bool Channel::listen(int port)
{
    if (m_server)
        return true;
    m_server = new QSslServer(this);
    m_server->setSslConfiguration(sslConfiguration(m_identity));
    m_server->setHandshakeTimeout(5000);
    m_server->setMaxPendingConnections(8);
    connect(m_server, &QSslServer::sslErrors, this, [](QSslSocket *socket, const QList<QSslError> &) {
        // Self-signed peers are expected: trust is the fingerprint, checked later
        socket->ignoreSslErrors();
    });
    connect(m_server, &QSslServer::pendingConnectionAvailable, this, [this] {
        while (m_server && m_server->hasPendingConnections()) {
            auto *socket = qobject_cast<QSslSocket *>(m_server->nextPendingConnection());
            if (socket)
                newLink(socket, false);
        }
    });
    if (port < 0)
        port = Ports::channel();
    if (!m_server->listen(QHostAddress::Any, quint16(port))) {
        qWarning() << "lingmo-continuity: can't listen on" << port << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    return true;
}

void Channel::close()
{
    cancelPairing();
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    const auto links = findChildren<Link *>();
    for (Link *l : links)
        l->close();
}

Link *Channel::newLink(QSslSocket *socket, bool outgoing)
{
    auto *link = new Link(this, socket, outgoing);
    connect(link, &Link::closed, this, [this, link] { onLinkClosed(link); });
    return link;
}

void Channel::pair(const QString &host, int port)
{
    if (m_pairing) {
        emit pairingFinished(false, {}, {}, host, QStringLiteral("busy"));
        return;
    }
    auto *socket = new QSslSocket;
    Link *link = newLink(socket, true);
    link->purpose = Link::Purpose::Pair;
    m_pairing = link;
    m_localAccepted = m_remoteAccepted = false;
    m_pairingTimeout.start(kPairingMs);
    socket->connectToHostEncrypted(host, quint16(port));
}

void Channel::answerPairing(bool accept)
{
    if (!m_pairing)
        return;
    m_pairing->send({{QStringLiteral("t"), QStringLiteral("pair-answer")}, {QStringLiteral("accept"), accept}});
    if (!accept) {
        finishPairing(false, QStringLiteral("rejected-here"));
        return;
    }
    m_localAccepted = true;
    checkPairingDone();
}

void Channel::cancelPairing()
{
    if (!m_pairing)
        return;
    m_pairing->send({{QStringLiteral("t"), QStringLiteral("pair-answer")}, {QStringLiteral("accept"), false}});
    finishPairing(false, QStringLiteral("rejected-here"));
}

void Channel::checkPairingDone()
{
    if (m_pairing && m_localAccepted && m_remoteAccepted)
        finishPairing(true, QString());
}

void Channel::finishPairing(bool ok, const QString &reason)
{
    m_pairingTimeout.stop();
    QPointer<Link> link = m_pairing;
    m_pairing = nullptr;
    m_localAccepted = m_remoteAccepted = false;
    const QJsonObject hello = link ? link->peerHello : QJsonObject();
    const QString id = link ? link->peerId : QString();
    const QString host = link ? link->host() : QString();
    if (link)
        link->close();
    emit pairingFinished(ok, hello, id, host, reason);
}

void Channel::sendMessage(const QString &host, int port, const QString &id, const QJsonObject &message)
{
    auto *socket = new QSslSocket;
    Link *link = newLink(socket, true);
    link->purpose = Link::Purpose::Message;
    link->expectedId = id;
    link->pendingMessage = message;
    socket->connectToHostEncrypted(host, quint16(port));
}

void Channel::sendFiles(const QString &host, int port, const QString &id, const QStringList &paths)
{
    QStringList files;
    for (const QString &p : paths) {
        const QFileInfo fi(p);
        // Folders aren't sent (yet): only regular files
        if (fi.isFile() && fi.isReadable() && files.size() < kMaxFiles)
            files << fi.absoluteFilePath();
    }
    if (files.isEmpty()) {
        emit sendFinished(id, false, 0);
        return;
    }
    auto *socket = new QSslSocket;
    Link *link = newLink(socket, true);
    link->purpose = Link::Purpose::Files;
    link->expectedId = id;
    link->pendingFiles = files;
    socket->connectToHostEncrypted(host, quint16(port));
}

void Channel::onLinkHello(Link *link)
{
    switch (link->purpose) {
    case Link::Purpose::Pair:
        if (link == m_pairing)
            emit pairingStarted(link->peerHello, link->peerId,
                                Identity::pairingCode(m_identity->fingerprint(), link->peerId), false, link->host());
        break;
    case Link::Purpose::Message:
        link->send(link->pendingMessage);
        link->send({{QStringLiteral("t"), QStringLiteral("bye")}});
        link->close();
        break;
    case Link::Purpose::Files: {
        qint64 total = 0;
        for (const QString &f : std::as_const(link->pendingFiles))
            total += QFileInfo(f).size();
        link->send({{QStringLiteral("t"), QStringLiteral("files")},
                    {QStringLiteral("count"), int(link->pendingFiles.size())},
                    {QStringLiteral("total"), double(total)}});
        QMetaObject::invokeMethod(link, [link] { link->pumpFiles(); }, Qt::QueuedConnection);
        break;
    }
    case Link::Purpose::Incoming:
        break;
    }
}

void Channel::onLinkMessage(Link *link, const QJsonObject &message)
{
    const QString t = message.value(QStringLiteral("t")).toString();
    if (t == QLatin1String("pair")) {
        if (link->purpose != Link::Purpose::Incoming)
            return;
        if (m_pairing && m_pairing != link) {
            link->send({{QStringLiteral("t"), QStringLiteral("pair-answer")}, {QStringLiteral("accept"), false},
                        {QStringLiteral("reason"), QStringLiteral("busy")}});
            link->close();
            return;
        }
        m_pairing = link;
        m_localAccepted = m_remoteAccepted = false;
        m_pairingTimeout.start(kPairingMs);
        emit pairingStarted(link->peerHello, link->peerId,
                            Identity::pairingCode(m_identity->fingerprint(), link->peerId), true, link->host());
        return;
    }
    if (t == QLatin1String("pair-answer")) {
        if (link != m_pairing)
            return;
        if (message.value(QStringLiteral("accept")).toBool()) {
            m_remoteAccepted = true;
            checkPairingDone();
        } else {
            finishPairing(false, message.value(QStringLiteral("reason")).toString() == QLatin1String("busy")
                                     ? QStringLiteral("busy")
                                     : QStringLiteral("rejected-there"));
        }
        return;
    }
    if (t == QLatin1String("bye")) {
        link->close();
        return;
    }
    // Everything else only from paired PCs
    if (!m_trusted || !m_trusted(link->peerId)) {
        link->close();
        return;
    }
    emit this->message(link->peerId, message, link->host());
}

void Channel::onLinkClosed(Link *link)
{
    if (link == m_pairing.data()) {
        m_pairing = nullptr;
        m_pairingTimeout.stop();
        const bool reached = link->helloSeen;
        m_localAccepted = m_remoteAccepted = false;
        emit pairingFinished(false, link->peerHello, link->peerId, link->host(),
                             reached ? QStringLiteral("lost") : QStringLiteral("unreachable"));
    }
    if (link->purpose == Link::Purpose::Files && !link->pendingFiles.isEmpty())
        emit sendFinished(link->expectedId, false, 0);
    link->deleteLater();
}

QByteArray Channel::frame(char type, const QByteArray &payload)
{
    QByteArray out;
    out.reserve(payload.size() + 5);
    out.append(type);
    char len[4];
    qToBigEndian<quint32>(quint32(payload.size()), len);
    out.append(len, 4);
    out.append(payload);
    return out;
}

QString Channel::safeFileName(const QString &name)
{
    QString base = QFileInfo(QString(name).replace(QLatin1Char('\\'), QLatin1Char('/'))).fileName();
    base.remove(QChar(0));
    base = base.trimmed();
    // No hidden files, no "." or ".."
    while (base.startsWith(QLatin1Char('.')))
        base.remove(0, 1);
    if (base.isEmpty())
        base = QStringLiteral("file");
    return base.left(200);
}

QString Channel::uniquePath(const QString &dir, const QString &name)
{
    const QFileInfo fi(name);
    const QString stem = fi.completeBaseName().isEmpty() ? name : fi.completeBaseName();
    const QString suffix = fi.suffix().isEmpty() ? QString() : QLatin1Char('.') + fi.suffix();
    QString candidate = dir + QLatin1Char('/') + name;
    for (int n = 2; QFileInfo::exists(candidate) || QFileInfo::exists(candidate + QStringLiteral(".part")); ++n)
        candidate = QStringLiteral("%1/%2 (%3)%4").arg(dir, stem).arg(n).arg(suffix);
    return candidate;
}

// ---------------------------------------------------------------- Link

Link::Link(Channel *channel, QSslSocket *socket, bool outgoing)
    : QObject(channel)
    , m_channel(channel)
    , m_socket(socket)
{
    socket->setParent(this);
    m_idle.setSingleShot(true);
    m_idle.setInterval(kIdleMs);
    connect(&m_idle, &QTimer::timeout, this, [this] {
        // A pairing waits for people: its own timeout handles it
        if (this == m_channel->m_pairing.data())
            m_idle.start();
        else
            close();
    });
    m_idle.start();

    connect(socket, &QSslSocket::readyRead, this, &Link::onReadyRead);
    connect(socket, &QSslSocket::disconnected, this, [this] {
        closing = true;
        abortReceiving();
        emitClosed();
    });
    connect(socket, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (m_socket->state() != QAbstractSocket::ConnectedState
            && m_socket->state() != QAbstractSocket::ClosingState) {
            closing = true;
            abortReceiving();
            emitClosed();
        }
    });
    connect(socket, &QSslSocket::bytesWritten, this, [this] {
        m_idle.start();
        if (purpose == Purpose::Files && m_sendIndex >= 0)
            pumpFiles();
    });

    if (outgoing) {
        socket->setSslConfiguration(sslConfiguration(m_channel->m_identity));
        connect(socket, &QSslSocket::sslErrors, socket, [socket](const QList<QSslError> &) {
            socket->ignoreSslErrors();
        });
        connect(socket, &QSslSocket::encrypted, this, &Link::onEncrypted);
    } else {
        // The server hands it over once the handshake is done
        QMetaObject::invokeMethod(this, [this] {
            onEncrypted();
            if (!closing)
                onReadyRead();
        }, Qt::QueuedConnection);
    }
}

Link::~Link()
{
    abortReceiving();
}

QString Link::host() const
{
    QHostAddress addr = m_socket->peerAddress();
    bool ok = false;
    const quint32 v4 = addr.toIPv4Address(&ok);
    if (ok)
        addr = QHostAddress(v4);
    return addr.isNull() ? m_socket->peerName() : addr.toString();
}

void Link::onEncrypted()
{
    const QSslCertificate cert = m_socket->peerCertificate();
    if (cert.isNull()) {
        close();
        return;
    }
    peerId = Identity::fingerprintOf(cert);
    // A paired PC must be the one we know, not whoever answers at its address
    if (!expectedId.isEmpty() && peerId.compare(expectedId, Qt::CaseInsensitive) != 0) {
        qWarning() << "lingmo-continuity: certificate of" << host() << "is not the paired one";
        close();
        return;
    }
    QJsonObject hello = m_channel->m_hello;
    hello.insert(QStringLiteral("t"), QStringLiteral("hello"));
    send(hello);
    if (purpose == Purpose::Pair)
        send({{QStringLiteral("t"), QStringLiteral("pair")}});
}

void Link::send(const QJsonObject &message)
{
    if (closing || m_socket->state() != QAbstractSocket::ConnectedState)
        return;
    m_socket->write(Channel::frame('J', QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void Link::sendData(const QByteArray &data)
{
    if (closing)
        return;
    m_socket->write(Channel::frame('D', data));
}

void Link::close()
{
    if (closing)
        return;
    closing = true;
    abortReceiving();
    m_sendFile.close();
    if (m_socket->state() == QAbstractSocket::ConnectedState) {
        // Let what's queued go out first; `disconnected` follows
        m_socket->disconnectFromHost();
        if (m_socket->state() == QAbstractSocket::UnconnectedState)
            emitClosed();
        else
            QTimer::singleShot(10000, this, [this] {
                m_socket->abort();
                emitClosed();
            });
    } else {
        m_socket->abort();
        emitClosed();
    }
}

void Link::emitClosed()
{
    if (m_closedEmitted)
        return;
    m_closedEmitted = true;
    emit closed();
}

void Link::onReadyRead()
{
    m_idle.start();
    m_buffer += m_socket->readAll();
    parseBuffer();
}

void Link::parseBuffer()
{
    // Nothing is read before we know who's on the other side
    if (peerId.isEmpty())
        return;
    while (!closing && m_buffer.size() >= 5) {
        const char type = m_buffer.at(0);
        const quint32 len = qFromBigEndian<quint32>(m_buffer.constData() + 1);
        if ((type == 'J' && len > quint32(kMaxJson)) || (type == 'D' && len > quint32(kMaxData))
            || (type != 'J' && type != 'D')) {
            close();
            return;
        }
        if (m_buffer.size() < int(5 + len))
            return;
        const QByteArray payload = m_buffer.mid(5, int(len));
        m_buffer.remove(0, int(5 + len));
        handleFrame(type, payload);
    }
}

void Link::handleFrame(char type, const QByteArray &payload)
{
    if (type == 'D') {
        receiveData(payload);
        return;
    }
    QJsonParseError err;
    const QJsonObject message = QJsonDocument::fromJson(payload, &err).object();
    if (err.error != QJsonParseError::NoError) {
        close();
        return;
    }
    const QString t = message.value(QStringLiteral("t")).toString();
    if (!helloSeen) {
        if (t != QLatin1String("hello")) {
            close();
            return;
        }
        helloSeen = true;
        peerHello = message;
        m_channel->onLinkHello(this);
        return;
    }
    const bool trusted = m_channel->m_trusted && m_channel->m_trusted(peerId);
    if (t == QLatin1String("files") || t == QLatin1String("file") || t == QLatin1String("files-done")) {
        if (!trusted || purpose != Purpose::Incoming) {
            close();
            return;
        }
        if (t == QLatin1String("files"))
            startReceiving(message);
        else if (t == QLatin1String("file"))
            receiveFileHeader(message);
        else
            finishReceiving();
        return;
    }
    if (t == QLatin1String("files-ok") && purpose == Purpose::Files) {
        const int count = m_sentCount;
        pendingFiles.clear();
        emit m_channel->sendFinished(peerId, true, count);
        close();
        return;
    }
    if (t == QLatin1String("files-refused") && purpose == Purpose::Files) {
        close();
        return;
    }
    m_channel->onLinkMessage(this, message);
}

void Link::pumpFiles()
{
    if (closing || purpose != Purpose::Files)
        return;
    if (m_sendIndex < 0)
        m_sendIndex = 0;
    while (m_socket->bytesToWrite() < kQueue) {
        if (!m_sendFile.isOpen()) {
            if (m_sendIndex >= pendingFiles.size()) {
                if (m_sendIndex == pendingFiles.size()) {
                    send({{QStringLiteral("t"), QStringLiteral("files-done")}});
                    ++m_sendIndex;
                }
                return;
            }
            m_sendFile.setFileName(pendingFiles.at(m_sendIndex));
            if (!m_sendFile.open(QIODevice::ReadOnly)) {
                ++m_sendIndex;
                continue;
            }
            send({{QStringLiteral("t"), QStringLiteral("file")},
                  {QStringLiteral("name"), QFileInfo(m_sendFile.fileName()).fileName()},
                  {QStringLiteral("size"), double(m_sendFile.size())}});
            m_sendLeft = m_sendFile.size();
        }
        if (m_sendLeft <= 0) {
            m_sendFile.close();
            ++m_sentCount;
            ++m_sendIndex;
            continue;
        }
        const QByteArray chunk = m_sendFile.read(qMin<qint64>(kChunk, m_sendLeft));
        if (chunk.isEmpty()) {
            // The file shrank under us: the other side can't finish it
            close();
            return;
        }
        m_sendLeft -= chunk.size();
        sendData(chunk);
    }
}

void Link::startReceiving(const QJsonObject &message)
{
    const int count = message.value(QStringLiteral("count")).toInt();
    const qint64 total = qint64(message.value(QStringLiteral("total")).toDouble());
    const QString dir = m_channel->m_downloadDir;
    QDir().mkpath(dir);
    const QStorageInfo storage(dir);
    if (count <= 0 || count > kMaxFiles || total < 0
        || (storage.isValid() && storage.bytesAvailable() < total + 64 * 1024 * 1024)) {
        send({{QStringLiteral("t"), QStringLiteral("files-refused")}});
        close();
        return;
    }
    m_receiving = true;
    m_expectFiles = count;
    m_received.clear();
}

void Link::receiveFileHeader(const QJsonObject &message)
{
    if (!m_receiving || m_recvFile.isOpen() || m_received.size() >= m_expectFiles) {
        close();
        return;
    }
    const QString dir = m_channel->m_downloadDir;
    m_recvFinalName = Channel::uniquePath(dir, Channel::safeFileName(message.value(QStringLiteral("name")).toString()));
    m_recvLeft = qint64(message.value(QStringLiteral("size")).toDouble());
    if (m_recvLeft < 0) {
        close();
        return;
    }
    m_recvFile.setFileName(m_recvFinalName + QStringLiteral(".part"));
    if (!m_recvFile.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        close();
        return;
    }
    if (m_recvLeft == 0)
        receiveData({});
}

void Link::receiveData(const QByteArray &data)
{
    if (!m_receiving || !m_recvFile.isOpen() || data.size() > m_recvLeft) {
        close();
        return;
    }
    if (!data.isEmpty() && m_recvFile.write(data) != data.size()) {
        close();
        return;
    }
    m_recvLeft -= data.size();
    if (m_recvLeft > 0)
        return;
    m_recvFile.close();
    QString finalName = m_recvFinalName;
    if (QFileInfo::exists(finalName))
        finalName = Channel::uniquePath(QFileInfo(finalName).absolutePath(), QFileInfo(finalName).fileName());
    QFile::rename(m_recvFile.fileName(), finalName);
    m_received << finalName;
}

void Link::finishReceiving()
{
    if (!m_receiving || m_recvFile.isOpen()) {
        close();
        return;
    }
    m_receiving = false;
    send({{QStringLiteral("t"), QStringLiteral("files-ok")}, {QStringLiteral("count"), int(m_received.size())}});
    emit m_channel->filesReceived(peerId, peerHello, m_received);
    m_received.clear();
}

void Link::abortReceiving()
{
    if (m_recvFile.isOpen()) {
        m_recvFile.close();
        m_recvFile.remove();
    }
    m_receiving = false;
}
