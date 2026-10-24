/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "deskflowrunner.h"
#include "store.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

#include <csignal>
#include <sys/prctl.h>

// Deskflow names its IPC socket after the app: <TMPDIR>/deskflow-core
static const QString kIpcName = QStringLiteral("deskflow-core");

DeskflowRunner::DeskflowRunner(QObject *parent)
    : QObject(parent)
{
    m_retry.setSingleShot(true);
    connect(&m_retry, &QTimer::timeout, this, &DeskflowRunner::launch);
    m_ipcRetry.setSingleShot(true);
    connect(&m_ipcRetry, &QTimer::timeout, this, &DeskflowRunner::connectIpc);
}

DeskflowRunner::~DeskflowRunner()
{
    stop();
}

QString DeskflowRunner::binary()
{
    const QString env = qEnvironmentVariable("LINGMO_CONTINUITY_DESKFLOW");
    if (!env.isEmpty())
        return QFileInfo(env).isExecutable() ? env : QString();
    return QStandardPaths::findExecutable(QStringLiteral("deskflow-core"));
}

QString DeskflowRunner::version()
{
    static QString cached;
    static QString cachedFor;
    const QString bin = binary();
    if (bin.isEmpty())
        return {};
    if (cachedFor == bin)
        return cached;
    QProcess p;
    p.start(bin, {QStringLiteral("--version")});
    p.waitForFinished(5000);
    // "deskflow-core: 1.27.0"
    const QString out = QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError());
    const auto m = QRegularExpression(QStringLiteral("(\\d+\\.\\d+\\.\\d+)")).match(out);
    cached = m.hasMatch() ? m.captured(1) : QStringLiteral("?");
    cachedFor = bin;
    return cached;
}

void DeskflowRunner::start(const QString &mode, const QString &settingsPath)
{
    if (m_wanted && mode == m_mode && settingsPath == m_settings && m_process)
        return restart();
    stop();
    m_mode = mode;
    m_settings = settingsPath;
    m_wanted = true;
    m_failures = 0;
    launch();
}

void DeskflowRunner::restart()
{
    if (!m_wanted)
        return;
    const QString mode = m_mode, settings = m_settings;
    stop();
    m_mode = mode;
    m_settings = settings;
    m_wanted = true;
    // Give the old one a moment to let go of the port
    m_retry.start(300);
}

