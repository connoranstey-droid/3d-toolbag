#pragma once
#include <QElapsedTimer>
#include <QWidget>

#include <functional>
#include <vector>

namespace facet {

// Radial menu in the style of Maya marking menus. Works both as a gesture (press, drag toward
// an item, release) and as a click menu (quick click opens it; click an item to run it).
class MarkingMenu : public QWidget {
    Q_OBJECT
public:
    enum Dir { N, NE, E, SE, S, SW, W, NW, List };
    struct Item {
        QString label;
        Dir dir = List;
        std::function<void()> fn;
        bool enabled = true;
        bool checked = false;
    };

    explicit MarkingMenu(QWidget* parent);
    void popup(QPoint globalCenter, std::vector<Item> items, const QString& title = {});
    void track(QPoint globalPos);
    void release(QPoint globalPos);
    bool isOpen() const { return isVisible(); }

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    QRect itemRect(int i) const;
    int itemAt(QPoint local) const;
    void activate(int i);

    std::vector<Item> items_;
    QString title_;
    QPoint center_, cursor_;
    int hover_ = -1;
    bool sticky_ = false;
    QElapsedTimer timer_;
};

// Hotbox: every main-menu title laid out around the cursor while Space is held.
class Hotbox : public QWidget {
    Q_OBJECT
public:
    struct Entry { QString title; class QMenu* menu; };
    explicit Hotbox(QWidget* parent);
    void showAt(QPoint globalCenter, std::vector<std::vector<Entry>> rows);
    void dismiss();

signals:
    // Click in the hotbox centre zone (Maya's centre marking menu: view switching).
    void centerClicked(QPoint globalPos);

protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;

private:
    struct Cell { QRect rect; QMenu* menu; QString title; };
    std::vector<Cell> cells_;
    int hover_ = -1;
    QPoint center_;
};

} // namespace facet
