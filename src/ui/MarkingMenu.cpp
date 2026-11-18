#include "ui/MarkingMenu.h"

#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace facet {

namespace {
const QPoint kDirOffset[8] = {{0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}};
constexpr int kRadius = 96;
constexpr int kItemH = 22;
} // namespace

MarkingMenu::MarkingMenu(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    hide();
}

void MarkingMenu::popup(QPoint globalCenter, std::vector<Item> items, const QString& title)
{
    items_ = std::move(items);
    title_ = title;
    setGeometry(parentWidget()->rect());
    center_ = cursor_ = mapFromGlobal(globalCenter);
    hover_ = -1;
    sticky_ = false;
    timer_.start();
    show();
    raise();
    update();
}

QRect MarkingMenu::itemRect(int i) const
{
    const Item& it = items_[size_t(i)];
    QFontMetrics fm(font());
    int w = fm.horizontalAdvance(it.checked ? QString::fromUtf8("✓ ") + it.label : it.label) + 22;
    if (it.dir != List) {
        QPoint o = kDirOffset[it.dir];
        float diag = (o.x() != 0 && o.y() != 0) ? 0.72f : 1.0f;
        QPoint c = center_ + QPoint(int(o.x() * kRadius * diag * 1.15f), int(o.y() * kRadius * diag * 0.78f));
        int x = o.x() > 0 ? c.x() : o.x() < 0 ? c.x() - w : c.x() - w / 2;
        return QRect(x, c.y() - kItemH / 2, w, kItemH);
    }
    // List items stack below the south slot.
    int idx = 0;
    for (int k = 0; k < i; ++k)
        if (items_[size_t(k)].dir == List) ++idx;
    // Leave a gap under the South item so a downward flick still means South.
    int top = center_.y() + int(kRadius * 0.78f) + kItemH * 2;
    return QRect(center_.x() - 85, top + idx * kItemH, 170, kItemH - 1);
}

int MarkingMenu::itemAt(QPoint p) const
{
    for (int i = 0; i < int(items_.size()); ++i)
        if (itemRect(i).adjusted(-2, -2, 2, 2).contains(p)) return i;
    QPoint d = p - center_;
    if (std::hypot(d.x(), d.y()) < 28) return -1;
    // Sector of the gesture direction.
    double ang = std::atan2(double(d.x()), double(-d.y())); // 0 = north, clockwise
    int sector = int(std::lround(ang / (M_PI / 4))) & 7;
    for (int i = 0; i < int(items_.size()); ++i)
        if (items_[size_t(i)].dir == sector) return i;
    return -1;
}

void MarkingMenu::activate(int i)
{
    hide();
    if (i >= 0 && items_[size_t(i)].enabled && items_[size_t(i)].fn) items_[size_t(i)].fn();
}

void MarkingMenu::track(QPoint g)
{
    cursor_ = mapFromGlobal(g);
    int h = itemAt(cursor_);
    if (h != hover_) hover_ = h;
    update();
}

void MarkingMenu::release(QPoint g)
{
    QPoint p = mapFromGlobal(g);
    QPoint d = p - center_;
    if (std::hypot(d.x(), d.y()) < 8 && timer_.elapsed() < 450) {
        sticky_ = true; // quick click: keep it open as a click menu
        setFocus();
        update();
        return;
    }
    activate(itemAt(p));
}

void MarkingMenu::mouseMoveEvent(QMouseEvent* e)
{
    cursor_ = e->position().toPoint();
    hover_ = itemAt(cursor_);
    update();
}

void MarkingMenu::mousePressEvent(QMouseEvent* e)
{
    int i = itemAt(e->position().toPoint());
    activate(i);
}

void MarkingMenu::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) hide();
}

