#include "CommandLine.hpp"

#include "utils/LocalSocket.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QStringList>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <csignal>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace command_line {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitNotRunning = 3;

const char* const kUsage =
    "Usage: orbit [command] [options]\n"
    "\n"
    "Without a command, Orbit opens its window (or brings it to the front).\n"
    "\n"
    "Commands:\n"
    "  add <link|file>...           Add downloads (http(s) links, magnet links, .torrent and\n"
    "                               Metalink (.meta4) files)\n"
    "      -d, --dir <folder>       Save them in this folder instead of the default one\n"
    "      -i, --input <file>       Also add every link in this file, one per line (- = stdin)\n"
    "      --checksum <type:hex>    Verify the file when it finishes (sha256:..., md5:...)\n"
    "      --force                  Add it even if the same link was downloaded before\n"
    "      --mirror <link>          Another link to the same file (repeatable); each serves\n"
    "                               some of the connections\n"
    "  list, ls                     Show every download\n"
    "  status                       Show totals and current speeds\n"
    "      -w, --watch              Keep running and print a line whenever it changes\n"
    "                               (for status bars; add --json for one object per line)\n"
    "      --interval <seconds>     How often --watch asks Orbit (default 1)\n"
    "  pause <id>... | --all        Pause downloads\n"
    "  resume <id>... | --all       Resume paused or failed downloads\n"
    "  cancel <id>... | --all       Stop unfinished downloads and delete what they had so far\n"
    "                               (finished files are kept; seeding torrents stop seeding)\n"
    "  remove, rm <id>...           Remove finished, paused or failed downloads from the list\n"
    "                               (files on disk are never deleted)\n"
    "  feeds                        Show your torrent feeds\n"
    "  feeds add <url> [--filter <words>] [-d <folder>]\n"
    "                               Follow a feed: new matching items download by themselves\n"
    "  feeds remove <url>           Stop following a feed\n"
    "  feeds check                  Check every feed now\n"
    "  config [list]                Show Orbit's settings\n"
    "  config get <name>            Show one setting\n"
    "  config set <name> <value>    Change one, as Settings in the window would\n"
    "                               (e.g. config set torrentListenPort 51820)\n"
    "  remote [status]              Show the web dashboard's state (Settings > Remote)\n"
    "  remote enable | disable      Turn the web dashboard on or off\n"
    "  remote set-password <user>   Set its sign-in; asks for the password (or reads\n"
    "                               one line from standard input)\n"
    "  show                         Open Orbit's window\n"
    "  new                          Open the window to add a download\n"
    "  help                         Show this help\n"
    "\n"
    "Options:\n"
    "  --json                       Print the answer as JSON (list, status, add)\n"
    "  --background                 Start Orbit in the tray without opening the window\n"
    "  --headless                   Run without any window or tray (for servers; use the\n"
    "                               web dashboard or these commands to control it)\n"
    "  --version                    Print Orbit's version\n";

bool useColor() {
    return ::isatty(STDOUT_FILENO) && std::getenv("NO_COLOR") == nullptr;
}

void printLine(const QString& text) {
    std::fputs(text.toLocal8Bit().constData(), stdout);
    std::fputc('\n', stdout);
}

void printError(const QString& text) {
    std::fprintf(stderr, "orbit: %s\n", text.toLocal8Bit().constData());
}

QString formatBytes(double bytes) {
    static const char* const units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 4) {
        bytes /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(static_cast<qint64>(bytes))
                     : QStringLiteral("%1 %2").arg(bytes, 0, 'f', bytes < 10 ? 1 : 0).arg(QLatin1String(units[unit]));
}

QString formatRate(double bytesPerSec) {
    return bytesPerSec <= 0 ? QStringLiteral("-") : formatBytes(bytesPerSec) + QStringLiteral("/s");
}

QString formatEta(double seconds) {
    if (seconds <= 0) return {};
    const auto s = static_cast<qint64>(seconds);
    if (s < 60) return QStringLiteral("%1s").arg(s);
    if (s < 3600) return QStringLiteral("%1m%2s").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1h%2m").arg(s / 3600).arg((s % 3600) / 60, 2, 10, QLatin1Char('0'));
}

