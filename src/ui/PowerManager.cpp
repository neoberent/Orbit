#include "PowerManager.hpp"

#include "DownloadListModel.hpp"
#include "utils/Logger.hpp"

#include <QSettings>
#include <QVariantMap>

#if defined(_WIN32)
#include <windows.h>
#include <powrprof.h>
#endif

namespace {

bool dryRun() { return qEnvironmentVariableIntValue("ORBIT_POWER_DRY_RUN") == 1; }

const QStringList& knownActions() {
    static const QStringList actions = {"none", "exit", "sleep", "hibernate", "shutdown"};
    return actions;
}

#if defined(_WIN32)
bool enableShutdownPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid) &&
                    AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr) &&
                    GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

// A power request is what Windows honours on Modern Standby PCs (most current laptops),
// where SetThreadExecutionState alone doesn't keep the system awake, and it shows up with
// this reason in `powercfg /requests`.
HANDLE createPowerRequest() {
    static wchar_t reasonText[] = L"Orbit is downloading files";
    REASON_CONTEXT reason{};
    reason.Version = POWER_REQUEST_CONTEXT_VERSION;
    reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
    reason.Reason.SimpleReasonString = reasonText;
    HANDLE request = PowerCreateRequest(&reason);
    if (request == INVALID_HANDLE_VALUE) {
        ORBIT_LOG_ERROR("power", "PowerCreateRequest failed: " + std::to_string(GetLastError()));
        return nullptr;
    }
    if (!PowerSetRequest(request, PowerRequestSystemRequired)) {
        ORBIT_LOG_ERROR("power", "PowerSetRequest(SystemRequired) failed: " + std::to_string(GetLastError()));
    }
    // Keeps the process running instead of being throttled on Modern Standby; on S3
    // systems it implies SystemRequired.
    PowerSetRequest(request, PowerRequestExecutionRequired);
    return request;
}
#endif

// Windows drops power requests when the user puts the PC to sleep (lid, power button,
// Start > Sleep) and doesn't restore them on resume, so they're renewed periodically.
constexpr int kRefreshIntervalMs = 60 * 1000;

}

PowerManager::PowerManager(DownloadListModel& model, QObject* parent) : QObject(parent), model_(model) {
    preventSleep_ = QSettings().value("power/preventSleep", true).toBool();
    refreshTimer_.setInterval(kRefreshIntervalMs);
    connect(&refreshTimer_, &QTimer::timeout, this, &PowerManager::refreshSleepBlock);
    connect(&model_, &DownloadListModel::revisionChanged, this, &PowerManager::evaluate);
#if defined(__linux__)
    hibernateAvailable_ = linux_integration::canHibernate();
#endif
    evaluate();
}

PowerManager::~PowerManager() {
    applySleepBlock(false);
    releasePowerRequest();
}

void PowerManager::setPreventSleep(bool enabled) {
    if (enabled == preventSleep_) return;
    preventSleep_ = enabled;
    QSettings().setValue("power/preventSleep", enabled);
    emit preventSleepChanged();
    evaluate();
}

void PowerManager::setWhenDoneAction(const QString& action) {
    const QString normalized = knownActions().contains(action) ? action : QStringLiteral("none");
    if (normalized == whenDoneAction_) return;
    whenDoneAction_ = normalized;
    sawActivity_ = false;
    ORBIT_LOG_INFO("power", "When downloads finish: " + normalized.toStdString());
    emit whenDoneActionChanged();
    evaluate();
}

bool PowerManager::hibernateAvailable() const {
#if defined(_WIN32)
    return IsPwrHibernateAllowed() != FALSE;
#elif defined(__linux__)
    return hibernateAvailable_;
#else
    return false;
#endif
}

QString PowerManager::actionLabel(const QString& action) const {
    if (action == QLatin1String("exit")) return QStringLiteral("Exit Orbit");
    if (action == QLatin1String("sleep")) return QStringLiteral("Sleep");
    if (action == QLatin1String("hibernate")) return QStringLiteral("Hibernate");
    if (action == QLatin1String("shutdown")) return QStringLiteral("Shut down");
    return QStringLiteral("Do nothing");
}

