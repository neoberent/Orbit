import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Orbit.Native 1.0

Dialog {
    id: root
    title: ""
    modal: true
    standardButtons: Dialog.NoButton
    width: 600
    anchors.centerIn: parent
    padding: 0

    property string fileName: ""
    readonly property string comparison: compareField.text.trim().length === 0 || calculator.running || calculator.sha256.length === 0
                                         ? "" : calculator.compare(compareField.text)

    function openFor(item) {
        fileName = item.name
        compareField.text = item.expectedChecksum || ""
        open()
        calculator.start(item.destination)
    }

    onClosed: calculator.cancel()

    ChecksumCalculator { id: calculator }
    TextEdit { id: clipboardHelper; visible: false }

    component DigestRow: RowLayout {
        property string label
        property string value
        Layout.fillWidth: true
        spacing: 10
        Text {
            Layout.preferredWidth: 64
            text: parent.label
            color: Theme.textTertiary
            font.pixelSize: Theme.fontMicro
            font.weight: Font.DemiBold
            font.letterSpacing: Theme.trackingWider
        }
        TextEdit {
            Layout.fillWidth: true
            text: parent.value.length > 0 ? parent.value : (calculator.running ? "Calculating…" : "—")
            readOnly: true
            selectByMouse: true
            color: parent.value.length > 0 ? Theme.textPrimary : Theme.textDisabled
            font.pixelSize: Theme.fontCaption
            font.family: Theme.monoFamily
            wrapMode: TextEdit.WrapAnywhere
            Accessible.name: parent.label
        }
        IconButton {
            implicitWidth: 24
            implicitHeight: 24
            iconName: "copy"
            tooltip: "Copy " + parent.label
            enabled: parent.value.length > 0
            onClicked: {
                clipboardHelper.text = parent.value
                clipboardHelper.selectAll()
                clipboardHelper.copy()
            }
        }
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
                Icon { anchors.centerIn: parent; name: "shield"; iconSize: 15; color: Theme.accent }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text { text: "Verify Checksum"; color: Theme.textPrimary; font.pixelSize: Theme.fontHeading + 1; font.weight: Font.DemiBold }
                Text {
                    Layout.fillWidth: true
                    text: root.fileName
                    color: Theme.textTertiary
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideMiddle
                }
            }
            IconButton { iconName: "close"; tooltip: "Close"; onClicked: root.close() }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 20
            spacing: 14

            ProgressIndicator {
                visible: calculator.running
                Layout.fillWidth: true
                height: 4
                value: calculator.progress
                fillColor: Theme.accent
            }

            Text {
                visible: calculator.error.length > 0
                Layout.fillWidth: true
                text: calculator.error
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
            }

            DigestRow { label: "MD5"; value: calculator.md5 }
            DigestRow { label: "SHA-1"; value: calculator.sha1 }
            DigestRow { label: "SHA-256"; value: calculator.sha256 }

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 7
                Text { text: "Compare with"; color: Theme.textSecondary; font.pixelSize: Theme.fontCaption; font.weight: Font.DemiBold }
                FormField {
                    id: compareField
                    Layout.fillWidth: true
                    monospace: true
                    accessibleLabel: "Compare with"
                    placeholderText: "Paste the checksum published by the site"
                }
                RowLayout {
                    visible: root.comparison.length > 0
                    spacing: 7
                    Icon {
                        name: root.comparison === "match" ? "check" : "alert"
                        iconSize: 13
                        color: root.comparison === "match" ? Theme.success : Theme.danger
                    }
                    Text {
                        text: root.comparison === "match" ? "Checksum matches - the file is intact"
                              : root.comparison === "mismatch" ? "Checksum doesn't match - the file may be corrupted or tampered with"
                              : root.comparison === "unsupported" ? "SHA-512 is checked automatically when set in Download Options"
                              : "That doesn't look like an MD5, SHA-1 or SHA-256 checksum"
                        color: root.comparison === "match" ? Theme.success : Theme.danger
                        font.pixelSize: Theme.fontCaption
                        font.weight: Font.DemiBold
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            Layout.leftMargin: 20
            spacing: 8
            Item { Layout.fillWidth: true }
            ModernButton { text: "Done"; variant: "primary"; onClicked: root.close() }
        }
    }
}
