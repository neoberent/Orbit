#include "core/TorrentEngine.hpp"
#include "core/InterfaceDns.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

#include "utils/FileUtils.hpp"
#include "utils/Logger.hpp"
#include "utils/StringUtils.hpp"

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/announce_entry.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/span.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp> 
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <list>
#include <set>
#include <system_error>
#include <vector>

namespace lt = libtorrent;

namespace orbit::core {

namespace {

DownloadStats toDownloadStats(const lt::torrent_status& st) {
    DownloadStats stats;
    stats.bytesDownloaded = static_cast<std::uint64_t>(std::max<std::int64_t>(0, st.total_done));
    stats.bytesUploaded = static_cast<std::uint64_t>(std::max<std::int64_t>(0, st.total_upload));
    stats.totalBytes = static_cast<std::uint64_t>(std::max<std::int64_t>(0, st.total_wanted));
    stats.downloadRateBytesPerSec = static_cast<double>(st.download_rate);
    stats.uploadRateBytesPerSec = static_cast<double>(st.upload_rate);
    stats.numPeers = st.num_peers;
    stats.numSeeds = st.num_seeds;
    stats.activeConnections = st.num_peers;
    if (st.download_rate > 0 && stats.totalBytes > stats.bytesDownloaded) {
        const auto remaining = stats.totalBytes - stats.bytesDownloaded;
        stats.eta = std::chrono::seconds(
            static_cast<std::int64_t>(static_cast<double>(remaining) / static_cast<double>(st.download_rate)));
    }
    return stats;
}

DownloadState toDownloadState(const lt::torrent_status& st, bool seedingStopped) {
    if (st.errc) return DownloadState::Error;
    if (!st.has_metadata) return DownloadState::Connecting;
    const bool paused = (st.flags & lt::torrent_flags::paused) == lt::torrent_flags::paused;
    if (paused && seedingStopped && (st.is_seeding || st.is_finished)) return DownloadState::Completed;
    if (paused) return DownloadState::Paused;
    if (st.is_seeding) return DownloadState::Seeding;
    if (st.is_finished) return DownloadState::Completed;
    return DownloadState::Downloading;
}

std::filesystem::path dhtStateFilePath() { return utils::appDataDir() / "dht_state.dat"; }

std::filesystem::path resumeDataDir() { return utils::appDataDir() / "resume"; }

std::filesystem::path resumeDataFilePath(DownloadId id) {
    return resumeDataDir() / (std::to_string(id) + ".fastresume");
}

int toLimit(std::uint64_t bytesPerSec) {
    return static_cast<int>(std::min<std::uint64_t>(bytesPerSec, static_cast<std::uint64_t>(INT32_MAX)));
}

// The name the user sees for a torrent: its single file, or its top-level folder. Taken
// from the (possibly renamed) file layout rather than the metadata name, so a rename
// survives a restart.
std::string currentFilePath(const lt::torrent_info& ti, const lt::renamed_files& renames, int index) {
    return renames.file_path(ti.layout(), lt::file_index_t{index});
}

int firstRealFile(const lt::file_storage& fs) {
    for (int i = 0; i < fs.num_files(); ++i) {
        if (!fs.pad_file_at(lt::file_index_t{i})) return i;
    }
    return 0;
}

std::string rootName(const lt::torrent_info& ti, const lt::renamed_files& renames) {
    const lt::file_storage& fs = ti.layout();
    if (fs.num_files() == 0) return ti.name();
    const std::string first = currentFilePath(ti, renames, firstRealFile(fs));
    if (fs.num_files() == 1) return first;
    const auto sep = first.find_first_of("/\\");
    return sep == std::string::npos ? first : first.substr(0, sep);
}

const std::vector<std::string>& supplementalPublicTrackers() {
    static const std::vector<std::string> trackers = {
        "udp://tracker.opentrackr.org:1337/announce",
        "udp://open.tracker.cl:1337/announce",
        "udp://tracker.openbittorrent.com:6969/announce",
        "udp://exodus.desync.com:6969/announce",
        "udp://tracker.torrent.eu.org:451/announce",
        "udp://open.stealth.si:80/announce",
        "udp://tracker.moeking.me:6969/announce",
        "udp://explodie.org:6969/announce",
        "udp://tracker-udp.gbitt.info:80/announce",
        "udp://tracker.dler.org:6969/announce",
        "udp://ttk2.nbaonlineservice.com:6969/announce",
        "udp://tracker.tiny-vps.com:6969/announce",
        "udp://bt1.archive.org:6969/announce",
        "udp://bt2.archive.org:6969/announce",
        "udp://vibe.sleepyinternetfun.xyz:1738/announce",
        "udp://tracker.bittor.pw:1337/announce",
        "http://tracker.openbittorrent.com:80/announce",
        "https://tracker.gbitt.info:443/announce",
    };
    return trackers;
}

}

struct TorrentEngine::Impl {
    std::unique_ptr<lt::session> session;
    TorrentEngineSettings settings;
    bool reachabilityWarnLogged{false};
    std::chrono::steady_clock::time_point lastResumeCheckpoint{std::chrono::steady_clock::now()};

    struct Entry {
        lt::torrent_handle handle;
        DownloadRequest request;
        DownloadState lastState{DownloadState::Queued};
        bool sequential{false};
        bool nameReported{false};
        bool seedingStopped{false};
        // Set while the interface lock has swapped this torrent's trackers (see lockTrackers).
        bool trackersLocked{false};
        std::vector<lt::announce_entry> originalTrackers{};
        std::vector<std::string> originalUrlSeeds{};
    };

    ProxySettings userProxy;
    ProxySettings relayProxy;

    double seedRatioLimit{0};
    int seedMinutesLimit{0};
    bool seedAfterDownload{false};
    bool noCowOnBtrfs{true};
    bool addPublicTrackers{true};

    mutable std::mutex mutex;
    std::unordered_map<DownloadId, Entry> entries;

    // Background lookups of locked torrents' UDP trackers. Declared after the entries
    // they update (and the session), so they're stopped and joined before those go.
    struct Lookup {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::mutex lookupsMutex;
    std::list<Lookup> lookups;

