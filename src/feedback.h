/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QPointer>
#include <QVariantMap>

class QQmlApplicationEngine;

// What people see from the daemon: the "Agora em: Notebook" toast, the
// "Recebido de …" fly-in at the cursor, the pairing dialog (glass windows
// from qml/), and the connection state in the status bar's Island
// (com.lingmo.Island) or, without it, a notification.
class Feedback : public QObject
{
    Q_OBJECT
public:
    explicit Feedback(QObject *parent = nullptr);
    ~Feedback() override;

    // Loads the QML (needs a display); false without one
    bool init();

    void toast(const QString &title, const QString &text, const QString &icon);
    void filesReceived(const QString &from, const QStringList &paths);
    // info: label, code, fingerprint, incoming, other (bool: not a Lingmo PC)
    void showPairing(const QVariantMap &info);
    void pairingDone(bool ok, const QString &message);

    // Connected to other PCs (subtitle names them) or not
    void setConnection(bool connected, const QString &subtitle);

    // Called from the QML dialog
    Q_INVOKABLE void answerPairing(bool accept);
    Q_INVOKABLE void openFile(const QString &path);
    Q_INVOKABLE void openFolder(const QString &path);

signals:
    void pairingAnswered(bool accept);

private:
    void islandStart();
    void islandUpdate();
    void islandFinish(const QString &message);
    bool islandAvailable() const;
    void notify(const QString &summary, const QString &body);

    QQmlApplicationEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    bool m_connected = false;
    QString m_subtitle;
    uint m_islandId = 0;
    bool m_islandStarting = false;
};
