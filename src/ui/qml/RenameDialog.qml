import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    title: ""
    modal: true
    standardButtons: Dialog.NoButton
    width: 440
    anchors.centerIn: parent
    padding: 0

    property var taskId: 0
    property string originalName: ""
    property string errorText: ""

    function openFor(item) {
        taskId = item.taskId
        originalName = item.name
        nameField.text = item.name
        errorText = ""
        open()
    }

    function submit() {
        const name = nameField.text.trim()
        if (name === originalName) { close(); return }
        const error = downloadModel.renameItem(taskId, name)
        if (error.length > 0) { errorText = error; return }
        close()
    }

    onOpened: {
        nameField.field.forceActiveFocus()
        const dot = originalName.lastIndexOf(".")
        nameField.field.select(0, dot > 0 && dot > originalName.length - 7 ? dot : originalName.length)
    }

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
        NumberAnimation { property: "scale"; from: 0.98; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast } }

    Overlay.modal: Rectangle { color: Theme.withAlpha("#000000", 0.6) }
    background: DialogSurface {}

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            Layout.bottomMargin: 14
            spacing: 12
            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                radius: Theme.radiusMedium
                color: Theme.withAlpha(Theme.accent, 0.12)
                border.width: 1
                border.color: Theme.withAlpha(Theme.accent, 0.22)
                Icon { anchors.centerIn: parent; name: "edit"; iconSize: 15; color: Theme.accent }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { text: "Rename"; color: Theme.textPrimary; font.pixelSize: Theme.fontHeading + 1; font.weight: Font.DemiBold }
                Text {
                    Layout.fillWidth: true
                    text: "The file on disk is renamed too"
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontCaption
                }
            }
            IconButton { iconName: "close"; tooltip: "Close"; onClicked: root.close() }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.bottomMargin: 18
            spacing: 8

            FormField {
                id: nameField
                Layout.fillWidth: true
                accessibleLabel: "New name"
                field.onTextChanged: root.errorText = ""
                field.Keys.onReturnPressed: root.submit()
                field.Keys.onEnterPressed: root.submit()
            }
            Text {
                visible: root.errorText.length > 0
                Layout.fillWidth: true
                text: root.errorText
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            Layout.leftMargin: 20
            spacing: 8
            Item { Layout.fillWidth: true }
            ModernButton { text: "Cancel"; variant: "ghost"; onClicked: root.close() }
            ModernButton {
                text: "Rename"
                variant: "primary"
                enabled: nameField.text.trim().length > 0
                onClicked: root.submit()
            }
        }
    }
}
