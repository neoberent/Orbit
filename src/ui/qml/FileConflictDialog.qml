import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: ""
    modal: true
    closePolicy: Popup.CloseOnEscape
    standardButtons: Dialog.NoButton
    width: 480
    anchors.centerIn: parent
    padding: 0

    property var taskId: 0
    property var info: ({})
    property string taskName: ""
    property var queue: []
    property bool rememberChoice: false
    property string errorText: ""

    readonly property int waitingCount: queue.length

    function enqueue(id) {
        if ((visible && id === taskId) || queue.indexOf(id) >= 0) return
        queue = queue.concat([id])
        if (!visible) showNext()
    }

    function showNext() {
        while (queue.length > 0) {
            const id = queue[0]
            queue = queue.slice(1)
            const details = downloadModel.fileConflictInfo(id)
            if (details.path === undefined) continue
            taskId = id
            info = details
            taskName = downloadModel.get(id).name || details.name
            errorText = ""
            open()
            return
        }
        close()
    }

    function decide(action) {
        if (rememberChoice) appController.fileConflictAction = action === "replace" ? "overwrite" : "rename"
        const error = downloadModel.resolveFileConflict(taskId, action)
        if (error.length > 0) { errorText = error; return }
        if (rememberChoice) {
            for (const id of queue) downloadModel.resolveFileConflict(id, action)
            queue = []
        }
        advance()
    }

    function cancelDownload() {
        downloadModel.cancelItem(taskId)
        advance()
    }

    function later() {
        advance()
    }

    function advance() {
        rememberChoice = false
        if (queue.length > 0) showNext()
        else close()
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast } }

    Overlay.modal: Rectangle { color: Theme.withAlpha("#000000", 0.6) }
    background: DialogSurface {}

    component FileLine: RowLayout {
        property string heading
        property string detail
        property string iconName
        property color tint: Theme.textSecondary
        spacing: 12
        Rectangle {
            Layout.preferredWidth: 30
            Layout.preferredHeight: 30
            radius: Theme.radiusMedium
            color: Theme.withAlpha(tint, 0.1)
            border.width: 1
            border.color: Theme.withAlpha(tint, 0.2)
            Icon { anchors.centerIn: parent; name: iconName; iconSize: 13; color: tint }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Text { text: heading; color: Theme.textPrimary; font.pixelSize: Theme.fontBody; font.weight: Font.Medium }
            Text {
                Layout.fillWidth: true
                text: detail
                color: Theme.textTertiary
                font.pixelSize: Theme.fontCaption
                font.features: { "tnum": 1 }
                elide: Text.ElideRight
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            Layout.bottomMargin: 14
            spacing: 12
            Rectangle {
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: Theme.radiusMedium
                color: Theme.withAlpha(Theme.warning, 0.12)
                border.width: 1
                border.color: Theme.withAlpha(Theme.warning, 0.24)
                Icon { anchors.centerIn: parent; name: "alert"; iconSize: 15; color: Theme.warning }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                Text {
                    Layout.fillWidth: true
                    text: "“" + (root.info.name || "") + "” already exists"
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontHeading + 1
                    font.weight: Font.DemiBold
                    elide: Text.ElideMiddle
                }
                Text {
                    Layout.fillWidth: true
                    text: "in " + (root.info.folder || "") + (root.waitingCount > 0 ? "  ·  " + root.waitingCount + " more waiting" : "")
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideMiddle
                }
            }
            IconButton { iconName: "close"; tooltip: "Decide later"; onClicked: root.later() }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            implicitHeight: lines.implicitHeight + 24
            radius: Theme.radiusMedium
            color: Theme.surfaceInset
            border.width: 1
            border.color: Theme.border

            ColumnLayout {
                id: lines
                anchors.fill: parent
                anchors.margins: 12
                spacing: 12
                FileLine {
                    Layout.fillWidth: true
                    heading: "File on disk"
                    detail: (root.info.existingSizeText || "") + (root.info.existingModifiedText ? "  ·  modified " + root.info.existingModifiedText : "")
                    iconName: "folder"
                }
                FileLine {
                    Layout.fillWidth: true
                    heading: "New download"
                    detail: (root.info.incomingSizeText || "") + "  ·  " + Theme.sourceHost(downloadModel.get(root.taskId).source || "", false)
                    iconName: "arrowDown"
                    tint: Theme.accent
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.topMargin: 10
            spacing: 8

            Item {
                Layout.preferredWidth: rememberRow.implicitWidth
                Layout.preferredHeight: 24
                Accessible.role: Accessible.CheckBox
                Accessible.name: "Do this for every file from now on"
                Accessible.checked: root.rememberChoice
                Accessible.onToggleAction: root.rememberChoice = !root.rememberChoice

                RowLayout {
                    id: rememberRow
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Rectangle {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        radius: 4
                        color: root.rememberChoice ? Theme.accent : "transparent"
                        border.width: 1.4
                        border.color: root.rememberChoice ? Theme.accent : Theme.withAlpha(Theme.textPrimary, 0.22)
                        Icon { anchors.centerIn: parent; visible: root.rememberChoice; name: "check"; iconSize: 10; color: "#ffffff" }
                    }
                    Text { text: "Do this for every file from now on"; color: Theme.textSecondary; font.pixelSize: Theme.fontCaption }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.rememberChoice = !root.rememberChoice
                }
            }
            Item { Layout.fillWidth: true }
            ModernButton {
                text: "Show File"
                variant: "ghost"
                iconName: "folder"
                onClicked: appController.revealFile(root.info.path || "")
            }
        }

        Text {
            visible: root.errorText.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.topMargin: 6
            text: root.errorText
            color: Theme.danger
            font.pixelSize: Theme.fontCaption
            wrapMode: Text.Wrap
        }

        Item { Layout.preferredHeight: 16 }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            Layout.leftMargin: 20
            spacing: 8
            ModernButton { text: "Cancel Download"; variant: "danger"; onClicked: root.cancelDownload() }
            Item { Layout.fillWidth: true }
            ModernButton {
                text: "Replace"
                variant: "secondary"
                onClicked: root.decide("replace")
                OrbitToolTip { visible: parent.hovered; text: "The file on disk is overwritten" }
            }
            ModernButton {
                id: keepBothButton
                text: "Keep Both"
                variant: "primary"
                onClicked: root.decide("keepBoth")
                OrbitToolTip { visible: parent.hovered; text: "Saves the new file as “" + (root.info.keepBothName || "") + "”" }
            }
        }
    }

    onOpened: keepBothButton.forceActiveFocus()
    onRejected: advance()
}
