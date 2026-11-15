import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Windows: a Linux-only Orbit release, shown once. There's nothing to install here.
Dialog {
    id: root
    title: ""
    modal: true
    standardButtons: Dialog.NoButton
    width: Math.min(480, parent ? parent.width - 48 : 480)
    anchors.centerIn: parent
    padding: 0

    Overlay.modal: Rectangle { color: Theme.withAlpha("#000000", 0.6) }
    background: DialogSurface {}
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.animNormal; easing.type: Theme.easeStandard } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.animFast } }
    onClosed: updateManager.markLinuxNewsSeen()

    contentItem: ColumnLayout {
        spacing: 0

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 22
            spacing: 4
            Text {
                Layout.fillWidth: true
                text: "New in Orbit " + updateManager.linuxNewsVersion + " for Linux"
                color: Theme.textPrimary
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
            Text {
                Layout.fillWidth: true
                text: (updateManager.linuxNewsDate.length > 0 ? updateManager.linuxNewsDate + " · " : "") +
                      "This release is for Linux only. Your Windows version stays as it is; here's what changed there."
                color: Theme.textTertiary
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 22
            spacing: 8
            Repeater {
                model: updateManager.linuxNewsNotes
                delegate: RowLayout {
                    required property string modelData
                    Layout.fillWidth: true
                    spacing: 10
                    Rectangle { Layout.alignment: Qt.AlignTop; Layout.topMargin: 6; width: 5; height: 5; radius: 2.5; color: Theme.accent }
                    Text { Layout.fillWidth: true; text: modelData; color: Theme.textPrimary; font.pixelSize: Theme.fontBody; wrapMode: Text.Wrap }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 14
            Layout.leftMargin: 22
            spacing: 8
            Item { Layout.fillWidth: true }
            ModernButton {
                visible: updateManager.linuxNewsPage.length > 0
                text: "Learn More"
                variant: "secondary"
                onClicked: Qt.openUrlExternally(updateManager.linuxNewsPage)
            }
            ModernButton { text: "OK"; variant: "primary"; onClicked: root.close() }
        }
    }
}