QString stateColor(const QString& state) {
    if (state == QLatin1String("Downloading") || state == QLatin1String("Connecting")) return QStringLiteral("\x1b[36m");
    if (state == QLatin1String("Completed") || state == QLatin1String("Seeding")) return QStringLiteral("\x1b[32m");
    if (state == QLatin1String("Error")) return QStringLiteral("\x1b[31m");
    if (state == QLatin1String("Paused") || state == QLatin1String("Cancelled")) return QStringLiteral("\x1b[33m");
    return QStringLiteral("\x1b[2m");
}

QLocalSocket* connectToOrbit(int timeoutMs) {
    auto* socket = new QLocalSocket();
    const QString name = orbit::utils::localServerName(QString::fromLatin1(kControlSocketName));
    QDeadlineTimer deadline(timeoutMs);
    while (true) {
        socket->connectToServer(name);
        if (socket->waitForConnected(500)) return socket;
        if (deadline.hasExpired()) break;
        QThread::msleep(150);
    }
    delete socket;
    return nullptr;
}

std::optional<QJsonObject> send(QLocalSocket* socket, const QJsonObject& request) {
    socket->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
    if (!socket->waitForBytesWritten(5000)) return std::nullopt;
    QByteArray reply;
    QDeadlineTimer deadline(15000);
    while (!reply.contains('\n')) {
        if (deadline.hasExpired() || !socket->waitForReadyRead(static_cast<int>(deadline.remainingTime()))) return std::nullopt;
        reply += socket->readAll();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(reply.left(reply.indexOf('\n')));
    if (!doc.isObject()) return std::nullopt;
    return doc.object();
}

void printDownloads(const QJsonArray& downloads) {
    if (downloads.isEmpty()) {
        printLine(QStringLiteral("No downloads."));
        return;
    }
    const bool color = useColor();
    const QString dim = color ? QStringLiteral("\x1b[2m") : QString();
    const QString reset = color ? QStringLiteral("\x1b[0m") : QString();
    printLine(dim + QStringLiteral("%1  %2  %3  %4  %5  %6")
                        .arg(QStringLiteral("ID"), -4)
                        .arg(QStringLiteral("STATE"), -11)
                        .arg(QStringLiteral("DONE"), 5)
                        .arg(QStringLiteral("SPEED"), 10)
                        .arg(QStringLiteral("SIZE"), 9)
                        .arg(QStringLiteral("NAME")) +
              reset);
    for (const QJsonValue& value : downloads) {
        const QJsonObject d = value.toObject();
        const QString state = d.value("state").toString();
        const double total = d.value("total").toDouble();
        const QString done = total > 0 ? QStringLiteral("%1%").arg(qRound(d.value("progress").toDouble() * 100)) : QStringLiteral("-");
        const double rate = state == QLatin1String("Seeding") ? d.value("uploadRate").toDouble() : d.value("downloadRate").toDouble();
        QString speed = formatRate(rate);
        if (state == QLatin1String("Seeding") && rate > 0) speed = QStringLiteral("↑") + speed;
        QString name = d.value("name").toString();
        if (name.isEmpty()) name = d.value("source").toString();
        const QString eta = formatEta(d.value("eta").toDouble());
        if (!eta.isEmpty() && (state == QLatin1String("Downloading"))) name += dim + QStringLiteral("  (%1 left)").arg(eta) + reset;
        if (state == QLatin1String("Error") && d.contains("error")) name += dim + QStringLiteral("  - ") + d.value("error").toString() + reset;
        const QString stateText = QStringLiteral("%1").arg(state, -11);
        printLine(QStringLiteral("%1  %2  %3  %4  %5  %6")
                      .arg(d.value("id").toString(), -4)
                      .arg(color ? stateColor(state) + stateText + reset : stateText)
                      .arg(done, 5)
                      .arg(speed, 10)
                      .arg(total > 0 ? formatBytes(total)
                           : d.value("downloaded").toDouble() > 0 ? formatBytes(d.value("downloaded").toDouble())
                                                                  : QStringLiteral("-"), 9)
                      .arg(name));
    }
}

void printStatus(const QJsonObject& s) {
    QStringList parts;
    const std::pair<const char*, const char*> counts[] = {
        {"active", "downloading"}, {"queued", "queued"}, {"paused", "paused"},
        {"seeding", "seeding"}, {"completed", "completed"}, {"failed", "failed"},
    };
    for (const auto& [key, label] : counts) {
        const int n = s.value(QLatin1String(key)).toInt();
        if (n > 0) parts << QStringLiteral("%1 %2").arg(n).arg(QLatin1String(label));
    }
    printLine(QStringLiteral("Orbit is running: ") + (parts.isEmpty() ? QStringLiteral("no downloads") : parts.join(QStringLiteral(", "))));
    printLine(QStringLiteral("Speed: ↓ %1   ↑ %2").arg(formatRate(s.value("downloadRate").toDouble()), formatRate(s.value("uploadRate").toDouble())));
    if (!s.value("networkInterface").toString().isEmpty()) {
        printLine(QStringLiteral("Bound to network interface: ") + s.value("networkInterface").toString());
    }
    if (s.value("metered").toBool()) printLine(QStringLiteral("Paused while the connection is metered"));
}

// True once nobody reads our standard output any more (the status bar that started
// `status --watch` went away). A pipe whose reader closed reports POLLERR.
bool outputClosed() {
    pollfd fd{STDOUT_FILENO, 0, 0};
    return ::poll(&fd, 1, 0) > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
}

QString watchLine(const QJsonObject& s, bool json) {
    if (json) return QString::fromUtf8(QJsonDocument(s).toJson(QJsonDocument::Compact));
    if (!s.value("running").toBool()) return QStringLiteral("Orbit isn't running");
    QStringList parts;
    parts << QStringLiteral("↓ %1  ↑ %2").arg(formatRate(s.value("downloadRate").toDouble()), formatRate(s.value("uploadRate").toDouble()));
    QStringList counts;
    const std::pair<const char*, const char*> labels[] = {{"active", "downloading"}, {"queued", "queued"}, {"paused", "paused"}, {"seeding", "seeding"}};
    for (const auto& [key, label] : labels) {
        const int n = s.value(QLatin1String(key)).toInt();
        if (n > 0) counts << QStringLiteral("%1 %2").arg(n).arg(QLatin1String(label));
    }
    parts << (counts.isEmpty() ? QStringLiteral("idle") : counts.join(QStringLiteral(", ")));
    if (s.contains("progress")) parts << QStringLiteral("%1%").arg(static_cast<int>(s.value("progress").toDouble() * 100));
    if (const QString eta = formatEta(s.value("eta").toDouble()); !eta.isEmpty()) parts << eta + QStringLiteral(" left");
    if (s.value("metered").toBool()) parts << QStringLiteral("paused: metered connection");
    return parts.join(QStringLiteral("  ·  "));
}

// `orbit status --watch`: one line each time the status changes, for status bars
// (Noctalia, Waybar, ...). It never gives up while Orbit is closed, it just reports
// that and keeps checking, and it ends once its reader is gone.
int watchStatus(bool json, int intervalMs) {
#if defined(__linux__)
    // Also end with the program that started us, even while nothing is being printed.
    ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    QLocalSocket* socket = nullptr;
    QString last;
    while (!outputClosed()) {
        std::optional<QJsonObject> reply;
        // Orbit drops a connection after 30 s, so one failed request reconnects once.
        for (int attempt = 0; attempt < 2 && !reply; ++attempt) {
            if (!socket) socket = connectToOrbit(0);
            if (!socket) break;
            reply = send(socket, QJsonObject{{"type", "status"}});
            if (!reply) {
                delete socket;
                socket = nullptr;
            }
        }
        QJsonObject state{{"running", false}};
        if (reply) {
            state = *reply;
            state.remove("ok");
            state.insert("running", true);
        }
        if (const QString line = watchLine(state, json); line != last) {
            printLine(line);
            if (std::fflush(stdout) != 0 || std::ferror(stdout)) break;
            last = line;
        }
        QThread::msleep(static_cast<unsigned long>(reply ? intervalMs : std::max(intervalMs, 2000)));
    }
    delete socket;
    return kExitOk;
}

// A password from the terminal without echoing it, or one line from a pipe.
QString readSecret(const char* prompt) {
    const bool tty = ::isatty(STDIN_FILENO);
    termios saved{};
    if (tty) {
        std::fputs(prompt, stderr);
        std::fflush(stderr);
        ::tcgetattr(STDIN_FILENO, &saved);
        termios quiet = saved;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    char buffer[1024];
    QString line;
    if (std::fgets(buffer, sizeof(buffer), stdin)) line = QString::fromLocal8Bit(buffer);
    if (tty) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        std::fputc('\n', stderr);
    }
    while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) line.chop(1);
    std::fill(std::begin(buffer), std::end(buffer), '\0');
    return line;
}