    // Looks up the DHT's bootstrap routers through the locked interface. Declared last, so
    // it's stopped and joined before the session it configures goes away.
    std::jthread bootstrapResolver;
};

namespace {
const char* const kDefaultBootstrapNodes = "dht.libtorrent.org:25401";
const std::pair<const char*, int> kBootstrapRouters[] = {
    {"dht.libtorrent.org", 25401}, {"router.bittorrent.com", 6881}, {"dht.transmissionbt.com", 6881}};
void prepareNoCow(const lt::torrent_info& ti, const std::string& savePath);
}

TorrentEngine::TorrentEngine(ProgressReporter reporter, TorrentEngineSettings settings)
    : reporter_(std::move(reporter)) {
    impl_ = std::make_unique<Impl>();
    impl_->settings = settings;

    if (impl_->settings.defaultSavePath.empty()) {
        impl_->settings.defaultSavePath = utils::defaultDownloadsDir();
    }
    utils::ensureDirectoryExists(impl_->settings.defaultSavePath);

    lt::settings_pack pack;
    pack.set_str(lt::settings_pack::user_agent, std::string("Orbit/") + ORBIT_VERSION);
    pack.set_int(lt::settings_pack::alert_mask, lt::alert_category::error | lt::alert_category::status |
                                                     lt::alert_category::tracker | lt::alert_category::port_mapping |
                                                     lt::alert_category::dht | lt::alert_category::storage);
    pack.set_bool(lt::settings_pack::enable_dht, settings.enableDht);
    pack.set_int(lt::settings_pack::download_rate_limit, settings.downloadRateLimitBytesPerSec);
    pack.set_int(lt::settings_pack::upload_rate_limit, settings.uploadRateLimitBytesPerSec);
    if (settings.networkInterface.empty()) {
        pack.set_str(lt::settings_pack::listen_interfaces,
                     "0.0.0.0:" + std::to_string(settings.listenPort) + ",[::]:" + std::to_string(settings.listenPort));
    } else {
        // Locked before the session exists, so not even its first packets or lookups
        // leave over another connection.
        pack.set_str(lt::settings_pack::listen_interfaces, settings.networkInterface + ":" + std::to_string(settings.listenPort));
        pack.set_str(lt::settings_pack::outgoing_interfaces, settings.networkInterface);
        pack.set_str(lt::settings_pack::dht_bootstrap_nodes, "");
    }

    pack.set_int(lt::settings_pack::mixed_mode_algorithm, lt::settings_pack::prefer_tcp);

    pack.set_int(lt::settings_pack::connections_limit, 400);
    pack.set_int(lt::settings_pack::torrent_connect_boost, 50);
    pack.set_int(lt::settings_pack::max_peerlist_size, 8000);
    pack.set_int(lt::settings_pack::peer_connect_timeout, 8);
    pack.set_int(lt::settings_pack::min_reconnect_time, 15);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    // Port mapping asks the router of the normal connection, which is no use (and a
    // giveaway) while torrents are locked to a VPN's interface.
    pack.set_bool(lt::settings_pack::enable_upnp, settings.networkInterface.empty());
    pack.set_bool(lt::settings_pack::enable_natpmp, settings.networkInterface.empty());

    pack.set_bool(lt::settings_pack::announce_to_all_trackers, true);
    pack.set_bool(lt::settings_pack::announce_to_all_tiers, true);

    lt::session_params params;
    if (std::ifstream in{dhtStateFilePath(), std::ios::binary}; in) {
        const std::vector<char> buf{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        if (!buf.empty()) {
            try {
                params = lt::read_session_params(lt::span<char const>(buf.data(), static_cast<int>(buf.size())),
                                                  lt::session_handle::save_dht_state);
                ORBIT_LOG_INFO("TorrentEngine", "Restored DHT routing table from previous session");
            } catch (const std::exception& e) {
                ORBIT_LOG_WARN("TorrentEngine", std::string("Failed to load saved DHT state: ") + e.what());
            }
        }
    }
    params.settings = pack;

    impl_->session = std::make_unique<lt::session>(std::move(params));

    alertThread_ = std::jthread([this](std::stop_token st) { alertLoop(st); });
    if (!impl_->settings.networkInterface.empty()) resolveBootstrapThrough(impl_->settings.networkInterface);
}

void TorrentEngine::resolveBootstrapThrough(const std::string& interfaceName) {
    // libtorrent would look the routers' names up with the system resolver, outside the
    // locked interface; give it addresses found through the interface instead.
    impl_->bootstrapResolver = std::jthread([this, interfaceName](std::stop_token stop) {
        std::string nodes;
        for (const auto& [host, port] : kBootstrapRouters) {
            if (stop.stop_requested()) return;
            for (const std::string& address : InterfaceDns::resolve(host, interfaceName)) {
                if (!nodes.empty()) nodes += ',';
                nodes += (address.find(':') != std::string::npos ? "[" + address + "]" : address) + ":" + std::to_string(port);
            }
        }
        if (stop.stop_requested() || nodes.empty()) return;
        lt::settings_pack pack;
        pack.set_str(lt::settings_pack::dht_bootstrap_nodes, nodes);
        impl_->session->apply_settings(std::move(pack));
        ORBIT_LOG_INFO("TorrentEngine", "DHT bootstrap routers looked up through " + interfaceName);
    });
}

TorrentEngine::~TorrentEngine() {
    if (!impl_ || !impl_->session) return;

    alertThread_.request_stop();
    if (alertThread_.joinable()) alertThread_.join();

    saveAllResumeDataBlocking();

    try {
        const lt::session_params sp = impl_->session->session_state(lt::session_handle::save_dht_state);
        const std::vector<char> buf = lt::write_session_params_buf(sp, lt::session_handle::save_dht_state);
        utils::ensureDirectoryExists(utils::appDataDir());
        utils::writeFileAtomically(dhtStateFilePath(), std::string(buf.begin(), buf.end()));
    } catch (const std::exception& e) {
        ORBIT_LOG_WARN("TorrentEngine", std::string("Failed to save DHT state: ") + e.what());
    }
}

void TorrentEngine::writeResumeData(const lt::torrent_handle& handle, const lt::add_torrent_params& params) {
    DownloadId id = 0;
    {
        std::scoped_lock lock(impl_->mutex);
        for (auto& [entryId, entry] : impl_->entries) {
            if (entry.handle == handle) {
                id = entryId;
                break;
            }
        }
    }
    if (id == 0) return;

    std::error_code fsec;
    std::filesystem::create_directories(resumeDataDir(), fsec);

    // While the lock has swapped this torrent's trackers for addresses, the resume data
    // keeps the real ones, so they come back as they were (and are looked up afresh).
    bool locked = false;
    lt::add_torrent_params patched;
    {
        std::scoped_lock lock(impl_->mutex);
        const auto it = impl_->entries.find(id);
        if (it != impl_->entries.end() && it->second.trackersLocked) {
            locked = true;
            patched = params;
            patched.trackers.clear();
            patched.tracker_tiers.clear();
            for (const lt::announce_entry& tracker : it->second.originalTrackers) {
                patched.trackers.push_back(tracker.url);
                patched.tracker_tiers.push_back(tracker.tier);
            }
            patched.url_seeds = it->second.originalUrlSeeds;
        }
    }
    const std::vector<char> buf = lt::write_resume_data_buf(locked ? patched : params);
    // Written in place, a crash mid-write left a file libtorrent can't read, and the
    // torrent restarted from its source (gone, for an uploaded .torrent) or rechecked.
    if (!utils::writeFileAtomically(resumeDataFilePath(id), std::string(buf.begin(), buf.end()))) {
        ORBIT_LOG_WARN("TorrentEngine", "Couldn't save resume data for download " + std::to_string(id));
    }
}

void TorrentEngine::saveAllResumeDataBlocking() {
    std::vector<std::pair<DownloadId, lt::torrent_handle>> snapshot;
    {
        std::scoped_lock lock(impl_->mutex);
        snapshot.reserve(impl_->entries.size());
        for (auto& [id, entry] : impl_->entries) {
            if (entry.handle.is_valid()) snapshot.emplace_back(id, entry.handle);
        }
    }
    if (snapshot.empty()) return;

    int pending = 0;
    for (auto& [id, handle] : snapshot) {
        if (!handle.status().has_metadata) continue;
        handle.save_resume_data(lt::torrent_handle::save_info_dict);
        ++pending;
    }
    if (pending == 0) return;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (pending > 0 && std::chrono::steady_clock::now() < deadline) {
        impl_->session->wait_for_alert(std::chrono::milliseconds(300));
        std::vector<lt::alert*> alerts;
        impl_->session->pop_alerts(&alerts);
        for (lt::alert* a : alerts) {
            if (auto* ok = lt::alert_cast<lt::save_resume_data_alert>(a)) {
                --pending;
                writeResumeData(ok->handle, ok->params);
            } else if (lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
                --pending;
            }
        }
    }
}

bool TorrentEngine::parseTorrentFile(const std::string& path, std::string& outName,
                                      std::vector<TorrentFileEntry>& outFiles, std::string& outError) {
    outName.clear();
    outFiles.clear();

    lt::add_torrent_params atp;
    try {
        atp = lt::load_torrent_file(path);
    } catch (const std::exception& e) {
        outError = e.what();
        return false;
    }

    if (!atp.ti) {
        outError = "Torrent file has no metadata";
        return false;
    }

    outName = atp.ti->name();
    const lt::file_storage& files = atp.ti->layout();
    outFiles.reserve(static_cast<size_t>(files.num_files()));
    for (int i = 0; i < files.num_files(); ++i) {
        const lt::file_index_t idx{i};
        TorrentFileEntry entry;
        entry.path = files.file_path(idx);
        entry.size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, files.file_size(idx)));
        outFiles.push_back(std::move(entry));
    }
    return true;
}

void TorrentEngine::start(DownloadId id, const DownloadRequest& request) {
    lt::add_torrent_params atp;
    bool loadedFromResume = false;

    const std::filesystem::path resumePath = resumeDataFilePath(id);
    std::error_code resumeExistsEc;
    if (std::filesystem::exists(resumePath, resumeExistsEc)) {
        std::ifstream in(resumePath, std::ios::binary);
        if (in) {
            const std::vector<char> buf{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            if (!buf.empty()) {
                lt::error_code lec;
                atp = lt::read_resume_data(lt::span<char const>(buf.data(), static_cast<int>(buf.size())), lec);
                if (!lec) {
                    loadedFromResume = true;
                } else {
                    ORBIT_LOG_WARN("TorrentEngine", "Failed to parse saved resume data for download " +
                                                         std::to_string(id) + ": " + lec.message());
                }
            }
        }
    }

    if (!loadedFromResume) {
        if (utils::isMagnetUri(request.source)) {
            lt::error_code ec;
            atp = lt::parse_magnet_uri(request.source, ec);
            if (ec) {
                reporter_.reportError(id, "Invalid magnet link: " + ec.message());
                reporter_.reportStateChanged(id, DownloadState::Error);
                return;
            }
        } else {
            try {
                atp = lt::load_torrent_file(request.source);
            } catch (const std::exception& e) {
                reporter_.reportError(id, std::string("Failed to load torrent file: ") + e.what());
                reporter_.reportStateChanged(id, DownloadState::Error);
                return;
            }
        }

        bool addPublic = true;
        {
            std::scoped_lock lock(impl_->mutex);
            addPublic = impl_->addPublicTrackers;
        }
        if (addPublic && atp.ti && !atp.ti->priv()) {
            for (const std::string& tracker : supplementalPublicTrackers()) {
                if (std::find(atp.trackers.begin(), atp.trackers.end(), tracker) == atp.trackers.end()) {
                    atp.trackers.push_back(tracker);
                }
            }
        }

        if (atp.ti && !request.torrentExcludedFileIndices.empty()) {
            const lt::file_storage& files = atp.ti->layout();
            std::vector<lt::download_priority_t> priorities(static_cast<size_t>(files.num_files()),
                                                              lt::default_priority);
            for (int excluded : request.torrentExcludedFileIndices) {
                if (excluded >= 0 && static_cast<size_t>(excluded) < priorities.size()) {
                    priorities[static_cast<size_t>(excluded)] = lt::dont_download;
                }
            }
            atp.file_priorities = std::move(priorities);
        }

        const std::string savePath =
            !request.destinationDir.empty() ? request.destinationDir : impl_->settings.defaultSavePath.string();
        utils::ensureDirectoryExists(savePath);
        atp.save_path = savePath;
    } else {
        // Resume data already carries save_path, file_priorities, and piece state.
        utils::ensureDirectoryExists(atp.save_path);
    }

    // Adding the same torrent twice (e.g. its .torrent file and then its magnet link)
    // would make two list entries share one libtorrent handle, so refuse it up front.
    const lt::info_hash_t hashes = atp.ti ? atp.ti->info_hashes() : atp.info_hashes;
    if (hashes.has_v1() || hashes.has_v2()) {
        const lt::torrent_handle existing = impl_->session->find_torrent(hashes.get_best());
        bool ownedByAnotherTask = false;
        if (existing.is_valid()) {
            std::scoped_lock lock(impl_->mutex);
            for (const auto& [otherId, entry] : impl_->entries) {
                if (otherId != id && entry.handle == existing) ownedByAnotherTask = true;
            }
        }
        if (ownedByAnotherTask) {
            reporter_.reportError(id, "This torrent is already in your downloads");
            reporter_.reportStateChanged(id, DownloadState::Error);
            return;
        }
    }

    bool noCow = false;
    {
        std::scoped_lock lock(impl_->mutex);
        noCow = impl_->noCowOnBtrfs;
    }
    if (noCow && atp.ti) prepareNoCow(*atp.ti, atp.save_path);

    // While locked, the torrent is added paused, so it can't announce to a tracker by name
    // before lockTrackers has swapped them; then it gets its own flags back.
    const bool locked = !impl_->settings.networkInterface.empty();
    const lt::torrent_flags_t startFlags = atp.flags;
    if (locked) {
        atp.flags |= lt::torrent_flags::paused;
        atp.flags &= ~lt::torrent_flags::auto_managed;
    }

    lt::error_code ec;
    lt::torrent_handle handle = impl_->session->add_torrent(std::move(atp), ec);
    if (ec || !handle.is_valid()) {
        reporter_.reportError(id, "Failed to add torrent: " + ec.message());
        reporter_.reportStateChanged(id, DownloadState::Error);
        return;
    }
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->entries[id] = Impl::Entry{.handle = handle, .request = request, .lastState = DownloadState::Connecting,
                                         .sequential = request.sequential};
    }
    if (locked) {
        lockTrackers(id, handle);
        handle.set_flags(startFlags, lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
    }

    handle.force_dht_announce();
    handle.force_reannounce();
    if (request.speedLimitBytesPerSec > 0) handle.set_download_limit(toLimit(request.speedLimitBytesPerSec));
    if (request.uploadLimitBytesPerSec > 0) handle.set_upload_limit(toLimit(request.uploadLimitBytesPerSec));
    if (request.sequential) handle.set_flags(lt::torrent_flags::sequential_download);
    reporter_.reportStateChanged(id, DownloadState::Connecting);
}

void TorrentEngine::pause(DownloadId id) {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || !it->second.handle.is_valid()) return;
    // An auto-managed torrent gets resumed again by libtorrent's queueing, so a user
    // pause has to take it out of auto-management first or it won't stay paused.
    it->second.handle.unset_flags(lt::torrent_flags::auto_managed);
    it->second.handle.pause();
}

void TorrentEngine::resume(DownloadId id) {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || !it->second.handle.is_valid()) return;
    it->second.seedingStopped = false;
    it->second.handle.set_flags(lt::torrent_flags::auto_managed);
    it->second.handle.resume();
}

