#include "ui/Hotkeys.h"

#include <QAction>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <map>

namespace facet {

HotkeyManager::HotkeyManager(QObject* parent) : QObject(parent) {}

QString HotkeyManager::cleanName(QString text)
{
    text.remove('&');
    text.remove(QStringLiteral("..."));
    return text.trimmed();
}

void HotkeyManager::registerAction(QAction* a, const QString& category)
{
    const QString name = cleanName(a->text());
    QString id = cleanName(category) + "/" + name;
    // Keep ids unique even if two menus share a label.
    QString unique = id;
    for (int n = 2; find(unique); ++n) unique = id + QString(" #%1").arg(n);
    entries_.push_back({a, unique, cleanName(category), name, a->shortcuts()});
}

HotkeyManager::Entry* HotkeyManager::find(const QString& id)
{
    for (auto& e : entries_)
        if (e.id == id) return &e;
    return nullptr;
}

QStringList HotkeyManager::conflicts(const QKeySequence& seq, const QString& except) const
{
    QStringList out;
    if (seq.isEmpty()) return out;
    for (const auto& e : entries_) {
        if (e.id == except || !e.action) continue;
        if (e.action->shortcuts().contains(seq)) out << e.id;
    }
    return out;
}

QStringList HotkeyManager::assign(const QString& id, const QKeySequence& seq)
{
    Entry* e = find(id);
    if (!e || !e->action) return {};
    QStringList taken = conflicts(seq, id);
    for (const QString& other : taken) {
        Entry* o = find(other);
        auto list = o->action->shortcuts();
        list.removeAll(seq);
        o->action->setShortcuts(list);
    }
    e->action->setShortcuts({seq});
    save();
    emit changed();
    return taken;
}

void HotkeyManager::clear(const QString& id)
{
    if (Entry* e = find(id); e && e->action) {
        e->action->setShortcuts({});
        save();
        emit changed();
    }
}

void HotkeyManager::reset(const QString& id)
{
    if (Entry* e = find(id); e && e->action) {
        // Defaults win over whatever currently holds the key.
        for (const auto& seq : e->defaults)
            for (const QString& other : conflicts(seq, id)) {
                Entry* o = find(other);
                auto list = o->action->shortcuts();
                list.removeAll(seq);
                o->action->setShortcuts(list);
            }
        e->action->setShortcuts(e->defaults);
        save();
        emit changed();
    }
}

void HotkeyManager::resetAll()
{
    for (auto& e : entries_)
        if (e.action) e.action->setShortcuts(e.defaults);
    QSettings s;
    s.remove("hotkeys");
    emit changed();
}

void HotkeyManager::load()
{
    QSettings s;
    s.beginGroup("hotkeys");
    for (auto& e : entries_) {
        if (!e.action || !s.contains(e.id)) continue;
        QList<QKeySequence> keys;
        for (const QString& k : s.value(e.id).toStringList())
            if (!k.isEmpty()) keys << QKeySequence::fromString(k, QKeySequence::PortableText);
        e.action->setShortcuts(keys);
    }
    s.endGroup();
}

void HotkeyManager::save() const
{
    // Only store what differs from the defaults, so new defaults in future versions still apply.
    QSettings s;
    s.beginGroup("hotkeys");
    for (const auto& e : entries_) {
        if (!e.action) continue;
        if (e.action->shortcuts() == e.defaults) { s.remove(e.id); continue; }
        QStringList keys;
        for (const auto& k : e.action->shortcuts()) keys << k.toString(QKeySequence::PortableText);
        s.setValue(e.id, keys);
    }
    s.endGroup();
}

// ------------------------------------------------------------------------------------------

HotkeyEditor::HotkeyEditor(HotkeyManager* mgr, QWidget* parent) : QDialog(parent), mgr_(mgr)
{
    setWindowTitle("Hotkey Editor");
    resize(760, 620);
    auto* lay = new QVBoxLayout(this);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText("Search commands or keys (e.g. bevel, Ctrl+B)");
    filter_->setClearButtonEnabled(true);
    lay->addWidget(filter_);
    tree_ = new QTreeWidget;
    tree_->setColumnCount(3);
    tree_->setHeaderLabels({"Command", "Shortcut", "Default"});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    lay->addWidget(tree_, 1);

    auto* row = new QHBoxLayout;
    row->addWidget(new QLabel("New shortcut:"));
    edit_ = new QKeySequenceEdit;
    row->addWidget(edit_, 1);
    auto* assign = new QPushButton("Assign");
    auto* clear = new QPushButton("Clear");
    auto* reset = new QPushButton("Reset");
    row->addWidget(assign);
    row->addWidget(clear);
    row->addWidget(reset);
    lay->addLayout(row);
    status_ = new QLabel;
    status_->setWordWrap(true);
    lay->addWidget(status_);
    auto* bottom = new QHBoxLayout;
    auto* resetAll = new QPushButton("Restore All Defaults");
    auto* close = new QPushButton("Close");
    bottom->addWidget(resetAll);
    bottom->addStretch(1);
    bottom->addWidget(close);
    lay->addLayout(bottom);
    lay->addWidget(new QLabel("<small>Held keys in the viewport (not rebindable): Space (hotbox / four view), B (soft select / brush radius), "
                              "M (brush strength), X (grid snap), J (rotate snap), Alt + mouse (camera).</small>"));

    connect(filter_, &QLineEdit::textChanged, this, &HotkeyEditor::rebuild);
    connect(tree_, &QTreeWidget::currentItemChanged, this, &HotkeyEditor::onSelect);
    connect(edit_, &QKeySequenceEdit::keySequenceChanged, this, [this](const QKeySequence& k) {
        QStringList c = mgr_->conflicts(k, currentId());
        status_->setText(c.isEmpty() ? QString() : QString("<b>%1</b> is used by: %2 (it will be reassigned)").arg(k.toString(QKeySequence::NativeText), c.join(", ")));
    });
    connect(assign, &QPushButton::clicked, this, [this] {
        QString id = currentId();
        if (id.isEmpty() || edit_->keySequence().isEmpty()) return;
        const QKeySequence seq(edit_->keySequence()[0]); // single key combination
        QStringList taken = mgr_->assign(id, seq);
        status_->setText(taken.isEmpty() ? QString("Assigned %1 to %2").arg(seq.toString(QKeySequence::NativeText), id)
                                         : QString("Assigned; removed it from: %1").arg(taken.join(", ")));
        rebuild();
    });
    connect(clear, &QPushButton::clicked, this, [this] { if (!currentId().isEmpty()) { mgr_->clear(currentId()); rebuild(); } });
    connect(reset, &QPushButton::clicked, this, [this] { if (!currentId().isEmpty()) { mgr_->reset(currentId()); rebuild(); } });
    connect(resetAll, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Hotkey Editor", "Restore every shortcut to its default?") == QMessageBox::Yes) {
            mgr_->resetAll();
            rebuild();
        }
    });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    rebuild();
}

