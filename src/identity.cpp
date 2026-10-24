/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "identity.h"
#include "store.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

QString Identity::pemPath() const
{
    if (!m_pemPath.isEmpty())
        return m_pemPath;
    return Paths::deskflowDir() + QStringLiteral("/tls/deskflow.pem");
}

bool Identity::ensure()
{
    const QString path = pemPath();
    if (!QFile::exists(path) && !generate(path))
        return false;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray pem = f.readAll();
    const auto certs = QSslCertificate::fromData(pem, QSsl::Pem);
    QSslKey key(pem, QSsl::Rsa, QSsl::Pem, QSsl::PrivateKey);
    if (certs.isEmpty() || key.isNull()) {
        // Broken file: start over (peers must pair again)
        qWarning() << "lingmo-continuity: unreadable certificate, making a new one";
        QFile::remove(path);
        if (!generate(path))
            return false;
        return ensure();
    }
    m_cert = certs.first();
    m_key = key;
    m_fingerprint = fingerprintOf(m_cert);
    return true;
}

bool Identity::generate(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile::setPermissions(QFileInfo(path).absolutePath(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

    const QString openssl = QStandardPaths::findExecutable(QStringLiteral("openssl"));
    if (openssl.isEmpty()) {
        qWarning() << "lingmo-continuity: openssl not found, can't make a certificate";
        return false;
    }
    QTemporaryDir tmp(QFileInfo(path).absolutePath() + QStringLiteral("/.new-XXXXXX"));
    if (!tmp.isValid())
        return false;
    const QString keyPath = tmp.filePath(QStringLiteral("key.pem"));
    const QString certPath = tmp.filePath(QStringLiteral("cert.pem"));
    QProcess p;
    p.start(openssl, {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"),
                      QStringLiteral("rsa:2048"), QStringLiteral("-sha256"), QStringLiteral("-nodes"),
                      QStringLiteral("-days"), QStringLiteral("3650"), QStringLiteral("-subj"),
                      QStringLiteral("/CN=") + Store::computerName(), QStringLiteral("-keyout"), keyPath,
                      QStringLiteral("-out"), certPath});
    if (!p.waitForFinished(60000) || p.exitCode() != 0) {
        qWarning() << "lingmo-continuity: openssl failed:" << p.readAllStandardError();
        return false;
    }
    QFile k(keyPath), c(certPath);
    if (!k.open(QIODevice::ReadOnly) || !c.open(QIODevice::ReadOnly))
        return false;
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    out.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    out.write(k.readAll());
    out.write(c.readAll());
    return out.flush();
}

QString Identity::fingerprintOf(const QSslCertificate &cert)
{
    return QString::fromLatin1(cert.digest(QCryptographicHash::Sha256).toHex()).toUpper();
}

QString Identity::formatFingerprint(const QString &hex)
{
    QStringList parts;
    for (int i = 0; i + 1 < hex.size(); i += 2)
        parts << hex.mid(i, 2);
    return parts.join(QLatin1Char(':'));
}

QString Identity::pairingCode(const QString &fingerprintA, const QString &fingerprintB)
{
    const QString a = fingerprintA.toUpper(), b = fingerprintB.toUpper();
    const QByteArray both = (a < b ? a + b : b + a).toLatin1();
    const QByteArray hash = QCryptographicHash::hash(both, QCryptographicHash::Sha256);
    const quint32 n = qFromBigEndian<quint32>(hash.constData()) % 1000000;
    const QString digits = QStringLiteral("%1").arg(n, 6, 10, QLatin1Char('0'));
    return digits.left(3) + QLatin1Char(' ') + digits.mid(3);
}
