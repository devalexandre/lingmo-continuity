/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "service.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSocketNotifier>
#include <QTranslator>

#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

// SIGTERM (logout) and SIGINT end the event loop, so Deskflow and the sockets
// are closed properly
static int s_signalFd[2];

static void onSignal(int)
{
    char c = 1;
    [[maybe_unused]] auto n = ::write(s_signalFd[0], &c, 1);
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("lingmo-continuityd"));
    app.setApplicationVersion(QStringLiteral(LINGMO_CONTINUITY_VERSION));
    app.setDesktopFileName(QStringLiteral("lingmo-continuityd"));
    app.setQuitOnLastWindowClosed(false);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Shared keyboard and mouse for Lingmo (Deskflow)"));
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption autostart(QStringLiteral("autostart"),
                                 QStringLiteral("Started with the session: quit at once if the feature is off"));
    parser.addOption(autostart);
    parser.process(app);

    const QString dir = qEnvironmentVariable("LINGMO_CONTINUITY_TRANSLATIONS", QStringLiteral(TRANSLATIONS_DIR));
    auto *translator = new QTranslator(&app);
    if (translator->load(QLocale(), QStringLiteral("lingmo-continuity"), QStringLiteral("_"), dir))
        app.installTranslator(translator);

    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, s_signalFd) == 0) {
        auto *notifier = new QSocketNotifier(s_signalFd[1], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [] {
            char c;
            [[maybe_unused]] auto n = ::read(s_signalFd[1], &c, 1);
            QCoreApplication::quit();
        });
        struct sigaction sa = {};
        sa.sa_handler = onSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGHUP, &sa, nullptr);
    }

    Service service;
    if (!service.registerOnBus()) {
        qInfo() << "lingmo-continuityd: already running";
        return 0;
    }
    service.start(true);
    if (parser.isSet(autostart) && !service.isEnabled()) {
        // Off: nothing to do this session. Settings starts us over D-Bus
        // (data/com.lingmo.Continuity.service) when someone turns it on
        return 0;
    }
    return app.exec();
}
