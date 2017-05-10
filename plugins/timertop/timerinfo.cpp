/*
  timerinfo.cpp

  This file is part of GammaRay, the Qt application inspection and
  manipulation tool.

  Copyright (C) 2010-2017 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Thomas McGuire <thomas.mcguire@kdab.com>

  Licensees holding valid commercial KDAB GammaRay licenses may use this file in
  accordance with GammaRay Commercial License Agreement provided with the Software.

  Contact info@kdab.com if any conditions of this licensing are not clear to you.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "timerinfo.h"
#include "timermodel.h"

#include <core/util.h>
#include <core/probe.h>

#include <QObject>
#include <QTimer>
#include <QThread>

using namespace GammaRay;

namespace GammaRay {
uint qHash(const TimerId &id)
{
    switch (id.m_type) {
    case TimerId::InvalidType:
        Q_UNREACHABLE();
        break;

    case TimerId::QQmlTimerType:
    case TimerId::QTimerType:
        return ::qHash(id.m_timerAddress);

    case TimerId::QObjectType:
        return ::qHash(id.m_timerId);
    }

    return 0;
}
}

TimerId::TimerId()
    : m_type(InvalidType)
    , m_timerAddress(0)
    , m_timerId(-1)
{
}

TimerId::TimerId(QObject *timer)
    : m_type(QQmlTimerType)
    , m_timerAddress(quintptr(timer))
    , m_timer(timer)
    , m_timerId(-1)
{
    Q_ASSERT(m_timer);

    if (QTimer *t = qobject_cast<QTimer *>(timer)) {
        m_type = QTimerType;
    }
}

TimerId::TimerId(int timerId)
    : m_type(QObjectType)
    , m_timerAddress(0)
    , m_timerId(timerId)
{
    Q_ASSERT(m_timerId != -1);
}

bool TimerId::isValid() const
{
    return m_timer || m_timerId != -1;
}

TimerId::Type TimerId::type() const
{
    return m_type;
}

quintptr TimerId::address() const
{
    return m_timerAddress;
}

QTimer *TimerId::timer() const
{
    if (m_type != QTimerType)
        return nullptr;
    return qobject_cast<QTimer *>(m_timer);
}

QObject *TimerId::timerObject() const
{
    return m_timer;
}

int TimerId::timerId() const
{
    return m_timerId;
}

bool TimerId::operator==(const TimerId &other) const
{
    if (m_type != other.m_type)
        return false;

    switch (m_type) {
    case TimerId::InvalidType:
        Q_UNREACHABLE();
        break;

    case TimerId::QQmlTimerType:
    case TimerId::QTimerType:
        return m_timerAddress == other.m_timerAddress;

    case TimerId::QObjectType:
        return m_timerId == other.m_timerId;
    }

    return false;
}

bool TimerId::operator==(QObject *timer) const
{
    return m_timerAddress == quintptr(timer);
}

bool TimerId::operator==(int timerId) const
{
    return m_timerId == timerId;
}

bool TimerId::operator<(const TimerId &other) const
{
    if (m_timerAddress != 0) {
        if (other.m_timerAddress != 0)
            return m_timerAddress < other.m_timerAddress;
        return other.m_timerId != -1 ? m_timerAddress < quintptr(other.m_timerId) : false;
    } else if (m_timerId != -1) {
        if (other.m_timerId != -1)
            return m_timerId < other.m_timerId;
        return other.m_timerAddress != 0 ? quintptr(m_timerId) < other.m_timerAddress : false;
    }

    Q_ASSERT(false);
    return false;
}

TimerIdInfo::TimerIdInfo(const TimerId &id, QObject *receiver)
    : timerId(-1)
    , totalWakeups(0)
{
    update(id, receiver);
}

void TimerIdInfo::update(const TimerId &id, QObject *receiver)
{
    Q_ASSERT(id.isValid());
    const auto locker = Probe::objectLocker(receiver ? receiver : id.timerObject());
    const bool validObject = Probe::instance()->isValidObject(receiver ? receiver : id.timerObject());

    switch (id.type()) {
    case TimerId::InvalidType: {
        Q_UNREACHABLE();
        break;
    }

    case TimerId::QQmlTimerType: {
        const QObject *const timer = validObject ? id.timerObject() : nullptr;

        timerId = -1;
        objectName = validObject ? Util::displayString(timer) : Util::addressToString(id.timerObject());
        state = TimerModel::tr("None");

        if (timer) {
            const int interval = timer->property("interval").toInt();

            if (!timer->property("running").toBool())
                state = TimerModel::tr("Inactive (%1 ms)").arg(interval);
            else if (!timer->property("repeat").toBool())
                state = TimerModel::tr("Singleshot (%1 ms)").arg(interval);
            else
                state = TimerModel::tr("Repeating (%1 ms)").arg(interval);
        }

        break;
    }

    case TimerId::QTimerType: {
        const QTimer *const timer = validObject ? id.timer() : nullptr;

        timerId = timer ? timer->timerId() : -1;
        objectName = validObject ? Util::displayString(timer) : Util::addressToString(id.timerObject());
        state = TimerModel::tr("None");

        if (timer) {
            if (!timer->isActive())
                state = TimerModel::tr("Inactive (%1 ms)").arg(timer->interval());
            else if (timer->isSingleShot())
                state = TimerModel::tr("Singleshot (%1 ms)").arg(timer->interval());
            else
                state = TimerModel::tr("Repeating (%1 ms)").arg(timer->interval());
        }

        break;
    }

    case TimerId::QObjectType: {
        timerId = id.timerId();
        objectName = receiver
                ? (validObject ? Util::displayString(receiver) : Util::addressToString(receiver))
                : TimerModel::tr("Unknown QObject");
        state = TimerModel::tr("N/A");
        break;
    }
    }

    wakeupsPerSec = QStringLiteral("0");
    timePerWakeup = TimerModel::tr("N/A");
    maxWakeupTime = TimerModel::tr("N/A");
}