void printRemoteStatus(const QJsonObject& s) {
    const bool enabled = s.value("enabled").toBool();
    printLine(QStringLiteral("Web dashboard: ") +
              (!enabled ? QStringLiteral("off")
                        : s.value("running").toBool() ? QStringLiteral("on, at ") + s.value("url").toString()
                                                      : s.value("message").toString()));
    printLine(QStringLiteral("Sign-in: ") + (s.value("configured").toBool() ? s.value("username").toString()
                                                                             : QStringLiteral("not set (orbit remote set-password <user>)")));
    if (!s.value("tunnelHosts").toString().isEmpty()) printLine(QStringLiteral("Tunnel: ") + s.value("tunnelHosts").toString());
}

QString absoluteSource(const QString& arg) {
    QString source = arg;
    if (source.startsWith(QLatin1String("file://"), Qt::CaseInsensitive)) source = QUrl(source).toLocalFile();
    const bool localFile = source.endsWith(QLatin1String(".torrent"), Qt::CaseInsensitive) ||
                           source.endsWith(QLatin1String(".meta4"), Qt::CaseInsensitive) ||
                           source.endsWith(QLatin1String(".metalink"), Qt::CaseInsensitive);
    if (localFile && !source.contains(QLatin1String("://")) &&
        !source.startsWith(QLatin1String("magnet:"), Qt::CaseInsensitive)) {
        return QFileInfo(source).absoluteFilePath();
    }
    return source;
}

