#pragma once
#include <QDialog>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <vector>

class QAction;
class QComboBox;
class QKeySequenceEdit;
class QLabel;
class QLineEdit;
class QTreeWidget;

namespace facet {

// Owns the editable keyboard shortcuts of every registered command (menus, tools, plugins) and
// persists user overrides with QSettings, like Maya's Hotkey Editor.
class HotkeyManager : public QObject {
    Q_OBJECT
public:
    struct Entry {
        QPointer<QAction> action;
        QString id;        // stable key used for saving: "<Category>/<Command>"
        QString category;
        QString name;
        QList<QKeySequence> defaults;
    };
    explicit HotkeyManager(QObject* parent = nullptr);

    void registerAction(QAction* a, const QString& category);
    const std::vector<Entry>& entries() const { return entries_; }
    Entry* find(const QString& id);

    // Assigns `seq` as the only shortcut of `id`. Returns the ids that already used it (they lose it).
    QStringList assign(const QString& id, const QKeySequence& seq);
    void clear(const QString& id);
    void reset(const QString& id);
    void resetAll();
    // Ids (other than `except`) currently bound to `seq`.
    QStringList conflicts(const QKeySequence& seq, const QString& except = {}) const;

    void load();
    void save() const;

    // Hotkey sets: "Default" (Maya-like) and "UK" (AltGr-safe: no Ctrl+Alt defaults, because on a UK
    // Windows keyboard AltGr *is* Ctrl+Alt and types characters). Switching keeps user overrides.
    QStringList presets() const { return {"Default", "UK"}; }
    QString preset() const { return preset_; }
    void setPreset(const QString& name);
    QList<QKeySequence> effectiveDefaults(const Entry& e) const;
    // True when the keyboard / system locale is en-GB.
    static bool ukLayoutDetected();

    static QString cleanName(QString text);

signals:
    void changed();

private:
    void applyDefaultsAndOverrides();
    std::vector<Entry> entries_;
    QString preset_ = "Default";
};

class HotkeyEditor : public QDialog {
    Q_OBJECT
public:
    explicit HotkeyEditor(HotkeyManager* mgr, QWidget* parent = nullptr);

private:
    void rebuild();
    void onSelect();
    QString currentId() const;

    HotkeyManager* mgr_;
    QLineEdit* filter_;
    QComboBox* presetBox_;
    QTreeWidget* tree_;
    QKeySequenceEdit* edit_;
    QLabel* status_;
};

} // namespace facet
