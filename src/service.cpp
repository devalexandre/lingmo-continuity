/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "service.h"
#include "firewall.h"

#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInformation>
#include <QNetworkInterface>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>

static constexpr qint64 kOnlineMs = 3 * 60 * 1000;

static qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

static QJsonArray screensJson(const QList<QRect> &screens)
{
    QJsonArray a;
    for (const QRect &r : screens)
        a.append(QJsonArray{r.x(), r.y(), r.width(), r.height()});
    return a;
}

static QList<QRect> screensFrom(const QJsonValue &v)
{
    QList<QRect> out;
    for (const QJsonValue &s : v.toArray()) {
        const QJsonArray r = s.toArray();
        if (r.size() == 4 && r.at(2).toInt() > 0 && r.at(3).toInt() > 0 && out.size() < 16)
            out.append(QRect(r.at(0).toInt(), r.at(1).toInt(), qMin(r.at(2).toInt(), 32768),
                             qMin(r.at(3).toInt(), 32768)));
    }
    return out;
}

// A Lingmo PC as it describes itself in discovery and in the channel's hello
static Peer peerFromHello(const QJsonObject &o, const QString &id, const QString &host)
{
    Peer p;
    p.id = id.toUpper();
    p.name = Store::cleanName(o.value(QStringLiteral("name")).toString());
    p.label = o.value(QStringLiteral("label")).toString().left(64);
    if (p.label.isEmpty())
        p.label = p.name;
    p.host = host;
    p.port = qBound(0, o.value(QStringLiteral("port")).toInt(), 65535);
    p.deskflowPort = qBound(0, o.value(QStringLiteral("df")).toInt(), 65535);
    p.role = o.value(QStringLiteral("role")).toString() == QLatin1String("client") ? QStringLiteral("client")
                                                                                 : QStringLiteral("server");
    p.os = QStringLiteral("lingmo");
    p.screens = screensFrom(o.value(QStringLiteral("screens")));
    p.lastSeen = now();
    return p;
}

Service::Service(QObject *parent)
    : QObject(parent)
    , m_channel(&m_identity)
{
    m_stateTimer.setSingleShot(true);
    m_stateTimer.setInterval(60);
    connect(&m_stateTimer, &QTimer::timeout, this, [this] {
        emit StateChanged(QString::fromUtf8(QJsonDocument(stateJson()).toJson(QJsonDocument::Compact)));
    });
    m_expireTimer.setInterval(30000);
    connect(&m_expireTimer, &QTimer::timeout, this, [this] {
        const qint64 t = now();
        for (auto it = m_discovered.begin(); it != m_discovered.end();) {
            if (t - it->lastSeen > kOnlineMs)
                it = m_discovered.erase(it);
            else
                ++it;
        }
        scheduleState();
    });
    m_networkDebounce.setSingleShot(true);
    m_networkDebounce.setInterval(2000);
    connect(&m_networkDebounce, &QTimer::timeout, this, [this] {
        if (!m_store.enabled)
            return;
        // New network (Wi-Fi changed, cable plugged, resumed): look again and
        // reconnect now instead of waiting for Deskflow's retry
        m_discovery.setAnnouncement(helloJson());
        m_discovery.query();
        if (m_store.role == QLatin1String("client"))
            m_runner.restart();
    });

    connect(&m_runner, &DeskflowRunner::stateChanged, this, [this] {
        updateConnectionFeedback();
        if (m_store.role == QLatin1String("server"))
            pushLayout();
        scheduleState();
    });
    connect(&m_runner, &DeskflowRunner::switched, this, [this](const QString &, const QString &to) {
        m_active = to;
        updateConnectionFeedback();
        scheduleState();
    });
    connect(&m_runner, &DeskflowRunner::left, this, [this] {
        m_hasLeft = true;
    });
    connect(&m_runner, &DeskflowRunner::entered, this, [this] {
        // The principal "enters" itself when it starts: only after it left
        if (m_store.role == QLatin1String("client") || m_hasLeft)
            m_feedback.toast(tr("Now on"), m_store.label, QStringLiteral("computer"));
        if (m_store.role == QLatin1String("server"))
            m_active = Store::computerName();
    });
    connect(&m_runner, &DeskflowRunner::peerFingerprint, this, &Service::onPeerFingerprint);
    connect(&m_runner, &DeskflowRunner::unrecognisedClient, this, &Service::onUnrecognisedClient);

    connect(&m_discovery, &Discovery::found, this, &Service::onFound);

    m_channel.setTrust([this](const QString &id) { return isTrusted(id); });
    connect(&m_channel, &Channel::pairingStarted, this, &Service::onPairingStarted);
    connect(&m_channel, &Channel::pairingFinished, this, &Service::onPairingFinished);
    connect(&m_channel, &Channel::message, this, &Service::onChannelMessage);
    connect(&m_channel, &Channel::filesReceived, this,
            [this](const QString &id, const QJsonObject &hello, const QStringList &paths) {
        const Peer *p = m_store.findById(id);
        const QString from = p ? p->label : hello.value(QStringLiteral("label")).toString();
        m_feedback.filesReceived(from, paths);
    });
    connect(&m_channel, &Channel::sendFinished, this, [this](const QString &id, bool ok, int) {
        if (ok)
            return;
        const Peer *p = m_store.findById(id);
        m_feedback.toast(tr("Couldn't send the files"), p ? p->label : QString(), QStringLiteral("dialog-error"));
    });

    connect(&m_drag, &DragWatcher::dragAtEdge, this, &Service::onDragAtEdge);
    // The edge accepts the drop only where a paired Lingmo PC is
    m_drag.setAcceptor([this](const QString &dir, double fraction) {
        if (!m_store.enabled || status() != QLatin1String("connected"))
            return false;
        const QString to = DeskflowConfig::neighbourAt(effectiveLayout(), Store::computerName(), dir, fraction);
        const Peer *peer = to.isEmpty() ? nullptr : m_store.findByName(to);
        return peer && peer->paired && peer->os == QLatin1String("lingmo") && peer->port > 0;
    });
    connect(&m_feedback, &Feedback::pairingAnswered, this, &Service::onPairingAnswered);
}