void TorrentEngine::stopSeeding(DownloadId id) {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || !it->second.handle.is_valid()) return;
    it->second.seedingStopped = true;
    it->second.handle.unset_flags(lt::torrent_flags::auto_managed);
    it->second.handle.pause();
}

void TorrentEngine::cancel(DownloadId id) {
    lt::torrent_handle handle;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end()) return;
        handle = it->second.handle;
        impl_->entries.erase(it);
    }
    if (handle.is_valid()) {
        impl_->session->remove_torrent(handle, lt::session::delete_files);
    }
    std::error_code fsec;
    std::filesystem::remove(resumeDataFilePath(id), fsec);
    reporter_.reportStateChanged(id, DownloadState::Cancelled);
}

void TorrentEngine::forget(DownloadId id) {
    lt::torrent_handle handle;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end()) return;
        handle = it->second.handle;
        impl_->entries.erase(it);
    }
    if (handle.is_valid()) {
        impl_->session->remove_torrent(handle);
    }
    std::error_code fsec;
    std::filesystem::remove(resumeDataFilePath(id), fsec);
}

bool TorrentEngine::forgetAndDeleteFiles(DownloadId id) {
    lt::torrent_handle handle;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end()) return false;
        handle = it->second.handle;
        impl_->entries.erase(it);
    }
    if (handle.is_valid()) {
        impl_->session->remove_torrent(handle, lt::session::delete_files);
    }
    std::error_code fsec;
    std::filesystem::remove(resumeDataFilePath(id), fsec);
    return handle.is_valid();
}