void PowerManager::evaluate() {
    const QVariantMap summary = model_.summary();
    const int transferring = summary.value("active").toInt();
    const int waiting = summary.value("queued").toInt();

    applySleepBlock(preventSleep_ && transferring > 0);

    if (whenDoneAction_ == QLatin1String("none")) return;
    if (transferring + waiting > 0) {
        sawActivity_ = true;
        return;
    }
    if (!sawActivity_) return;

    // One-shot: disarm first so a cancelled countdown doesn't fire again.
    const QString action = whenDoneAction_;
    whenDoneAction_ = QStringLiteral("none");
    sawActivity_ = false;
    emit whenDoneActionChanged();
    ORBIT_LOG_INFO("power", "All downloads finished; asking to " + action.toStdString());
    emit countdownRequested(action);
}

void PowerManager::applySleepBlock(bool block) {
    if (block == sleepBlocked_) return;
    sleepBlocked_ = block;
    if (block) {
        refreshSleepBlock();
        refreshTimer_.start();
    } else {
        refreshTimer_.stop();
        releasePowerRequest();
#if defined(_WIN32)
        SetThreadExecutionState(ES_CONTINUOUS);
#endif
    }
    ORBIT_LOG_INFO("power", block ? "Preventing sleep while downloads run" : "Allowing sleep again");
    emit sleepBlockedChanged();
}

void PowerManager::refreshSleepBlock() {
#if defined(_WIN32)
    // The new request is taken before the old one is released so there's no gap.
    HANDLE previous = powerRequest_;
    powerRequest_ = createPowerRequest();
    if (previous) CloseHandle(previous);
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
#elif defined(__linux__)
    // Inhibitors don't lapse across a suspend on Linux; this only re-takes a lost one.
    inhibitor_.acquire(QStringLiteral("Orbit is downloading files"));
#endif
}

void PowerManager::releasePowerRequest() {
#if defined(_WIN32)
    // Closing the handle clears every request type set on it.
    if (powerRequest_) CloseHandle(powerRequest_);
#elif defined(__linux__)
    inhibitor_.release();
#endif
    powerRequest_ = nullptr;
}

void PowerManager::executeAction(const QString& action) {
    const bool simulated = dryRun() && action != QLatin1String("exit");
    ORBIT_LOG_INFO("power", "Executing when-done action: " + action.toStdString() + (simulated ? " (dry run)" : ""));
    if (action == QLatin1String("exit")) {
        emit quitRequested();
        return;
    }
    if (dryRun()) return;
#if defined(_WIN32)
    if (action == QLatin1String("sleep") || action == QLatin1String("hibernate")) {
        applySleepBlock(false);
        if (!SetSuspendState(action == QLatin1String("hibernate") ? TRUE : FALSE, FALSE, FALSE)) {
            ORBIT_LOG_ERROR("power", "SetSuspendState failed: " + std::to_string(GetLastError()));
        }
    } else if (action == QLatin1String("shutdown")) {
        enableShutdownPrivilege();
        if (ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF, SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED)) {
            // Close cleanly (saving torrent resume data) while Windows shuts down.
            emit quitRequested();
        } else {
            ORBIT_LOG_ERROR("power", "ExitWindowsEx failed: " + std::to_string(GetLastError()));
        }
    }
#elif defined(__linux__)
    if (action == QLatin1String("sleep") || action == QLatin1String("hibernate")) applySleepBlock(false);
    linux_integration::requestPowerAction(action, this, [this, action](const QString& error) {
        if (!error.isEmpty()) {
            ORBIT_LOG_ERROR("power", action.toStdString() + " was refused: " + error.toStdString());
            evaluate();
            return;
        }
        // Close cleanly (saving torrent resume data) while the system shuts down.
        if (action == QLatin1String("shutdown")) emit quitRequested();
    });
#endif
}
