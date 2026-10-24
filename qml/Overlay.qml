/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick

// The daemon's windows; Feedback (C++) calls these functions
QtObject {
    id: root

    property Toast toast: Toast {}
    property FlyIn flyIn: FlyIn {}
    property PairDialog pairDialog: PairDialog {}

    // The screen under a point (virtual desktop coordinates)
    function screenAt(x, y) {
        const screens = Qt.application.screens
        for (let i = 0; i < screens.length; ++i) {
            const s = screens[i]
            if (x >= s.virtualX && x < s.virtualX + s.width && y >= s.virtualY && y < s.virtualY + s.height)
                return s
        }
        return screens.length > 0 ? screens[0] : null
    }

    function showToast(title, text, icon, x, y) {
        toast.show(title, text, icon, x, y, screenAt(x, y))
    }

    function showFlyIn(from, files, x, y) {
        flyIn.show(from, files, x, y, screenAt(x, y))
    }

    function showPairing(info) {
        pairDialog.start(info)
    }

    function pairingDone(ok, message) {
        pairDialog.done(ok, message)
    }
}
