/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import LingmoUI.CompatibleModule 3.0 as LingmoUI

// Pairing: shown on both PCs. Lingmo PCs show the same 6-digit code, made
// from both certificates; a PC with Deskflow on another OS shows its
// certificate fingerprint, to compare with what its Deskflow shows.
LingmoUI.Window {
    id: control

    width: 460
    height: Math.max(minimumHeight, content.implicitHeight + header.height + LingmoUI.Units.largeSpacing * 3)
    minimumWidth: 460
    maximumWidth: 460
    minimumHeight: 280
    minimizeButtonVisible: false
    visible: false
    flags: Qt.Dialog | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    title: qsTr("Shared Keyboard and Mouse")

    background.opacity: LingmoUI.Theme.darkMode ? 0.88 : 0.97
    header.height: LingmoUI.Units.windowHeaderHeight

    property var info: ({})
    // "confirm", "waiting" (we said yes, the other side hasn't), "done"
    property string phase: "confirm"
    property bool ok: false
    property string message: ""

    readonly property bool other: !!info.other
    readonly property string label: info.label || ""

    function start(i) {
        info = i
        phase = "confirm"
        message = ""
        closeTimer.stop()
        const s = Qt.application.screens[0]
        if (s) {
            x = s.virtualX + (s.width - width) / 2
            y = s.virtualY + (s.height - height) / 3
        }
        show()
        raise()
        requestActivate()
    }

    function done(success, text) {
        if (!visible)
            return
        ok = success
        message = text
        phase = "done"
        closeTimer.restart()
    }

    onClosing: (close) => {
        if (phase !== "done" && !info.preview)
            feedback.answerPairing(false)
    }

    LingmoUI.WindowBlur {
        view: control
        geometry: Qt.rect(control.x, control.y, control.width, control.height)
        windowRadius: control.windowRadius
        enabled: true
    }

    Timer {
        id: closeTimer
        interval: 2600
        onTriggered: control.hide()
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        anchors.margins: LingmoUI.Units.largeSpacing * 1.5
        anchors.topMargin: LingmoUI.Units.smallSpacing
        spacing: LingmoUI.Units.largeSpacing

        RowLayout {
            spacing: LingmoUI.Units.largeSpacing
            Layout.fillWidth: true

            Rectangle {
                Layout.preferredWidth: 52
                Layout.preferredHeight: 52
                Layout.alignment: Qt.AlignTop
                radius: 16
                color: control.phase === "done" && !control.ok ? LingmoUI.Theme.redColor : LingmoUI.Theme.highlightColor

                LingmoUI.IconItem {
                    anchors.centerIn: parent
                    width: 28
                    height: 28
                    source: control.phase === "done" ? (control.ok ? "emblem-ok-symbolic" : "dialog-error-symbolic")
                                                     : (control.other ? "computer" : "computer-laptop")
                    color: "white"
                }
            }

            ColumnLayout {
                spacing: LingmoUI.Units.smallSpacing
                Layout.fillWidth: true

                Label {
                    text: control.phase === "done" ? (control.ok ? qsTr("Paired") : qsTr("Not paired"))
                          : control.other ? qsTr("Trust %1?").arg(control.label)
                          : qsTr("Pair with %1?").arg(control.label)
                    font.bold: true
                    font.pointSize: LingmoUI.Theme.fontSize * 1.25
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Label {
                    visible: control.phase !== "done"
                    text: control.other
                          ? qsTr("It wants to share a keyboard and mouse with this computer through Deskflow. Trust it only if the fingerprint below is the one its Deskflow shows (Deskflow › Settings › Security).")
                          : qsTr("Both computers show a code. Pair only if it's the same on %1: then the connection is really between the two of them.").arg(control.label)
                    color: LingmoUI.Theme.secondaryTextColor
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }

                Label {
                    visible: control.phase === "done"
                    text: control.message
                    color: LingmoUI.Theme.secondaryTextColor
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }

        // The shared code
        Rectangle {
            visible: !control.other && control.phase !== "done"
            Layout.fillWidth: true
            Layout.preferredHeight: 84
            radius: LingmoUI.Theme.bigRadius
            color: LingmoUI.Theme.secondBackgroundColor

            Label {
                anchors.centerIn: parent
                text: control.info.code || ""
                font.family: "monospace"
                font.pointSize: 30
                font.bold: true
                font.letterSpacing: 4
                color: LingmoUI.Theme.highlightColor
            }
        }

        // The certificate fingerprint
        ColumnLayout {
            visible: control.phase !== "done"
            spacing: 2
            Layout.fillWidth: true

            Label {
                text: control.other ? qsTr("Its fingerprint (SHA-256)") : qsTr("Its certificate (SHA-256)")
                color: LingmoUI.Theme.secondaryTextColor
                font.pointSize: LingmoUI.Theme.fontSize * 0.85
            }

            Label {
                text: control.info.fingerprint || ""
                font.family: "monospace"
                font.pointSize: control.other ? LingmoUI.Theme.fontSize : LingmoUI.Theme.fontSize * 0.8
                color: control.other ? LingmoUI.Theme.textColor : LingmoUI.Theme.secondaryTextColor
                wrapMode: Text.WrapAnywhere
                Layout.fillWidth: true
            }
        }

        RowLayout {
            visible: control.phase === "waiting"
            spacing: LingmoUI.Units.largeSpacing
            Layout.fillWidth: true

            LingmoUI.Spinner {
                running: visible
                size: 20
            }

            Label {
                text: qsTr("Waiting for %1 to confirm…").arg(control.label)
                color: LingmoUI.Theme.secondaryTextColor
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
        }

        Item { Layout.fillHeight: true }

        RowLayout {
            spacing: LingmoUI.Units.largeSpacing
            Layout.fillWidth: true

            LingmoUI.ActionButton {
                text: control.phase === "done" ? qsTr("Close") : qsTr("Cancel")
                Layout.fillWidth: true
                onClicked: {
                    if (control.phase !== "done" && !control.info.preview)
                        feedback.answerPairing(false)
                    control.hide()
                }
            }

            LingmoUI.ActionButton {
                visible: control.phase === "confirm"
                text: control.other ? qsTr("Trust") : qsTr("Pair")
                primary: true
                Layout.fillWidth: true
                onClicked: {
                    control.phase = "waiting"
                    if (!control.info.preview)
                        feedback.answerPairing(true)
                }
            }
        }
    }
}
