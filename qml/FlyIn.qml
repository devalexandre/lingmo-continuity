/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import LingmoUI.CompatibleModule 3.0 as LingmoUI

// "Recebido de Notebook": files dragged in from another PC fly out of the
// cursor into a glass card with Open / Show in folder
Window {
    id: control

    flags: Qt.ToolTip | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"
    visible: false
    width: 340
    height: layout.implicitHeight + LingmoUI.Units.largeSpacing * 2

    property string from
    property var files: []
    readonly property int shown: Math.min(files.length, 3)

    // Where the cursor is, relative to the card: it flies out from there
    property point origin: Qt.point(0, 0)

    function show(from, files, cx, cy, screen) {
        control.from = from
        control.files = files
        if (screen)
            control.screen = screen
        const sx = screen ? screen.virtualX : 0, sy = screen ? screen.virtualY : 0
        const sw = screen ? screen.width : 1920, sh = screen ? screen.height : 1080
        const margin = 16
        let x = cx + 24, y = cy + 24
        if (x + width > sx + sw - margin)
            x = cx - width - 24
        if (y + height > sy + sh - margin)
            y = cy - height - 24
        control.x = Math.max(sx + margin, Math.min(x, sx + sw - width - margin))
        control.y = Math.max(sy + margin, Math.min(y, sy + sh - height - margin))
        origin = Qt.point(cx - control.x, cy - control.y)
        hideTimer.restart()
        card.opacity = 0
        card.scale = 0.3
        card.x = origin.x - card.width / 2
        card.y = origin.y - card.height / 2
        visible = true
        flyIn.restart()
    }

    LingmoUI.WindowBlur {
        view: control
        geometry: Qt.rect(control.x + card.x, control.y + card.y, card.width * card.scale, card.height * card.scale)
        windowRadius: LingmoUI.Theme.hugeRadius
        enabled: card.opacity > 0.5
    }

    Timer {
        id: hideTimer
        interval: 7000
        onTriggered: if (!hover.hovered) flyOut.restart()
    }

    ParallelAnimation {
        id: flyIn
        NumberAnimation { target: card; property: "x"; to: 0; duration: 380; easing.type: Easing.OutCubic }
        NumberAnimation { target: card; property: "y"; to: 0; duration: 380; easing.type: Easing.OutCubic }
        NumberAnimation { target: card; property: "scale"; to: 1; duration: 420; easing.type: Easing.OutBack }
        NumberAnimation { target: card; property: "opacity"; to: 1; duration: 200 }
    }

    SequentialAnimation {
        id: flyOut
        NumberAnimation { target: card; property: "opacity"; to: 0; duration: 220 }
        ScriptAction { script: control.visible = false }
    }

    Item {
        id: card
        width: control.width
        height: control.height
        transformOrigin: Item.TopLeft

        HoverHandler {
            id: hover
            onHoveredChanged: if (!hovered) hideTimer.restart()
        }

        Rectangle {
            anchors.fill: parent
            radius: LingmoUI.Theme.hugeRadius
            color: LingmoUI.Theme.backgroundColor
            opacity: LingmoUI.Theme.darkMode ? 0.78 : 0.86
            border.width: 1
            border.color: LingmoUI.Theme.darkMode ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.08)
        }

        ColumnLayout {
            id: layout
            anchors.fill: parent
            anchors.margins: LingmoUI.Units.largeSpacing
            spacing: LingmoUI.Units.smallSpacing

            RowLayout {
                spacing: LingmoUI.Units.largeSpacing
                Layout.fillWidth: true

                Rectangle {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 40
                    radius: 12
                    color: LingmoUI.Theme.highlightColor

                    LingmoUI.IconItem {
                        anchors.centerIn: parent
                        width: 22
                        height: 22
                        source: "document-save"
                        color: "white"
                    }
                }

                ColumnLayout {
                    spacing: 0
                    Layout.fillWidth: true

                    Label {
                        text: qsTr("Received from %1").arg(control.from)
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }

                    Label {
                        text: qsTr("%n file(s) in Downloads", "", control.files.length)
                        color: LingmoUI.Theme.secondaryTextColor
                        font.pointSize: LingmoUI.Theme.fontSize * 0.9
                        Layout.fillWidth: true
                    }
                }
            }

            Repeater {
                model: control.shown

                RowLayout {
                    required property int index
                    spacing: LingmoUI.Units.smallSpacing
                    Layout.fillWidth: true
                    Layout.leftMargin: 52

                    LingmoUI.IconItem {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        source: "text-x-generic"
                    }

                    Label {
                        text: control.files[parent.index].name
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                }
            }

            Label {
                visible: control.files.length > control.shown
                text: qsTr("and %n more", "", control.files.length - control.shown)
                color: LingmoUI.Theme.secondaryTextColor
                Layout.leftMargin: 52
            }

            RowLayout {
                spacing: LingmoUI.Units.smallSpacing
                Layout.fillWidth: true
                Layout.topMargin: LingmoUI.Units.smallSpacing

                Item { Layout.fillWidth: true }

                LingmoUI.ActionButton {
                    text: qsTr("Show in Folder")
                    flat: true
                    onClicked: {
                        if (control.files.length > 0)
                            feedback.openFolder(control.files[0].path)
                        flyOut.restart()
                    }
                }

                LingmoUI.ActionButton {
                    text: qsTr("Open")
                    primary: true
                    visible: control.files.length === 1
                    onClicked: {
                        feedback.openFile(control.files[0].path)
                        flyOut.restart()
                    }
                }
            }
        }
    }
}