DownloadStats TorrentEngine::stats(DownloadId id) const {
    lt::torrent_handle handle;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end()) return {};
        handle = it->second.handle;
    }
    if (!handle.is_valid()) return {};
    return toDownloadStats(handle.status());
}

bool TorrentEngine::hasEntry(DownloadId id) const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->entries.contains(id);
}

void TorrentEngine::setSequentialDownload(DownloadId id, bool sequential) {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || !it->second.handle.is_valid()) return;
    it->second.sequential = sequential;
    if (sequential) {
        it->second.handle.set_flags(lt::torrent_flags::sequential_download);
    } else {
        it->second.handle.unset_flags(lt::torrent_flags::sequential_download);
    }
}

void TorrentEngine::setDownloadRateLimit(int bytesPerSec) {
    lt::settings_pack pack;
    pack.set_int(lt::settings_pack::download_rate_limit, bytesPerSec);
    impl_->session->apply_settings(std::move(pack));
    impl_->settings.downloadRateLimitBytesPerSec = bytesPerSec;
}

void TorrentEngine::setUploadRateLimit(int bytesPerSec) {
    lt::settings_pack pack;
    pack.set_int(lt::settings_pack::upload_rate_limit, bytesPerSec);
    impl_->session->apply_settings(std::move(pack));
    impl_->settings.uploadRateLimitBytesPerSec = bytesPerSec;
}

void TorrentEngine::setTorrentRateLimits(DownloadId id, std::uint64_t downloadBytesPerSec, std::uint64_t uploadBytesPerSec) {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || !it->second.handle.is_valid()) return;
    it->second.handle.set_download_limit(downloadBytesPerSec > 0 ? toLimit(downloadBytesPerSec) : -1);
    it->second.handle.set_upload_limit(uploadBytesPerSec > 0 ? toLimit(uploadBytesPerSec) : -1);
}

void TorrentEngine::setProxy(const ProxySettings& proxy) {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->userProxy = proxy;
    }
    applyProxy();
}

void TorrentEngine::setRelayProxy(const ProxySettings& relay) {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->relayProxy = relay;
    }
    applyProxy();
}

void TorrentEngine::applyProxy() {
    ProxySettings user;
    ProxySettings relay;
    bool locked = false;
    {
        std::scoped_lock lock(impl_->mutex);
        user = impl_->userProxy;
        relay = impl_->relayProxy;
        locked = !impl_->settings.networkInterface.empty();
    }
    // The user's own proxy carries everything. The relay only carries the HTTP(S)
    // trackers (their names then never reach the system resolver): peers and the DHT are
    // bound to the interface by libtorrent itself, and UDP trackers go by address.
    const bool useRelay = !user.enabled() && locked && relay.enabled();
    const ProxySettings& proxy = user.enabled() ? user : relay;
    lt::settings_pack pack;
    if (!user.enabled() && !useRelay) {
        pack.set_int(lt::settings_pack::proxy_type, lt::settings_pack::none);
    } else {
        const bool auth = !proxy.username.empty();
        const int type = proxy.type == ProxySettings::Type::Socks5
                             ? (auth ? lt::settings_pack::socks5_pw : lt::settings_pack::socks5)
                             : (auth ? lt::settings_pack::http_pw : lt::settings_pack::http);
        pack.set_int(lt::settings_pack::proxy_type, type);
        pack.set_str(lt::settings_pack::proxy_hostname, proxy.host);
        pack.set_int(lt::settings_pack::proxy_port, proxy.port);
        pack.set_str(lt::settings_pack::proxy_username, proxy.username);
        pack.set_str(lt::settings_pack::proxy_password, proxy.password);
        pack.set_bool(lt::settings_pack::proxy_hostnames, true);
        pack.set_bool(lt::settings_pack::proxy_peer_connections, !useRelay);
        pack.set_bool(lt::settings_pack::proxy_tracker_connections, true);
    }
    impl_->session->apply_settings(std::move(pack));
}

