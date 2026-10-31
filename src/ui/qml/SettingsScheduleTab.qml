import QtQuick
import QtQuick.Layouts

// Schedule settings: when Orbit may download, and how fast.
ColumnLayout {
    id: tab
    required property var dialog

    Layout.fillWidth: true
    Layout.margins: 24
    Layout.topMargin: 20
    spacing: 22

    // Fills the fields from the current settings; called each time the dialog opens.
    function load() {
        scheduleStartField.text = appController.scheduleStartTime
        scheduleEndField.text = appController.scheduleEndTime
        ruleStartField.text = appController.speedRuleStartTime
        ruleEndField.text = appController.speedRuleEndTime
        ruleLimitField.text = appController.speedRuleLimitKBps
    }

    SettingsGroup {
        title: "Download window"
        description: "Crossing midnight is fine, e.g. 22:00 to 07:00 - the window belongs to the day it starts on."

        SettingsRow {
            divider: false
            label: "Only download during a window"
            description: "Active downloads pause outside the window and resume when it opens"
            ToggleSwitch {
                checked: appController.scheduleEnabled
                onToggled: appController.scheduleEnabled = checked
            }
        }

        SettingsRow {
            label: "Days"
            dimmed: !appController.scheduleEnabled
            enabled: appController.scheduleEnabled
            DayPicker {
                days: appController.scheduleDays
                onChanged: (d) => appController.scheduleDays = d
            }
        }

        SettingsRow {
            label: "From"
            description: "24-hour time"
            dimmed: !appController.scheduleEnabled
            enabled: appController.scheduleEnabled
            FormField {
                id: scheduleStartField
                Layout.preferredWidth: 90
                iconName: "clock"
                monospace: true
                placeholderText: "22:00"
                validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                onEditingFinished: if (field.acceptableInput) appController.scheduleStartTime = text
            }
            Text { text: "to"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption }
            FormField {
                id: scheduleEndField
                Layout.preferredWidth: 90
                iconName: "clock"
                monospace: true
                placeholderText: "07:00"
                validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                onEditingFinished: if (field.acceptableInput) appController.scheduleEndTime = text
            }
        }
    }

    SettingsGroup {
        title: "Scheduled speed limit"
        description: "Caps direct downloads and torrents during busy hours; the lower of this and your normal limit applies."

        SettingsRow {
            divider: false
            label: "Limit speed on a schedule"
            description: appController.speedRuleActive ? "In effect right now" : "Not in effect right now"
            ToggleSwitch {
                checked: appController.speedRuleEnabled
                onToggled: appController.speedRuleEnabled = checked
            }
        }
        SettingsRow {
            label: "Days"
            dimmed: !appController.speedRuleEnabled
            enabled: appController.speedRuleEnabled
            DayPicker {
                days: appController.speedRuleDays
                onChanged: (d) => appController.speedRuleDays = d
            }
        }
        SettingsRow {
            label: "From"
            dimmed: !appController.speedRuleEnabled
            enabled: appController.speedRuleEnabled
            FormField {
                id: ruleStartField
                Layout.preferredWidth: 90
                iconName: "clock"
                monospace: true
                validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                onEditingFinished: if (field.acceptableInput) appController.speedRuleStartTime = text
            }
            Text { text: "to"; color: Theme.textTertiary; font.pixelSize: Theme.fontCaption }
            FormField {
                id: ruleEndField
                Layout.preferredWidth: 90
                iconName: "clock"
                monospace: true
                validator: RegularExpressionValidator { regularExpression: /^([01][0-9]|2[0-3]):[0-5][0-9]$/ }
                onEditingFinished: if (field.acceptableInput) appController.speedRuleEndTime = text
            }
        }
        SettingsRow {
            label: "Maximum speed"
            dimmed: !appController.speedRuleEnabled
            enabled: appController.speedRuleEnabled
            FormField {
                id: ruleLimitField
                Layout.preferredWidth: 120
                suffix: "KB/s"
                validator: IntValidator { bottom: 1; top: 10000000 }
                onEditingFinished: appController.speedRuleLimitKBps = parseInt(text.length > 0 ? text : "512")
            }
        }
    }

    Text {
        Layout.fillWidth: true
        text: "A download you pause yourself stays paused, even after the window opens. To start one download later, use Start at a set time in the Add dialog or in Download Options."
        color: Theme.textTertiary
        font.pixelSize: Theme.fontCaption
        wrapMode: Text.Wrap
    }
}
