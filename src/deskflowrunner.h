/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QElapsedTimer>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

// Runs `deskflow-core server|client -s Deskflow.conf` (Deskflow 1.2x; no GUI),
// restarts it if it dies, and reports what it does: connection state, peer
// fingerprints and connected clients come from its IPC socket (in our private
// runtime directory, through TMPDIR), switches from its log.
class DeskflowRunner : public QObject
{
    Q_OBJECT
public:
    explicit DeskflowRunner(QObject *parent = nullptr);
    ~DeskflowRunner() override;

    // deskflow-core, or $LINGMO_CONTINUITY_DESKFLOW
    static QString binary();
    // "1.27.0" ("" when Deskflow isn't installed)
    static QString version();

    void start(const QString &mode, const QString &settingsPath);
    void stop();
    void restart();
    bool isRunning() const { return m_wanted; }
    QString mode() const { return m_mode; }

    // Deskflow's ConnectionState: "Disconnected", "Connecting", "Connected",
    // "Listening" ("" while starting)
    QString state() const { return m_state; }
    QStringList connectedClients() const { return m_clients; }
    QString lastError() const { return m_error; }

signals:
    void stateChanged();
    // Server: the cursor moved from one computer to another
    void switched(const QString &from, const QString &to);
    // This computer got / lost the keyboard and mouse
    void entered();
    void left();
    // The other side's certificate, each time a TLS connection is checked
    void peerFingerprint(const QString &fingerprint);
    // Server: a client with a trusted certificate but a name not in the layout
    void unrecognisedClient(const QString &name);

private:
    void launch();
    void onOutput();
    void parseLine(const QString &line);
    void connectIpc();
    void onIpcData();
    void setState(const QString &state);

    QProcess *m_process = nullptr;
    QLocalSocket *m_ipc = nullptr;
    QTimer m_retry;
    QTimer m_ipcRetry;
    QElapsedTimer m_started;
    QByteArray m_outBuffer;
    QByteArray m_ipcBuffer;
    QString m_mode;
    QString m_settings;
    QString m_state;
    QString m_error;
    QStringList m_clients;
    bool m_wanted = false;
    int m_failures = 0;
    int m_ipcAttempts = 0;
};
