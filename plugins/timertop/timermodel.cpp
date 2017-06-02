
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
#include <QAbstractEventDispatcher>

#include <iostream>

namespace GammaRay {
struct TimerIdData;
}

using namespace GammaRay;
using namespace std;

static TimerModel *s_timerModel = nullptr;
static const char s_qmlTimerClassName[] = "QQmlTimer";
static const char s_gammarayFilterProperty[] = "GammaRayFreeTimerFilter";
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

struct TimerIdData : TimerIdInfo
{
    TimerIdData()
        : totalWakeupsEvents(0)
        , lastReceiver(0)
    { }
    using TimerIdInfo::TimerIdInfo;

    void update(const TimerId &id, QObject *receiver = nullptr) override
    {
        // If the receiver changed, the timer was stopped / restarted and is no longer the same timer.
        if (lastReceiver != quintptr(receiver)) {
            clearHistory();
        }

        TimerIdInfo::update(id, receiver);
        lastReceiver = quintptr(receiver);
    }

    int totalWakeups(TimerId::Type type) const
    {
        Q_UNUSED(type);
        return totalWakeupsEvents;
    }

    QString wakeupsPerSec(TimerId::Type type) const
    {
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

    QString timePerWakeup(TimerId::Type type) const
    {
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

    QString maxWakeupTime(TimerId::Type type) const
    {
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

    void addEvent(const GammaRay::TimeoutEvent &event)
    {
        timeoutEvents.append(event);
        if (timeoutEvents.size() > s_maxTimeoutEvents)
            timeoutEvents.removeFirst();
        totalWakeupsEvents++;
        changed = true;
    }

    void clearHistory()
    {
        TimerIdInfo::totalWakeups = 0;
        TimerIdInfo::state.clear();
        TimerIdInfo::wakeupsPerSec.clear();
        TimerIdInfo::timePerWakeup.clear();
        TimerIdInfo::maxWakeupTime.clear();

        totalWakeupsEvents = 0;
        if (functionCallTimer.active())
            functionCallTimer.stop();
        timeoutEvents.clear();
        lastReceiver = 0;
        changed = true;
    }

    TimerIdInfo &toInfo(TimerId::Type type)
    {
        TimerIdInfo::totalWakeups =  TimerIdData::totalWakeups(type);
        TimerIdInfo::wakeupsPerSec = TimerIdData::wakeupsPerSec(type);
        TimerIdInfo::timePerWakeup = TimerIdData::timePerWakeup(type);
        TimerIdInfo::maxWakeupTime = TimerIdData::maxWakeupTime(type);
        return *this;
    }

    int totalWakeupsEvents;
    FunctionCallTimer functionCallTimer;
    QList<TimeoutEvent> timeoutEvents;
    quintptr lastReceiver; // free timers only

    bool changed;
};

class FreeTimerFilter : public QObject
{
public:
    using QObject::QObject;

    static void timerEventFilter(QObject *watched, QEvent *event)
    {
        QMutexLocker locker(Probe::objectLock());
        const QTimerEvent *const timerEvent = static_cast<QTimerEvent *>(event);
        Q_ASSERT(timerEvent->timerId() != -1);
        const QTimer *const timer = qobject_cast<QTimer *>(watched);

        // If there is a QTimer associated with this timer ID, don't handle it here, it will be handled
        // by the signal hooks preSignalActivate/postSignalActivate.
        if (timer && timer->timerId() == timerEvent->timerId()) {
            return;
        }

        const TimerId id(timerEvent->timerId());
        auto it = s_gatheredTimersData.find(id);

        if (it == s_gatheredTimersData.end()) {
            it = s_gatheredTimersData.insert(id, TimerIdData());
        }

        const TimeoutEvent timeoutEvent(QTime::currentTime(), -1);

        it.value().update(id, watched);
        it.value().addEvent(timeoutEvent);

        TimerModel::triggerPushChanges();
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Timer) {
            FreeTimerFilter::timerEventFilter(watched, event);
        }

        return QObject::eventFilter(watched, event);
    }
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
    } else {
        if (index.row() < m_sourceModel->rowCount() + m_freeTimersInfo.count())
            return &m_freeTimersInfo[index.row() - m_sourceModel->rowCount()];
    }

    return nullptr;
}

void TimerModel::triggerPushChanges()
{
    if (!TimerModel::isInitialized())
        return;

    QMutexLocker locker(Probe::objectLock());

    if (!s_timerModel->m_pushTimer) {
        s_timerModel->m_pushTimer = QSharedPointer<QTimer>(new QTimer);
        s_timerModel->m_pushTimer->setSingleShot(true);
        s_timerModel->m_pushTimer->setInterval(5000);
        s_timerModel->m_pushTimer->moveToThread(s_timerModel->thread());
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

    QMutexLocker locker(Probe::objectLock());
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

    QMutexLocker locker(Probe::objectLock());
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

    const TimeoutEvent timeoutEvent(QTime::currentTime(), it.value().functionCallTimer.stop());

    it.value().update(id);
    it.value().addEvent(timeoutEvent);

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
        } else {
            return createIndex(row, column, row - m_sourceModel->rowCount());
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
    return m_sourceModel->rowCount() + m_freeTimersInfo.count();
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
    if (event->type() == QEvent::Timer) {
        FreeTimerFilter::timerEventFilter(watched, event);
    }

    return QAbstractTableModel::eventFilter(watched, event);
}

void TimerModel::clearHistory()
{
    QMutexLocker locker(Probe::objectLock());

    for (auto it = s_gatheredTimersData.begin(); it != s_gatheredTimersData.end(); ) {
        switch (it.key().type()) {
        case TimerId::InvalidType:
        case TimerId::QObjectType:
            it = s_gatheredTimersData.erase(it);
            break;

        case TimerId::QQmlTimerType:
        case TimerId::QTimerType:
            it.value().clearHistory();
            ++it;
            break;
        }
    }

    // don't send dataChanged here to avoid network traffic for nothing.
    // triggerPushChanges will anyway update all non free timers on its next iteration.
    m_timersInfo.clear();

    if (!m_freeTimersInfo.isEmpty()) {
        beginRemoveRows(QModelIndex(), m_sourceModel->rowCount(), m_sourceModel->rowCount() + m_freeTimersInfo.count() - 1);
        m_freeTimersInfo.clear();
        endRemoveRows();
    }

    triggerPushChanges();
}

void TimerModel::objectCreated(QObject *object)
{
    // The probe object lock is (or should if called manually) already hold at this point
    if (QThread *thread = qobject_cast<QThread*>(object)) {
//        if (QAbstractEventDispatcher *dispatcher = QAbstractEventDispatcher::instance(thread)) {
//            FreeTimerFilter *filter = new FreeTimerFilter;
//            filter->moveToThread(thread);
//            dispatcher->setProperty(s_gammarayFilterProperty, QVariant::fromValue(filter));
//            dispatcher->installEventFilter(filter);
//            connect(dispatcher, SIGNAL(destroyed(QObject*)), filter, SLOT(deleteLater()));
//        }

        if (!thread->property(s_gammarayFilterProperty).isValid()) {
            FreeTimerFilter *filter = new FreeTimerFilter;
            filter->moveToThread(thread);
            thread->setProperty(s_gammarayFilterProperty, QVariant::fromValue(filter));
            thread->installEventFilter(filter);
            connect(thread, SIGNAL(destroyed(QObject*)), filter, SLOT(deleteLater()));
        }
    }
}

void TimerModel::applyChanges(const GammaRay::TimerIdInfoHash &changes)
{
    QSet<TimerId> handledIds;
    QVector<QPair<int, int>> dataChangedRanges; // pair of first/last
    QSet<int> qtimerIds;

    // QQmlTimer / QTimer
    for (int i = 0; i < m_sourceModel->rowCount(); ++i) {
        const QModelIndex idx = index(i, 0);
        const TimerId id = TimerId(static_cast<QObject *>(idx.internalPointer()));
        const auto cit = changes.constFind(id);
        auto it = m_timersInfo.find(id);

        if (it == m_timersInfo.end()) {
            it = m_timersInfo.insert(id, TimerIdInfo());
        }

        if (cit != changes.constEnd()) {
            handledIds << id;
            it.value() = cit.value();

            if (dataChangedRanges.isEmpty() || dataChangedRanges.last().second != i - 1) {
                dataChangedRanges << qMakePair(i, i);
            } else {
                dataChangedRanges.last().second = i;
            }
        }

        // Remember QTimer's id for later
        if (id.type() == TimerId::QTimerType && it.value().timerId != -1) {
            qtimerIds << it.value().timerId;
        }
    }

    // Existing free timers
    for (auto it = m_freeTimersInfo.begin(), end = m_freeTimersInfo.end(); it != end; ++it) {
        const TimerId id((*it).timerId);
        const auto cit = changes.constFind(id);

        if (cit != changes.constEnd()) {
            const int i = m_sourceModel->rowCount() + std::distance(m_freeTimersInfo.begin(), it);
            handledIds << id;
            (*it) = cit.value();

            if (dataChangedRanges.isEmpty() || dataChangedRanges.last().second != i - 1) {
                dataChangedRanges << qMakePair(i, i);
            } else {
                dataChangedRanges.last().second = i;
            }
        }
    }

    // Remaining new free timers
    int inserted = 0;
    for (auto it = changes.constBegin(), end = changes.constEnd(); it != end; ++it) {
        // If a TimerId of type TimerId::QObjectType just changed then this free timer id is still valid,
        // remove it from qtimerIds.
        if (it.key().type() == TimerId::QObjectType && qtimerIds.contains(it.key().timerId())) {
            qtimerIds.remove(it.key().timerId());
        }

        if (handledIds.contains(it.key()))
            continue;

        m_freeTimersInfo << it.value();
        ++inserted;
    }

    // Inform model about new rows
    if (inserted > 0) {
        const int first = m_sourceModel->rowCount() + m_freeTimersInfo.count() - inserted;
        const int last = m_sourceModel->rowCount() + m_freeTimersInfo.count() - 1;

        beginInsertRows(QModelIndex(), first, last);
        // We already inserted at end
        endInsertRows();
    }

    // Inform model about data changes
    foreach (const auto &range, dataChangedRanges) {
        emit dataChanged(index(range.first, 0), index(range.second, columnCount() - 1));
    }

    // Check for invalid free timers
    // Invalid QQmlTimer/QTimer are handled in the source model begin remove rows slot already.
    // We considere free timers invalid if an id is used by a qtimer already and free timer id
    // is not part of this changes.
    QVector<QPair<int, int>> removeRowsRanges; // pair of first/last

    for (int row = 0; row < m_freeTimersInfo.count(); ++row) {
        const TimerIdInfo &it = m_freeTimersInfo[row];

        // This is an invalid free timer
        if (qtimerIds.contains(it.timerId)) {
            const int i = m_sourceModel->rowCount() + row;

            if (removeRowsRanges.isEmpty() || removeRowsRanges.last().second != i - 1) {
                removeRowsRanges << qMakePair(i, i);
            } else {
                removeRowsRanges.last().second = i;
            }
        }
    }

    // Inform model about rows removal
    for (int i = removeRowsRanges.count() -1; i >= 0; --i) {
        const auto &range = removeRowsRanges[i];

        beginRemoveRows(QModelIndex(), range.first, range.second);
        m_freeTimersInfo.remove(range.first - m_sourceModel->rowCount(), range.second - range.first + 1);
        endRemoveRows();
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
    m_freeTimersInfo.clear();
}

void TimerModel::slotEndReset()
{
    endResetModel();
}
