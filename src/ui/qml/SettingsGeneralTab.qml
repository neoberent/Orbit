import QtQuick
import QtQuick.Layouts

// General settings: appearance, notifications, power and the clipboard.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    // Fills the fields from the current settings; called each time the dialog opens.
    function load() {
        clipboardExtensionsField.text = clipboardMonitor.extensions
    }

    SettingsGroup {
        title: "Appearance"

        SettingsRow {
            divider: false
            label: "Dark theme"
            description: "Use a dark interface throughout Orbit"
            ToggleSwitch {
                checked: appController.darkTheme
                onToggled: appController.darkTheme = checked
            }
        }
    }

    SettingsGroup {
        title: "Notifications"

        SettingsRow {
            divider: false
            label: "Notify when downloads finish"
            description: "Shows a " + (Theme.windows ? "Windows " : "desktop ") + "notification when a download completes or fails while Orbit is in the background"
            ToggleSwitch {
                checked: Prefs.notifyOnFinish
                onToggled: Prefs.notifyOnFinish = checked
            }
        }
    }

    SettingsGroup {
        title: "Power"

        SettingsRow {
            divider: false
            label: "Keep the PC awake while downloading"
            description: (Theme.windows ? "Windows" : "Your PC") + " won't go to sleep while a download is running"
            ToggleSwitch {
                checked: powerManager.preventSleep
                onToggled: powerManager.preventSleep = checked
            }
        }
        SettingsRow {
            label: "When downloads finish"
            description: "Runs once after everything that's downloading or queued is done, with 30 seconds to cancel"
            SegmentedControl {
                options: {
                    const list = [{ label: "Nothing", value: "none" }, { label: "Exit", value: "exit" }, { label: "Sleep", value: "sleep" }]
                    if (powerManager.hibernateAvailable) list.push({ label: "Hibernate", value: "hibernate" })
                    list.push({ label: "Shut down", value: "shutdown" })
                    return list
                }
                value: powerManager.whenDoneAction
                onActivated: (v) => powerManager.whenDoneAction = v
            }
        }
    }

    SettingsGroup {
        title: "Clipboard"

        SettingsRow {
            divider: false
            label: "Offer to download copied links"
            description: "When you copy a magnet link or a link to a file in another app, Orbit offers to download it"
            ToggleSwitch {
                checked: clipboardMonitor.enabled
                onToggled: clipboardMonitor.enabled = checked
            }
        }
        SettingsRow {
            label: "File types"
            description: "Extensions that count as a download link"
            dimmed: !clipboardMonitor.enabled
            enabled: clipboardMonitor.enabled
            extra: FormField {
                id: clipboardExtensionsField
                Layout.fillWidth: true
                monospace: true
                onEditingFinished: clipboardMonitor.extensions = text
            }
        }
        SettingsRow {
            label: "Include video page links"
            description: "YouTube, TikTok, Twitch and similar"
            dimmed: !clipboardMonitor.enabled
            enabled: clipboardMonitor.enabled
            ToggleSwitch {
                checked: clipboardMonitor.catchMedia
                onToggled: clipboardMonitor.catchMedia = checked
            }
        }
    }
}
