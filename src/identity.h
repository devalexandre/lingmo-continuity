/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QSslCertificate>
#include <QSslKey>
#include <QString>

// This PC's TLS identity: one self-signed certificate, used by Deskflow
// (deskflow/tls/deskflow.pem, key and certificate in one file, as Deskflow
// wants it) and by the pairing/files channel. Peers know us by its SHA-256.
class Identity
{
public:
    // Default: ~/.config/lingmo-continuity/deskflow/tls/deskflow.pem
    explicit Identity(const QString &pemPath = QString()) : m_pemPath(pemPath) {}

    // Loads the certificate, making one first if there is none
    bool ensure();

    QString pemPath() const;
    const QSslCertificate &certificate() const { return m_cert; }
    const QSslKey &key() const { return m_key; }
    // Upper-case hex, as Deskflow's trust files compare it
    QString fingerprint() const { return m_fingerprint; }

    static QString fingerprintOf(const QSslCertificate &cert);
    // "AF:30:B4:…" for people
    static QString formatFingerprint(const QString &hex);
    // The 6-digit code both sides show while pairing: the same on both PCs
    // only when each one sees the other's real certificate (no one in between)
    static QString pairingCode(const QString &fingerprintA, const QString &fingerprintB);

private:
    bool generate(const QString &path);

    QString m_pemPath;
    QSslCertificate m_cert;
    QSslKey m_key;
    QString m_fingerprint;
};
