/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "dragwatcher.h"

#include <QDebug>
#include <QSocketNotifier>
#include <QUrl>

#include <xcb/xfixes.h>

#include <cstdlib>
#include <cstring>

// The edge strips' thickness: the pointer can't go further than the last pixel
static constexpr int kStrip = 2;
static constexpr uint32_t kXdndVersion = 5;

static bool debugEnabled()
{
    static const bool on = qEnvironmentVariableIsSet("LINGMO_CONTINUITY_DEBUG");
    return on;
}

DragWatcher::DragWatcher(QObject *parent)
    : QObject(parent)
{
    // Idle: a cheap pointer query 8 times a second; faster during a drag
    m_poll.setInterval(120);
    connect(&m_poll, &QTimer::timeout, this, &DragWatcher::poll);
}

DragWatcher::~DragWatcher()
{
    stop();
}

xcb_atom_t DragWatcher::atom(const char *name)
{
    auto cookie = xcb_intern_atom(m_conn, 0, uint16_t(strlen(name)), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(m_conn, cookie, nullptr);
    const xcb_atom_t a = reply ? reply->atom : XCB_ATOM_NONE;
    free(reply);
    return a;
}

bool DragWatcher::start()
{
    if (m_conn)
        return true;
    int screenNumber = 0;
    m_conn = xcb_connect(nullptr, &screenNumber);
    if (!m_conn || xcb_connection_has_error(m_conn)) {
        if (m_conn)
            xcb_disconnect(m_conn);
        m_conn = nullptr;
        return false;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(m_conn));
    for (int i = 0; i < screenNumber && it.rem; ++i)
        xcb_screen_next(&it);
    m_root = it.data->root;

    const xcb_query_extension_reply_t *ext = xcb_get_extension_data(m_conn, &xcb_xfixes_id);
    if (!ext || !ext->present) {
        stop();
        return false;
    }
    m_xfixesEvent = ext->first_event;
    free(xcb_xfixes_query_version_reply(m_conn, xcb_xfixes_query_version(m_conn, 5, 0), nullptr));

    m_xdndSelection = atom("XdndSelection");
    m_uriList = atom("text/uri-list");
    m_property = atom("LINGMO_CONTINUITY_DND");
    m_xdndAware = atom("XdndAware");
    m_xdndEnter = atom("XdndEnter");
    m_xdndPosition = atom("XdndPosition");
    m_xdndStatus = atom("XdndStatus");
    m_xdndLeave = atom("XdndLeave");
    m_xdndDrop = atom("XdndDrop");
    m_xdndFinished = atom("XdndFinished");
    m_xdndActionCopy = atom("XdndActionCopy");

    // An unmapped window that receives the drag source's answers
    m_window = xcb_generate_id(m_conn);
    xcb_create_window(m_conn, XCB_COPY_FROM_PARENT, m_window, m_root, -10, -10, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
    // Each new drag takes XdndSelection: that's how a drag is told from a click
    xcb_xfixes_select_selection_input(m_conn, m_window, m_xdndSelection,
                                      XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER);

    // The drop targets along the edges, mapped only during a drag
    const uint32_t overrideRedirect = 1;
    for (xcb_window_t &strip : m_strips) {
        strip = xcb_generate_id(m_conn);
        xcb_create_window(m_conn, XCB_COPY_FROM_PARENT, strip, m_root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                          XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT, &overrideRedirect);
        xcb_change_property(m_conn, XCB_PROP_MODE_REPLACE, strip, m_xdndAware, XCB_ATOM_ATOM, 32, 1, &kXdndVersion);
    }
    xcb_flush(m_conn);

    m_notifier = new QSocketNotifier(xcb_get_file_descriptor(m_conn), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &DragWatcher::processEvents);
    m_poll.start();
    return true;
}

void DragWatcher::stop()
{
    m_poll.stop();
    delete m_notifier;
    m_notifier = nullptr;
    if (m_conn) {
        for (xcb_window_t strip : m_strips) {
            if (strip)
                xcb_destroy_window(m_conn, strip);
        }
        if (m_window)
            xcb_destroy_window(m_conn, m_window);
        xcb_disconnect(m_conn);
    }
    m_conn = nullptr;
    m_window = 0;
    for (xcb_window_t &strip : m_strips)
        strip = 0;
    m_stripsShown = m_dragActive = m_requested = false;
    m_overStrip = 0;
}

QString DragWatcher::edgeOf(xcb_window_t strip) const
{
    static const QString names[] = {QStringLiteral("left"), QStringLiteral("right"), QStringLiteral("up"),
                                    QStringLiteral("down")};
    for (int i = 0; i < 4; ++i) {
        if (m_strips[i] == strip)
            return names[i];
    }
    return {};
}

void DragWatcher::showStrips(bool show)
{
    if (!m_conn || show == m_stripsShown)
        return;
    m_stripsShown = show;
    if (show) {
        // The root may have grown (monitors plugged): ask, don't trust the setup
        auto *geo = xcb_get_geometry_reply(m_conn, xcb_get_geometry(m_conn, m_root), nullptr);
        if (!geo)
            return;
        const uint32_t w = geo->width, h = geo->height;
        free(geo);
        const uint32_t rects[4][4] = {
            {0, 0, kStrip, h}, {w - kStrip, 0, kStrip, h}, {0, 0, w, kStrip}, {0, h - kStrip, w, kStrip}};
        for (int i = 0; i < 4; ++i) {
            const uint32_t values[] = {rects[i][0], rects[i][1], rects[i][2], rects[i][3], XCB_STACK_MODE_ABOVE};
            xcb_configure_window(m_conn, m_strips[i],
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH
                                     | XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_STACK_MODE,
                                 values);
            xcb_map_window(m_conn, m_strips[i]);
        }
    } else {
        for (xcb_window_t strip : m_strips)
            xcb_unmap_window(m_conn, strip);
    }
    xcb_flush(m_conn);
}

void DragWatcher::poll()
{
    if (!m_conn)
        return;
    if (xcb_connection_has_error(m_conn)) {
        stop();
        return;
    }
    // Replies read by xcb may have queued events behind them, which the
    // socket notifier won't see: drain them here too
    processEvents();
    if (!m_conn)
        return;
    auto *reply = xcb_query_pointer_reply(m_conn, xcb_query_pointer(m_conn, m_root), nullptr);
    if (!reply)
        return;
    const bool down = reply->mask & XCB_BUTTON_MASK_1;
    free(reply);

    if (!down) {
        if (m_dragActive) {
            m_poll.setInterval(120);
            showStrips(false);
        }
        m_dragActive = m_requested = false;
        m_overStrip = 0;
        return;
    }
    // Resting on an edge without moving: the source sends no new positions
    if (m_dragActive && m_overStrip && !m_requested && m_onEdge.isValid() && m_onEdge.elapsed() >= m_dwellMs
        && (!m_accept || m_accept(m_edge, m_fraction)))
        requestFiles(m_positionTime);
}

void DragWatcher::sendStatus(xcb_window_t strip, bool accept)
{
    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = m_source;
    ev.type = m_xdndStatus;
    ev.data.data32[0] = strip;
    // accept; and keep sending positions (empty rectangle)
    ev.data.data32[1] = (accept ? 1u : 0u) | 2u;
    ev.data.data32[4] = accept ? m_xdndActionCopy : XCB_NONE;
    xcb_send_event(m_conn, 0, m_source, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
    xcb_flush(m_conn);
}

void DragWatcher::sendFinished(xcb_window_t strip, bool accepted)
{
    xcb_client_message_event_t ev = {};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = m_source;
    ev.type = m_xdndFinished;
    ev.data.data32[0] = strip;
    ev.data.data32[1] = accepted ? 1u : 0u;
    ev.data.data32[2] = accepted ? m_xdndActionCopy : XCB_NONE;
    xcb_send_event(m_conn, 0, m_source, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char *>(&ev));
    xcb_flush(m_conn);
}

void DragWatcher::handleClientMessage(const xcb_client_message_event_t *e)
{
    const QString edge = edgeOf(e->window);
    if (edge.isEmpty())
        return;
    if (e->type == m_xdndEnter) {
        m_source = e->data.data32[0];
        m_overStrip = e->window;
        m_edge = edge;
        m_onEdge.start();
    } else if (e->type == m_xdndPosition) {
        m_source = e->data.data32[0];
        if (m_overStrip != e->window) {
            m_overStrip = e->window;
            m_onEdge.start();
        }
        m_edge = edge;
        const int x = int(e->data.data32[2] >> 16), y = int(e->data.data32[2] & 0xffff);
        auto *geo = xcb_get_geometry_reply(m_conn, xcb_get_geometry(m_conn, m_root), nullptr);
        const int w = geo ? geo->width : 1, h = geo ? geo->height : 1;
        free(geo);
        m_fraction = (edge == QLatin1String("left") || edge == QLatin1String("right")) ? double(y) / qMax(1, h - 1)
                                                                                        : double(x) / qMax(1, w - 1);
        // The time the source checks our data request against
        m_positionTime = e->data.data32[3];
        const bool accept = !m_accept || m_accept(m_edge, m_fraction);
        sendStatus(e->window, accept);
        if (accept && !m_requested && m_onEdge.elapsed() >= m_dwellMs)
            requestFiles(m_positionTime);
    } else if (e->type == m_xdndLeave) {
        m_overStrip = 0;
    } else if (e->type == m_xdndDrop) {
        m_source = e->data.data32[0];
        const bool accept = !m_accept || m_accept(m_edge, m_fraction);
        // Dropped right on the edge: send now if it wasn't sent yet
        if (accept && !m_requested)
            requestFiles(e->data.data32[2]);
        sendFinished(e->window, accept);
        m_overStrip = 0;
    }
}

void DragWatcher::requestFiles(xcb_timestamp_t time)
{
    m_requested = true;
    m_requestEdge = m_edge;
    m_requestFraction = m_fraction;
    if (debugEnabled())
        qInfo() << "lingmo-continuity: asking the drag source for its files, edge" << m_edge << "time" << time;
    xcb_convert_selection(m_conn, m_window, m_xdndSelection, m_uriList, m_property, time);
    xcb_flush(m_conn);
}

void DragWatcher::processEvents()
{
    if (!m_conn)
        return;
    while (xcb_generic_event_t *event = m_conn ? xcb_poll_for_event(m_conn) : nullptr) {
        const uint8_t type = event->response_type & ~0x80;
        if (type == m_xfixesEvent + XCB_XFIXES_SELECTION_NOTIFY) {
            auto *e = reinterpret_cast<xcb_xfixes_selection_notify_event_t *>(event);
            if (e->selection == m_xdndSelection && e->owner != XCB_NONE) {
                // Taken while the left button is held: a drag started
                auto *p = xcb_query_pointer_reply(m_conn, xcb_query_pointer(m_conn, m_root), nullptr);
                const bool down = p && (p->mask & XCB_BUTTON_MASK_1);
                free(p);
                if (debugEnabled())
                    qInfo() << "lingmo-continuity: drag started, button down:" << down;
                if (down) {
                    m_dragActive = true;
                    m_requested = false;
                    m_overStrip = 0;
                    m_poll.setInterval(40);
                    showStrips(true);
                }
            }
        } else if (type == XCB_CLIENT_MESSAGE) {
            handleClientMessage(reinterpret_cast<xcb_client_message_event_t *>(event));
        } else if (type == XCB_SELECTION_NOTIFY) {
            auto *e = reinterpret_cast<xcb_selection_notify_event_t *>(event);
            if (e->requestor == m_window) {
                QStringList files;
                if (e->property == m_property) {
                    auto *prop = xcb_get_property_reply(
                        m_conn,
                        xcb_get_property(m_conn, 1, m_window, m_property, XCB_GET_PROPERTY_TYPE_ANY, 0, 1 << 20),
                        nullptr);
                    if (prop && prop->format == 8) {
                        const QByteArray data(static_cast<const char *>(xcb_get_property_value(prop)),
                                              xcb_get_property_value_length(prop));
                        files = parseUriList(data);
                    }
                    free(prop);
                }
                if (debugEnabled())
                    qInfo() << "lingmo-continuity: dragged files at" << m_requestEdge << m_requestFraction << files;
                if (!files.isEmpty() && !m_requestEdge.isEmpty())
                    emit dragAtEdge(m_requestEdge, m_requestFraction, files);
            }
        }
        free(event);
    }
}

QStringList DragWatcher::parseUriList(const QByteArray &data)
{
    QStringList out;
    for (const QByteArray &raw : data.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QUrl url = QUrl::fromEncoded(line);
        if (url.isLocalFile())
            out << url.toLocalFile();
    }
    return out;
}