namespace {

// "udp://tracker.example.org:6969/announce" -> its host, or empty for an address or
// anything that isn't a UDP tracker.
std::string udpTrackerName(const std::string& url) {
    std::string scheme = url.substr(0, std::min<size_t>(url.size(), 6));
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (scheme != "udp://") return {};
    const std::string host = InterfaceDns::hostOf(url);
    return InterfaceDns::isIpLiteral(host) ? std::string() : host;
}

// The same tracker URL with its host replaced by an address.
std::string withAddress(const std::string& url, const std::string& address) {
    const size_t authorityStart = url.find("://") + 3;
    size_t authorityEnd = url.find_first_of("/?#", authorityStart);
    if (authorityEnd == std::string::npos) authorityEnd = url.size();
    std::string authority = url.substr(authorityStart, authorityEnd - authorityStart);
    if (const auto at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    const auto colon = authority.rfind(':');
    const std::string port = colon == std::string::npos ? std::string() : authority.substr(colon);
    const std::string host = address.find(':') != std::string::npos ? "[" + address + "]" : address;
    return url.substr(0, authorityStart) + host + port + url.substr(authorityEnd);
}

}

void TorrentEngine::lockTrackers(DownloadId id, const lt::torrent_handle& handle) {
    if (!handle.is_valid()) return;
    const std::vector<lt::announce_entry> current = handle.trackers();
    const std::set<std::string> seeds = handle.url_seeds();
    std::string interfaceName;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end() || it->second.trackersLocked) return;
        it->second.trackersLocked = true;
        it->second.originalTrackers = current;
        it->second.originalUrlSeeds.assign(seeds.begin(), seeds.end());
        interfaceName = impl_->settings.networkInterface;
    }

    std::vector<lt::announce_entry> kept;
    std::vector<lt::announce_entry> named;
    for (const lt::announce_entry& tracker : current) {
        (udpTrackerName(tracker.url).empty() ? kept : named).push_back(tracker);
    }
    if (!named.empty()) handle.replace_trackers(kept);
    // Web seeds are peer connections, which the relay doesn't carry.
    for (const std::string& seed : seeds) handle.remove_url_seed(seed);
    if (named.empty()) return;

    auto done = std::make_shared<std::atomic<bool>>(false);
    std::jthread lookup([this, id, handle, named, interfaceName, done](std::stop_token stop) {
        int added = 0;
        for (const lt::announce_entry& tracker : named) {
            if (stop.stop_requested()) break;
            const std::vector<std::string> addresses = InterfaceDns::resolve(udpTrackerName(tracker.url), interfaceName);
            if (addresses.empty()) continue;
            // One address per tracker (IPv4 first: many VPNs carry no IPv6).
            const auto v4 = std::find_if(addresses.begin(), addresses.end(), [](const std::string& a) { return a.find(':') == std::string::npos; });
            lt::announce_entry entry(withAddress(tracker.url, v4 != addresses.end() ? *v4 : addresses.front()));
            entry.tier = tracker.tier;
            {
                // Unlocked (or removed) meanwhile: the originals are back already.
                std::scoped_lock lock(impl_->mutex);
                const auto it = impl_->entries.find(id);
                if (it == impl_->entries.end() || !it->second.trackersLocked || it->second.handle != handle) break;
            }
            handle.add_tracker(entry);
            ++added;
        }
        ORBIT_LOG_INFO("TorrentEngine", "Download " + std::to_string(id) + ": " + std::to_string(added) + " of " +
                                            std::to_string(named.size()) + " UDP trackers looked up through " + interfaceName);
        done->store(true);
    });
    std::scoped_lock lock(impl_->lookupsMutex);
    impl_->lookups.remove_if([](const Impl::Lookup& l) { return l.done->load(); });
    impl_->lookups.push_back(Impl::Lookup{std::move(lookup), done});
}

void TorrentEngine::unlockTrackers(DownloadId id) {
    lt::torrent_handle handle;
    std::vector<lt::announce_entry> trackers;
    std::vector<std::string> seeds;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end() || !it->second.trackersLocked) return;
        it->second.trackersLocked = false;
        handle = it->second.handle;
        trackers = std::move(it->second.originalTrackers);
        seeds = std::move(it->second.originalUrlSeeds);
        it->second.originalTrackers.clear();
        it->second.originalUrlSeeds.clear();
    }
    if (!handle.is_valid()) return;
    handle.replace_trackers(trackers);
    for (const std::string& seed : seeds) handle.add_url_seed(seed);
}

void TorrentEngine::setNetworkInterface(const std::string& interfaceName) {
    const std::string port = std::to_string(impl_->settings.listenPort);
    lt::settings_pack pack;
    // libtorrent takes device names in both lists; with a device that's down it neither
    // listens nor connects, so peers never see the real address.
    pack.set_str(lt::settings_pack::listen_interfaces,
                 interfaceName.empty() ? "0.0.0.0:" + port + ",[::]:" + port : interfaceName + ":" + port);
    pack.set_str(lt::settings_pack::outgoing_interfaces, interfaceName);
    pack.set_str(lt::settings_pack::dht_bootstrap_nodes, interfaceName.empty() ? kDefaultBootstrapNodes : "");
    pack.set_bool(lt::settings_pack::enable_upnp, interfaceName.empty());
    pack.set_bool(lt::settings_pack::enable_natpmp, interfaceName.empty());
    impl_->session->apply_settings(std::move(pack));
    std::vector<std::pair<DownloadId, lt::torrent_handle>> torrents;
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->settings.networkInterface = interfaceName;
        for (const auto& [id, entry] : impl_->entries) torrents.emplace_back(id, entry.handle);
    }
    applyProxy();
    for (const auto& [id, handle] : torrents) {
        if (interfaceName.empty()) unlockTrackers(id);
        else lockTrackers(id, handle);
    }
    if (!interfaceName.empty()) resolveBootstrapThrough(interfaceName);
    ORBIT_LOG_INFO("torrent", interfaceName.empty() ? std::string("Torrents use any network interface")
                                                    : "Torrents bound to network interface " + interfaceName);
}

void TorrentEngine::setListenPort(int port) {
    port = std::clamp(port, 1024, 65535);
    std::string interfaceName;
    {
        std::scoped_lock lock(impl_->mutex);
        if (port == impl_->settings.listenPort) return;
        impl_->settings.listenPort = port;
        interfaceName = impl_->settings.networkInterface;
    }
    const std::string p = std::to_string(port);
    lt::settings_pack pack;
    pack.set_str(lt::settings_pack::listen_interfaces, interfaceName.empty() ? "0.0.0.0:" + p + ",[::]:" + p : interfaceName + ":" + p);
    impl_->session->apply_settings(std::move(pack));
    ORBIT_LOG_INFO("TorrentEngine", "Listening for peers on port " + p);
}

