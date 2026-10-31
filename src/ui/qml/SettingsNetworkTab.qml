import QtQuick
import QtQuick.Layouts

// Network settings: the proxy for Orbit's traffic.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    property string proxyMode: "none"
    property string proxyMessage: ""
    property bool proxyMessageOk: true

    function applyProxy() {
        const error = appController.setProxy(tab.proxyMode, proxyHostField.text, parseInt(proxyPortField.text || "0"),
                                             proxyUserField.text, proxyPasswordField.text, false)
        tab.proxyMessageOk = error.length === 0
        tab.proxyMessage = error.length > 0 ? error
                            : (tab.proxyMode === "none" ? "Orbit connects directly" : "Saved - new connections go through the proxy")
        proxyPasswordField.text = ""
    }

    // Fills the fields from the current settings; called each time the dialog opens.
    function load() {
        tab.proxyMode = appController.proxyType
        proxyHostField.text = appController.proxyHost
        proxyPortField.text = appController.proxyPort > 0 ? appController.proxyPort : ""
        proxyUserField.text = appController.proxyUsername
        proxyPasswordField.text = ""
        tab.proxyMessage = ""
    }

    SettingsGroup {
        title: "Proxy"
        description: "Applies to direct downloads, torrents (peers, trackers and DHT), media downloads and update checks."

        SettingsRow {
            divider: false
            label: "Connection"
            SegmentedControl {
                options: [{ label: "Direct", value: "none" }, { label: "HTTP", value: "http" }, { label: "SOCKS5", value: "socks5" }]
                value: tab.proxyMode
                onActivated: (v) => tab.proxyMode = v
            }
        }
        SettingsRow {
            label: "Server"
            description: "Host name or IP address, and port"
            dimmed: tab.proxyMode === "none"
            enabled: tab.proxyMode !== "none"
            FormField {
                id: proxyHostField
                Layout.preferredWidth: 200
                monospace: true
                accessibleLabel: "Proxy host"
                placeholderText: "127.0.0.1"
            }
            FormField {
                id: proxyPortField
                Layout.preferredWidth: 80
                monospace: true
                accessibleLabel: "Proxy port"
                placeholderText: "1080"
                validator: IntValidator { bottom: 1; top: 65535 }
            }
        }
        SettingsRow {
            label: "Username"
            description: "Leave empty if the proxy doesn't need a sign-in"
            dimmed: tab.proxyMode === "none"
            enabled: tab.proxyMode !== "none"
            FormField {
                id: proxyUserField
                Layout.preferredWidth: 200
                accessibleLabel: "Proxy username"
            }
        }
        SettingsRow {
            label: "Password"
            description: appController.proxyHasPassword ? "A password is saved (encrypted for your " + Theme.osName + " account). Type a new one to replace it." : "Stored encrypted for your " + Theme.osName + " account"
            dimmed: tab.proxyMode === "none"
            enabled: tab.proxyMode !== "none"
            FormField {
                id: proxyPasswordField
                Layout.preferredWidth: 200
                echoMode: TextInput.Password
                accessibleLabel: "Proxy password"
                placeholderText: appController.proxyHasPassword ? "••••••••" : ""
            }
        }
        SettingsRow {
            label: " "
            Text {
                visible: tab.proxyMessage.length > 0
                text: tab.proxyMessage
                color: tab.proxyMessageOk ? Theme.success : Theme.danger
                font.pixelSize: Theme.fontCaption
            }
            ModernButton {
                text: "Apply"
                variant: "primary"
                onClicked: tab.applyProxy()
            }
        }
    }
}