void MarkingMenu::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 255, 255, 170), 1.5));
    p.drawLine(center_, cursor_);
    p.setBrush(QColor(40, 40, 40, 200));
    p.drawEllipse(center_, 5, 5);
    if (!title_.isEmpty()) {
        p.setPen(QColor(200, 200, 200));
        p.drawText(QRect(center_.x() - 100, center_.y() - 30, 200, 18), Qt::AlignCenter, title_);
    }
    QFontMetrics fm(font());
    for (int i = 0; i < int(items_.size()); ++i) {
        const Item& it = items_[size_t(i)];
        QRect r = itemRect(i);
        bool hot = i == hover_;
        QPainterPath path;
        path.addRoundedRect(r, 4, 4);
        p.fillPath(path, hot ? QColor(82, 133, 166) : QColor(58, 58, 58, 235));
        p.setPen(QPen(QColor(20, 20, 20), 1));
        p.drawPath(path);
        p.setPen(it.enabled ? QColor(235, 235, 235) : QColor(130, 130, 130));
        QString label = it.checked ? QString::fromUtf8("✓ ") + it.label : it.label;
        p.drawText(r.adjusted(10, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
    }
}

// ------------------------------------------------------------------------------------------

Hotbox::Hotbox(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    hide();
}

void Hotbox::showAt(QPoint g, std::vector<std::vector<Entry>> rows)
{
    setGeometry(parentWidget()->rect());
    center_ = mapFromGlobal(g);
    cells_.clear();
    QFontMetrics fm(font());
    int rowH = 26;
    int y = center_.y() - 70 - int(rows.size()) * rowH;
    for (const auto& row : rows) {
        int total = 0;
        for (const auto& e : row) total += fm.horizontalAdvance(e.title) + 26;
        int x = center_.x() - total / 2;
        for (const auto& e : row) {
            int w = fm.horizontalAdvance(e.title) + 22;
            cells_.push_back({QRect(x, y, w, rowH - 4), e.menu, e.title});
            x += w + 4;
        }
        y += rowH;
    }
    hover_ = -1;
    show();
    raise();
    update();
}

void Hotbox::dismiss() { hide(); }

void Hotbox::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0, 0, 0, 40));
    for (int i = 0; i < int(cells_.size()); ++i) {
        QPainterPath path;
        path.addRoundedRect(cells_[size_t(i)].rect, 3, 3);
        p.fillPath(path, i == hover_ ? QColor(82, 133, 166) : QColor(55, 55, 55, 235));
        p.setPen(QColor(230, 230, 230));
        p.drawText(cells_[size_t(i)].rect, Qt::AlignCenter, cells_[size_t(i)].title);
    }
    p.setPen(QPen(QColor(255, 255, 255, 90), 1, Qt::DashLine));
    p.setBrush(QColor(255, 255, 255, 18));
    p.drawEllipse(center_, 60, 60);
    p.setPen(QColor(220, 220, 220, 200));
    p.drawText(QRect(center_.x() - 60, center_.y() - 9, 120, 18), Qt::AlignCenter, "Views (click)");
    p.drawText(QRect(center_.x() - 150, center_.y() + 66, 300, 18), Qt::AlignCenter, "Hotbox — release Space to close");
}

void Hotbox::mouseMoveEvent(QMouseEvent* e)
{
    int h = -1;
    for (int i = 0; i < int(cells_.size()); ++i)
        if (cells_[size_t(i)].rect.contains(e->position().toPoint())) h = i;
    if (h != hover_) { hover_ = h; update(); }
}

void Hotbox::mousePressEvent(QMouseEvent* e)
{
    for (const auto& c : cells_)
        if (c.rect.contains(e->position().toPoint()) && c.menu) {
            QMenu* m = c.menu;
            QPoint at = mapToGlobal(c.rect.bottomLeft());
            hide();
            m->popup(at);
            return;
        }
    QPoint d = e->position().toPoint() - center_;
    if (std::hypot(d.x(), d.y()) < 70.0) {
        QPoint g = mapToGlobal(e->position().toPoint());
        hide();
        emit centerClicked(g);
        return;
    }
    hide();
}

} // namespace facet
