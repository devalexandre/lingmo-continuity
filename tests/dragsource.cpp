/*
 * SPDX-FileCopyrightText: 2026 LingmoOS Team
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
// Test helper: a window that starts an XDND drag of the files given on the
// command line as soon as the left button is pressed in it (what a file
// manager does), so the edge drag can be tried with xdotool on Xvfb.
#include <QDrag>
#include <QGuiApplication>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QRasterWindow>
#include <QUrl>

class Source : public QRasterWindow
{
public:
    QList<QUrl> urls;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(QRect(QPoint(0, 0), size()), QColor(0x2f, 0x7c, 0xf6));
        p.setPen(Qt::white);
        p.drawText(QRect(QPoint(0, 0), size()), Qt::AlignCenter, QStringLiteral("drag me (%1 files)").arg(urls.size()));
    }
    void mousePressEvent(QMouseEvent *) override
    {
        auto *drag = new QDrag(this);
        auto *mime = new QMimeData;
        mime->setUrls(urls);
        drag->setMimeData(mime);
        const Qt::DropAction result = drag->exec(Qt::CopyAction);
        qInfo("drag finished: %d", int(result));
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    Source w;
    for (int i = 1; i < argc; ++i)
        w.urls << QUrl::fromLocalFile(QString::fromLocal8Bit(argv[i]));
    w.setTitle(QStringLiteral("dragsource"));
    w.setGeometry(100, 100, 300, 160);
    w.show();
    return app.exec();
}