Service::~Service()
{
    stopAll();
}

bool Service::registerOnBus()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.registerService(QStringLiteral("com.lingmo.Continuity")))
        return false;
    return bus.registerObject(QStringLiteral("/Continuity"), this,
                              QDBusConnection::ExportScriptableSlots | QDBusConnection::ExportScriptableSignals);
}

void Service::start(bool withUi)
{
    m_withUi = withUi;
    m_store.load();
    if (withUi)
        m_feedback.init();
    if (QNetworkInformation::loadDefaultBackend() && QNetworkInformation::instance()) {
        connect(QNetworkInformation::instance(), &QNetworkInformation::reachabilityChanged, this,
                [this] { m_networkDebounce.start(); });
    }
    if (auto *app = qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        auto screensChanged = [this] {
            if (!m_store.enabled)
                return;
            m_discovery.setAnnouncement(helloJson());
            m_discovery.announce();
            apply();
        };
        connect(app, &QGuiApplication::screenAdded, this, screensChanged);
        connect(app, &QGuiApplication::screenRemoved, this, screensChanged);
    }
    apply();
}

// ------------------------------------------------------------------ state

void Service::save()
{
    if (!m_store.save())
        qWarning() << "lingmo-continuity: can't save" << Store::path();
}

QList<QRect> Service::ownScreens() const
{
    QList<QRect> out;
    if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        for (QScreen *s : QGuiApplication::screens()) {
            const qreal dpr = s->devicePixelRatio();
            const QRect g = s->geometry();
            out.append(QRect(g.topLeft() * dpr, g.size() * dpr));
        }
    }
    if (out.isEmpty())
        out.append(QRect(0, 0, 1920, 1080));
    return out;
}

QSize Service::ownSize() const
{
    QRect all;
    for (const QRect &r : ownScreens())
        all = all.united(r);
    return all.size();
}

QJsonObject Service::helloJson() const
{
    return {
        {QStringLiteral("id"), m_identity.fingerprint()},
        {QStringLiteral("name"), Store::computerName()},
        {QStringLiteral("label"), m_store.label},
        {QStringLiteral("role"), m_store.role},
        {QStringLiteral("port"), Ports::channel()},
        {QStringLiteral("df"), Ports::deskflow()},
        {QStringLiteral("screens"), screensJson(ownScreens())},
        {QStringLiteral("version"), QStringLiteral(LINGMO_CONTINUITY_VERSION)},
    };
}

QStringList Service::trustedIds() const
{
    QStringList ids;
    for (const Peer &p : m_store.peers) {
        if (p.paired && !p.id.isEmpty())
            ids << p.id;
    }
    return ids;
}

bool Service::isTrusted(const QString &id) const
{
    return !id.isEmpty() && trustedIds().contains(id.toUpper());
}

Peer *Service::serverPeer()
{
    if (Peer *p = m_store.find(m_store.serverKey); p && p->paired)
        return p;
    // Only one principal paired: that one
    Peer *found = nullptr;
    for (Peer &p : m_store.peers) {
        if (p.paired && (p.role == QLatin1String("server") || p.os == QLatin1String("other"))) {
            if (found)
                return nullptr;
            found = &p;
        }
    }
    return found;
}

