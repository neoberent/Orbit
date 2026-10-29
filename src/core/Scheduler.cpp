#include "core/Scheduler.hpp"

#include <algorithm>

namespace orbit::core {

namespace {

bool dayEnabled(int days, const QDate& date) {
    const int bit = date.dayOfWeek() - 1;  // Qt: 1 = Monday ... 7 = Sunday
    return (days & (1 << bit)) != 0;
}

}

bool WeeklyWindow::contains(const QDateTime& when) const {
    const QTime now = when.time();
    const QDate today = when.date();
    if (start == end) return dayEnabled(days, today);
    if (start < end) return dayEnabled(days, today) && now >= start && now < end;
    // Wraps past midnight: the evening part belongs to today, the early-morning part to yesterday.
    return (dayEnabled(days, today) && now >= start) || (dayEnabled(days, today.addDays(-1)) && now < end);
}

Scheduler::Scheduler(DownloadManager& manager, QObject* parent) : QObject(parent), manager_(manager) {
    timer_ = new QTimer(this);
    timer_->setInterval(15000);
    connect(timer_, &QTimer::timeout, this, &Scheduler::tick);
    timer_->start();
}

void Scheduler::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled_) {
        for (DownloadId id : autoPaused_) manager_.resume(id);
        autoPaused_.clear();
    }
    tick();
}

void Scheduler::setWindow(const QString& start, const QString& end, int days) {
    const QTime s = QTime::fromString(start, "HH:mm");
    const QTime e = QTime::fromString(end, "HH:mm");
    if (s.isValid()) window_.start = s;
    if (e.isValid()) window_.end = e;
    window_.days = days & 0x7f;
    if (enabled_) tick();
}

void Scheduler::setSpeedRule(bool enabled, const QString& start, const QString& end, int days, int limitKBps) {
    speedRuleEnabled_ = enabled;
    const QTime s = QTime::fromString(start, "HH:mm");
    const QTime e = QTime::fromString(end, "HH:mm");
    if (s.isValid()) speedWindow_.start = s;
    if (e.isValid()) speedWindow_.end = e;
    speedWindow_.days = days & 0x7f;
    speedLimitKBps_ = std::max(1, limitKBps);
    speedRuleActive_ = speedRuleEnabled_ && speedWindow_.contains(QDateTime::currentDateTime());
    emit speedRuleChanged(speedRuleActive_, speedLimitKBps_);
}

void Scheduler::tick() {
    const QDateTime now = QDateTime::currentDateTime();
    const auto snapshots = manager_.snapshot();

    const qint64 nowSeconds = now.toSecsSinceEpoch();
    for (const TaskSnapshot& snap : snapshots) {
        if (snap.state == DownloadState::Paused && snap.options.scheduledStartAt > 0 &&
            snap.options.scheduledStartAt <= nowSeconds && (!enabled_ || window_.contains(now))) {
            manager_.resume(snap.id);
        }
    }

    const bool ruleActive = speedRuleEnabled_ && speedWindow_.contains(now);
    if (ruleActive != speedRuleActive_) {
        speedRuleActive_ = ruleActive;
        emit speedRuleChanged(ruleActive, speedLimitKBps_);
    }

    if (!enabled_) return;

    if (window_.contains(now)) {
        for (DownloadId id : autoPaused_) manager_.resume(id);
        autoPaused_.clear();
        return;
    }

    for (const TaskSnapshot& snap : snapshots) {
        if (snap.state == DownloadState::Downloading || snap.state == DownloadState::Connecting) {
            manager_.pause(snap.id);
            autoPaused_.insert(snap.id);
        }
    }
}

}