QString HotkeyEditor::currentId() const
{
    auto* it = tree_->currentItem();
    return it ? it->data(0, Qt::UserRole).toString() : QString();
}

void HotkeyEditor::rebuild()
{
    const QString keep = currentId();
    const QString f = filter_->text().trimmed();
    tree_->clear();
    std::map<QString, QTreeWidgetItem*> cats;
    auto keys = [](const QList<QKeySequence>& l) {
        QStringList s;
        for (const auto& k : l) s << k.toString(QKeySequence::NativeText);
        return s.join(", ");
    };
    for (const auto& e : mgr_->entries()) {
        if (!e.action) continue;
        QString cur = keys(e.action->shortcuts()), def = keys(e.defaults);
        if (!f.isEmpty() && !e.name.contains(f, Qt::CaseInsensitive) && !e.category.contains(f, Qt::CaseInsensitive) &&
            !cur.contains(f, Qt::CaseInsensitive))
            continue;
        auto& cat = cats[e.category];
        if (!cat) {
            cat = new QTreeWidgetItem(tree_, {e.category});
            cat->setFirstColumnSpanned(true);
            QFont fo = cat->font(0);
            fo.setBold(true);
            cat->setFont(0, fo);
            cat->setFlags(Qt::ItemIsEnabled);
        }
        auto* it = new QTreeWidgetItem(cat, {e.name, cur, def});
        it->setData(0, Qt::UserRole, e.id);
        if (cur != def) it->setForeground(1, QColor(255, 200, 90));
        if (e.id == keep) tree_->setCurrentItem(it);
    }
    tree_->expandAll();
}

void HotkeyEditor::onSelect()
{
    edit_->clear();
    status_->clear();
}

} // namespace facet
