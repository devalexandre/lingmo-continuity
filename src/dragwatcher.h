/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <functional>

#include <xcb/xcb.h>

class QSocketNotifier;

// Deskflow doesn't carry file drags (its drag-and-drop was removed, and it
// keeps the cursor on the PC while a mouse button is held). This notices a
// file drag (an XDND drag: someone takes XdndSelection while the left button
// is down) and, for as long as it lasts, puts thin XDND drop targets along
// the screen edges. When the drag rests on an edge that leads to another PC,
// it asks the source for its text/uri-list (with the timestamp the source
// gave in XdndPosition, as XDND wants) and reports the local files, so they
// can be sent to the PC on that side. Dropping on the edge works too.
class DragWatcher : public QObject
{
    Q_OBJECT
public:
    explicit DragWatcher(QObject *parent = nullptr);
    ~DragWatcher() override;

    bool start();
    void stop();
    bool isRunning() const { return m_conn != nullptr; }

    // How long the drag must rest on the edge
    void setDwell(int ms) { m_dwellMs = ms; }
    // Whether files dropped on that edge, at that point, can go somewhere
    void setAcceptor(std::function<bool(const QString &, double)> acceptor) { m_accept = std::move(acceptor); }

    // Pure helper: file paths in a text/uri-list
    static QStringList parseUriList(const QByteArray &data);

signals:
    // `dir`: "left", "right", "up", "down"; `fraction`: where along that edge (0..1)
    void dragAtEdge(const QString &dir, double fraction, const QStringList &files);

private:
    void poll();
    void processEvents();
    void showStrips(bool show);
    void handleClientMessage(const xcb_client_message_event_t *e);
    void sendStatus(xcb_window_t strip, bool accept);
    void sendFinished(xcb_window_t strip, bool accepted);
    void requestFiles(xcb_timestamp_t time);
    xcb_atom_t atom(const char *name);
    QString edgeOf(xcb_window_t strip) const;

    xcb_connection_t *m_conn = nullptr;
    xcb_window_t m_root = 0;
    xcb_window_t m_window = 0;
    // left, right, up, down
    xcb_window_t m_strips[4] = {0, 0, 0, 0};
    bool m_stripsShown = false;
    xcb_atom_t m_xdndSelection = 0;
    xcb_atom_t m_uriList = 0;
    xcb_atom_t m_property = 0;
    xcb_atom_t m_xdndAware = 0, m_xdndEnter = 0, m_xdndPosition = 0, m_xdndStatus = 0, m_xdndLeave = 0,
               m_xdndDrop = 0, m_xdndFinished = 0, m_xdndActionCopy = 0;
    uint8_t m_xfixesEvent = 0;
    QSocketNotifier *m_notifier = nullptr;
    QTimer m_poll;

    bool m_dragActive = false;
    bool m_requested = false;
    xcb_window_t m_source = 0;
    xcb_window_t m_overStrip = 0;
    QString m_edge;
    double m_fraction = 0;
    xcb_timestamp_t m_positionTime = XCB_CURRENT_TIME;
    QString m_requestEdge;
    double m_requestFraction = 0;
    QElapsedTimer m_onEdge;
    int m_dwellMs = 350;
    std::function<bool(const QString &, double)> m_accept;
};