int runCommand(const QString& command, const QStringList& args) {
    bool json = false;
    bool all = false;
    bool force = false;
    bool watch = false;
    int intervalMs = 1000;
    QString directory;
    QString checksum;
    QString filter;
    QStringList mirrors;
    QStringList positional;
    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        if (a == QLatin1String("--json")) json = true;
        else if (a == QLatin1String("--all") || a == QLatin1String("-a")) all = true;
        else if (a == QLatin1String("--force") || a == QLatin1String("-f")) force = true;
        else if (a == QLatin1String("--watch") || a == QLatin1String("-w")) watch = true;
        else if (a == QLatin1String("--interval")) {
            bool ok = false;
            const double seconds = i + 1 < args.size() ? args[i + 1].toDouble(&ok) : 0;
            if (!ok || seconds < 0.25 || seconds > 60) { printError(QStringLiteral("--interval needs seconds between 0.25 and 60")); return kExitUsage; }
            intervalMs = static_cast<int>(seconds * 1000);
            ++i;
        }
        else if (a == QLatin1String("-d") || a == QLatin1String("--dir")) {
            if (i + 1 >= args.size()) { printError(QStringLiteral("%1 needs a folder").arg(a)); return kExitUsage; }
            directory = QFileInfo(args[++i]).absoluteFilePath();
        } else if (a.startsWith(QLatin1String("--dir="))) directory = QFileInfo(a.mid(6)).absoluteFilePath();
        else if (a == QLatin1String("-i") || a == QLatin1String("--input")) {
            if (i + 1 >= args.size()) { printError(QStringLiteral("%1 needs a file").arg(a)); return kExitUsage; }
            const QString path = args[++i];
            QFile file;
            const bool opened = path == QLatin1String("-") ? file.open(stdin, QIODevice::ReadOnly | QIODevice::Text)
                                                           : (file.setFileName(path), file.open(QIODevice::ReadOnly | QIODevice::Text));
            if (!opened) { printError(QStringLiteral("can't read %1").arg(path)); return kExitFailed; }
            while (!file.atEnd()) {
                // Lines are capped too, so a huge file without newlines can't fill memory.
                const QString line = QString::fromUtf8(file.readLine(64 * 1024)).trimmed();
                if (!line.isEmpty() && !line.startsWith(QLatin1Char('#'))) positional << line;
                if (positional.size() > 1000) { printError(QStringLiteral("too many links (1000 at most)")); return kExitUsage; }
            }
        } else if (a == QLatin1String("--filter")) {
            if (i + 1 >= args.size()) { printError(QStringLiteral("--filter needs words")); return kExitUsage; }
            filter = args[++i];
        } else if (a == QLatin1String("--mirror")) {
            if (i + 1 >= args.size()) { printError(QStringLiteral("--mirror needs a link")); return kExitUsage; }
            mirrors << args[++i];
        } else if (a == QLatin1String("--checksum")) {
            if (i + 1 >= args.size()) { printError(QStringLiteral("--checksum needs a value")); return kExitUsage; }
            checksum = args[++i];
        } else if (a == QLatin1String("--")) {
            positional << args.mid(i + 1);
            break;
        } else if (a.startsWith(QLatin1Char('-')) && a.size() > 1) {
            printError(QStringLiteral("unknown option %1 (see orbit help)").arg(a));
            return kExitUsage;
        } else positional << a;
    }

    if (watch) {
        if (command != QLatin1String("status")) { printError(QStringLiteral("--watch only works with status")); return kExitUsage; }
        return watchStatus(json, intervalMs);
    }

    const bool isAdd = command == QLatin1String("add");
    if (isAdd && positional.isEmpty()) { printError(QStringLiteral("add needs a link or a .torrent file")); return kExitUsage; }
    const bool needsIds = command == QLatin1String("pause") || command == QLatin1String("resume") ||
                          command == QLatin1String("cancel") || command == QLatin1String("remove");
    if (needsIds && positional.isEmpty() && !all) {
        printError(QStringLiteral("%1 needs download ids (from orbit list) or --all").arg(command));
        return kExitUsage;
    }

    QLocalSocket* socket = connectToOrbit(300);
    const bool opensWindow = command == QLatin1String("show") || command == QLatin1String("new");
    if (!socket && (isAdd || opensWindow)) {
        // Adding (or showing) is worth starting Orbit for; it stays in the tray.
        QStringList launchArgs;
        if (isAdd) launchArgs << QStringLiteral("--background");
        if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), launchArgs)) {
            printError(QStringLiteral("couldn't start Orbit"));
            return kExitFailed;
        }
        socket = connectToOrbit(15000);
        if (socket && command == QLatin1String("show")) {
            delete socket;
            return kExitOk;
        }
    }
    if (!socket) {
        if (command == QLatin1String("status") && !json) printLine(QStringLiteral("Orbit isn't running."));
        else printError(QStringLiteral("Orbit isn't running"));
        return kExitNotRunning;
    }

    int exitCode = kExitOk;
    auto request = [&](const QJsonObject& r) -> std::optional<QJsonObject> {
        auto reply = send(socket, r);
        if (!reply) {
            printError(QStringLiteral("Orbit didn't answer"));
            exitCode = kExitFailed;
        }
        return reply;
    };

    if (isAdd) {
        QJsonArray added;
        for (const QString& arg : positional) {
            QJsonObject r{{"type", "add"}, {"source", absoluteSource(arg)}};
            if (!directory.isEmpty()) r.insert("directory", directory);
            if (!checksum.isEmpty()) r.insert("checksum", checksum);
            if (force) r.insert("force", true);
            if (!mirrors.isEmpty()) r.insert("mirrors", QJsonArray::fromStringList(mirrors));
            const auto reply = request(r);
            if (!reply) break;
            if (!reply->value("ok").toBool()) {
                printError(arg + QStringLiteral(": ") + reply->value("error").toString());
                exitCode = kExitFailed;
                continue;
            }
            if (reply->value("pending").toBool()) {  // a Metalink link, read in the background
                if (!json) printLine(arg + QStringLiteral(": ") + reply->value("message").toString());
                continue;
            }
            QJsonArray downloads = reply->value("downloads").toArray();  // a Metalink's files
            if (downloads.isEmpty()) downloads.append(reply->value("download").toObject());
            for (const QJsonValue& value : downloads) {
                const QJsonObject d = value.toObject();
                added.append(d);
                if (!json) {
                    const QString name = d.value("name").toString();
                    printLine(QStringLiteral("Added %1  %2").arg(d.value("id").toString(), name.isEmpty() ? arg : name));
                }
            }
        }
        if (json) printLine(QString::fromUtf8(QJsonDocument(added).toJson(QJsonDocument::Indented)).trimmed());
    } else if (command == QLatin1String("list") || command == QLatin1String("status")) {
        if (const auto reply = request(QJsonObject{{"type", command}})) {
            if (json) {
                const QJsonValue value = command == QLatin1String("list") ? reply->value("downloads") : QJsonValue(*reply);
                const QJsonDocument doc = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
                printLine(QString::fromUtf8(doc.toJson(QJsonDocument::Indented)).trimmed());
            } else if (command == QLatin1String("list")) {
                printDownloads(reply->value("downloads").toArray());
            } else {
                printStatus(*reply);
            }
        }
    } else if (needsIds) {
        QJsonObject r{{"type", command}, {"all", all}};
        if (!all) r.insert("ids", QJsonArray::fromStringList(positional));
        if (const auto reply = request(r)) {
            if (!reply->value("ok").toBool()) {
                printError(reply->value("error").toString());
                exitCode = kExitFailed;
            } else {
                const int changed = reply->value("changed").toInt();
                const QString verb = command == QLatin1String("pause") ? QStringLiteral("Paused")
                                     : command == QLatin1String("resume") ? QStringLiteral("Resumed")
                                     : command == QLatin1String("cancel") ? QStringLiteral("Cancelled")
                                                                          : QStringLiteral("Removed");
                printLine(QStringLiteral("%1 %2 download%3").arg(verb).arg(changed).arg(changed == 1 ? "" : "s"));
            }
        }
    } else if (opensWindow) {
        request(QJsonObject{{"type", "show"}, {"add", command == QLatin1String("new")}});
    } else if (command == QLatin1String("config")) {
        const QString action = positional.value(0, QStringLiteral("list"));
        const qsizetype wanted = action == QLatin1String("set") ? 3 : action == QLatin1String("get") ? 2 : 1;
        if (positional.size() < wanted) {
            printError(action == QLatin1String("set") ? QStringLiteral("config set needs a name and a value")
                                                      : QStringLiteral("config get needs a name (see orbit config list)"));
            exitCode = kExitUsage;
        } else if (const auto reply = request(QJsonObject{{"type", "config"}, {"action", action},
                                                          {"name", positional.value(1)}, {"value", positional.value(2)}})) {
            if (!reply->value("ok").toBool()) {
                printError(reply->value("error").toString());
                exitCode = kExitFailed;
            } else if (action == QLatin1String("list")) {
                const QJsonArray settings = reply->value("settings").toArray();
                if (json) {
                    printLine(QString::fromUtf8(QJsonDocument(settings).toJson(QJsonDocument::Indented)).trimmed());
                } else {
                    for (const QJsonValue& v : settings) {
                        const QJsonObject s = v.toObject();
                        printLine(QStringLiteral("%1 = %2").arg(s.value("name").toString(), -32).arg(s.value("value").toVariant().toString()));
                    }
                }
            } else {
                printLine(json ? QString::fromUtf8(QJsonDocument(*reply).toJson(QJsonDocument::Compact))
                               : reply->value("value").toVariant().toString());
            }
        }
    } else if (command == QLatin1String("remote")) {
        const QString action = positional.value(0, QStringLiteral("status"));
        QJsonObject r{{"type", "remote"}, {"action", action}};
        bool ready = true;
        if (action == QLatin1String("set-password")) {
            if (positional.size() < 2) {
                printError(QStringLiteral("remote set-password needs a username"));
                exitCode = kExitUsage;
                ready = false;
            } else {
                const QString password = readSecret("Password: ");
                if (::isatty(STDIN_FILENO) && readSecret("Again: ") != password) {
                    printError(QStringLiteral("the passwords don't match"));
                    exitCode = kExitFailed;
                    ready = false;
                }
                r.insert("username", positional.value(1));
                r.insert("password", password);
            }
        }
        if (ready) {
            if (const auto reply = request(r)) {
                if (!reply->value("ok").toBool()) {
                    printError(reply->value("error").toString());
                    exitCode = kExitFailed;
                } else if (json) {
                    printLine(QString::fromUtf8(QJsonDocument(*reply).toJson(QJsonDocument::Indented)).trimmed());
                } else {
                    printRemoteStatus(*reply);
                }
            }
        }
    } else if (command == QLatin1String("feeds")) {
        const QString action = positional.value(0, QStringLiteral("list"));
        QJsonObject r{{"type", "feeds"}, {"action", action}, {"url", positional.value(1)}, {"filter", filter}};
        if (!directory.isEmpty()) r.insert("directory", directory);
        if ((action == QLatin1String("add") || action == QLatin1String("remove")) && positional.size() < 2) {
            printError(QStringLiteral("feeds %1 needs the feed's address").arg(action));
            exitCode = kExitUsage;
        } else if (const auto reply = request(r)) {
            if (!reply->value("ok").toBool()) {
                printError(reply->value("error").toString());
                exitCode = kExitFailed;
            } else if (json) {
                printLine(QString::fromUtf8(QJsonDocument(reply->value("feeds").toArray()).toJson(QJsonDocument::Indented)).trimmed());
            } else {
                const QJsonArray feeds = reply->value("feeds").toArray();
                if (feeds.isEmpty()) printLine(QStringLiteral("No feeds."));
                for (const QJsonValue& v : feeds) {
                    const QJsonObject f = v.toObject();
                    printLine(QStringLiteral("%1%2  %3").arg(f.value("enabled").toBool() ? QString() : QStringLiteral("(off) "),
                                                             f.value("name").toString(), f.value("url").toString()));
                    QString detail = f.value("filter").toString().isEmpty() ? QStringLiteral("every new item")
                                                                            : QStringLiteral("filter: ") + f.value("filter").toString();
                    detail += QStringLiteral(", %1 downloaded").arg(f.value("matched").toInt());
                    if (!f.value("lastError").toString().isEmpty()) detail += QStringLiteral(", ") + f.value("lastError").toString();
                    printLine(QStringLiteral("    ") + detail);
                }
            }
        }
    }
    socket->disconnectFromServer();
    delete socket;
    return exitCode;
}

}

std::optional<int> run(int argc, char* argv[]) {
    if (argc < 2) return std::nullopt;
    QString command = QString::fromLocal8Bit(argv[1]);
    if (command == QLatin1String("ls")) command = QStringLiteral("list");
    if (command == QLatin1String("rm")) command = QStringLiteral("remove");

    if (command == QLatin1String("help") || command == QLatin1String("--help") || command == QLatin1String("-h")) {
        std::fputs(kUsage, stdout);
        return kExitOk;
    }
    if (command == QLatin1String("--version") || command == QLatin1String("-v")) {
        std::printf("Orbit %s\n", ORBIT_VERSION);
        return kExitOk;
    }
    static const QStringList commands{
        QStringLiteral("add"), QStringLiteral("list"), QStringLiteral("status"), QStringLiteral("pause"),
        QStringLiteral("resume"), QStringLiteral("cancel"), QStringLiteral("remove"), QStringLiteral("show"), QStringLiteral("new"),
        QStringLiteral("feeds"), QStringLiteral("config"), QStringLiteral("remote"),
    };
    if (!commands.contains(command)) return std::nullopt;

    QCoreApplication app(argc, argv);
    QStringList args;
    for (int i = 2; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    return runCommand(command, args);
}

}
