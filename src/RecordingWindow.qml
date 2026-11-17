import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The small "recording" badge shown while the shortcut is held, and while
// the memo is being transcribed. Hyprland's rule for this title floats it at
// the top of the screen without taking focus from the app underneath.
Window {
    id: badge
    width: 400
    height: 84
    // Hyprland's window rule for the recording badge matches this title.
    title: "Omni Reminder Recording"
    color: "transparent"
    visible: backend.recordingState !== "idle"
    flags: Qt.Window | Qt.WindowDoesNotAcceptFocus

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    function formatSeconds(seconds) {
        const s = Math.max(0, Math.round(seconds))
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }

    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        Rectangle {
            anchors.fill: parent
            radius: theme.radius
            color: Qt.rgba(0.055, 0.055, 0.063, 0.9)
            border.color: backend.recordingState === "recording" ? "#ff4d4d" : theme.accent
            border.width: 1

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 12
                spacing: 14
                Rectangle {
                    width: 16; height: 16; radius: 8
                    color: backend.recordingState === "recording" ? "#ff4d4d" : theme.accent
                    SequentialAnimation on opacity {
                        running: badge.visible
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.25; duration: 600 }
                        NumberAnimation { to: 1; duration: 600 }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Label {
                        Layout.fillWidth: true
                        text: backend.recordingState === "recording"
                              ? qsTr("Recording %1").arg(badge.formatSeconds(backend.recordingSeconds))
                              : qsTr("Transcribing…")
                        color: "white"
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: backend.recordingState === "recording"
                              ? qsTr("Release to save · %1 max").arg(badge.formatSeconds(backend.maxRecordingSeconds))
                              : qsTr("The new reminder opens when it is done")
                        color: "#9a9aa2"
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
                ToolButton {
                    visible: backend.recordingState === "recording"
                    text: qsTr("Discard")
                    font.pixelSize: 12
                    onClicked: backend.cancelRecording()
                }
                ToolButton {
                    visible: backend.recordingState === "recording"
                    text: qsTr("Stop")
                    font.pixelSize: 12
                    onClicked: backend.stopRecording()
                }
            }
        }
    }
}
