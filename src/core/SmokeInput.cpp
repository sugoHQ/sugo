// SPDX-License-Identifier: GPL-3.0-or-later
#include "SmokeInput.h"

#include <QCoreApplication>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWheelEvent>

bool SmokeInput::wheel(QQuickItem *item, int angleX, int angleY)
{
    QQuickWindow *window = item ? item->window() : nullptr;
    if (!window)
        return false;
    const QPointF position = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    QWheelEvent event(position, window->mapToGlobal(position), QPoint(), QPoint(angleX, angleY), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(window, &event);
    return true;
}
