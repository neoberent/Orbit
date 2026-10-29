#include "DownloadFilterProxyModel.hpp"

#include "DownloadListModel.hpp"
#include "core/Types.hpp"

#include <algorithm>

using orbit::core::DownloadState;
using orbit::core::DownloadType;

DownloadFilterProxyModel::DownloadFilterProxyModel(QObject* parent) : QSortFilterProxyModel(parent) {
    setDynamicSortFilter(true);
    applySort();
}

void DownloadFilterProxyModel::setSortKey(const QString& key) {
    if (sortKey_ == key) return;
    sortKey_ = key;
    applySort();
    emit sortChanged();
}

void DownloadFilterProxyModel::setSortDescending(bool descending) {
    if (sortDescending_ == descending) return;
    sortDescending_ = descending;
    applySort();
    emit sortChanged();
}

void DownloadFilterProxyModel::applySort() {
    // sort() is a no-op when column and order are unchanged, which is always the case
    // when only the sort key changes, so force a full re-sort afterwards.
    sort(0, sortDescending_ ? Qt::DescendingOrder : Qt::AscendingOrder);
    invalidate();
}

bool DownloadFilterProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const {
    auto num = [](const QModelIndex& idx, int role) { return idx.data(role).toDouble(); };

    // Ties (and the "added" order) fall back to insertion order, so rows with equal keys
    // keep a stable position instead of shuffling on every progress update.
    const bool insertionLess = left.row() < right.row();

    if (sortKey_ == "name") {
        const int cmp = QString::localeAwareCompare(left.data(DownloadListModel::NameRole).toString(),
                                                    right.data(DownloadListModel::NameRole).toString());
        return cmp != 0 ? cmp < 0 : insertionLess;
    }

    int role = -1;
    if (sortKey_ == "progress") role = DownloadListModel::ProgressRole;
    else if (sortKey_ == "size") role = DownloadListModel::TotalBytesRole;
    else if (sortKey_ == "speed") role = DownloadListModel::SpeedBytesRole;
    else if (sortKey_ == "status") role = DownloadListModel::StateRole;
    else if (sortKey_ == "time") role = DownloadListModel::EtaSecondsRole;

    if (role < 0) return insertionLess;

    double a = num(left, role);
    double b = num(right, role);
    if (sortKey_ == "time") {
        // Unknown ETAs (-1) always sink to the bottom regardless of direction.
        if (a < 0 && b < 0) return insertionLess;
        if (a < 0) return sortDescending_;
        if (b < 0) return !sortDescending_;
    }
    if (a == b) return insertionLess;
    return a < b;
}

qlonglong DownloadFilterProxyModel::idAt(int row) const {
    if (row < 0 || row >= rowCount()) return -1;
    return index(row, 0).data(DownloadListModel::IdRole).toLongLong();
}

int DownloadFilterProxyModel::rowOfId(qlonglong id) const {
    for (int r = 0; r < rowCount(); ++r) {
        if (index(r, 0).data(DownloadListModel::IdRole).toLongLong() == id) return r;
    }
    return -1;
}

QVariantList DownloadFilterProxyModel::idsBetween(int fromRow, int toRow) const {
    QVariantList ids;
    const int lo = std::max(0, std::min(fromRow, toRow));
    const int hi = std::min(rowCount() - 1, std::max(fromRow, toRow));
    for (int r = lo; r <= hi; ++r) ids.push_back(index(r, 0).data(DownloadListModel::IdRole));
    return ids;
}

QVariantList DownloadFilterProxyModel::allIds() const { return idsBetween(0, rowCount() - 1); }

void DownloadFilterProxyModel::setFilterCategory(const QString& category) {
    if (filterCategory_ == category) return;
    filterCategory_ = category;
    emit filterCategoryChanged();
    invalidateFilter();
}

void DownloadFilterProxyModel::setSearchText(const QString& text) {
    if (searchText_ == text) return;
    searchText_ = text;
    emit searchTextChanged();
    invalidateFilter();
}

bool DownloadFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const {
    QAbstractItemModel* source = sourceModel();
    if (!source) return true;

    const QModelIndex idx = source->index(sourceRow, 0, sourceParent);
    if (!idx.isValid()) return false;

    if (!searchText_.isEmpty()) {
        const QString name = source->data(idx, DownloadListModel::NameRole).toString();
        if (!name.contains(searchText_, Qt::CaseInsensitive)) return false;
    }

    if (filterCategory_ == "all" || filterCategory_.isEmpty()) return true;

    const auto state = static_cast<DownloadState>(source->data(idx, DownloadListModel::StateRole).toInt());
    const auto type = static_cast<DownloadType>(source->data(idx, DownloadListModel::TypeRole).toInt());

    if (filterCategory_ == "torrents") {
        return type == DownloadType::Torrent;
    }
    if (filterCategory_ == "media") {
        return type == DownloadType::Media;
    }
    if (filterCategory_ == "completed") {
        return state == DownloadState::Completed;
    }
    if (filterCategory_ == "downloading") {
        return state == DownloadState::Queued || state == DownloadState::Connecting ||
               state == DownloadState::Downloading || state == DownloadState::Paused ||
               state == DownloadState::Seeding;
    }
    return true;
}
