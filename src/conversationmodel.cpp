/******************************************************************************
**
** This file is part of libcommhistory.
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** Contact: Reto Zingg <reto.zingg@nokia.com>
**
** This library is free software; you can redistribute it and/or modify it
** under the terms of the GNU Lesser General Public License version 2.1 as
** published by the Free Software Foundation.
**
** This library is distributed in the hope that it will be useful, but
** WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
** or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
** License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with this library; if not, write to the Free Software Foundation, Inc.,
** 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
**
******************************************************************************/

#include <QtDBus/QtDBus>
#include <QDebug>
#include <QSqlQuery>
#include <QSqlError>

#include "eventmodel_p.h"
#include "conversationmodel.h"
#include "conversationmodel_p.h"
#include "constants.h"
#include "commhistorydatabase.h"
#include "databaseio_p.h"
#include "contactlistener.h"
#include "debug.h"

namespace {
static CommHistory::Event::PropertySet unusedProperties = CommHistory::Event::PropertySet()
                                             << CommHistory::Event::IsDraft
                                             << CommHistory::Event::IsMissedCall
                                             << CommHistory::Event::IsEmergencyCall
                                             << CommHistory::Event::BytesReceived
                                             << CommHistory::Event::EventCount;
}
namespace CommHistory {

ConversationModelPrivate::ConversationModelPrivate(EventModel *model)
            : EventModelPrivate(model)
            , filterType(Event::UnknownType)
            , filterAccount(QString())
            , filterDirection(Event::UnknownDirection)
{
    contactChangesEnabled = true;
    QDBusConnection::sessionBus().connect(
        QString(), QString(), "com.nokia.commhistory", "groupsUpdatedFull",
        this, SLOT(groupsUpdatedFullSlot(const QList<CommHistory::Group> &)));
    QDBusConnection::sessionBus().connect(
        QString(), QString(), "com.nokia.commhistory", GROUPS_DELETED_SIGNAL,
        this, SLOT(groupsDeletedSlot(const QList<int> &)));
    // remove call properties
    propertyMask -= unusedProperties;
}

void ConversationModelPrivate::groupsUpdatedFullSlot(const QList<CommHistory::Group> &groups)
{
    DEBUG() << Q_FUNC_INFO;
    if (filterDirection == Event::Outbound
        || filterGroupIds.isEmpty()
        || !propertyMask.contains(Event::Contacts))
        return;

    foreach (Group g, groups) {
        if (filterGroupIds.contains(g.id()) && !g.remoteUids().isEmpty()) {
            // update in memory events for contact info
            updateEvents(g, g.contacts(),
                         g.remoteUids().first());
            break;
        }
    }
}

void ConversationModelPrivate::groupsDeletedSlot(const QList<int> &groupIds)
{
    Q_Q(ConversationModel);
    bool changed = false;

    foreach (int group, groupIds) {
        if (filterGroupIds.remove(group))
            changed = true;
    }

    // XXX This could be more efficient by removing events from this group without
    // refreshing others
    if (changed)
        q->getEvents(filterGroupIds.toList());
}

// update contacts for all inbound events,
// should be called only for p2p chats
void ConversationModelPrivate::updateEvents(const Group &group,
                                            const QList<Event::Contact> &contacts,
                                            const QString &remoteUid)
{
    Q_Q(ConversationModel);

    for (int row = 0; row < eventRootItem->childCount(); row++) {
        Event &event = eventRootItem->eventAt(row);

        if (event.groupId() == group.id()
            && event.contacts() != contacts
            && event.direction() == Event::Inbound
            && remoteAddressMatch(event.remoteUid(), remoteUid)) {
            //update and continue
            event.setContacts(contacts);

            // XXX This would be more efficient by merging ranges..
            emit q->dataChanged(q->createIndex(row,
                                               EventModel::Contacts,
                                               eventRootItem->child(row)),
                                q->createIndex(row,
                                               EventModel::Contacts,
                                               eventRootItem->child(row)));
        }
    }
}

bool ConversationModelPrivate::acceptsEvent(const Event &event) const
{
    DEBUG() << __PRETTY_FUNCTION__ << event.id();
    if ((event.type() != Event::IMEvent
         && event.type() != Event::SMSEvent
         && event.type() != Event::MMSEvent
         && event.type() != Event::StatusMessageEvent))
        return false;

    if (filterType != Event::UnknownType &&
        event.type() != filterType) return false;

    if (!filterAccount.isEmpty() &&
        event.localUid() != filterAccount) return false;

    if (filterDirection != Event::UnknownDirection &&
        event.direction() != filterDirection) return false;

    if (!filterGroupIds.contains(event.groupId())) return false;

    DEBUG() << __PRETTY_FUNCTION__ << ": true";
    return true;
}

bool ConversationModelPrivate::fillModel(int start, int end, QList<CommHistory::Event> events)
{
    Q_UNUSED(start);
    Q_UNUSED(end);

    DEBUG() << __FUNCTION__ << ": read" << events.count() << "messages";

    Q_Q(ConversationModel);

    q->beginInsertRows(QModelIndex(), q->rowCount(),
                       q->rowCount() + events.count() - 1);
    foreach (Event event, events) {
        eventRootItem->appendChild(new EventTreeItem(event, eventRootItem));
    }
    q->endInsertRows();

    return true;
}

QSqlQuery ConversationModelPrivate::buildQuery() const
{
    QList<int> groups = filterGroupIds.toList();
    QString q;
    int unionCount = 0;

    qint64 firstTimestamp = 0;
    int firstId = -1;
    if (eventRootItem->childCount() > 0) {
        Event firstEvent = eventRootItem->eventAt(eventRootItem->childCount() - 1);
        firstTimestamp = firstEvent.startTime().toTime_t();
        firstId = firstEvent.id();
    }

    QString filters;
    if (!filterAccount.isEmpty())
        filters += "AND Events.localUid = :filterAccount ";
    if (filterType != Event::UnknownType)
        filters += "AND Events.type = :filterType ";
    if (filterDirection != Event::UnknownDirection)
        filters += "AND Events.direction = :filterDirection ";
    if (firstId >= 0) {
        filters += "AND (Events.startTime < :firstTimestamp OR (Events.startTime = :firstTimestamp "
                    "AND Events.id < :firstId)) ";
    }

    /* Rather than the intuitive solution of groupId IN (1,2),
     * this query is built as:
     *
     * SELECT .. FROM Events WHERE groupId=1
     * UNION ALL
     * SELECT .. FROM Events WHERE groupId=2
     *
     * Because SQLite is unable to use indexes for ORDER BY after a IN
     * or OR expression in the query, yet somehow is able to use that indexes
     * on the UNION ALL of these queries. This is true at least up to SQLite
     * 3.8.1. */
    do {
        if (unionCount)
            q += "UNION ALL ";
        q += DatabaseIOPrivate::eventQueryBase();

        if (unionCount < groups.size())
            q += "WHERE Events.groupId = " + QString::number(groups[unionCount]) + " ";
        else
            q += "WHERE 1 ";

        q += filters;

        unionCount++;
    } while (unionCount < groups.size());

    q += "ORDER BY Events.startTime DESC, Events.id DESC ";

    if (queryLimit > 0)
        q += "LIMIT " + QString::number(queryLimit);
    else if (queryMode == EventModel::StreamedAsyncQuery && chunkSize > 0)
        q += "LIMIT " + QString::number((firstId < 0 && firstChunkSize > 0) ? firstChunkSize : chunkSize);

    QSqlQuery query = CommHistoryDatabase::prepare(q.toLatin1(), DatabaseIOPrivate::instance()->connection());
    if (!filterAccount.isEmpty())
        query.bindValue(":filterAccount", filterAccount);
    if (filterType != Event::UnknownType)
        query.bindValue(":filterType", filterType);
    if (filterDirection != Event::UnknownDirection)
        query.bindValue(":filterDirection", filterDirection);
    if (firstId >= 0) {
        query.bindValue(":firstTimestamp", firstTimestamp);
        query.bindValue(":firstId", firstId);
    }

    return query;
}

void ConversationModelPrivate::eventsReceivedSlot(int start, int end, QList<CommHistory::Event> events)
{
    // There is no more data when a query returns no rows
    if (queryMode == EventModel::StreamedAsyncQuery && events.size() == 0)
        isReady = true;

    EventModelPrivate::eventsReceivedSlot(start, end, events);
}

void ConversationModelPrivate::modelUpdatedSlot(bool successful)
{
    if (queryMode == EventModel::StreamedAsyncQuery) {
        if (isReady) {
            if (successful) {
                if (messagePartsReady)
                    emit modelReady(true);
            } else {
                emit modelReady(false);
            }
        }
    } else{
        EventModelPrivate::modelUpdatedSlot(successful);
    }
}

bool ConversationModelPrivate::isModelReady() const
{
    return isReady;
}

ConversationModel::ConversationModel(QObject *parent)
        : EventModel(*new ConversationModelPrivate(this), parent)
{
}

ConversationModel::~ConversationModel()
{
}

bool ConversationModel::setFilter(Event::EventType type,
                                  const QString &account,
                                  Event::EventDirection direction)
{
    Q_D(ConversationModel);

    d->filterType = type;
    d->filterAccount = account;
    d->filterDirection = direction;

    if (!d->filterGroupIds.isEmpty()) {
        return getEvents(d->filterGroupIds.toList());
    }

    return true;
}

bool ConversationModel::getEvents(int groupId)
{
    return getEvents(QList<int>() << groupId);
}

bool ConversationModel::getEvents(QList<int> groupIds)
{
    Q_D(ConversationModel);

    d->filterGroupIds = QSet<int>::fromList(groupIds);

    beginResetModel();
    d->clearEvents();
    endResetModel();

    if (groupIds.isEmpty())
        return true;

    QSqlQuery query = d->buildQuery();
    return d->executeQuery(query);
}

bool ConversationModel::canFetchMore(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    Q_D(const ConversationModel);

    return !d->isModelReady();
}

void ConversationModel::fetchMore(const QModelIndex &parent)
{
    Q_UNUSED(parent);
    Q_D(ConversationModel);

    // isModelReady() is true when there are no more events to request
    if (d->isModelReady() || d->eventRootItem->childCount() < 1)
        return;

    QSqlQuery query = d->buildQuery();
    d->executeQuery(query);
}

}
