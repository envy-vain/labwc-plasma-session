#pragma once
// Shared tray-popup behaviour: frameless, closes on focus loss / Esc, and a tray click that arrives
// right after the focus-loss hide counts as "close" instead of reopening it.
#include <QElapsedTimer>
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QWidget>

class Popup : public QWidget {
public:
    explicit Popup(const QString &title)
    {
        setWindowTitle(title);
        setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    }

    void toggleVisible()
    {
        if (isVisible())
            hide();
        else if (!hiddenAt.isValid() || hiddenAt.elapsed() > 300)
            open();
    }

    void open()
    {
        show(); // first, so aboutToOpen() sees isVisible(); nothing paints before we return to the loop
        aboutToOpen();
        raise();
        activateWindow();
    }

protected:
    virtual void aboutToOpen() { }

    void changeEvent(QEvent *e) override
    {
        if (e->type() == QEvent::ActivationChange && !isActiveWindow() && isVisible())
            hide();
        QWidget::changeEvent(e);
    }
    void hideEvent(QHideEvent *e) override
    {
        hiddenAt.start();
        QWidget::hideEvent(e);
    }
    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape)
            hide();
        else
            QWidget::keyPressEvent(e);
    }

    static QLabel *dimLabel(QWidget *parent = nullptr)
    {
        auto *l = new QLabel(parent);
        QPalette p = l->palette();
        QColor c = p.color(QPalette::WindowText);
        c.setAlphaF(0.65);
        p.setColor(QPalette::WindowText, c);
        l->setPalette(p);
        return l;
    }

private:
    QElapsedTimer hiddenAt;
};