// A newly paired PC goes next to this one (right, left, below, above), or else
// next to another one: always touching something, never overlapping
void Service::placeNewPeers()
{
    QList<QRect> taken{QRect(m_store.selfPos, ownSize())};
    for (const Peer &p : std::as_const(m_store.peers)) {
        if (p.paired && p.placed)
            taken.append(QRect(p.pos, p.desktopSize()));
    }
    bool changed = false;
    for (Peer &p : m_store.peers) {
        if (!p.paired || p.placed)
            continue;
        const QSize size = p.desktopSize();
        auto free = [&](const QPoint &at) {
            const QRect r(at, size);
            return std::none_of(taken.begin(), taken.end(), [&](const QRect &t) { return t.intersects(r); });
        };
        QPoint spot;
        bool found = false;
        for (const QRect &t : std::as_const(taken)) {
            const QPoint candidates[] = {
                QPoint(t.x() + t.width(), t.y()), QPoint(t.x() - size.width(), t.y()),
                QPoint(t.x(), t.y() + t.height()), QPoint(t.x(), t.y() - size.height()),
            };
            for (const QPoint &c : candidates) {
                if (free(c)) {
                    spot = c;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        }
        p.pos = found ? spot : QPoint(m_store.selfPos.x() + ownSize().width(), m_store.selfPos.y());
        p.placed = true;
        taken.append(QRect(p.pos, size));
        changed = true;
    }
    if (changed)
        save();
}

QList<DeskflowConfig::Computer> Service::layout() const
{
    QList<DeskflowConfig::Computer> out;
    out.append({Store::computerName(), QRect(m_store.selfPos, ownSize())});
    for (const Peer &p : m_store.peers) {
        if (p.paired && p.placed && !p.name.isEmpty())
            out.append({p.name, QRect(p.pos, p.desktopSize())});
    }
    return out;
}

// What decides where the edges lead: our own layout on the principal, the
// principal's on a secundário
QList<DeskflowConfig::Computer> Service::effectiveLayout() const
{
    return m_store.role == QLatin1String("server") ? layout() : m_remoteLayout;
}

QString Service::labelForName(const QString &name) const
{
    if (name.compare(Store::computerName(), Qt::CaseInsensitive) == 0)
        return m_store.label;
    for (const Peer &p : m_store.peers) {
        if (p.name.compare(name, Qt::CaseInsensitive) == 0)
            return p.label;
    }
    return m_remoteLabels.value(name, name);
}

QString Service::status() const
{
    if (!m_store.enabled)
        return QStringLiteral("off");
    if (!m_identityOk)
        return QStringLiteral("error");
    if (DeskflowRunner::binary().isEmpty())
        return QStringLiteral("nodeskflow");
    if (m_runner.lastError() == QLatin1String("port-in-use"))
        return QStringLiteral("portinuse");
    const QString st = m_runner.state();
    if (m_store.role == QLatin1String("server")) {
        if (st == QLatin1String("Connected") && !m_runner.connectedClients().isEmpty())
            return QStringLiteral("connected");
        if (st == QLatin1String("Listening") || st == QLatin1String("Connected"))
            return QStringLiteral("waiting");
        return QStringLiteral("starting");
    }
    if (!const_cast<Service *>(this)->serverPeer())
        return QStringLiteral("noserver");
    return st == QLatin1String("Connected") ? QStringLiteral("connected") : QStringLiteral("connecting");
}

QJsonObject Service::stateJson() const
{
    const QString st = status();
    const bool server = m_store.role == QLatin1String("server");
    const QStringList clients = m_runner.connectedClients();
    Peer *chosen = server ? nullptr : const_cast<Service *>(this)->serverPeer();
    const qint64 t = now();

    QJsonArray peers;
    auto describe = [&](const Peer &p) {
        bool connected = false;
        if (server)
            connected = !p.name.isEmpty() && clients.contains(p.name, Qt::CaseInsensitive);
        else
            connected = chosen && chosen->key() == p.key() && st == QLatin1String("connected");
        const QSize size = p.desktopSize();
        QJsonObject o = p.toJson();
        o.insert(QStringLiteral("key"), p.key());
        o.insert(QStringLiteral("fingerprint"), Identity::formatFingerprint(p.id));
        o.insert(QStringLiteral("online"), connected || (p.lastSeen && t - p.lastSeen < kOnlineMs));
        o.insert(QStringLiteral("connected"), connected);
        o.insert(QStringLiteral("w"), size.width());
        o.insert(QStringLiteral("h"), size.height());
        o.insert(QStringLiteral("isServer"), chosen && chosen->key() == p.key());
        o.insert(QStringLiteral("canPair"), !p.paired && p.os == QLatin1String("lingmo") && p.port > 0);
        o.insert(QStringLiteral("active"), !m_active.isEmpty() && p.name.compare(m_active, Qt::CaseInsensitive) == 0);
        peers.append(o);
    };
    for (const Peer &p : m_store.peers) {
        Peer copy = p;
        if (const auto it = m_discovered.constFind(p.id); !p.id.isEmpty() && it != m_discovered.constEnd())
            copy.lastSeen = it->lastSeen;
        describe(copy);
    }
    for (const Peer &p : m_discovered) {
        if (!m_store.peers.isEmpty() && const_cast<Store &>(m_store).findById(p.id))
            continue;
        describe(p);
    }

    QStringList connectedLabels;
    for (const QString &c : clients)
        connectedLabels << labelForName(c);

    const QSize own = ownSize();
    QJsonObject pairing;
    if (!m_pairingState.isEmpty()) {
        pairing = {
            {QStringLiteral("state"), m_pairingState},
            {QStringLiteral("label"), m_pairingPeer.label},
            {QStringLiteral("key"), m_pairingPeer.key()},
            {QStringLiteral("code"), m_pairingCode},
        };
    }
    // Where other PCs reach this one (for Deskflow on another OS)
    QJsonArray addresses;
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)
            || (flags & QNetworkInterface::IsLoopBack) || iface.type() == QNetworkInterface::Virtual)
            continue;
        // Containers' and VMs' bridges aren't where other PCs come from
        static const QStringList bridges{QStringLiteral("docker"), QStringLiteral("br-"), QStringLiteral("veth"),
                                         QStringLiteral("virbr"), QStringLiteral("vmnet"), QStringLiteral("lxc"),
                                         QStringLiteral("podman"), QStringLiteral("cni"), QStringLiteral("waydroid")};
        if (std::any_of(bridges.begin(), bridges.end(),
                        [&](const QString &b) { return iface.name().startsWith(b); }))
            continue;
        for (const QNetworkAddressEntry &e : iface.addressEntries()) {
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol)
                addresses.append(e.ip().toString());
        }
    }
    const QString version = DeskflowRunner::version();
    return {
        {QStringLiteral("addresses"), addresses},
        {QStringLiteral("enabled"), m_store.enabled},
        {QStringLiteral("role"), m_store.role},
        {QStringLiteral("name"), Store::computerName()},
        {QStringLiteral("label"), m_store.label},
        {QStringLiteral("fingerprint"), Identity::formatFingerprint(m_identity.fingerprint())},
        {QStringLiteral("status"), st},
        {QStringLiteral("active"), m_active.isEmpty() ? QString() : labelForName(m_active)},
        {QStringLiteral("connectedTo"), server ? connectedLabels.join(QStringLiteral(", "))
                                               : (chosen && st == QLatin1String("connected") ? chosen->label : QString())},
        {QStringLiteral("server"), chosen ? chosen->key() : QString()},
        {QStringLiteral("deskflow"), QJsonObject{{QStringLiteral("available"), !version.isEmpty()},
                                                 {QStringLiteral("version"), version}}},
        {QStringLiteral("lingmoShare"), !QStandardPaths::findExecutable(QStringLiteral("lingmo-share")).isEmpty()},
        {QStringLiteral("ports"), QJsonObject{{QStringLiteral("deskflow"), Ports::deskflow()},
                                              {QStringLiteral("discovery"), Ports::discovery()},
                                              {QStringLiteral("channel"), Ports::channel()}}},
        {QStringLiteral("firewall"), m_store.enabled ? Firewall::hint(server) : QJsonObject()},
        {QStringLiteral("options"),
         QJsonObject{
             {QStringLiteral("clipboard"), m_store.options.clipboard},
             {QStringLiteral("switchMode"), m_store.options.switchMode},
             {QStringLiteral("switchDelay"), m_store.options.switchDelay},
             {QStringLiteral("doubleTap"), m_store.options.doubleTap},
             {QStringLiteral("lockHotkey"), m_store.options.lockHotkey},
             {QStringLiteral("lockHotkeyLabel"), DeskflowConfig::hotkeyLabel(m_store.options.lockHotkey)},
             {QStringLiteral("switchHotkey"), m_store.options.switchHotkey},
             {QStringLiteral("switchHotkeyLabel"), DeskflowConfig::hotkeyLabel(m_store.options.switchHotkey)},
         }},
        {QStringLiteral("self"),
         QJsonObject{
             {QStringLiteral("x"), m_store.selfPos.x()},
             {QStringLiteral("y"), m_store.selfPos.y()},
             {QStringLiteral("w"), own.width()},
             {QStringLiteral("h"), own.height()},
             {QStringLiteral("screens"), screensJson(ownScreens())},
             {QStringLiteral("active"), m_active.isEmpty() || m_active == Store::computerName()},
         }},
        {QStringLiteral("peers"), peers},
        {QStringLiteral("pairing"), pairing},
    };
}