void TorrentEngine::setAddPublicTrackers(bool add) {
    std::scoped_lock lock(impl_->mutex);
    impl_->addPublicTrackers = add;
}

void TorrentEngine::setNoCowOnBtrfs(bool enabled) {
    std::scoped_lock lock(impl_->mutex);
    impl_->noCowOnBtrfs = enabled;
}

bool TorrentEngine::isOnBtrfs(const std::string& path) {
#if defined(__linux__)
    struct statfs info {};
    constexpr long kBtrfsMagic = 0x9123683E;
    return ::statfs(path.c_str(), &info) == 0 && static_cast<long>(info.f_type) == kBtrfsMagic;
#else
    (void)path;
    return false;
#endif
}

bool TorrentEngine::markNoCow(const std::string& path) {
#if defined(__linux__)
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    int flags = 0;
    bool ok = ::ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0;
    if (ok && !(flags & FS_NOCOW_FL)) {
        flags |= FS_NOCOW_FL;
        ok = ::ioctl(fd, FS_IOC_SETFLAGS, &flags) == 0;
    }
    ::close(fd);
    return ok;
#else
    (void)path;
    return false;
#endif
}

namespace {
// Prepares where a torrent's data will go: the top folder of a multi-file torrent (files
// created in it inherit the flag), or the empty file of a single-file one.
void prepareNoCow(const lt::torrent_info& ti, const std::string& savePath) {
    if (!TorrentEngine::isOnBtrfs(savePath)) return;
    const lt::file_storage& files = ti.files();
    if (files.num_files() <= 0) return;
    const std::string first = files.file_path(lt::file_index_t{0});
    const auto slash = first.find_first_of("/\\");
    std::error_code ec;
    if (files.num_files() > 1 || slash != std::string::npos) {
        const std::filesystem::path top = std::filesystem::path(savePath) / first.substr(0, slash);
        if (std::filesystem::exists(top, ec)) return;  // Files may already be in it.
        std::filesystem::create_directories(top, ec);
        if (!ec && TorrentEngine::markNoCow(top.string())) {
            ORBIT_LOG_INFO("TorrentEngine", "Marked " + top.string() + " no-copy-on-write (btrfs)");
        }
    } else {
        const std::filesystem::path file = std::filesystem::path(savePath) / first;
        if (std::filesystem::exists(file, ec)) return;  // Only an empty, new file can take the flag.
        std::ofstream{file, std::ios::binary};
        if (TorrentEngine::markNoCow(file.string())) {
            ORBIT_LOG_INFO("TorrentEngine", "Marked " + file.string() + " no-copy-on-write (btrfs)");
        }
    }
}
}

void TorrentEngine::setSeedAfterDownload(bool seed) {
    std::scoped_lock lock(impl_->mutex);
    impl_->seedAfterDownload = seed;
}

void TorrentEngine::setSeedingLimits(double ratio, int minutes) {
    std::scoped_lock lock(impl_->mutex);
    impl_->seedRatioLimit = std::max(0.0, ratio);
    impl_->seedMinutesLimit = std::max(0, minutes);
}

std::vector<TorrentFileStatus> TorrentEngine::files(DownloadId id) const {
    lt::torrent_handle handle;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end()) return {};
        handle = it->second.handle;
    }
    std::vector<TorrentFileStatus> result;
    if (!handle.is_valid()) return result;
    const auto ti = handle.torrent_file();
    if (!ti) return result;
    const lt::file_storage& fs = ti->layout();
    const lt::renamed_files renames = handle.get_renamed_files();
    const std::vector<std::int64_t> progress = handle.file_progress(lt::torrent_handle::piece_granularity);
    const std::vector<lt::download_priority_t> priorities = handle.get_file_priorities();
    result.reserve(static_cast<size_t>(fs.num_files()));
    for (int i = 0; i < fs.num_files(); ++i) {
        const lt::file_index_t idx{i};
        TorrentFileStatus file;
        file.index = i;
        file.padding = fs.pad_file_at(idx);
        file.path = currentFilePath(*ti, renames, i);
        file.size = static_cast<std::uint64_t>(std::max<std::int64_t>(0, fs.file_size(idx)));
        if (static_cast<size_t>(i) < progress.size()) file.downloaded = static_cast<std::uint64_t>(std::max<std::int64_t>(0, progress[static_cast<size_t>(i)]));
        if (static_cast<size_t>(i) < priorities.size()) file.priority = static_cast<int>(static_cast<std::uint8_t>(priorities[static_cast<size_t>(i)]));
        result.push_back(std::move(file));
    }
    return result;
}

bool TorrentEngine::setFilePriority(DownloadId id, int fileIndex, int priority) {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || !it->second.handle.is_valid()) return false;
    const auto ti = it->second.handle.torrent_file();
    if (!ti || fileIndex < 0 || fileIndex >= ti->num_files()) return false;
    const auto value = static_cast<std::uint8_t>(std::clamp(priority, 0, 7));
    it->second.handle.file_priority(lt::file_index_t{fileIndex}, lt::download_priority_t{value});
    return true;
}

std::vector<TorrentPeerInfo> TorrentEngine::peers(DownloadId id) const {
    lt::torrent_handle handle;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end()) return {};
        handle = it->second.handle;
    }
    std::vector<TorrentPeerInfo> result;
    if (!handle.is_valid()) return result;
    std::vector<lt::peer_info> infos;
    handle.get_peer_info(infos);
    result.reserve(infos.size());
    for (const lt::peer_info& p : infos) {
        TorrentPeerInfo peer;
        const auto endpoint = p.remote_endpoint();
        const auto address = endpoint.address();
        peer.address = (address.is_v6() ? "[" + address.to_string() + "]" : address.to_string()) + ":" +
                       std::to_string(endpoint.port());
        peer.client = p.client;
        std::string flags;
        if (p.flags & lt::peer_info::utp_socket) flags += "uTP ";
        else flags += "TCP ";
        if (!(p.flags & lt::peer_info::outgoing_connection)) flags += "in ";
        if ((p.flags & lt::peer_info::rc4_encrypted) || (p.flags & lt::peer_info::plaintext_encrypted)) flags += "encrypted";
        while (!flags.empty() && flags.back() == ' ') flags.pop_back();
        peer.flags = flags;
        peer.downloadRate = p.payload_down_speed;
        peer.uploadRate = p.payload_up_speed;
        peer.progress = static_cast<double>(p.progress);
        peer.seed = static_cast<bool>(p.flags & lt::peer_info::seed);
        result.push_back(std::move(peer));
    }
    std::sort(result.begin(), result.end(), [](const TorrentPeerInfo& a, const TorrentPeerInfo& b) {
        return a.downloadRate + a.uploadRate > b.downloadRate + b.uploadRate;
    });
    return result;
}