void DeskflowRunner::stop()
{
    m_wanted = false;
    m_retry.stop();
    m_ipcRetry.stop();
    if (m_ipc) {
        m_ipc->disconnect(this);
        m_ipc->abort();
        m_ipc->deleteLater();
        m_ipc = nullptr;
    }
    if (m_process) {
        m_process->disconnect(this);
        if (m_process->state() != QProcess::NotRunning) {
            m_process->terminate();
            if (!m_process->waitForFinished(3000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
    m_clients.clear();
    setState(QString());
}

void DeskflowRunner::launch()
{
    if (!m_wanted)
        return;
    const QString bin = binary();
    if (bin.isEmpty()) {
        m_error = QStringLiteral("deskflow-not-installed");
        setState(QStringLiteral("Disconnected"));
        return;
    }
    m_error.clear();
    m_outBuffer.clear();

    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Its IPC socket lands in our private directory, not in a shared /tmp
    env.insert(QStringLiteral("TMPDIR"), Paths::runtimeDir());
    // Never Deskflow's own windows or tray
    env.remove(QStringLiteral("QT_QPA_PLATFORMTHEME"));
    env.remove(QStringLiteral("QT_STYLE_OVERRIDE"));
    m_process->setProcessEnvironment(env);
    // If the daemon dies (crash, kill), Deskflow goes with it: never a
    // leftover listening on the network
    m_process->setChildProcessModifier([] { prctl(PR_SET_PDEATHSIG, SIGTERM); });
    connect(m_process, &QProcess::readyRead, this, &DeskflowRunner::onOutput);
    connect(m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        qWarning() << "lingmo-continuity: deskflow-core exited" << code << status;
        if (m_ipc) {
            m_ipc->disconnect(this);
            m_ipc->abort();
            m_ipc->deleteLater();
            m_ipc = nullptr;
        }
        m_process->deleteLater();
        m_process = nullptr;
        m_clients.clear();
        setState(QStringLiteral("Disconnected"));
        if (!m_wanted)
            return;
        // Quick deaths back off: 1 s, 2 s, 5 s, 10 s, then every 30 s
        static const int delays[] = {1000, 2000, 5000, 10000, 30000};
        const bool quick = m_started.elapsed() < 10000;
        m_failures = quick ? qMin(m_failures + 1, 4) : 0;
        m_retry.start(delays[m_failures]);
    });

    m_started.start();
    m_process->start(bin, {m_mode, QStringLiteral("-s"), m_settings, QStringLiteral("--new-instance")});
    setState(QStringLiteral("Connecting"));
    m_ipcAttempts = 0;
    m_ipcRetry.start(400);
}

void DeskflowRunner::onOutput()
{
    if (!m_process)
        return;
    m_outBuffer += m_process->readAll();
    int nl;
    while ((nl = m_outBuffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(m_outBuffer.left(nl)).trimmed();
        m_outBuffer.remove(0, nl + 1);
        if (!line.isEmpty())
            parseLine(line);
    }
    // A runaway line without an end
    if (m_outBuffer.size() > 65536)
        m_outBuffer.clear();
}

void DeskflowRunner::parseLine(const QString &line)
{
    if (qEnvironmentVariableIsSet("LINGMO_CONTINUITY_DEBUG"))
        qInfo().noquote() << "deskflow:" << line;

    static const QRegularExpression switchRe(QStringLiteral("switch from \"([^\"]+)\" to \"([^\"]+)\""));
    if (const auto m = switchRe.match(line); m.hasMatch()) {
        emit switched(m.captured(1), m.captured(2));
        return;
    }
    if (line.endsWith(QLatin1String("INFO: entering computer"))) {
        emit entered();
        return;
    }
    if (line.endsWith(QLatin1String("INFO: leaving computer"))) {
        emit left();
        return;
    }
    static const QRegularExpression unknownRe(QStringLiteral("unrecognised client name \"([^\"]+)\""));
    if (const auto m = unknownRe.match(line); m.hasMatch()) {
        emit unrecognisedClient(m.captured(1));
        return;
    }
    if (line.contains(QLatin1String("failed to listen")) || line.contains(QLatin1String("cannot listen"))
        || line.contains(QLatin1String("address already in use"), Qt::CaseInsensitive)) {
        m_error = QStringLiteral("port-in-use");
        emit stateChanged();
    }
}

void DeskflowRunner::connectIpc()
{
    if (!m_process || m_ipc)
        return;
    m_ipc = new QLocalSocket(this);
    connect(m_ipc, &QLocalSocket::connected, this, [this] {
        m_ipc->write("hello=lingmo-continuity\n");
    });
    connect(m_ipc, &QLocalSocket::readyRead, this, &DeskflowRunner::onIpcData);
    connect(m_ipc, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError) {
        if (!m_ipc)
            return;
        m_ipc->deleteLater();
        m_ipc = nullptr;
        // Not listening yet: try for a few seconds
        if (m_process && ++m_ipcAttempts < 30)
            m_ipcRetry.start(500);
    });
    m_ipc->connectToServer(Paths::runtimeDir() + QLatin1Char('/') + kIpcName);
}

void DeskflowRunner::onIpcData()
{
    if (!m_ipc)
        return;
    m_ipcBuffer += m_ipc->readAll();
    int nl;
    while ((nl = m_ipcBuffer.indexOf('\n')) >= 0) {
        const QString message = QString::fromUtf8(m_ipcBuffer.left(nl)).trimmed();
        m_ipcBuffer.remove(0, nl + 1);
        const int eq = message.indexOf(QLatin1Char('='));
        const QString command = eq < 0 ? message : message.left(eq);
        const QString args = eq < 0 ? QString() : message.mid(eq + 1);

        if (command == QLatin1String("versionMismatch")) {
            // The hello must carry the core's exact version: say it back
            m_ipc->write("hello=" + args.toUtf8() + "\n");
        } else if (command == QLatin1String("connectionState")) {
            if (args != QLatin1String("Connected"))
                m_clients.clear();
            setState(args);
        } else if (command == QLatin1String("connectedClients")) {
            m_clients = args.split(QLatin1Char(','), Qt::SkipEmptyParts);
            emit stateChanged();
        } else if (command == QLatin1String("peerFingerprint")) {
            emit peerFingerprint(args.toUpper());
        } else if (command == QLatin1String("unrecognisedClient")) {
            emit unrecognisedClient(args);
        }
    }
}

void DeskflowRunner::setState(const QString &state)
{
    if (m_state == state)
        return;
    m_state = state;
    emit stateChanged();
}