void Service::scheduleState()
{
    m_stateTimer.start();
}

QString Service::State()
{
    return QString::fromUtf8(QJsonDocument(stateJson()).toJson(QJsonDocument::Compact));
}

// -------------------------------------------------------------- running

void Service::stopAll()
{
    m_runner.stop();
    m_discovery.stop();
    m_channel.close();
    m_drag.stop();
    m_expireTimer.stop();
    m_discovered.clear();
    m_remoteLayout.clear();
    m_active.clear();
    m_hasLeft = false;
    m_configHash.clear();
    m_feedback.setConnection(false, QString());
}

void Service::apply()
{
    if (!m_store.enabled) {
        stopAll();
        scheduleState();
        return;
    }
    m_identityOk = m_identity.ensure();
    if (!m_identityOk) {
        stopAll();
        scheduleState();
        return;
    }
    placeNewPeers();

    m_channel.setHello(helloJson());
    m_channel.listen();
    m_discovery.setAnnouncement(helloJson());
    if (!m_discovery.isRunning())
        m_discovery.start();
    m_expireTimer.start();
    if (m_withUi && !m_drag.isRunning())
        m_drag.start();

    // Deskflow's files: trust only paired certificates, both ways
    const QString dir = Paths::deskflowDir();
    const QStringList ids = trustedIds();
    DeskflowConfig::writeTrustFile(dir + QStringLiteral("/tls/trusted-clients"), ids);
    DeskflowConfig::writeTrustFile(dir + QStringLiteral("/tls/trusted-servers"), ids);

    DeskflowConfig::Settings st;
    st.mode = m_store.role;
    st.name = Store::computerName();
    st.port = Ports::deskflow();
    st.certificate = m_identity.pemPath();
    st.options = m_store.options;
    if (qEnvironmentVariableIsSet("LINGMO_CONTINUITY_DEBUG"))
        st.logLevel = QStringLiteral("DEBUG");
    QByteArray hashInput = ids.join(QLatin1Char(',')).toUtf8();
    if (m_store.role == QLatin1String("server")) {
        const auto computers = layout();
        for (const auto &c : computers)
            st.computers << c.name;
        st.serverConfig = dir + QStringLiteral("/server.conf");
        const QString text = DeskflowConfig::serverConfig(computers, m_store.options);
        QFile f(st.serverConfig);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            f.write(text.toUtf8());
        hashInput += text.toUtf8();
    } else {
        Peer *server = serverPeer();
        if (!server || server->host.isEmpty()) {
            m_runner.stop();
            m_configHash.clear();
            updateConnectionFeedback();
            scheduleState();
            return;
        }
        st.remoteHost = server->host;
        // Deskflow's port on the principal (24800 unless it says otherwise)
        st.port = server->deskflowPort > 0 ? server->deskflowPort : 24800;
    }
    const QString settingsPath = dir + QStringLiteral("/Deskflow.conf");
    DeskflowConfig::writeSettings(settingsPath, st);
    QFile conf(settingsPath);
    if (conf.open(QIODevice::ReadOnly))
        hashInput += conf.readAll();
    const QByteArray hash = QCryptographicHash::hash(hashInput, QCryptographicHash::Sha256);

    if (DeskflowRunner::binary().isEmpty()) {
        m_runner.stop();
    } else if (hash != m_configHash || !m_runner.isRunning() || m_runner.mode() != m_store.role) {
        // Something changed (layout, peers, options): Deskflow reads its
        // files at start, so start it again
        m_configHash = hash;
        m_hasLeft = false;
        m_runner.start(m_store.role, settingsPath);
    }
    if (m_store.role == QLatin1String("server"))
        pushLayout();
    scheduleState();
}

