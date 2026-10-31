import QtQuick
import QtQuick.Layouts

// Remote access settings: the web interface and its sign-in.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    // Fills the fields from the current settings; called each time the dialog opens.
    function load() {
        remoteUsernameField.text = appController.remoteCredentialsConfigured ? appController.remoteUsername : ""
        remotePasswordField.text = ""
        remotePasswordConfirmField.text = ""
        remoteCredError.text = ""
        portField.text = appController.remotePort
        tunnelHostField.text = appController.remoteTunnelHost
    }

    SettingsGroup {
        title: "Web dashboard"

        SettingsRow {
            divider: false
            label: "Enable remote access"
            description: "Runs a private, password-protected dashboard for your downloads"
            ToggleSwitch {
                checked: appController.remoteAccessEnabled
                onToggled: appController.remoteAccessEnabled = checked
            }
        }

        SettingsRow {
            label: "Status"
            description: appController.remoteStatusMessage
            StatusBadge {
                label: appController.remoteRunning ? "Running" : (appController.remoteAccessEnabled ? "Offline" : "Off")
                tone: appController.remoteRunning ? Theme.success : (appController.remoteAccessEnabled ? Theme.danger : Theme.textTertiary)
            }
        }

        SettingsRow {
            label: "Port"
            description: "Always bound to 127.0.0.1 and never exposed directly to your network"
            FormField {
                id: portField
                Layout.preferredWidth: 100
                monospace: true
                validator: IntValidator { bottom: 1024; top: 65535 }
                onEditingFinished: appController.remotePort = parseInt(text.length > 0 ? text : "8765")
            }
        }
    }

    SettingsGroup {
        title: "Sign-in"
        description: appController.remoteCredentialsConfigured
                     ? "Signed in as “" + appController.remoteUsername + "”. Enter a new password to change it."
                     : "Required before remote access can turn on. Use a password you don't use anywhere else."

        SettingsRow {
            divider: false
            label: "Username"
            FormField {
                id: remoteUsernameField
                Layout.preferredWidth: 240
                placeholderText: "Username"
            }
        }
        SettingsRow {
            label: "New password"
            description: "At least 8 characters"
            FormField {
                id: remotePasswordField
                Layout.preferredWidth: 240
                echoMode: TextInput.Password
                placeholderText: "New password"
            }
        }
        SettingsRow {
            label: "Confirm password"
            FormField {
                id: remotePasswordConfirmField
                Layout.preferredWidth: 240
                echoMode: TextInput.Password
                placeholderText: "Repeat password"
            }
        }
        SettingsRow {
            label: " "
            implicitHeight: 52
            Text {
                id: remoteCredError
                text: ""
                visible: text.length > 0
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
            }
            ModernButton {
                text: "Save Credentials"
                variant: "primary"
                onClicked: {
                    if (remoteUsernameField.text.trim().length === 0) {
                        remoteCredError.text = "Enter a username."
                        return
                    }
                    if (remotePasswordField.text.length < 8) {
                        remoteCredError.text = "Password must be at least 8 characters."
                        return
                    }
                    if (remotePasswordField.text !== remotePasswordConfirmField.text) {
                        remoteCredError.text = "Passwords don't match."
                        return
                    }
                    if (appController.setRemoteCredentials(remoteUsernameField.text.trim(), remotePasswordField.text)) {
                        remoteCredError.text = ""
                        remotePasswordField.text = ""
                        remotePasswordConfirmField.text = ""
                    } else {
                        remoteCredError.text = "Couldn't save those credentials."
                    }
                }
            }
        }
    }

    SettingsGroup {
        title: "Access from anywhere"
        description: "The dashboard only listens on this PC. To reach it from your phone or another network, tunnel it out with Cloudflare's free cloudflared tool instead of opening a port on your router."

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 14
            spacing: 6

            RowLayout {
                spacing: 8
                Text { text: "Quick Tunnel"; color: Theme.textPrimary; font.pixelSize: Theme.fontBody; font.weight: Font.DemiBold }
                StatusBadge { label: "Testing"; tone: Theme.textTertiary }
            }
            StepText { text: "1. Install cloudflared" }
            CopyableCommand { command: "winget install --id Cloudflare.cloudflared" }
            StepText { text: "2. Run this in a terminal" }
            CopyableCommand { command: "cloudflared tunnel --url " + appController.remoteLocalUrl }
            StepText { text: "3. Open the printed *.trycloudflare.com link on your phone and sign in with your Orbit username and password." }
            StepText { text: "The link changes every time you run the command. Good for a quick check, not for daily use."; color: Theme.textTertiary }
        }

        Rectangle { Layout.fillWidth: true; Layout.leftMargin: 14; Layout.rightMargin: 14; Layout.preferredHeight: 1; color: Theme.border }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: 14
            spacing: 6

            RowLayout {
                spacing: 8
                Text { text: "Named Tunnel"; color: Theme.textPrimary; font.pixelSize: Theme.fontBody; font.weight: Font.DemiBold }
                StatusBadge { label: "Recommended"; tone: Theme.success }
            }
            StepText { text: "1. Link your Cloudflare account" }
            CopyableCommand { command: "cloudflared tunnel login" }
            StepText { text: "2. Create a tunnel named orbit" }
            CopyableCommand { command: "cloudflared tunnel create orbit" }
            StepText { text: "3. Route a hostname to it" }
            CopyableCommand { command: "cloudflared tunnel route dns orbit orbit.yourdomain.com" }
            StepText { text: "4. Point the tunnel's config.yml at " + appController.remoteLocalUrl + ", then run" }
            CopyableCommand { command: "cloudflared tunnel run orbit" }
            StepText { text: "5. Optional: keep it running after a reboot" }
            CopyableCommand { command: "cloudflared service install" }
            StepText { text: "6. Enter the hostname here, so Orbit answers requests for it" }
            FormField {
                id: tunnelHostField
                Layout.preferredWidth: 280
                monospace: true
                placeholderText: "orbit.yourdomain.com"
                onEditingFinished: {
                    appController.remoteTunnelHost = text
                    text = appController.remoteTunnelHost
                }
            }
            StepText { text: "Then visit https://orbit.yourdomain.com from anywhere."; color: Theme.textTertiary }
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.preferredHeight: warnText.implicitHeight + 22
        radius: Theme.radiusLarge
        color: Theme.withAlpha(Theme.warning, 0.07)
        border.width: 1
        border.color: Theme.withAlpha(Theme.warning, 0.22)

        RowLayout {
            anchors.fill: parent
            anchors.margins: 11
            anchors.leftMargin: 14
            spacing: 10
            Icon { Layout.alignment: Qt.AlignTop; name: "alert"; iconSize: 14; color: Theme.warning }
            Text {
                id: warnText
                Layout.fillWidth: true
                text: "Keep Orbit's username and password protection on at all times. The tunnel only controls who can reach the page, not who can sign in."
                color: Theme.textSecondary
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }
        }
    }
}
