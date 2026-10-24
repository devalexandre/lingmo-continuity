/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import LingmoUI.CompatibleModule 3.0 as LingmoUI

// "Agora em: Notebook": a glass pill next to the cursor when it arrives on
// this PC. Doesn't take focus or clicks.
Window {
    id: control

    flags: Qt.ToolTip | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.WindowDoesNotAcceptFocus | Qt.WindowTransparentForInput
    color: "transparent"
    visible: false
    width: Math.ceil(row.implicitWidth + LingmoUI.Units.largeSpacing * 2 + 8)
    height: 58

    property string title
    property string text
    property string iconName: "computer"

    function show(title, text, icon, cx, cy, screen) {
        control.title = title
        control.text = text
        control.iconName = icon || "computer"
        if (screen)
            control.screen = screen
        const sx = screen ? screen.virtualX : 0, sy = screen ? screen.virtualY : 0
        const sw = screen ? screen.width : 1920, sh = screen ? screen.height : 1080
        const margin = 16
        // Below-right of the cursor; flipped when that leaves the screen
        let x = cx + 18, y = cy + 22
        if (x + width > sx + sw - margin)
            x = cx - width - 18
        if (y + height > sy + sh - margin)
            y = cy - height - 22
        control.x = Math.max(sx + margin, Math.min(x, sx + sw - width - margin))
        control.y = Math.max(sy + margin, Math.min(y, sy + sh - height - margin))
        hideTimer.restart()
        if (!visible) {
            content.opacity = 0
            content.scale = 0.88
            visible = true
        }
        appear.restart()
    }

    LingmoUI.WindowBlur {
        view: control
        geometry: Qt.rect(control.x, control.y, control.width, control.height)
        windowRadius: background.radius
        enabled: true
    }

    Timer {
        id: hideTimer
        interval: 1700
        onTriggered: disappear.restart()
    }

    ParallelAnimation {
        id: appear
        NumberAnimation { target: content; property: "opacity"; to: 1; duration: 160; easing.type: Easing.OutCubic }
        NumberAnimation { target: content; property: "scale"; to: 1; duration: 220; easing.type: Easing.OutBack }
    }

    SequentialAnimation {
        id: disappear
        NumberAnimation { target: content; property: "opacity"; to: 0; duration: 220; easing.type: Easing.InCubic }
        ScriptAction { script: control.visible = false }
    }

    Item {
        id: content
        anchors.fill: parent
        transformOrigin: Item.Left

        Rectangle {
            id: background
            anchors.fill: parent
            radius: height / 2
            color: LingmoUI.Theme.backgroundColor
            opacity: LingmoUI.Theme.darkMode ? 0.72 : 0.80
            border.width: 1
            border.color: LingmoUI.Theme.darkMode ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.08)
        }

        RowLayout {
            id: row
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 9
            spacing: LingmoUI.Units.largeSpacing

            Rectangle {
                Layout.preferredWidth: 40
                Layout.preferredHeight: 40
                radius: 20
                color: LingmoUI.Theme.highlightColor

                LingmoUI.IconItem {
                    anchors.centerIn: parent
                    width: 22
                    height: 22
                    source: control.iconName
                    color: "white"
                }
            }

            ColumnLayout {
                spacing: 0
                Layout.rightMargin: LingmoUI.Units.largeSpacing

                Label {
                    text: control.title
                    color: LingmoUI.Theme.secondaryTextColor
                    font.pointSize: LingmoUI.Theme.fontSize * 0.85
                }

                Label {
                    text: control.text
                    font.bold: true
                    font.pointSize: LingmoUI.Theme.fontSize * 1.1
                    elide: Text.ElideRight
                    Layout.maximumWidth: 280
                }
            }
        }
    }
}