void Service::updateConnectionFeedback()
{
    const bool connected = status() == QLatin1String("connected");
    QString subtitle;
    if (connected) {
        if (m_store.role == QLatin1String("server")) {
            QStringList labels;
            for (const QString &c : m_runner.connectedClients())
                labels << labelForName(c);
            subtitle = tr("With %1").arg(labels.join(QStringLiteral(", ")));
            if (!m_active.isEmpty() && m_active != Store::computerName())
                subtitle = tr("Now on %1").arg(labelForName(m_active));
        } else if (Peer *p = serverPeer()) {
            subtitle = tr("With %1").arg(p->label);
        }
    }
    m_feedback.setConnection(connected, subtitle);
}

// The principal tells its Lingmo secundários where everyone is, so a file
// dragged against their edges knows where to go
void Service::pushLayout()
{
    if (m_store.role != QLatin1String("server") || !m_store.enabled)
        return;
    const QStringList clients = m_runner.connectedClients();
    // Who disconnected gets it again when it comes back
    for (auto it = m_layoutSent.begin(); it != m_layoutSent.end();) {
        const Peer *p = m_store.findById(it.key());
        if (!p || !clients.contains(p->name, Qt::CaseInsensitive))
            it = m_layoutSent.erase(it);
        else
            ++it;
    }
    if (clients.isEmpty())
        return;
    QJsonArray computers;
    for (const auto &c : layout()) {
        computers.append(QJsonObject{
            {QStringLiteral("name"), c.name},
            {QStringLiteral("label"), labelForName(c.name)},
            {QStringLiteral("x"), c.rect.x()},
            {QStringLiteral("y"), c.rect.y()},
            {QStringLiteral("w"), c.rect.width()},
            {QStringLiteral("h"), c.rect.height()},
        });
    }
    const QJsonObject message{{QStringLiteral("t"), QStringLiteral("layout")},
                              {QStringLiteral("computers"), computers}};
    const QByteArray digest = QJsonDocument(message).toJson(QJsonDocument::Compact);
    for (const Peer &p : m_store.peers) {
        if (!p.paired || p.os != QLatin1String("lingmo") || p.port <= 0 || p.host.isEmpty()
            || !clients.contains(p.name, Qt::CaseInsensitive))
            continue;
        if (m_layoutSent.value(p.id) == digest)
            continue;
        m_layoutSent.insert(p.id, digest);
        m_channel.sendMessage(p.host, p.port, p.id, message);
    }
}

// ----------------------------------------------------------- D-Bus calls

void Service::SetEnabled(bool enabled)
{
    if (m_store.enabled == enabled)
        return;
    m_store.enabled = enabled;
    save();
    apply();
}

void Service::SetRole(const QString &role)
{
    const QString r = role == QLatin1String("client") ? QStringLiteral("client") : QStringLiteral("server");
    if (r == m_store.role)
        return;
    m_store.role = r;
    m_remoteLayout.clear();
    m_active.clear();
    save();
    if (m_store.enabled) {
        // Tell paired PCs right away
        m_discovery.setAnnouncement(helloJson());
        m_discovery.announce();
    }
    apply();
}

void Service::SetLabel(const QString &label)
{
    const QString l = label.trimmed().left(64);
    if (l.isEmpty() || l == m_store.label)
        return;
    m_store.label = l;
    save();
    if (m_store.enabled) {
        m_discovery.setAnnouncement(helloJson());
        m_discovery.announce();
    }
    apply();
}

void Service::SetOption(const QString &key, const QString &value)
{
    Options &o = m_store.options;
    if (key == QLatin1String("clipboard")) {
        o.clipboard = value == QLatin1String("true");
    } else if (key == QLatin1String("switchMode")) {
        if (value != QLatin1String("immediate") && value != QLatin1String("delay") && value != QLatin1String("doubletap"))
            return;
        o.switchMode = value;
    } else if (key == QLatin1String("switchDelay")) {
        o.switchDelay = qBound(50, value.toInt(), 3000);
    } else if (key == QLatin1String("doubleTap")) {
        o.doubleTap = qBound(50, value.toInt(), 3000);
    } else if (key == QLatin1String("lockHotkey") || key == QLatin1String("switchHotkey")) {
        if (!value.isEmpty() && !DeskflowConfig::isValidHotkey(value))
            return;
        (key == QLatin1String("lockHotkey") ? o.lockHotkey : o.switchHotkey) = value;
    } else {
        return;
    }
    save();
    apply();
}

