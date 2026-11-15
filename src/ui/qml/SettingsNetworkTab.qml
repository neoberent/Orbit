import QtQuick
import QtQuick.Layouts

// Network settings: the proxy for Orbit's traffic, the network interface it may use, and
// what happens on a metered connection.
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
    property var interfaces: []

    function interfaceOptions() {
        const options = [{ label: "Any", value: "" }]
        tab.interfaces.forEach(i => options.push({ label: i.name, value: i.name }))
        return options
    }

    function interfaceDescription() {
        const name = appController.networkInterface
        if (name === "") return "Orbit uses whichever connection the system picks"
        const info = tab.interfaces.find(i => i.name === name)
        if (!info || !info.up) return "“" + name + "” is down - nothing downloads until it's back"
        return "Only through “" + name + "”" + (info.address ? " (" + info.address + ")" : "") + ". If it goes down, downloads stop instead of using another connection."
    }

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
        tab.interfaces = appController.networkInterfaces()
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

    SettingsGroup {
        visible: Theme.osName === "Linux"
        title: "Network interface"
        description: "Lock downloads to one connection, such as a VPN (wg0, tun0). Applies to direct downloads, torrents and media downloads. Direct downloads also look up site names through it (encrypted, via Quad9), so nothing goes out over another connection."

        SettingsRow {
            divider: false
            label: "Use"
            description: tab.interfaceDescription()
            SegmentedControl {
                options: tab.interfaceOptions()
                value: appController.networkInterface
                onActivated: (v) => appController.networkInterface = v
            }
            IconButton {
                iconName: "retry"
                tooltip: "Look for interfaces again"
                onClicked: tab.interfaces = appController.networkInterfaces()
            }
        }
    }

    SettingsGroup {
        visible: !Theme.windows
        title: "This network"
        description: appController.currentNetwork.length > 0
                     ? "A download limit that applies only while you're on “" + appController.currentNetwork + "”. Orbit remembers it for this network."
                     : "Connect to a network managed by NetworkManager to set a limit for it."

        SettingsRow {
            divider: false
            enabled: appController.currentNetwork.length > 0
            dimmed: appController.currentNetwork.length === 0
            label: "Limit downloads on this network"
            description: appController.currentNetworkLimitKBps > 0
                         ? "Direct downloads and torrents stay under " + appController.currentNetworkLimitKBps + " KB/s here"
                         : "No limit here beyond the general ones"
            FormField {
                id: networkLimitField
                visible: appController.currentNetworkLimitKBps > 0
                Layout.preferredWidth: 120
                suffix: "KB/s"
                text: appController.currentNetworkLimitKBps > 0 ? appController.currentNetworkLimitKBps : ""
                validator: IntValidator { bottom: 1; top: 10000000 }
                onEditingFinished: appController.currentNetworkLimitKBps = parseInt(text.length > 0 ? text : "1024")
            }
            ToggleSwitch {
                checked: appController.currentNetworkLimitKBps > 0
                onToggled: appController.currentNetworkLimitKBps = checked ? parseInt(networkLimitField.text || "1024") : 0
            }
        }
    }

    SettingsGroup {
        visible: appController.meteredDetectionAvailable
        title: "Metered connections"
        description: "Phone hotspots and capped plans are marked as metered by NetworkManager."

        SettingsRow {
            divider: false
            label: "Pause on metered connections"
            description: !appController.pauseOnMetered ? "Downloads keep going on any connection"
                         : appController.meteredConnection ? "This connection is metered - downloads and seeding are paused"
                         : "Downloads and seeding pause while connected to one, and continue afterwards"
            ToggleSwitch {
                checked: appController.pauseOnMetered
                onToggled: appController.pauseOnMetered = checked
            }
        }
    }
}
