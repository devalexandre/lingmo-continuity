/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "feedback.h"

#include <QCursor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDesktopServices>
#include <QFileInfo>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

static const QString kIsland = QStringLiteral("com.lingmo.Island");
static const QString kIslandPath = QStringLiteral("/Island");
static const QString kAppId = QStringLiteral("lingmo-continuity");

Feedback::Feedback(QObject *parent)
    : QObject(parent)
{
}

Feedback::~Feedback()
{
    if (m_islandId)
        islandFinish(QString());
}

bool Feedback::init()
{
    if (m_engine)
        return m_root != nullptr;
    m_engine = new QQmlApplicationEngine(this);
    m_engine->rootContext()->setContextProperty(QStringLiteral("feedback"), this);
    m_engine->load(QUrl(QStringLiteral("qrc:/qml/Overlay.qml")));
    m_root = m_engine->rootObjects().value(0);
    return m_root != nullptr;
}

void Feedback::toast(const QString &title, const QString &text, const QString &icon)
{
    if (!m_root)
        return;
    const QPoint cursor = QCursor::pos();
    QMetaObject::invokeMethod(m_root, "showToast", Q_ARG(QVariant, title), Q_ARG(QVariant, text),
                              Q_ARG(QVariant, icon), Q_ARG(QVariant, cursor.x()), Q_ARG(QVariant, cursor.y()));
}

void Feedback::filesReceived(const QString &from, const QStringList &paths)
{
    if (!m_root) {
        notify(tr("Received from %1").arg(from), tr("%n file(s) in Downloads", "", int(paths.size())));
        return;
    }
    QVariantList files;
    for (const QString &p : paths)
        files << QVariantMap{{QStringLiteral("path"), p}, {QStringLiteral("name"), QFileInfo(p).fileName()}};
    const QPoint cursor = QCursor::pos();
    QMetaObject::invokeMethod(m_root, "showFlyIn", Q_ARG(QVariant, from), Q_ARG(QVariant, files),
                              Q_ARG(QVariant, cursor.x()), Q_ARG(QVariant, cursor.y()));
}

void Feedback::showPairing(const QVariantMap &info)
{
    if (!m_root) {
        notify(tr("Pairing request"), info.value(QStringLiteral("label")).toString());
        return;
    }
    QMetaObject::invokeMethod(m_root, "showPairing", Q_ARG(QVariant, info));
}

void Feedback::pairingDone(bool ok, const QString &message)
{
    if (m_root)
        QMetaObject::invokeMethod(m_root, "pairingDone", Q_ARG(QVariant, ok), Q_ARG(QVariant, message));
}

void Feedback::answerPairing(bool accept)
{
    emit pairingAnswered(accept);
}

void Feedback::openFile(const QString &path)
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void Feedback::openFolder(const QString &path)
{
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

// ------------------------------------------------------------ status bar

bool Feedback::islandAvailable() const
{
    auto *iface = QDBusConnection::sessionBus().interface();
    return iface && iface->isServiceRegistered(kIsland);
}

void Feedback::setConnection(bool connected, const QString &subtitle)
{
    const bool changed = connected != m_connected;
    m_connected = connected;
    m_subtitle = subtitle;

    if (islandAvailable()) {
        if (connected) {
            if (m_islandId)
                islandUpdate();
            else
                islandStart();
        } else if (m_islandId) {
            islandFinish(tr("Disconnected"));
        }
        return;
    }
    // No Island: a notification when it connects or drops, not on every change
    if (changed)
        notify(connected ? tr("Keyboard and mouse shared") : tr("Keyboard and mouse no longer shared"), subtitle);
}

void Feedback::islandStart()
{
    if (m_islandStarting)
        return;
    m_islandStarting = true;
    QDBusMessage msg = QDBusMessage::createMethodCall(kIsland, kIslandPath, kIsland, QStringLiteral("StartWithHints"));
    msg << kAppId << QStringLiteral("generic") << tr("Shared keyboard and mouse") << m_subtitle
        << QStringLiteral("input-mouse") << double(-1) << QStringList() << QVariantMap();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        m_islandStarting = false;
        QDBusPendingReply<uint> reply = *w;
        if (reply.isError()) {
            // An older Island: fall back to a notification
            notify(tr("Keyboard and mouse shared"), m_subtitle);
            return;
        }
        m_islandId = reply.value();
        if (!m_connected)
            islandFinish(tr("Disconnected"));
        else
            islandUpdate();
    });
}

void Feedback::islandUpdate()
{
    if (!m_islandId)
        return;
    QVariantMap fields{
        {QStringLiteral("title"), tr("Shared keyboard and mouse")},
        {QStringLiteral("subtitle"), m_subtitle},
        {QStringLiteral("iconName"), QStringLiteral("input-mouse")},
    };
    QDBusMessage msg = QDBusMessage::createMethodCall(kIsland, kIslandPath, kIsland, QStringLiteral("UpdateFields"));
    msg << m_islandId << fields;
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}

void Feedback::islandFinish(const QString &message)
{
    if (!m_islandId)
        return;
    QDBusMessage msg = QDBusMessage::createMethodCall(kIsland, kIslandPath, kIsland, QStringLiteral("Finish"));
    msg << m_islandId << message;
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
    m_islandId = 0;
}

void Feedback::notify(const QString &summary, const QString &body)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                      QStringLiteral("/org/freedesktop/Notifications"),
                                                      QStringLiteral("org.freedesktop.Notifications"),
                                                      QStringLiteral("Notify"));
    msg << tr("Shared keyboard and mouse") << uint(0) << QStringLiteral("input-mouse") << summary << body
        << QStringList() << QVariantMap{{QStringLiteral("desktop-entry"), QStringLiteral("lingmo-settings")}}
        << int(5000);
    QDBusConnection::sessionBus().call(msg, QDBus::NoBlock);
}
