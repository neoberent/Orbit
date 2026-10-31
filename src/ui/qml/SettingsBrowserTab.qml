import QtQuick
import QtQuick.Layouts

// Browser integration settings.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    SettingsGroup {
        title: "Browser integration"

        SettingsRow {
            divider: false
            label: "Connect Orbit to your browsers"
            description: "Registers orbit:// links, .torrent files and the extension's native connection for your " + Theme.osName + " account"
            ToggleSwitch {
                checked: appController.browserIntegrationEnabled
                onToggled: appController.browserIntegrationEnabled = checked
            }
        }
        SettingsRow {
            label: "Status"
            description: appController.browserIntegrationStatus
            StatusBadge {
                label: appController.nativeHostRegistered ? "Connected" : (appController.browserIntegrationEnabled ? "Needs repair" : "Off")
                tone: appController.nativeHostRegistered ? Theme.success : (appController.browserIntegrationEnabled ? Theme.warning : Theme.textTertiary)
            }
            ModernButton {
                visible: appController.browserIntegrationEnabled
                text: "Repair"
                variant: "secondary"
                iconName: "retry"
                onClicked: appController.repairBrowserIntegration()
            }
        }
        SettingsRow {
            label: "Extension ID"
            description: "Chrome, Edge and Brave"
            Text {
                text: appController.chromeExtensionId
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                font.family: Theme.monoFamily
            }
        }
    }

    SettingsGroup {
        title: "How it works"

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 14
            spacing: 6
            StepText { text: "1. Install the Orbit extension in Chrome, Edge, Brave or Firefox." }
            StepText { text: "2. Click its toolbar icon once and choose “Allow” so Orbit receives your sign-in cookies with each download." }
            StepText { text: "3. Right-click a link and choose Download with Orbit, or turn on auto-catch in the extension. Downloads behind a sign-in now work, and the browser stops asking to open Orbit." }
            StepText { text: "Cookies are only kept until the download finishes, and are never included in backups."; color: Theme.textTertiary }
        }
    }
}
