/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "channel.h"
#include "deskflowconfig.h"
#include "deskflowrunner.h"
#include "discovery.h"
#include "dragwatcher.h"
#include "feedback.h"
#include "identity.h"
#include "store.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

// The daemon: keeps the saved state, runs Deskflow as principal (server) or
// secundário (client) from the layout, finds and pairs Lingmo PCs, and answers
// Settings on D-Bus (com.lingmo.Continuity, /Continuity; see
// data/com.lingmo.Continuity.xml). With the feature off nothing listens on
// the network.
class Service : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.lingmo.Continuity")
public:
    explicit Service(QObject *parent = nullptr);
    ~Service() override;

    bool registerOnBus();
    bool isEnabled() const { return m_store.enabled; }
    // Loads the state and starts what's turned on
    void start(bool withUi);

public Q_SLOTS:
    // Everything Settings shows, as JSON (see stateJson())
    Q_SCRIPTABLE QString State();
    Q_SCRIPTABLE void SetEnabled(bool enabled);
    // "server" (principal) or "client" (secundário)
    Q_SCRIPTABLE void SetRole(const QString &role);
    Q_SCRIPTABLE void SetLabel(const QString &label);
    // clipboard (true/false), switchMode (immediate/delay/doubletap),
    // switchDelay, doubleTap (ms), lockHotkey, switchHotkey (Deskflow key or "")
    Q_SCRIPTABLE void SetOption(const QString &key, const QString &value);
    // A PC by name or address: Lingmo PCs that don't show up, or PCs with
    // Deskflow/Synergy/Input Leap. Returns its key
    Q_SCRIPTABLE QString AddPeer(const QString &address);
    Q_SCRIPTABLE void Pair(const QString &key);
    Q_SCRIPTABLE void CancelPairing();
    Q_SCRIPTABLE void Unpair(const QString &key);
    // Forget a PC added by hand that isn't paired
    Q_SCRIPTABLE void RemovePeer(const QString &key);
    // Moves a PC in the layout ("self" for this one)
    Q_SCRIPTABLE void SetPosition(const QString &key, int x, int y);
    // Secundário: the principal to use
    Q_SCRIPTABLE void SetServer(const QString &key);
    Q_SCRIPTABLE void Discover();
    // Shows a toast, the pairing dialog or the received-files fly-in, to try
    // them: "toast", "pairing", "pairing-other", "received"
    Q_SCRIPTABLE void Preview(const QString &what);

Q_SIGNALS:
    Q_SCRIPTABLE void StateChanged(const QString &state);

private:
    void apply();
    void stopAll();
    void scheduleState();
    QJsonObject stateJson() const;
    QJsonObject helloJson() const;
    QList<QRect> ownScreens() const;
    QSize ownSize() const;
    void placeNewPeers();
    QList<DeskflowConfig::Computer> layout() const;
    QList<DeskflowConfig::Computer> effectiveLayout() const;
    QString labelForName(const QString &name) const;
    Peer *serverPeer();
    QStringList trustedIds() const;
    bool isTrusted(const QString &id) const;
    QString status() const;
    void updateConnectionFeedback();
    void pushLayout();
    void sendFilesTo(const Peer &peer, const QStringList &files);
    void save();

    void onFound(const QJsonObject &info, const QHostAddress &from);
    void onPairingStarted(const QJsonObject &hello, const QString &id, const QString &code, bool incoming,
                          const QString &host);
    void onPairingFinished(bool ok, const QJsonObject &hello, const QString &id, const QString &host,
                           const QString &reason);
    void onPairingAnswered(bool accept);
    void onChannelMessage(const QString &id, const QJsonObject &message, const QString &host);
    void onPeerFingerprint(const QString &fingerprint);
    void onUnrecognisedClient(const QString &name);
    void onDragAtEdge(const QString &dir, double fraction, const QStringList &files);

    Store m_store;
    Identity m_identity;
    DeskflowRunner m_runner;
    Discovery m_discovery;
    Channel m_channel;
    DragWatcher m_drag;
    Feedback m_feedback;
    bool m_withUi = false;
    bool m_identityOk = false;

    // Lingmo PCs seen on the network but not paired, by fingerprint
    QHash<QString, Peer> m_discovered;
    // Pairing in progress (Lingmo) or a certificate Deskflow refused (others)
    Peer m_pairingPeer;
    QString m_pairingState; // "", "waiting", "confirm"
    QString m_pairingCode;
    QString m_foreignFingerprint;
    QHash<QString, qint64> m_declined;
    // Secundário: the layout its principal sent
    QList<DeskflowConfig::Computer> m_remoteLayout;
    QHash<QString, QString> m_remoteLabels;
    // Principal: the layout last sent to each secundário (by fingerprint)
    QHash<QString, QByteArray> m_layoutSent;
    QString m_active;
    bool m_hasLeft = false;
    QByteArray m_configHash;
    QString m_lastError;

    QTimer m_stateTimer;
    QTimer m_expireTimer;
    QTimer m_networkDebounce;
};
