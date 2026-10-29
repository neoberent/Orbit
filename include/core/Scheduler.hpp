#pragma once

#include "core/DownloadManager.hpp"

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QTime>
#include <QTimer>

#include <unordered_set>

namespace orbit::core {

// A time range on selected weekdays. A range that crosses midnight (22:00-07:00)
// belongs to the day it starts on, so "Friday 22:00-07:00" runs into Saturday morning.
struct WeeklyWindow {
    QTime start{22, 0};
    QTime end{7, 0};
    int days{0x7f};  // bit 0 = Monday ... bit 6 = Sunday

    [[nodiscard]] bool contains(const QDateTime& when) const;
};

// Three jobs, checked every 15 seconds:
//  - the download window: outside it, active downloads are paused, and only the ones it
//    paused itself are resumed when it reopens (a download paused by hand stays paused);
//  - the speed rule: while it applies, speedRuleChanged() asks for a lower global limit;
//  - per-download start times: a paused download whose start time has come is resumed.
class Scheduler final : public QObject {
    Q_OBJECT
public:
    explicit Scheduler(DownloadManager& manager, QObject* parent = nullptr);

    void setEnabled(bool enabled);
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    // "HH:mm" strings; an invalid one is ignored and the previous bound kept.
    void setWindow(const QString& start, const QString& end, int days = 0x7f);

    void setSpeedRule(bool enabled, const QString& start, const QString& end, int days, int limitKBps);
    [[nodiscard]] bool speedRuleActive() const noexcept { return speedRuleActive_; }

    void tickNow() { tick(); }

signals:
    void speedRuleChanged(bool active, int limitKBps);

private:
    void tick();

    DownloadManager& manager_;
    QTimer* timer_{nullptr};
    bool enabled_{false};
    WeeklyWindow window_;
    std::unordered_set<DownloadId> autoPaused_;

    bool speedRuleEnabled_{false};
    WeeklyWindow speedWindow_{QTime(9, 0), QTime(18, 0), 0x1f};
    int speedLimitKBps_{512};
    bool speedRuleActive_{false};
};

}
