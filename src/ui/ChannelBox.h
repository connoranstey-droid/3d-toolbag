#pragma once
#include "scene/Scene.h"

#include <QLabel>
#include <QLineEdit>
#include <QWidget>

#include <functional>

class QFormLayout;
class QVBoxLayout;

namespace facet {

// Label that scrubs its channel when dragged (Maya's virtual slider).
class ScrubLabel : public QLabel {
    Q_OBJECT
public:
    using QLabel::QLabel;
    std::function<void()> onBegin;
    std::function<void(double pixels)> onDrag;
    std::function<void()> onEnd;

protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;

private:
    int startX_ = 0;
    bool dragging_ = false;
};

class ChannelBox : public QWidget {
    Q_OBJECT
public:
    explicit ChannelBox(Scene* scene, QWidget* parent = nullptr);

private:
    struct Channel {
        QString label;
        std::function<double(const ObjectState&)> get;
        std::function<void(ObjectState&, double)> set;
        double step = 0.01;
        bool integer = false;
        bool regenerate = false; // construction-history input: rebuild the mesh
        QLineEdit* edit = nullptr;
        double defaultValue = 0.0;
        QWidget* labelWidget = nullptr;
    };
public:
    // Right-click menu for channel `index` (Reset to Default, Copy/Paste Value, ...). Not shown.
    QMenu* channelMenu(int index);
    int channelCount() const { return int(channels_.size()); }
    QString channelLabel(int i) const { return channels_[size_t(i)].label; }
protected:
    void contextMenuEvent(QContextMenuEvent* e) override;
private:
    void rebuild();
    void refresh();
    void apply(Channel& c, double value, bool undoable);
    void addSection(const QString& title);
    void addChannel(Channel c);
    QString structureKey() const;

    Scene* scene_;
    QVBoxLayout* root_ = nullptr;
    QWidget* body_ = nullptr;
    QFormLayout* form_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    std::vector<Channel> channels_;
    std::vector<std::pair<uint32_t, ObjectState>> scrubStart_;
    QString key_;
};

} // namespace facet