QString Service::AddPeer(const QString &address)
{
    const QString host = address.trimmed().left(255);
    if (host.isEmpty())
        return {};
    if (Peer *existing = m_store.find(QStringLiteral("host:") + host))
        return existing->key();
    for (const Peer &p : m_store.peers) {
        if (p.host == host)
            return p.key();
    }
    Peer p;
    p.host = host;
    // A name (not an address) is usually also its Deskflow screen name
    if (QHostAddress(host).isNull())
        p.name = Store::cleanName(host.section(QLatin1Char('.'), 0, 0));
    p.label = host;
    p.os = QStringLiteral("other");
    m_store.peers.append(p);
    save();
    // It may be a Lingmo PC on another subnet: ask it directly
    if (m_store.enabled && m_discovery.isRunning() && !QHostAddress(host).isNull())
        m_discovery.queryAt(QHostAddress(host));
    apply();
    return p.key();
}

void Service::Pair(const QString &key)
{
    if (!m_store.enabled)
        return;
    Peer peer;
    if (Peer *p = m_store.find(key))
        peer = *p;
    else if (m_discovered.contains(key))
        peer = m_discovered.value(key);
    else
        return;
    if (peer.os != QLatin1String("lingmo") || peer.port <= 0) {
        // Deskflow on another OS: its fingerprint comes when it connects; the
        // dialog then asks to confirm it
        m_pairingPeer = peer;
        m_pairingState = QStringLiteral("waiting");
        m_pairingCode.clear();
        scheduleState();
        return;
    }
    if (m_channel.isPairing())
        return;
    m_pairingPeer = peer;
    m_pairingState = QStringLiteral("waiting");
    m_pairingCode.clear();
    m_channel.pair(peer.host, peer.port);
    scheduleState();
}

void Service::CancelPairing()
{
    m_channel.cancelPairing();
    m_foreignFingerprint.clear();
    m_pairingState.clear();
    m_pairingPeer = Peer();
    m_feedback.pairingDone(false, QString());
    scheduleState();
}

void Service::Unpair(const QString &key)
{
    Peer *p = m_store.find(key);
    if (!p)
        return;
    const Peer copy = *p;
    // Tell it, so it forgets us too (best effort)
    if (copy.paired && copy.os == QLatin1String("lingmo") && copy.port > 0 && !copy.host.isEmpty() && m_store.enabled)
        m_channel.sendMessage(copy.host, copy.port, copy.id, {{QStringLiteral("t"), QStringLiteral("unpair")}});
    m_store.peers.removeIf([&](const Peer &x) { return x.key() == key; });
    if (m_store.serverKey == key)
        m_store.serverKey.clear();
    // Its Deskflow keeps retrying: don't ask about it again right away
    if (!copy.id.isEmpty())
        m_declined.insert(copy.id, now());
    if (copy.os == QLatin1String("lingmo") && !copy.id.isEmpty()) {
        Peer seen = copy;
        seen.paired = false;
        seen.placed = false;
        if (seen.lastSeen)
            m_discovered.insert(seen.id, seen);
    }
    save();
    // Deskflow is restarted with the new trust files: the connection drops now
    apply();
}

void Service::RemovePeer(const QString &key)
{
    Peer *p = m_store.find(key);
    if (!p || p->paired)
        return;
    m_store.peers.removeIf([&](const Peer &x) { return x.key() == key; });
    save();
    scheduleState();
}

void Service::SetPosition(const QString &key, int x, int y)
{
    x = qBound(-100000, x, 100000);
    y = qBound(-100000, y, 100000);
    if (key == QLatin1String("self")) {
        m_store.selfPos = QPoint(x, y);
    } else if (Peer *p = m_store.find(key)) {
        p->pos = QPoint(x, y);
        p->placed = true;
    } else {
        return;
    }
    save();
    apply();
}

void Service::SetServer(const QString &key)
{
    if (m_store.serverKey == key)
        return;
    m_store.serverKey = key;
    m_remoteLayout.clear();
    save();
    apply();
}

void Service::Discover()
{
    if (!m_store.enabled)
        return;
    m_discovery.setAnnouncement(helloJson());
    m_discovery.query();
}

void Service::Preview(const QString &what)
{
    if (what == QLatin1String("toast")) {
        m_feedback.toast(tr("Now on"), m_store.label, QStringLiteral("computer"));
    } else if (what == QLatin1String("pairing") || what == QLatin1String("pairing-other")) {
        const bool other = what == QLatin1String("pairing-other");
        const QString fp = m_identity.fingerprint().isEmpty() ? QStringLiteral("AF30B496C3FC02E87653EED8BB8519B2")
                                                              : m_identity.fingerprint();
        m_feedback.showPairing({
            {QStringLiteral("label"), other ? QStringLiteral("DESKTOP-WIN11") : QStringLiteral("Notebook")},
            {QStringLiteral("code"), other ? QString() : QStringLiteral("482 913")},
            {QStringLiteral("fingerprint"), Identity::formatFingerprint(fp)},
            {QStringLiteral("incoming"), true},
            {QStringLiteral("other"), other},
            {QStringLiteral("preview"), true},
        });
    } else if (what == QLatin1String("received")) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        m_feedback.filesReceived(QStringLiteral("Notebook"),
                                 {dir + QStringLiteral("/Relatório anual.pdf"), dir + QStringLiteral("/foto-praia.jpg")});
    }
}

// --------------------------------------------------------------- events

