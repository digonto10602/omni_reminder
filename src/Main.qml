import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtMultimedia

// The reminders overlay: a see-through panel the daemon shows and hides. It
// starts hidden; the shortcut, `omni-reminder open`, or a new voice memo
// brings it up.
ApplicationWindow {
    id: win
    width: 960
    height: 680
    minimumWidth: 560
    minimumHeight: 420
    visible: false
    // Hyprland's window rule for omni-reminder matches this title.
    title: "Omni Reminder"
    color: "transparent"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property int selectedId: 0
    property int editingId: 0
    property int confirmId: 0
    // The edit in progress lives here, not in the row: the list is rebuilt
    // whenever the reminders change (also from outside), and a rebuilt row
    // picks the draft up again instead of losing it.
    property string draftText: ""
    property string draftDue: ""
    property real now: Date.now()
    property string playingUrl: ""
    // Letter shortcuts stand aside while the user types in a field.
    // A hidden field (a row's editor after the edit ended) does not count:
    // otherwise Esc and the letter shortcuts would stay disabled for good.
    readonly property bool typing: activeFocusItem !== null && activeFocusItem.visible
                                   && activeFocusItem.cursorPosition !== undefined
    readonly property var items: backend.reminders
    readonly property color panelColor: Qt.rgba(0.055, 0.055, 0.063, 0.86)
    readonly property color errorColor: "#ff6b6b"

    onActiveChanged: backend.overlayActive = active
    onClosing: close => {
        // Closing the window (Super+W) only hides the overlay; the daemon stays.
        close.accepted = false
        backend.hideOverlay()
    }

    function indexOfId(id) {
        for (let i = 0; i < items.length; ++i)
            if (items[i].id === id)
                return i
        return -1
    }
    function selectedItem() {
        const i = indexOfId(selectedId)
        return i >= 0 ? items[i] : null
    }
    function select(index) {
        if (items.length === 0) {
            selectedId = 0
            return
        }
        index = Math.max(0, Math.min(items.length - 1, index))
        selectedId = items[index].id
        list.currentIndex = index
        list.positionViewAtIndex(index, ListView.Contain)
    }
    function move(step) {
        select(Math.max(0, indexOfId(selectedId)) + step)
    }
    function startEdit(id) {
        if (id <= 0)
            return
        confirmId = 0
        selectedId = id
        list.currentIndex = indexOfId(id)
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        draftText = items[list.currentIndex].text
        draftDue = backend.dueEditText(id)
        editingId = id
    }
    function finishEdit() {
        editingId = 0
        list.forceActiveFocus()
    }
    function saveEdit() {
        if (backend.update(editingId, draftText, draftDue))
            finishEdit()
    }
    function togglePlay() {
        const item = selectedItem()
        if (!item || !item.hasClip)
            return
        if (playingUrl === item.clipUrl && player.playbackState === MediaPlayer.PlayingState) {
            player.pause()
            return
        }
        if (playingUrl !== item.clipUrl) {
            playingUrl = item.clipUrl
            player.source = item.clipUrl
        }
        player.play()
    }
    function formatDue(ms) {
        const date = new Date(ms)
        const locale = Qt.locale()
        const sameYear = date.getFullYear() === new Date(now).getFullYear()
        return date.toLocaleDateString(locale, sameYear ? "ddd d MMM" : "ddd d MMM yyyy") + ", "
            + date.toLocaleTimeString(locale, Locale.ShortFormat)
    }
    function relativeDue(ms) {
        translator.language // re-evaluated on a language switch
        const minutes = Math.round((ms - now) / 60000)
        if (minutes <= 0)
            return qsTr("overdue")
        if (minutes < 60)
            return qsTr("in %n min", "", minutes)
        if (minutes < 24 * 60)
            return qsTr("in %n h", "", Math.round(minutes / 60))
        return ""
    }
    function formatSeconds(seconds) {
        const s = Math.max(0, Math.round(seconds))
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }

    Connections {
        target: backend
        function onOverlayVisibleChanged() {
            if (backend.overlayVisible) {
                win.now = Date.now()
                win.show()
                win.raise()
                win.requestActivate()
                if (win.editingId === 0)
                    list.forceActiveFocus()
            } else {
                win.helpVisible = false
                win.confirmId = 0
                win.editingId = 0
                player.stop()
                win.hide()
            }
        }
        // Shown again from scratch, so the compositor focuses it.
        function onOverlayRemapRequested() {
            win.hide()
            remapTimer.start()
        }
        function onEditRequested(id) {
            win.startEdit(id)
        }
        function onRemindersChanged() {
            if (win.indexOfId(win.selectedId) < 0)
                win.select(Math.min(Math.max(0, list.currentIndex), win.items.length - 1))
            else
                list.currentIndex = win.indexOfId(win.selectedId)
            if (win.editingId && win.indexOfId(win.editingId) < 0)
                win.editingId = 0
        }
    }
    Timer {
        id: remapTimer
        interval: 60
        onTriggered: {
            win.show()
            win.requestActivate()
        }
    }
    Timer {
        interval: 30000
        repeat: true
        running: win.visible
        onTriggered: win.now = Date.now()
    }

    MediaPlayer {
        id: player
        audioOutput: AudioOutput {}
        onErrorOccurred: (error, message) => backend.showMessage(qsTr("Cannot play the clip: %1").arg(message), true)
    }

    // Every key, as an application shortcut. Letters only act outside text fields.
    Shortcut { sequences: ["?", "Shift+?"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequences: ["Q"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.hideOverlay() }
    Shortcut { sequences: ["N", "A"]; context: Qt.ApplicationShortcut; enabled: !win.typing && win.editingId === 0; onActivated: newText.forceActiveFocus() }
    Shortcut { sequences: ["Down", "J"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.move(1) }
    Shortcut { sequences: ["Up", "K"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.move(-1) }
    Shortcut { sequences: ["Home"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.select(0) }
    Shortcut { sequences: ["End"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.select(win.items.length - 1) }
    Shortcut {
        sequences: ["Return", "Enter", "E"]
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: {
            if (win.confirmId) {
                backend.remove(win.confirmId)
                win.confirmId = 0
            } else {
                win.startEdit(win.selectedId)
            }
        }
    }
    Shortcut {
        sequences: ["Delete", "D"]
        context: Qt.ApplicationShortcut
        enabled: !win.typing && win.selectedId > 0
        onActivated: {
            if (win.confirmId === win.selectedId) {
                backend.remove(win.confirmId)
                win.confirmId = 0
            } else {
                win.confirmId = win.selectedId
            }
        }
    }
    Shortcut { sequences: ["Space", "P"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.togglePlay() }
    Shortcut { sequences: ["R"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.toggleRecording() }
    Shortcut {
        sequences: ["X"]
        context: Qt.ApplicationShortcut
        enabled: !win.typing && win.selectedId > 0
        onActivated: {
            const item = win.items.find(i => i.id === win.selectedId)
            if (item)
                backend.setDone(item.id, !item.done)
        }
    }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.undo() }
    Shortcut { sequences: [StandardKey.Redo, "Ctrl+Shift+Z"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: backend.redo() }
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: {
            if (win.helpVisible)
                win.helpVisible = false
            else if (win.confirmId)
                win.confirmId = 0
            else if (win.editingId)
                win.finishEdit()
            else if (win.typing) {
                newText.text = ""
                newDue.text = ""
                list.forceActiveFocus()
            } else
                backend.hideOverlay()
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        Rectangle {
            id: panel
            anchors.fill: parent
            color: win.panelColor
            radius: theme.radius
            border.color: Qt.rgba(Qt.color(theme.accent).r, Qt.color(theme.accent).g, Qt.color(theme.accent).b, 0.55)
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 28
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14
                    Label {
                        text: qsTr("Reminders")
                        font.pixelSize: 26
                        font.weight: Font.DemiBold
                        color: "white"
                    }
                    Label {
                        text: qsTr("%n reminders", "", win.items.length)
                        color: "#9a9aa2"
                        font.pixelSize: 14
                        Layout.alignment: Qt.AlignBaseline
                    }
                    Item { Layout.fillWidth: true }
                    // Google sync: quiet when fine, red when it needs attention.
                    Label {
                        visible: backend.syncStatus !== ""
                        text: backend.syncStatus
                        color: backend.syncProblem ? win.errorColor : "#9a9aa2"
                        font.pixelSize: 13
                        elide: Text.ElideRight
                        Layout.maximumWidth: 420
                    }
                    Label {
                        text: qsTr("Press ? for shortcuts")
                        color: "#9a9aa2"
                        font.pixelSize: 13
                    }
                }

                // Errors and notes, said in the window rather than swallowed.
                Rectangle {
                    Layout.fillWidth: true
                    visible: backend.message !== ""
                    implicitHeight: messageRow.implicitHeight + 16
                    radius: Math.min(4, theme.radius)
                    color: backend.messageIsError ? Qt.rgba(1, 0.42, 0.42, 0.14) : Qt.rgba(1, 1, 1, 0.07)
                    border.color: backend.messageIsError ? win.errorColor : "transparent"
                    RowLayout {
                        id: messageRow
                        anchors.fill: parent
                        anchors.margins: 8
                        anchors.leftMargin: 12
                        Label {
                            Layout.fillWidth: true
                            text: backend.message
                            wrapMode: Text.Wrap
                            color: backend.messageIsError ? "#ffd2d2" : "white"
                        }
                        ToolButton {
                            text: "✕"
                            onClicked: backend.clearMessage()
                            focusPolicy: Qt.NoFocus
                        }
                    }
                }

                // While a voice memo records or transcribes, with a way to stop
                // it here if the key release never arrived.
                Rectangle {
                    Layout.fillWidth: true
                    visible: backend.recordingState !== "idle"
                    implicitHeight: 48
                    radius: Math.min(4, theme.radius)
                    color: Qt.rgba(1, 0.25, 0.25, 0.12)
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 8
                        spacing: 10
                        Rectangle {
                            width: 12; height: 12; radius: 6
                            color: backend.recordingState === "recording" ? "#ff4d4d" : theme.accent
                            SequentialAnimation on opacity {
                                running: backend.recordingState !== "idle"
                                loops: Animation.Infinite
                                NumberAnimation { to: 0.25; duration: 600 }
                                NumberAnimation { to: 1; duration: 600 }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            color: "white"
                            text: backend.recordingState === "recording"
                                  ? qsTr("Recording %1 of %2 max. Release the keys or press R to stop.")
                                        .arg(win.formatSeconds(backend.recordingSeconds))
                                        .arg(win.formatSeconds(backend.maxRecordingSeconds))
                                  : qsTr("Transcribing the voice memo…")
                            elide: Text.ElideRight
                        }
                        Button {
                            visible: backend.recordingState === "recording"
                            text: qsTr("Discard")
                            flat: true
                            focusPolicy: Qt.NoFocus
                            onClicked: backend.cancelRecording()
                        }
                        Button {
                            visible: backend.recordingState === "recording"
                            text: qsTr("Stop")
                            focusPolicy: Qt.NoFocus
                            onClicked: backend.stopRecording()
                        }
                    }
                }

                // New reminder: text, and optionally when.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    TextField {
                        id: newText
                        selectedTextColor: theme.accentForeground
                        Layout.fillWidth: true
                        placeholderText: qsTr("New reminder (N)")
                        onAccepted: newDue.text.trim() === "" ? addNew() : newDue.forceActiveFocus()
                        Keys.onTabPressed: newDue.forceActiveFocus()
                        function addNew() {
                            const id = backend.add(newText.text, newDue.text)
                            if (id > 0) {
                                newText.text = ""
                                newDue.text = ""
                                win.selectedId = id
                                win.select(win.indexOfId(id))
                                list.forceActiveFocus()
                            }
                        }
                    }
                    TextField {
                        id: newDue
                        selectedTextColor: theme.accentForeground
                        Layout.preferredWidth: Math.max(200, panel.width * 0.26)
                        placeholderText: qsTr("When? (optional)")
                        onAccepted: newText.addNew()
                        Keys.onBacktabPressed: newText.forceActiveFocus()
                    }
                    Button {
                        text: qsTr("Add")
                        highlighted: true
                        focusPolicy: Qt.NoFocus
                        onClicked: newText.addNew()
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: newDue.text.trim() !== ""
                    readonly property string problem: backend.dueProblem(newDue.text)
                    readonly property real preview: backend.duePreview(newDue.text)
                    text: problem !== "" ? problem : qsTr("Due %1").arg(win.formatDue(preview))
                    color: problem !== "" ? win.errorColor : theme.accent
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

                ListView {
                    id: list
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 6
                    model: win.items
                    focus: true
                    keyNavigationEnabled: false
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    delegate: ReminderRow {
                        width: list.width
                        item: modelData
                        selected: modelData.id === win.selectedId
                        editing: modelData.id === win.editingId
                        confirming: modelData.id === win.confirmId
                        playing: modelData.clipUrl === win.playingUrl && player.playbackState === MediaPlayer.PlayingState
                        dueLabel: modelData.hasDue ? win.formatDue(modelData.dueMs) : ""
                        relativeLabel: modelData.hasDue && !modelData.done ? win.relativeDue(modelData.dueMs) : ""
                        overdue: modelData.hasDue && !modelData.done && modelData.dueMs <= win.now
                        clipLabel: win.formatSeconds(modelData.clipSeconds)
                        formatDue: win.formatDue
                        onPicked: win.select(index)
                        onEditPicked: win.startEdit(modelData.id)
                        onPlayPicked: {
                            win.select(index)
                            win.togglePlay()
                        }
                        draftText: win.draftText
                        draftDue: win.draftDue
                        onDraftChanged: (text, due) => {
                            win.draftText = text
                            win.draftDue = due
                        }
                        // Main's functions do the saving: the update rebuilds
                        // the list, and this row with it.
                        onSaved: Qt.callLater(win.saveEdit)
                        onCancelled: Qt.callLater(win.finishEdit)
                    }

                    Label {
                        anchors.centerIn: parent
                        width: parent.width * 0.7
                        visible: win.items.length === 0
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        color: "#9a9aa2"
                        font.pixelSize: 16
                        text: qsTr("No reminders yet. Press N to write one, or hold Ctrl+Shift+Alt+R and speak.")
                    }
                }

                Label {
                    Layout.fillWidth: true
                    color: "#7c7c85"
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    text: win.confirmId
                          ? qsTr("Remove this reminder? Enter or Delete removes it, Esc keeps it.")
                          : qsTr("N new · Enter edit · X done · Delete remove · Space play · R record · Ctrl+Z undo · Esc close")
                }
            }
        }

        // The shortcut sheet.
        Rectangle {
            anchors.fill: parent
            visible: win.helpVisible
            color: Qt.rgba(0, 0, 0, 0.55)
            radius: theme.radius
            MouseArea { anchors.fill: parent; onClicked: win.helpVisible = false }
            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 60, 560)
                height: helpGrid.implicitHeight + 56
                radius: theme.radius
                color: "#17171a"
                border.color: theme.accent
                GridLayout {
                    id: helpGrid
                    anchors.fill: parent
                    anchors.margins: 28
                    columns: 2
                    columnSpacing: 24
                    rowSpacing: 8
                    Repeater {
                        model: [
                            ["Ctrl+Shift+Alt+R", qsTr("Tap: show or hide reminders. Hold: record a voice memo")],
                            ["N", qsTr("New reminder")],
                            ["↑ ↓ / K J", qsTr("Move the selection")],
                            ["Enter / E", qsTr("Edit the selected reminder")],
                            ["X", qsTr("Mark it done, or not done again")],
                            ["Delete / D", qsTr("Remove it (asks first)")],
                            ["Space / P", qsTr("Play or pause its voice clip")],
                            ["R", qsTr("Start or stop a voice memo")],
                            ["Ctrl+Z / Ctrl+Shift+Z", qsTr("Undo / redo")],
                            ["Esc / Q", qsTr("Close the overlay")],
                            ["?", qsTr("Show or hide this list")]
                        ]
                        delegate: RowLayout {
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                            spacing: 18
                            Label {
                                Layout.preferredWidth: 170
                                text: modelData[0]
                                color: theme.accent
                                font.family: "monospace"
                            }
                            Label {
                                Layout.fillWidth: true
                                text: modelData[1]
                                color: "white"
                                wrapMode: Text.Wrap
                            }
                        }
                    }
                }
            }
        }
    }
}
