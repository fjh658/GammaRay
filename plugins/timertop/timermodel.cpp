
/*
  timermodel.cpp

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
#include "timermodel.h"
#include "functioncalltimer.h"

#include <common/objectmodel.h>
#include <common/objectid.h>

#include <core/util.h>
#include <core/probe.h>

#include <QTimerEvent>
#include <QTime>
#include <QTimer>
#include <QThread>
#include <QCoreApplication>

#include <iostream>

namespace GammaRay {
class TimerIdData;
}

using namespace GammaRay;
using namespace std;

static TimerModel *s_timerModel = nullptr;
static const char s_qmlTimerClassName[] = "QQmlTimer";
static QHash<TimerId, TimerIdData> s_gatheredTimersData;
static const int s_maxTimeoutEvents = 1000;
static const int s_maxTimeSpan = 10000;

namespace GammaRay {
struct TimeoutEvent
{
    TimeoutEvent(const QTime &timeStamp = QTime(), int executionTime = -1)
        : timeStamp(timeStamp)
        , executionTime(executionTime)
    { }

    QTime timeStamp;
    int executionTime;
};

class TimerIdData : public TimerIdInfo
{
public:
    using TimerIdInfo::TimerIdInfo;

    int totalWakeups(TimerId::Type type) const {
        Q_UNUSED(type);
        return totalWakeupsEvents;
    }

    QString wakeupsPerSec(TimerId::Type type) const {
        Q_UNUSED(type);

        int totalWakeups = 0;
        int start = 0;
        int end = timeoutEvents.size() - 1;
        for (int i = end; i >= 0; i--) {
            const TimeoutEvent &event = timeoutEvents.at(i);
            if (event.timeStamp.msecsTo(QTime::currentTime()) > s_maxTimeSpan) {
                start = i;
                break;
            }
            totalWakeups++;
        }

        if (totalWakeups > 0 && end > start) {
            const QTime startTime = timeoutEvents[start].timeStamp;
            const QTime endTime = timeoutEvents[end].timeStamp;
            const int timeSpan = startTime.msecsTo(endTime);
            const float wakeupsPerSec = totalWakeups / (float)timeSpan * 1000.0f;
            return QString::number(wakeupsPerSec, 'f', 1);
        }
        return QStringLiteral("0");
    }

    QString timePerWakeup(TimerId::Type type) const {
        if (type == TimerId::QObjectType)
            return QStringLiteral("N/A");

        int totalWakeups = 0;
        int totalTime = 0;
        for (int i = timeoutEvents.size() - 1; i >= 0; i--) {
            const TimeoutEvent &event = timeoutEvents.at(i);
            if (event.timeStamp.msecsTo(QTime::currentTime()) > s_maxTimeSpan)
                break;
            totalWakeups++;
            totalTime += event.executionTime;
        }

        if (totalWakeups > 0)
            return QString::number(totalTime / (float)totalWakeups, 'f', 1);
        return QStringLiteral("N/A");
    }

    QString maxWakeupTime(TimerId::Type type) const {
        if (type == TimerId::QObjectType)
            return QStringLiteral("N/A");

        int max = 0;
        for (int i = 0; i < timeoutEvents.size(); i++) {
            const TimeoutEvent &event = timeoutEvents.at(i);
            if (event.executionTime > max)
                max = event.executionTime;
        }
        return QString::number(max);
    }

    void addEvent(const GammaRay::TimeoutEvent &event) {
        timeoutEvents.append(event);
        if (timeoutEvents.size() > s_maxTimeoutEvents)
            timeoutEvents.removeFirst();
        totalWakeupsEvents++;
        changed = true;
    }

    TimerIdInfo &toInfo(TimerId::Type type) {
        TimerIdInfo::totalWakeups =  TimerIdData::totalWakeups(type);
        TimerIdInfo::wakeupsPerSec = TimerIdData::wakeupsPerSec(type);
        TimerIdInfo::timePerWakeup = TimerIdData::timePerWakeup(type);
        TimerIdInfo::maxWakeupTime = TimerIdData::maxWakeupTime(type);
        return *this;
    }

    int totalWakeupsEvents;
    FunctionCallTimer functionCallTimer;
    QList<TimeoutEvent> timeoutEvents;

    bool changed;
};
}

Q_DECLARE_METATYPE(GammaRay::TimeoutEvent)

TimerModel::TimerModel(QObject *parent)
    : QAbstractTableModel(parent)
    , m_sourceModel(nullptr)
    , m_timeoutIndex(QTimer::staticMetaObject.indexOfSignal("timeout()"))
    , m_qmlTimerTriggeredIndex(-1)
    , m_pushTimerStart(QTimer::staticMetaObject.method(QTimer::staticMetaObject.indexOfSlot("start()")))
{
    qRegisterMetaType<TimerId>();
    qRegisterMetaType<TimerIdInfo>();
    qRegisterMetaType<TimerIdInfoHash>();
    qRegisterMetaType<GammaRay::TimeoutEvent>();

    const int applyChangesIndex = TimerModel::staticMetaObject.indexOfSlot("applyChanges(GammaRay::TimerIdInfoHash)");
    m_applyChangesMethod = TimerModel::staticMetaObject.method(applyChangesIndex);
    Q_ASSERT(m_applyChangesMethod.methodIndex() != -1);
}

const TimerIdInfo *TimerModel::findTimerInfo(const QModelIndex &index) const
{
    if (index.row() < m_sourceModel->rowCount()) {
        const QModelIndex sourceIndex = m_sourceModel->index(index.row(), 0);
        QObject * const timerObject = sourceIndex.data(ObjectModel::ObjectRole).value<QObject *>();
        const TimerId id = TimerId(timerObject);
        auto it = m_timersInfo.find(id);

        if (it == m_timersInfo.end()) {
            it = m_timersInfo.insert(id, TimerIdInfo(id));
        }

        if (it != m_timersInfo.end())
            return &it.value();
    }

    return nullptr;
}

void TimerModel::triggerPushChanges()
{
    if (!TimerModel::isInitialized())
        return;

    if (!s_timerModel->m_pushTimer) {
        s_timerModel->m_pushTimer = QSharedPointer<QTimer>(new QTimer);
        s_timerModel->m_pushTimer->setSingleShot(true);
        s_timerModel->m_pushTimer->setInterval(5000);
        QObject::connect(s_timerModel->m_pushTimer.data(), &QTimer::timeout,
                         s_timerModel->m_pushTimer.data(), TimerModel::pushChanges, Qt::QueuedConnection);
    }

    if (!s_timerModel->m_pushTimer->isActive()) {
        s_timerModel->m_pushTimerStart.invoke(s_timerModel->m_pushTimer.data(), Qt::QueuedConnection);
    }
}

void TimerModel::pushChanges()
{
    if (!TimerModel::isInitialized())
        return;

    QMutexLocker locker(Probe::objectLock());
    TimerIdInfoHash infoHash;

    infoHash.reserve(s_gatheredTimersData.count());
    for (auto it = s_gatheredTimersData.begin(), end = s_gatheredTimersData.end(); it != end; ++it) {
        TimerIdData &itInfo = it.value();

        if (itInfo.changed) {
            infoHash.insert(it.key(), itInfo.toInfo(it.key().type()));
            itInfo.changed = false;
        }
    }
    infoHash.squeeze();

    s_timerModel->m_applyChangesMethod.invoke(s_timerModel, Qt::QueuedConnection,
                                Q_ARG(GammaRay::TimerIdInfoHash, infoHash));
}

TimerModel::~TimerModel()
{
    m_timersInfo.clear();
    s_timerModel = nullptr;

    QMutexLocker locker(Probe::objectLock());
    s_gatheredTimersData.clear();
}

bool TimerModel::isInitialized()
{
    return s_timerModel != nullptr;
}

TimerModel *TimerModel::instance()
{
    if (!s_timerModel)
        s_timerModel = new TimerModel;

    Q_ASSERT(s_timerModel);
    return s_timerModel;
}

void TimerModel::preSignalActivate(QObject *caller, int methodIndex)
{
    // We are in the thread of the caller emmiting the signal
    Q_ASSERT(TimerModel::isInitialized());

    const auto locker = Probe::objectLocker(caller);
    const bool isQTimer = qobject_cast<QTimer *>(caller) != nullptr;
    const bool isQQmlTimer = caller->inherits(s_qmlTimerClassName);

    if (isQQmlTimer && m_qmlTimerTriggeredIndex < 0) {
        m_qmlTimerTriggeredIndex = caller->metaObject()->indexOfMethod("triggered()");
        Q_ASSERT(m_qmlTimerTriggeredIndex != -1);
    }

    if (!(isQTimer && m_timeoutIndex == methodIndex) &&
            !(isQQmlTimer && m_qmlTimerTriggeredIndex == methodIndex))
        return;

    if (!s_gatheredTimersData[TimerId(caller)].functionCallTimer.start()) {
        cout << "TimerModel::preSignalActivate(): Recursive timeout for timer "
             << (void *)caller << "!" << endl;
        return;
    }
}

void TimerModel::postSignalActivate(QObject *caller, int methodIndex)
{
    // We are in the thread of the caller emmiting the signal
    Q_UNUSED(methodIndex);
    Q_ASSERT(TimerModel::isInitialized());

    const auto locker = Probe::objectLocker(caller);
    const TimerId id(caller);
    auto it = s_gatheredTimersData.find(id);

    if (it == s_gatheredTimersData.end()) {
        const bool isQTimer = qobject_cast<QTimer *>(caller) != nullptr;
        const bool isQQmlTimer = caller->inherits(s_qmlTimerClassName);
        Q_ASSERT(!(isQTimer && m_timeoutIndex == methodIndex) &&
                !(isQQmlTimer && m_qmlTimerTriggeredIndex == methodIndex));
        return;
    }

    if (!it.value().functionCallTimer.active()) {
        cout << "TimerModel::postSignalActivate(): Timer not active: "
             << (void *)caller << "!" << endl;
        return;
    }

    const TimeoutEvent event(QTime::currentTime(), it.value().functionCallTimer.stop());

    it.value().update(id);
    it.value().addEvent(event);

    triggerPushChanges();
}

void TimerModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    Q_ASSERT(!m_sourceModel);
    beginResetModel();
    m_sourceModel = sourceModel;

    connect(m_sourceModel, SIGNAL(rowsAboutToBeInserted(QModelIndex,int,int)),
            this, SLOT(slotBeginInsertRows(QModelIndex,int,int)));
    connect(m_sourceModel, SIGNAL(rowsInserted(QModelIndex,int,int)),
            this, SLOT(slotEndInsertRows()));
    connect(m_sourceModel, SIGNAL(rowsAboutToBeRemoved(QModelIndex,int,int)),
            this, SLOT(slotBeginRemoveRows(QModelIndex,int,int)));
    connect(m_sourceModel, SIGNAL(rowsRemoved(QModelIndex,int,int)),
            this, SLOT(slotEndRemoveRows()));
    connect(m_sourceModel, SIGNAL(modelAboutToBeReset()),
            this, SLOT(slotBeginReset()));
    connect(m_sourceModel, SIGNAL(modelReset()),
            this, SLOT(slotEndReset()));
    connect(m_sourceModel, SIGNAL(layoutAboutToBeChanged()),
            this, SLOT(slotBeginReset()));
    connect(m_sourceModel, SIGNAL(layoutChanged()),
            this, SLOT(slotEndReset()));

    endResetModel();
}

QModelIndex TimerModel::index(int row, int column, const QModelIndex &parent) const
{
    if (hasIndex(row, column, parent)) {
        if (row < m_sourceModel->rowCount())  {
            const QModelIndex sourceIndex = m_sourceModel->index(row, 0);
            QObject *const timerObject = sourceIndex.data(ObjectModel::ObjectRole).value<QObject *>();
            return createIndex(row, column, timerObject);
        }

        Q_UNREACHABLE();
    }

    return QModelIndex();
}

int TimerModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return ColumnCount;
}

int TimerModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    if (!m_sourceModel || parent.isValid())
        return 0;
    return m_sourceModel->rowCount();
}

QVariant TimerModel::data(const QModelIndex &index, int role) const
{
    if (!m_sourceModel || !index.isValid())
        return QVariant();

    if (role == Qt::DisplayRole) {
        const TimerIdInfo *const timerInfo = findTimerInfo(index);

        if (!timerInfo)
            return QVariant();

        switch (index.column()) {
        case ObjectNameColumn:
            return timerInfo->objectName;
        case StateColumn:
            return timerInfo->state;
        case TotalWakeupsColumn:
            return timerInfo->totalWakeups;
        case WakeupsPerSecColumn:
            return timerInfo->wakeupsPerSec;
        case TimePerWakeupColumn:
            return timerInfo->timePerWakeup;
        case MaxTimePerWakeupColumn:
            return timerInfo->maxWakeupTime;
        case TimerIdColumn:
            return timerInfo->timerId;
        case ColumnCount:
            break;
        }
    }

    if (index.column() == 0) {
        if (role == TimerModel::ObjectIdRole) {
            return QVariant::fromValue(static_cast<QObject *>(index.internalPointer()));
        }
    }

    return QVariant();
}

QVariant TimerModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole) {
        switch (section) {
        case ObjectNameColumn:
            return tr("Object Name");
        case StateColumn:
            return tr("State");
        case TotalWakeupsColumn:
            return tr("Total Wakeups");
        case WakeupsPerSecColumn:
            return tr("Wakeups/Sec");
        case TimePerWakeupColumn:
            return tr("Time/Wakeup [uSecs]");
        case MaxTimePerWakeupColumn:
            return tr("Max Wakeup Time [uSecs]");
        case TimerIdColumn:
            return tr("Timer ID");
        case ColumnCount:
            break;
        }
    }
    return QAbstractTableModel::headerData(section, orientation, role);
}

QMap<int, QVariant> TimerModel::itemData(const QModelIndex &index) const
{
    auto d = QAbstractTableModel::itemData(index);
    if (index.column() == 0)
        d.insert(TimerModel::ObjectIdRole, QVariant::fromValue(static_cast<QObject *>(index.internalPointer())));
    return d;
}

bool TimerModel::eventFilter(QObject *watched, QEvent *event)
{
    return QAbstractTableModel::eventFilter(watched, event);
}

void TimerModel::applyChanges(const GammaRay::TimerIdInfoHash &changes)
{
    QVector<QPair<int, int>> ranges; // pair of first/last

    for (int i = 0; i < rowCount(); ++i) {
        const QModelIndex idx = index(i, 0);
        const TimerId id = TimerId(static_cast<QObject *>(idx.internalPointer()));
        auto it = changes.constFind(id);

        if (it != changes.constEnd()) {
            m_timersInfo[id] = it.value();

            if (ranges.isEmpty() || ranges.last().second != i - 1) {
                ranges << qMakePair(i, i);
            } else {
                ranges.last().second = i;
            }
        }
    }

    foreach (const auto &range, ranges) {
        emit dataChanged(index(range.first, 0), index(range.second, columnCount() - 1));
    }
}

void TimerModel::slotBeginRemoveRows(const QModelIndex &parent, int start, int end)
{
    Q_UNUSED(parent);

    QMutexLocker locker(Probe::objectLock());

    beginRemoveRows(QModelIndex(), start, end);

    // TODO: Use a delayed timer for that so the hash is iterated once only for a
    // group of successive removal ?
    for (auto it = m_timersInfo.begin(), end = m_timersInfo.end(); it != end; ++it) {
        if (!it.key().isValid()) {
            s_gatheredTimersData.remove(it.key());
            it = m_timersInfo.erase(it);

            if (it == m_timersInfo.end())
                break;
        }
    }
}

void TimerModel::slotEndRemoveRows()
{
    endRemoveRows();
}

void TimerModel::slotBeginInsertRows(const QModelIndex &parent, int start, int end)
{
    Q_UNUSED(parent);
    beginInsertRows(QModelIndex(), start, end);
}

void TimerModel::slotEndInsertRows()
{
    endInsertRows();
}

void TimerModel::slotBeginReset()
{
    QMutexLocker locker(Probe::objectLock());

    beginResetModel();

    s_gatheredTimersData.clear();
    m_timersInfo.clear();
}

void TimerModel::slotEndReset()
{
    endResetModel();
}
