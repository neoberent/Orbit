#pragma once

#include <QSortFilterProxyModel>
#include <QString>

class DownloadFilterProxyModel final : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString filterCategory READ filterCategory WRITE setFilterCategory NOTIFY filterCategoryChanged)
    Q_PROPERTY(QString searchText READ searchText WRITE setSearchText NOTIFY searchTextChanged)
    Q_PROPERTY(QString sortKey READ sortKey WRITE setSortKey NOTIFY sortChanged)
    Q_PROPERTY(bool sortDescending READ sortDescending WRITE setSortDescending NOTIFY sortChanged)

public:
    explicit DownloadFilterProxyModel(QObject* parent = nullptr);

    [[nodiscard]] QString filterCategory() const noexcept { return filterCategory_; }
    void setFilterCategory(const QString& category);

    [[nodiscard]] QString searchText() const noexcept { return searchText_; }
    void setSearchText(const QString& text);

    [[nodiscard]] QString sortKey() const noexcept { return sortKey_; }
    void setSortKey(const QString& key);
    [[nodiscard]] bool sortDescending() const noexcept { return sortDescending_; }
    void setSortDescending(bool descending);

    Q_INVOKABLE qlonglong idAt(int row) const;
    Q_INVOKABLE int rowOfId(qlonglong id) const;
    Q_INVOKABLE QVariantList idsBetween(int fromRow, int toRow) const;
    Q_INVOKABLE QVariantList allIds() const;

signals:
    void filterCategoryChanged();
    void searchTextChanged();
    void sortChanged();

protected:
    [[nodiscard]] bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    [[nodiscard]] bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    void applySort();

    QString filterCategory_{"all"};
    QString searchText_;
    QString sortKey_{"added"};
    bool sortDescending_{false};
};
