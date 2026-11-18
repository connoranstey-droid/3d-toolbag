#include "ui/Hotkeys.h"

#include <QAction>
#include <QComboBox>
#include <QGuiApplication>
#include <QInputMethod>
#include <QLocale>
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

namespace {
// UK / AltGr-safe replacements for the Ctrl+Alt defaults.
const QMap<QString, QString>& ukOverrides()
{
    static const QMap<QString, QString> m = {
        {"Tools/Sculpt Tool", "Ctrl+Shift+B"},
        {"Mesh Tools/Make Live (toggle)", "Ctrl+Shift+V"},
        {"Mesh Tools/Conform to Live Surface", "Ctrl+Shift+C"},
        {"Smart Unwrap/Smart Unwrap", "Ctrl+Shift+N"},
        {"Edit Mesh/Bridge", "Ctrl+Shift+R"},
        {"Edit Mesh/Merge", "Ctrl+Shift+M"},
        {"Mesh Display/Harden Edge", "Ctrl+Shift+H"},
        {"Mesh Display/Soften Edge", "Ctrl+Shift+F"},
    };
    return m;
}
} // namespace

bool HotkeyManager::ukLayoutDetected()
{
    auto isGB = [](const QLocale& l) { return l.territory() == QLocale::UnitedKingdom; };
    if (auto* im = QGuiApplication::inputMethod(); im && isGB(im->locale())) return true;
    return isGB(QLocale::system());
}

QList<QKeySequence> HotkeyManager::effectiveDefaults(const Entry& e) const
{
    if (preset_ == "UK") {
        auto it = ukOverrides().find(e.id);
        if (it != ukOverrides().end()) return {QKeySequence(it.value())};
    }
    return e.defaults;
}

void HotkeyManager::applyDefaultsAndOverrides()
{
    for (auto& e : entries_)
        if (e.action) e.action->setShortcuts(effectiveDefaults(e));
    // User overrides on top.
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

void HotkeyManager::setPreset(const QString& name)
{
    preset_ = presets().contains(name) ? name : QString("Default");
    QSettings().setValue("hotkeyPreset", preset_);
    applyDefaultsAndOverrides();
    emit changed();
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
        const auto defs = effectiveDefaults(*e);
        for (const auto& seq : defs)
            for (const QString& other : conflicts(seq, id)) {
                Entry* o = find(other);
                auto list = o->action->shortcuts();
                list.removeAll(seq);
                o->action->setShortcuts(list);
            }
        e->action->setShortcuts(defs);
        save();
        emit changed();
    }
}

void HotkeyManager::resetAll()
{
    QSettings s;
    s.remove("hotkeys");
    applyDefaultsAndOverrides();
    emit changed();
}

void HotkeyManager::load()
{
    QSettings s;
    preset_ = s.value("hotkeyPreset", "Default").toString();
    if (!presets().contains(preset_)) preset_ = "Default";
    applyDefaultsAndOverrides();
}

void HotkeyManager::save() const
{
    // Only store what differs from the defaults, so new defaults in future versions still apply.
    QSettings s;
    s.beginGroup("hotkeys");
    for (const auto& e : entries_) {
        if (!e.action) continue;
        if (e.action->shortcuts() == effectiveDefaults(e)) { s.remove(e.id); continue; }
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
    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel("Hotkey set:"));
    presetBox_ = new QComboBox;
    presetBox_->addItem("Default (Maya-style)", "Default");
    presetBox_->addItem("UK keyboard (AltGr-safe: no Ctrl+Alt)", "UK");
    presetBox_->setCurrentIndex(mgr_->preset() == "UK" ? 1 : 0);
    presetRow->addWidget(presetBox_, 1);
    lay->addLayout(presetRow);
    connect(presetBox_, &QComboBox::currentIndexChanged, this, [this](int i) {
        mgr_->setPreset(presetBox_->itemData(i).toString());
        status_->setText(QString("Hotkey set: %1").arg(presetBox_->currentText()));
        rebuild();
    });
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
                              "M (brush strength), X (grid snap), J (rotate snap), Alt + mouse (camera).<br>"
                              "Keyboard camera (rebindable, under View): Alt+Arrows orbit, Alt+Shift+Arrows pan, Alt+PgUp/PgDn zoom, "
                              "Alt+Home reset, Numpad 7/1/3/5 views, Numpad 8/2/4/6 orbit, Numpad +/- zoom.</small>"));

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
        QString cur = keys(e.action->shortcuts()), def = keys(mgr_->effectiveDefaults(e));
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
