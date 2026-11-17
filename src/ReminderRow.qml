import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One reminder in the overlay list: its text, due time and voice clip, or
// the editor for them.
Item {
    id: row
    property var item
    property bool selected: false
    property bool editing: false
    property bool confirming: false
    property bool playing: false
    property bool overdue: false
    property string dueLabel: ""
    property string relativeLabel: ""
    property string clipLabel: ""
    // Formats a due time (milliseconds) the way the list shows it.
    property var formatDue: ms => new Date(ms).toLocaleString()

    signal picked()
    signal editPicked()
    signal playPicked()
    property string draftText: ""
    property string draftDue: ""
    signal draftChanged(string text, string due)
    signal saved()
    signal cancelled()

    implicitHeight: content.implicitHeight + 24

    property bool loadingDraft: false
    function openEditor(fresh) {
        // A deferred call can arrive after the edit was saved or cancelled.
        if (!row.editing)
            return
        loadingDraft = true
        editText.text = row.draftText
        editDue.text = row.draftDue
        loadingDraft = false
        editText.forceActiveFocus()
        if (fresh)
            editText.selectAll()
        else
            editText.cursorPosition = editText.length
    }
    onEditingChanged: if (editing) openEditor(true)
    // A row rebuilt mid-edit is not in the window yet when completed; focus it after.
    Component.onCompleted: if (editing) Qt.callLater(openEditor, false)

    Rectangle {
        anchors.fill: parent
        radius: Math.min(4, theme.radius)
        color: row.confirming ? Qt.rgba(1, 0.42, 0.42, 0.16)
             : row.selected ? Qt.rgba(1, 1, 1, 0.09)
             : hover.hovered ? Qt.rgba(1, 1, 1, 0.05) : "transparent"
        Rectangle {
            width: 3
            height: parent.height
            color: theme.accent
            visible: row.selected
        }
    }

    HoverHandler { id: hover }
    TapHandler {
        onTapped: row.picked()
        onDoubleTapped: row.editPicked()
    }

    ColumnLayout {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 18
        anchors.rightMargin: 14
        spacing: 6

        Label {
            Layout.fillWidth: true
            visible: !row.editing
            text: row.item.text
            wrapMode: Text.Wrap
            color: "white"
            font.pixelSize: 17
        }

        // The editor: Enter saves, Shift+Enter starts a new line, Esc cancels.
        ColumnLayout {
            Layout.fillWidth: true
            visible: row.editing
            spacing: 8
            TextArea {
                id: editText
                selectedTextColor: theme.accentForeground
                Layout.fillWidth: true
                wrapMode: TextEdit.Wrap
                placeholderText: qsTr("Reminder")
                font.pixelSize: 17
                Keys.onReturnPressed: event => {
                    if (event.modifiers & Qt.ShiftModifier)
                        event.accepted = false
                    else
                        row.saved()
                }
                Keys.onEnterPressed: event => row.saved()
                Keys.onTabPressed: editDue.forceActiveFocus()
                onTextChanged: if (row.editing && !row.loadingDraft) row.draftChanged(editText.text, editDue.text)
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 12
                TextField {
                    id: editDue
                    selectedTextColor: theme.accentForeground
                    Layout.preferredWidth: 260
                    placeholderText: qsTr("When? (empty for none)")
                    onAccepted: row.saved()
                    Keys.onBacktabPressed: editText.forceActiveFocus()
                    onTextChanged: if (row.editing && !row.loadingDraft) row.draftChanged(editText.text, editDue.text)
                }
                Label {
                    Layout.fillWidth: true
                    readonly property string problem: backend.dueProblem(editDue.text)
                    readonly property real preview: backend.duePreview(editDue.text)
                    text: problem !== "" ? problem
                        : preview > 0 ? qsTr("Due %1").arg(row.formatDue(preview))
                        : qsTr("No due time")
                    color: problem !== "" ? "#ff6b6b" : "#9a9aa2"
                    wrapMode: Text.Wrap
                    font.pixelSize: 13
                }
                Button {
                    text: qsTr("Save")
                    highlighted: true
                    focusPolicy: Qt.NoFocus
                    onClicked: row.saved()
                }
                Button {
                    text: qsTr("Cancel")
                    flat: true
                    focusPolicy: Qt.NoFocus
                    onClicked: row.cancelled()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 14
            visible: row.dueLabel !== "" || row.item.hasClip || row.item.clipExpired

            Label {
                visible: row.dueLabel !== ""
                text: row.relativeLabel !== "" ? row.dueLabel + " · " + row.relativeLabel : row.dueLabel
                color: row.overdue ? "#ff8a8a" : theme.accent
                font.pixelSize: 13
            }
            ToolButton {
                visible: row.item.hasClip
                focusPolicy: Qt.NoFocus
                implicitHeight: 30
                padding: 4
                text: (row.playing ? "❚❚  " : "▶  ") + row.clipLabel
                font.pixelSize: 13
                onClicked: row.playPicked()
            }
            Label {
                visible: row.item.clipExpired && !row.item.hasClip
                text: qsTr("clip expired")
                color: "#7c7c85"
                font.pixelSize: 13
                font.italic: true
            }
            Item { Layout.fillWidth: true }
        }
    }
}
