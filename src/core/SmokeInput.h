// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>

class QQuickItem;

/**
 * Input that QML cannot make itself, for the smoke checks of `Main.qml` (exposed there as
 * `appSmokeInput`, only in a smoke run).
 */
class SmokeInput : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    /// Sends a wheel event (angle deltas in eighths of a degree, 120 a notch) to the middle of
    /// `item`, through its window as a real one goes. False if the item has no window.
    Q_INVOKABLE bool wheel(QQuickItem *item, int angleX, int angleY);
};