std::vector<TorrentTrackerInfo> TorrentEngine::trackers(DownloadId id) const {
    lt::torrent_handle handle;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end()) return {};
        handle = it->second.handle;
    }
    std::vector<TorrentTrackerInfo> result;
    if (!handle.is_valid()) return result;
    for (const lt::announce_entry& entry : handle.trackers()) {
        TorrentTrackerInfo info;
        info.url = entry.url;
        info.tier = entry.tier;
        bool updating = false, working = false, contacted = false;
        std::string error, message;
        for (const lt::announce_endpoint& endpoint : entry.endpoints) {
            for (const lt::announce_infohash& ih : endpoint.info_hashes) {
                if (ih.updating) updating = true;
                if (ih.last_error && error.empty()) error = ih.last_error.message();
                if (!ih.message.empty() && message.empty()) message = ih.message;
                if (ih.start_sent || ih.fails > 0 || ih.last_error) contacted = true;
                if (ih.start_sent && !ih.last_error && ih.fails == 0) working = true;
                info.seeds = std::max(info.seeds, ih.scrape_complete);
                info.peers = std::max(info.peers, ih.scrape_incomplete);
            }
        }
        info.status = updating ? "Updating" : working ? "Working" : !error.empty() ? "Error" : contacted ? "Not working" : "Not contacted";
        info.message = !error.empty() ? error : message;
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<std::uint8_t> TorrentEngine::pieceMap(DownloadId id, int buckets) const {
    lt::torrent_handle handle;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end()) return {};
        handle = it->second.handle;
    }
    if (!handle.is_valid() || buckets <= 0) return {};
    const lt::torrent_status st = handle.status(lt::torrent_handle::query_pieces);
    const int total = st.pieces.size();
    if (!st.has_metadata || total == 0) return {};
    const int cells = std::min(buckets, total);
    std::vector<std::uint8_t> map(static_cast<size_t>(cells), 0);
    for (int c = 0; c < cells; ++c) {
        const int from = static_cast<int>(static_cast<std::int64_t>(c) * total / cells);
        const int to = static_cast<int>(static_cast<std::int64_t>(c + 1) * total / cells);
        int have = 0;
        for (int p = from; p < to; ++p) {
            if (st.pieces.get_bit(lt::piece_index_t{p})) ++have;
        }
        map[static_cast<size_t>(c)] = static_cast<std::uint8_t>(to > from ? (have * 255) / (to - from) : 0);
    }
    return map;
}

int TorrentEngine::pieceCount(DownloadId id) const {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || !it->second.handle.is_valid()) return 0;
    const auto ti = it->second.handle.torrent_file();
    return ti ? ti->num_pieces() : 0;
}

bool TorrentEngine::forceRecheck(DownloadId id) {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || !it->second.handle.is_valid()) return false;
    it->second.handle.force_recheck();
    return true;
}

bool TorrentEngine::forceReannounce(DownloadId id) {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || !it->second.handle.is_valid()) return false;
    it->second.handle.force_reannounce();
    it->second.handle.force_dht_announce();
    return true;
}

std::string TorrentEngine::rename(DownloadId id, const std::string& newName) {
    lt::torrent_handle handle;
    {
        std::scoped_lock lock(impl_->mutex);
        auto it = impl_->entries.find(id);
        if (it == impl_->entries.end() || !it->second.handle.is_valid()) return "The torrent isn't loaded right now";
        handle = it->second.handle;
    }
    const auto ti = handle.torrent_file();
    if (!ti) return "Wait until the torrent's metadata has been fetched";
    const lt::file_storage& fs = ti->layout();
    const lt::renamed_files renames = handle.get_renamed_files();
    if (fs.num_files() == 1) {
        handle.rename_file(lt::file_index_t{0}, newName);
        return {};
    }
    const std::string oldRoot = rootName(*ti, renames);
    for (int i = 0; i < fs.num_files(); ++i) {
        const lt::file_index_t idx{i};
        const std::string path = currentFilePath(*ti, renames, i);
        if (path.rfind(oldRoot, 0) != 0) continue;
        handle.rename_file(idx, newName + path.substr(oldRoot.size()));
    }
    return {};
}

std::string TorrentEngine::moveStorage(DownloadId id, const std::string& directory) {
    std::scoped_lock lock(impl_->mutex);
    auto it = impl_->entries.find(id);
    if (it == impl_->entries.end() || !it->second.handle.is_valid()) return "The torrent isn't loaded right now";
    utils::ensureDirectoryExists(directory);
    it->second.request.destinationDir = directory;
    // Never over someone else's files: if any are in the way, the move fails and says so
    // (storage_moved_failed_alert -> "Couldn't move the torrent: File exists").
    it->second.handle.move_storage(directory, lt::move_flags_t::fail_if_exist);
    return {};
}

DownloadId TorrentEngine::idForHandle(const lt::torrent_handle& handle) const {
    std::scoped_lock lock(impl_->mutex);
    for (const auto& [entryId, entry] : impl_->entries) {
        if (entry.handle == handle) return entryId;
    }
    return 0;
}