void Service::onFound(const QJsonObject &info, const QHostAddress &from)
{
    const QString id = info.value(QStringLiteral("id")).toString().toUpper();
    if (id.isEmpty() || id == m_identity.fingerprint() || id.size() != 64)
        return;
    const Peer seen = peerFromHello(info, id, from.toString());
    if (Peer *p = m_store.findById(id)) {
        const bool hostChanged = p->host != seen.host;
        const bool roleChanged = p->role != seen.role;
        p->host = seen.host;
        p->port = seen.port;
        p->deskflowPort = seen.deskflowPort;
        p->label = seen.label;
        p->name = seen.name;
        p->role = seen.role;
        if (!seen.screens.isEmpty())
            p->screens = seen.screens;
        p->os = QStringLiteral("lingmo");
        m_discovered.insert(id, seen);
        save();
        // A secundário follows its principal to a new address
        if ((hostChanged || roleChanged) && m_store.enabled)
            apply();
    } else {
        // Seen at an address someone typed by hand: it's that one
        for (Peer &p : m_store.peers) {
            if (!p.paired && p.id.isEmpty() && p.host == seen.host) {
                p.os = QStringLiteral("lingmo");
                p.id = id;
                p.port = seen.port;
                p.deskflowPort = seen.deskflowPort;
                p.name = seen.name;
                p.label = seen.label;
                p.role = seen.role;
                p.screens = seen.screens;
                save();
                break;
            }
        }
        m_discovered.insert(id, seen);
    }
    scheduleState();
}

void Service::onPairingStarted(const QJsonObject &hello, const QString &id, const QString &code, bool incoming,
                               const QString &host)
{
    m_pairingPeer = peerFromHello(hello, id, host);
    if (Peer *known = m_store.findById(id))
        m_pairingPeer.pos = known->pos;
    m_pairingState = QStringLiteral("confirm");
    m_pairingCode = code;
    m_feedback.showPairing({
        {QStringLiteral("label"), m_pairingPeer.label},
        {QStringLiteral("code"), code},
        {QStringLiteral("fingerprint"), Identity::formatFingerprint(id)},
        {QStringLiteral("incoming"), incoming},
        {QStringLiteral("other"), false},
    });
    scheduleState();
}

void Service::onPairingAnswered(bool accept)
{
    if (!m_foreignFingerprint.isEmpty()) {
        const QString fp = m_foreignFingerprint;
        m_foreignFingerprint.clear();
        m_pairingState.clear();
        if (!accept) {
            m_declined.insert(fp, now());
            m_feedback.pairingDone(false, QString());
            scheduleState();
            return;
        }
        // Attach it to the PC added by hand we were waiting for, if any
        Peer *target = m_store.find(m_pairingPeer.key());
        if (!target || target->paired) {
            for (Peer &p : m_store.peers) {
                if (p.os == QLatin1String("other") && p.id.isEmpty()) {
                    target = &p;
                    break;
                }
            }
        }
        if (target) {
            target->id = fp;
            target->paired = true;
        } else {
            Peer p;
            p.id = fp;
            p.os = QStringLiteral("other");
            p.label = tr("Computer %1").arg(fp.left(4));
            p.paired = true;
            m_store.peers.append(p);
            target = &m_store.peers.last();
        }
        const QString label = target->label;
        m_pairingPeer = Peer();
        save();
        apply();
        m_feedback.pairingDone(true, tr("Paired with %1").arg(label));
        return;
    }
    m_channel.answerPairing(accept);
}

void Service::onPairingFinished(bool ok, const QJsonObject &hello, const QString &id, const QString &host,
                                const QString &reason)
{
    m_pairingState.clear();
    m_pairingCode.clear();
    if (!ok) {
        QString message;
        if (reason == QLatin1String("rejected-there"))
            message = tr("%1 declined").arg(m_pairingPeer.label);
        else if (reason == QLatin1String("busy"))
            message = tr("%1 is pairing with another computer").arg(m_pairingPeer.label);
        else if (reason == QLatin1String("timeout"))
            message = tr("Nobody confirmed in time");
        else if (reason == QLatin1String("unreachable"))
            message = tr("Couldn't reach %1").arg(m_pairingPeer.label.isEmpty() ? host : m_pairingPeer.label);
        else if (reason == QLatin1String("lost"))
            message = tr("The connection was lost");
        m_feedback.pairingDone(false, message);
        m_pairingPeer = Peer();
        scheduleState();
        return;
    }
    Peer fresh = peerFromHello(hello, id, host);
    Peer *p = m_store.findById(id);
    if (!p) {
        // A PC typed in by hand that turned out to be a Lingmo PC
        for (Peer &x : m_store.peers) {
            if (x.id.isEmpty() && x.host == host) {
                p = &x;
                break;
            }
        }
    }
    if (!p) {
        m_store.peers.append(fresh);
        p = &m_store.peers.last();
    }
    const QPoint pos = p->pos;
    const bool placed = p->placed;
    *p = fresh;
    p->pos = pos;
    p->placed = placed;
    p->paired = true;
    const QString label = p->label;
    m_discovered.remove(id);
    m_pairingPeer = Peer();
    save();
    apply();
    m_feedback.pairingDone(true, tr("Paired with %1").arg(label));
}

void Service::onChannelMessage(const QString &id, const QJsonObject &message, const QString &host)
{
    const QString t = message.value(QStringLiteral("t")).toString();
    Peer *p = m_store.findById(id);
    if (!p)
        return;
    if (!host.isEmpty() && p->host != host) {
        p->host = host;
        save();
    }
    if (t == QLatin1String("unpair")) {
        const QString key = p->key();
        m_store.peers.removeIf([&](const Peer &x) { return x.key() == key; });
        if (m_store.serverKey == key)
            m_store.serverKey.clear();
        save();
        apply();
    } else if (t == QLatin1String("layout")) {
        // Only from the principal we use
        Peer *server = serverPeer();
        if (m_store.role != QLatin1String("client") || !server || server->id != id)
            return;
        m_remoteLayout.clear();
        m_remoteLabels.clear();
        for (const QJsonValue &v : message.value(QStringLiteral("computers")).toArray()) {
            const QJsonObject c = v.toObject();
            const QString name = Store::cleanName(c.value(QStringLiteral("name")).toString());
            const QRect r(c.value(QStringLiteral("x")).toInt(), c.value(QStringLiteral("y")).toInt(),
                          qBound(1, c.value(QStringLiteral("w")).toInt(), 65536),
                          qBound(1, c.value(QStringLiteral("h")).toInt(), 65536));
            if (name.isEmpty() || m_remoteLayout.size() >= 32)
                continue;
            m_remoteLayout.append({name, r});
            m_remoteLabels.insert(name, c.value(QStringLiteral("label")).toString().left(64));
        }
        scheduleState();
    }
}

void Service::onPeerFingerprint(const QString &fingerprint)
{
    if (fingerprint.isEmpty() || isTrusted(fingerprint) || fingerprint == m_identity.fingerprint())
        return;
    // Someone with Deskflow whose certificate isn't paired: Deskflow already
    // refused it. Ask whether to trust it (not again for 2 minutes if declined)
    if (m_declined.contains(fingerprint) && now() - m_declined.value(fingerprint) < 120000)
        return;
    if (!m_foreignFingerprint.isEmpty() || m_channel.isPairing())
        return;
    m_foreignFingerprint = fingerprint;
    m_pairingState = QStringLiteral("confirm");
    QString label = m_pairingPeer.label;
    if (label.isEmpty()) {
        for (const Peer &p : m_store.peers) {
            if (p.os == QLatin1String("other") && p.id.isEmpty()) {
                label = p.label;
                break;
            }
        }
    }
    if (label.isEmpty())
        label = tr("A computer with Deskflow");
    m_feedback.showPairing({
        {QStringLiteral("label"), label},
        {QStringLiteral("code"), QString()},
        {QStringLiteral("fingerprint"), Identity::formatFingerprint(fingerprint)},
        {QStringLiteral("incoming"), true},
        {QStringLiteral("other"), true},
    });
    scheduleState();
}

void Service::onUnrecognisedClient(const QString &rawName)
{
    const QString name = Store::cleanName(rawName);
    if (name.isEmpty() || m_store.role != QLatin1String("server"))
        return;
    Peer *p = m_store.findByName(name);
    if (!p) {
        // A paired PC from another OS tells us its name only now
        for (Peer &x : m_store.peers) {
            if (x.paired && x.name.isEmpty()) {
                p = &x;
                break;
            }
        }
        if (!p)
            return;
        p->name = name;
        if (p->label.isEmpty() || p->label == tr("Computer %1").arg(p->id.left(4)))
            p->label = name;
    }
    if (!p->paired)
        return;
    save();
    apply();
}

void Service::onDragAtEdge(const QString &dir, double fraction, const QStringList &files)
{
    if (!m_store.enabled || status() != QLatin1String("connected"))
        return;
    const QString to = DeskflowConfig::neighbourAt(effectiveLayout(), Store::computerName(), dir, fraction);
    if (to.isEmpty())
        return;
    const Peer *peer = m_store.findByName(to);
    if (!peer || !peer->paired) {
        m_feedback.toast(tr("Files can't go there"), tr("%1 isn't paired with this computer").arg(labelForName(to)),
                         QStringLiteral("dialog-information"));
        return;
    }
    if (peer->os != QLatin1String("lingmo") || peer->port <= 0) {
        m_feedback.toast(tr("Files can't go there"), tr("Only Lingmo computers receive dragged files"),
                         QStringLiteral("dialog-information"));
        return;
    }
    sendFilesTo(*peer, files);
}

void Service::sendFilesTo(const Peer &peer, const QStringList &files)
{
    m_feedback.toast(tr("Sending to %1").arg(peer.label), tr("%n file(s)", "", int(files.size())),
                     QStringLiteral("document-send"));
    // lingmo-share (LocalSend) when installed; our own paired channel otherwise
    // or if it refuses
    const QString share = QStandardPaths::findExecutable(QStringLiteral("lingmo-share"));
    if (share.isEmpty() || qEnvironmentVariableIsSet("LINGMO_CONTINUITY_NO_SHARE")) {
        m_channel.sendFiles(peer.host, peer.port, peer.id, files);
        return;
    }
    auto *proc = new QProcess(this);
    const Peer target = peer;
    connect(proc, &QProcess::finished, this, [this, proc, target, files](int code, QProcess::ExitStatus status) {
        proc->deleteLater();
        if (status != QProcess::NormalExit || code != 0)
            m_channel.sendFiles(target.host, target.port, target.id, files);
    });
    connect(proc, &QProcess::errorOccurred, this, [this, proc, target, files](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        proc->deleteLater();
        m_channel.sendFiles(target.host, target.port, target.id, files);
    });
    proc->start(share, QStringList{QStringLiteral("send"), QStringLiteral("--to"), peer.host} + files);
}