void TorrentEngine::alertLoop(std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
        impl_->session->wait_for_alert(std::chrono::milliseconds(500));

        std::vector<lt::alert*> alerts;
        impl_->session->pop_alerts(&alerts);
        for (lt::alert* a : alerts) {
            const auto category = static_cast<std::uint32_t>(a->category());
            const auto errorOrTracker =
                static_cast<std::uint32_t>(lt::alert_category::error | lt::alert_category::tracker);
            const auto dhtCategory = static_cast<std::uint32_t>(lt::alert_category::dht);
            if (category & errorOrTracker) {
                ORBIT_LOG_WARN("TorrentEngine", a->message());
            } else if (auto* listenOk = lt::alert_cast<lt::listen_succeeded_alert>(a)) {
                ORBIT_LOG_INFO("TorrentEngine", listenOk->message());
            } else if (category & dhtCategory) {
                ORBIT_LOG_INFO("TorrentEngine", a->message());
            }

            if (lt::alert_cast<lt::portmap_alert>(a)) {
                ORBIT_LOG_INFO("TorrentEngine",
                    "Listening port " + std::to_string(impl_->settings.listenPort) +
                        " was forwarded automatically (UPnP/NAT-PMP) - this machine is reachable "
                        "from the internet, which should help peer counts and speed.");
            } else if (!impl_->reachabilityWarnLogged && lt::alert_cast<lt::portmap_error_alert>(a)) {
                impl_->reachabilityWarnLogged = true;
                ORBIT_LOG_WARN("TorrentEngine",
                    "Could not forward listening port " + std::to_string(impl_->settings.listenPort) +
                        " automatically (router rejected UPnP/NAT-PMP, or both are disabled). This "
                        "machine is likely unreachable from the internet, which limits torrent peer "
                        "counts and speed. For best results, forward TCP+UDP port " +
                        std::to_string(impl_->settings.listenPort) + " to this PC in your router.");
            } else if (auto* saved = lt::alert_cast<lt::save_resume_data_alert>(a)) {
                writeResumeData(saved->handle, saved->params);
            } else if (auto* meta = lt::alert_cast<lt::metadata_received_alert>(a)) {
                // A magnet link's file list just arrived; no piece has been written yet.
                bool noCow = false;
                bool locked = false;
                const DownloadId metaId = idForHandle(meta->handle);
                {
                    std::scoped_lock lock(impl_->mutex);
                    noCow = impl_->noCowOnBtrfs;
                    const auto it = impl_->entries.find(metaId);
                    locked = it != impl_->entries.end() && it->second.trackersLocked;
                }
                if (locked) {
                    // The metadata may name web seeds; under the lock they wait like the others.
                    for (const std::string& seed : meta->handle.url_seeds()) {
                        meta->handle.remove_url_seed(seed);
                        std::scoped_lock lock(impl_->mutex);
                        if (const auto it = impl_->entries.find(metaId); it != impl_->entries.end()) {
                            it->second.originalUrlSeeds.push_back(seed);
                        }
                    }
                }
                if (noCow) {
                    if (const auto ti = meta->handle.torrent_file()) {
                        prepareNoCow(*ti, meta->handle.status(lt::torrent_handle::query_save_path).save_path);
                    }
                }
            } else if (auto* moved = lt::alert_cast<lt::storage_moved_alert>(a)) {
                if (const DownloadId movedId = idForHandle(moved->handle)) {
                    const auto ti = moved->handle.torrent_file();
                    const std::string name = ti ? rootName(*ti, moved->handle.get_renamed_files()) : std::string();
                    reporter_.reportNameResolved(movedId, name, (std::filesystem::path(moved->storage_path()) / name).string());
                    reporter_.reportNotice(movedId, true, std::string("Moved to ") + moved->storage_path());
                    moved->handle.save_resume_data(lt::torrent_handle::save_info_dict);
                }
            } else if (auto* moveFailed = lt::alert_cast<lt::storage_moved_failed_alert>(a)) {
                if (const DownloadId failedId = idForHandle(moveFailed->handle)) {
                    reporter_.reportNotice(failedId, false, "Couldn't move the torrent: " + moveFailed->error.message());
                }
            } else if (auto* renamed = lt::alert_cast<lt::file_renamed_alert>(a)) {
                if (const DownloadId renamedId = idForHandle(renamed->handle)) {
                    const auto ti = renamed->handle.torrent_file();
                    const lt::torrent_status st = renamed->handle.status(lt::torrent_handle::query_save_path);
                    if (ti) {
                        const std::string name = rootName(*ti, renamed->handle.get_renamed_files());
                        reporter_.reportNameResolved(renamedId, name, (std::filesystem::path(st.save_path) / name).string());
                    }
                    renamed->handle.save_resume_data(lt::torrent_handle::save_info_dict);
                }
            } else if (auto* renameFailed = lt::alert_cast<lt::file_rename_failed_alert>(a)) {
                if (const DownloadId failedId = idForHandle(renameFailed->handle)) {
                    reporter_.reportNotice(failedId, false, "Couldn't rename: " + renameFailed->error.message());
                }
            }
        }

        if (std::chrono::steady_clock::now() - impl_->lastResumeCheckpoint > std::chrono::minutes(2)) {
            impl_->lastResumeCheckpoint = std::chrono::steady_clock::now();
            std::scoped_lock lock(impl_->mutex);
            for (auto& [id, entry] : impl_->entries) {
                if (entry.handle.is_valid() && entry.handle.status().has_metadata) {
                    entry.handle.save_resume_data(lt::torrent_handle::save_info_dict);
                }
            }
        }

        std::vector<std::pair<DownloadId, lt::torrent_handle>> snapshot;
        {
            std::scoped_lock lock(impl_->mutex);
            snapshot.reserve(impl_->entries.size());
            for (auto& [id, entry] : impl_->entries) snapshot.emplace_back(id, entry.handle);
        }

        for (auto& [id, handle] : snapshot) {
            if (!handle.is_valid()) continue;
            const lt::torrent_status st = handle.status();

            reporter_.reportProgress(id, toDownloadStats(st));

            bool seedingStopped = false;
            bool stopSeedingNow = false;
            {
                std::scoped_lock lock(impl_->mutex);
                auto it = impl_->entries.find(id);
                if (it != impl_->entries.end()) {
                    const bool pausedNow = (st.flags & lt::torrent_flags::paused) == lt::torrent_flags::paused;
                    const bool limited = !impl_->seedAfterDownload || impl_->seedRatioLimit > 0 || impl_->seedMinutesLimit > 0;
                    if (st.is_seeding && !pausedNow && !it->second.seedingStopped && limited) {
                        const double wanted = static_cast<double>(std::max<std::int64_t>(1, st.total_wanted));
                        const double ratio = static_cast<double>(st.all_time_upload) / wanted;
                        const auto seededMinutes = std::chrono::duration_cast<std::chrono::minutes>(st.seeding_duration).count();
                        if (!impl_->seedAfterDownload ||
                            (impl_->seedRatioLimit > 0 && ratio >= impl_->seedRatioLimit) ||
                            (impl_->seedMinutesLimit > 0 && seededMinutes >= impl_->seedMinutesLimit)) {
                            it->second.seedingStopped = true;
                            stopSeedingNow = true;
                        }
                    }
                    // With seeding off, a finished torrent that's paused (for example
                    // after Orbit was closed while it seeded) is simply complete.
                    seedingStopped = it->second.seedingStopped || !impl_->seedAfterDownload;
                }
            }
            if (stopSeedingNow) {
                ORBIT_LOG_INFO("TorrentEngine", "Download " + std::to_string(id) + " finished; stopping (seeding is off or its limit was reached)");
                handle.unset_flags(lt::torrent_flags::auto_managed);
                handle.pause();
                seedingStopped = true;
            }

            const DownloadState state = stopSeedingNow ? DownloadState::Completed : toDownloadState(st, seedingStopped);
            bool changed = false;
            bool reportName = false;
            {
                std::scoped_lock lock(impl_->mutex);
                auto it = impl_->entries.find(id);
                if (it != impl_->entries.end()) {
                    if (it->second.lastState != state) {
                        it->second.lastState = state;
                        changed = true;
                    }
                    if (st.has_metadata && !it->second.nameReported) {
                        it->second.nameReported = true;
                        reportName = true;
                    }
                }
            }
            if (reportName) {
                const auto ti = handle.torrent_file();
                const std::string name = ti ? rootName(*ti, handle.get_renamed_files()) : st.name;
                if (!name.empty()) {
                    reporter_.reportNameResolved(id, name, (std::filesystem::path(st.save_path) / name).string());
                }
            }
            if (changed) {
                reporter_.reportStateChanged(id, state);
                if (state == DownloadState::Error && st.errc) {
                    reporter_.reportError(id, st.errc.message());
                }
            }
        }
    }
}

}
