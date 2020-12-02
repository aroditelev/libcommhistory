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

#include <QtTest/QtTest>

#include <seasidecache.h>

#include <qtcontacts-extensions.h>
#include <qtcontacts-extensions_impl.h>

#include <QContactOriginMetadata>
#include <qcontactoriginmetadata_impl.h>

#include <QContact>
#include <QContactManager>
#include <QContactDetail>
#include <QContactDetailFilter>
#include <QContactIntersectionFilter>
#include <QContactRelationshipFilter>
#include <QContactUnionFilter>

#include <QContactFavorite>
#include <QContactName>
#include <QContactNickname>
#include <QContactOnlineAccount>
#include <QContactPhoneNumber>

#include <QEventLoop>
#include <QFile>
#include <QTextStream>

#include "eventmodel.h"
#include "groupmodel.h"
#include "singleeventmodel.h"
#include "event.h"
#include "recipient.h"
#include "common.h"
#include "commonutils.h"
#include "contactlistener.h"

QTCONTACTS_USE_NAMESPACE

using namespace QtContactsSqliteExtensions;

namespace {

static int contactNumber = 0;

QContactManager *createManager()
{
    return SeasideCache::manager();
}

QContactManager *manager()
{
    static QContactManager *manager = createManager();
    return manager;
}

QContact createTestContact(const QString &name, const QString &remoteUid, const QString &localUid, const QString &contactUri)
{
    QContact contact;

    if (!localUid.isEmpty() && !localUidComparesPhoneNumbers(localUid)) {
        // Create a metadata detail to link the contact with the account
        QContactOriginMetadata metadata;
        metadata.setGroupId(localUid);
        metadata.setId(remoteUid);
        metadata.setEnabled(true);
        if (!contact.saveDetail(&metadata)) {
            qWarning() << "Unable to add metadata to contact:" << contactUri;
            return QContact();
        }

        QContactOnlineAccount qcoa;
        qcoa.setValue(QContactOnlineAccount__FieldAccountPath, localUid);
        qcoa.setAccountUri(remoteUid);
        if (!contact.saveDetail(&qcoa)) {
            qWarning() << "Unable to add online account to contact:" << contactUri;
            return QContact();
        }
    } else {
        QContactPhoneNumber phoneNumberDetail;
        phoneNumberDetail.setNumber(remoteUid);
        if (!contact.saveDetail(&phoneNumberDetail)) {
            qWarning() << "Unable to add phone number to contact:" << contactUri;
            return QContact();
        }
    }

    QContactName nameDetail;
    nameDetail.setLastName(name);
    if (!contact.saveDetail(&nameDetail)) {
        qWarning() << "Unable to add name to contact:" << contactUri;
        return QContact();
    }

    return contact;
}

}

namespace CommHistory {

void waitForSignal(QObject *object, const char *signal)
{
    QEventLoop loop;
    QObject::connect(object, signal, &loop, SLOT(quit()));
    loop.exec();
}

}

using namespace CommHistory;

const int numWords = 23;
const char* msgWords[] = { "lorem","ipsum","dolor","sit","amet","consectetur",
    "adipiscing","elit","in","imperdiet","cursus","lacus","vitae","suscipit",
    "maecenas","bibendum","rutrum","dolor","at","hendrerit",":)",":P","OMG!!" };
quint64 allTicks = 0;
quint64 idleTicks = 0;

QSet<QContactId> addedContactIds;
QSet<int> addedEventIds;

int addTestEvent(EventModel &model,
                 Event::EventType type,
                 Event::EventDirection direction,
                 const QString &account,
                 int groupId,
                 const QString &text,
                 bool isDraft,
                 bool isMissedCall,
                 const QDateTime &when,
                 const QString &remoteUid,
                 bool toModelOnly,
                 const QString &messageToken,
                 const QString &subscriberIdentity)
{
    Event event;
    event.setType(type);
    event.setDirection(direction);
    event.setGroupId(groupId);
    event.setStartTime(when);
    if (type == Event::CallEvent)
        event.setEndTime(when.addSecs(TESTCALL_SECS));
    else
        event.setEndTime(event.startTime());
    event.setLocalUid(account);
    if (remoteUid.isEmpty()) {
        event.setRecipients(Recipient(account, type == Event::SMSEvent ? "555123456" : "td@localhost"));
    } else {
        event.setRecipients(Recipient(account, remoteUid));
    }
    event.setFreeText(text);
    event.setIsDraft( isDraft );
    event.setIsMissedCall( isMissedCall );
    event.setMessageToken(messageToken);
    event.setSubscriberIdentity(subscriberIdentity);
    if (model.addEvent(event, toModelOnly)) {
        addedEventIds.insert(event.id());
        return event.id();
    }
    return -1;
}

void addTestGroups(Group &group1, Group &group2)
{
    addTestGroup(group1,
                 "/org/freedesktop/Telepathy/Account/gabble/jabber/dut_40localhost0",
                 "td@localhost");
    addTestGroup(group2,
                 "/org/freedesktop/Telepathy/Account/gabble/jabber/dut_40localhost0",
                 "td2@localhost");
}

void addTestGroup(Group& grp, QString localUid, QString remoteUid)
{
    GroupModel groupModel;
    groupModel.setResolveContacts(GroupManager::DoNotResolve);
    grp.setLocalUid(localUid);
    grp.setRecipients(RecipientList::fromUids(localUid, QStringList() << remoteUid));
    QSignalSpy ready(&groupModel, SIGNAL(groupsCommitted(QList<int>,bool)));
    QVERIFY(groupModel.addGroup(grp));

    QVERIFY(waitSignal(ready));
    QVERIFY(ready.first().at(1).toBool());
}

QContactId localContactForAggregate(const QContactId &aggregateId)
{
    foreach (const QContactRelationship &relationship, manager()->relationships(QContactRelationship::Aggregates(), aggregateId, QContactRelationship::First)) {
        return relationship.second();
    }
    qWarning() << "error finding local contact for aggregate:" << aggregateId.localId();
    return QContactId();
}

int addTestContact(const QString &name, const QString &remoteUid, const QString &localUid)
{
    QString contactUri = QString("<testcontact:%1>").arg(contactNumber++);

    QContact contact(createTestContact(name, remoteUid, localUid, contactUri));
    if (contact.isEmpty())
        return -1;

    if (!manager()->saveContact(&contact)) {
        qWarning() << "Unable to store contact:" << contactUri;
        return -1;
    }

    foreach (const QContactRelationship &relationship, manager()->relationships(QContactRelationship::Aggregates(), contact.id(), QContactRelationship::Second)) {
        const QContactId &aggId = relationship.first();
        qDebug() << "********** contact id" << aggId;
        addedContactIds.insert(aggId);
        return internalContactId(aggId);
    }

    qWarning() << "Could not find aggregator";
    return internalContactId(contact.id());
}

QList<int> addTestContacts(const QList<QPair<QString, QPair<QString, QString> > > &details)
{
    QList<int> ids;
    QList<QContact> contacts;

    QList<QPair<QString, QPair<QString, QString> > >::const_iterator it = details.constBegin(), end = details.constEnd();
    for ( ; it != end; ++it) {
        QString contactUri = QString("<testcontact:%1>").arg(contactNumber++);

        QContact contact(createTestContact(it->first, it->second.first, it->second.second, contactUri));
        if (!contact.isEmpty())
            contacts.append(contact);
    }

    if (!manager()->saveContacts(&contacts)) {
        qWarning() << "Unable to store contacts";
        return ids;
    }

    QSet<QContactId> constituentIds;
    foreach (const QContact &contact, contacts) {
        constituentIds.insert(contact.id());
    }
    foreach (const QContactRelationship &relationship, manager()->relationships(QContactRelationship::Aggregates())) {
        if (constituentIds.contains(relationship.second())) {
            const QContactId &aggId = relationship.first();
            qDebug() << "********** contact id" << aggId;
            addedContactIds.insert(aggId);
            ids.append(internalContactId(aggId));
        }
    }

    return ids;
}

bool addTestContactAddress(int contactId, const QString &remoteUid, const QString &localUid)
{
    QContact existingAggregate = manager()->contact(apiContactId(contactId, manager()->managerUri()));
    if (internalContactId(existingAggregate.id()) != (unsigned)contactId) {
        qWarning() << "Could not retrieve contact:" << contactId;
        return false;
    }

    QContact existing = manager()->contact(localContactForAggregate(existingAggregate.id()));

    if (!localUidComparesPhoneNumbers(localUid)) {
        QContactOriginMetadata metadata = existing.detail<QContactOriginMetadata>();
        if (metadata.groupId().isEmpty()) {
            // Create a metadata detail to link the contact with the account
            metadata.setGroupId(localUid);
            metadata.setId(remoteUid);
            metadata.setEnabled(true);
            if (!existing.saveDetail(&metadata)) {
                qWarning() << "Unable to add metadata to contact:" << contactId;
                return false;
            }
        }

        QContactOnlineAccount qcoa;
        qcoa.setValue(QContactOnlineAccount__FieldAccountPath, localUid);
        qcoa.setAccountUri(remoteUid);
        if (!existing.saveDetail(&qcoa)) {
            qWarning() << "Unable to add online account to contact:" << contactId;
            return false;
        }
    } else {
        QContactPhoneNumber phoneNumberDetail;
        phoneNumberDetail.setNumber(remoteUid);
        if (!existing.saveDetail(&phoneNumberDetail)) {
            qWarning() << "Unable to add phone number to contact:" << contactId;
            return false;
        }
    }

    if (!manager()->saveContact(&existing)) {
        qWarning() << "Unable to store contact:" << contactId;
        return false;
    }

    return true;
}

void modifyTestContact(int id, const QString &name, bool favorite)
{
    qDebug() << Q_FUNC_INFO << id << name;

    QContact existingAggregate = manager()->contact(apiContactId(id, manager()->managerUri()));
    if (internalContactId(existingAggregate.id()) != (unsigned)id) {
        qWarning() << "Could not retrieve contact:" << id;
        return;
    }

    QContact contact = manager()->contact(localContactForAggregate(existingAggregate.id()));

    QContactName nameDetail = contact.detail<QContactName>();
    if (name != nameDetail.lastName()) {
        nameDetail.setLastName(name);
        if (!contact.saveDetail(&nameDetail)) {
            qWarning() << "Unable to add name to contact:" << id;
            return;
        }
    }

    QContactFavorite favoriteDetail = contact.detail<QContactFavorite>();
    if (favorite != favoriteDetail.isFavorite()) {
        favoriteDetail.setFavorite(favorite);
        if (!contact.saveDetail(&favoriteDetail)) {
            qWarning() << "Unable to add favorite status to contact:" << id;
            return;
        }
    }

    if (!manager()->saveContact(&contact)) {
        qWarning() << "Unable to store contact:" << id;
        return;
    }
}

void deleteTestContact(int id)
{
    const QContactId contactId = apiContactId(id, manager()->managerUri());
    const QContact contact = manager()->contact(contactId);

    const QContactId localId = localContactForAggregate(contactId);
    QContact localContact = manager()->contact(localId);

    if (!SeasideCache::removeContact(localContact)) {
        qWarning() << "error deleting contact:" << contactId.localId();
    }
    addedContactIds.remove(contactId);
}

void cleanUpTestContacts()
{
    if (!addedContactIds.isEmpty()) {
        QString aggregatesType = QContactRelationship::Aggregates();

        const QList<QContactId> contactIdsToRemove = addedContactIds.toList();
        QList<QContact> contactsToRemove;
        for (const QContactId &contactId : addedContactIds) {
            const QContactId localId = localContactForAggregate(contactId);
            QContact localContact = manager()->contact(localId);
            if (!localContact.isEmpty()) {
                contactsToRemove.append(localContact);
            }
        }

        if (!SeasideCache::removeContacts(contactsToRemove)) {
            qWarning() << "Unable to remove test contacts:" << addedContactIds;
        }

        addedContactIds.clear();
    }
}

void cleanupTestGroups()
{
    GroupModel groupModel;
    groupModel.setResolveContacts(GroupManager::DoNotResolve);
    groupModel.setQueryMode(EventModel::SyncQuery);
    if (!groupModel.getGroups()) {
        qCritical() << Q_FUNC_INFO << "groupModel::getGroups failed";
        return;
    }

    if (!groupModel.deleteAll())
        qCritical() << Q_FUNC_INFO << "groupModel::deleteAll failed";
}

void cleanupTestEvents()
{
    SingleEventModel model;

    QSet<int>::const_iterator it = addedEventIds.constBegin(), end = addedEventIds.constEnd();
    for ( ; it != end; ++it) {
        model.deleteEvent(*it);
    }

    addedEventIds.clear();
}

bool compareEvents(Event &e1, Event &e2)
{
    if (e1.type() != e2.type()) {
        qWarning() << Q_FUNC_INFO << "type:" << e1.type() << e2.type();
        return false;
    }
    if (e1.direction() != e2.direction()) {
        qWarning() << Q_FUNC_INFO << "direction:" << e1.direction() << e2.direction();
        return false;
    }
    if (e1.startTime().toTime_t() != e2.startTime().toTime_t()) {
        qWarning() << Q_FUNC_INFO << "startTime:" << e1.startTime().toString() << e2.startTime().toString();
        return false;
    }
    if (e1.endTime().toTime_t() != e2.endTime().toTime_t()) {
        qWarning() << Q_FUNC_INFO << "endTime:" << e1.endTime().toString() << e2.endTime().toString();
        return false;
    }
    if (e1.isDraft() != e2.isDraft()) {
        qWarning() << Q_FUNC_INFO << "isDraft:" << e1.isDraft() << e2.isDraft();
        return false;
    }
    if (e1.isRead() != e2.isRead()) {
        qWarning() << Q_FUNC_INFO << "isRead:" << e1.isRead() << e2.isRead();
        return false;
    }
    if (e1.type() == Event::CallEvent && e1.isMissedCall() != e2.isMissedCall()) {
        qWarning() << Q_FUNC_INFO << "isMissedCall:" << e1.isMissedCall() << e2.isMissedCall();
        return false;
    }
    if (e1.type() == Event::CallEvent && e1.isEmergencyCall() != e2.isEmergencyCall()) {
        qWarning() << Q_FUNC_INFO << "isEmergencyCall:" << e1.isEmergencyCall() << e2.isEmergencyCall();
        return false;
    }
    if (e1.type() == Event::CallEvent && e1.isVideoCall() != e2.isVideoCall()) {
        qWarning() << Q_FUNC_INFO << "isVideoCall:" << e1.isVideoCall() << e2.isVideoCall();
        return false;
    }
//    QCOMPARE(e1.bytesSent(), e2.bytesSent());
//    QCOMPARE(e1.bytesReceived(), e2.bytesReceived());
    if (e1.localUid() != e2.localUid()) {
        qWarning() << Q_FUNC_INFO << "localUid:" << e1.localUid() << e2.localUid();
        return false;
    }
    if (e1.recipients() != e2.recipients()) {
        qWarning() << Q_FUNC_INFO << "recipients:" << e1.recipients() << e2.recipients();
        return false;
    }
    if (e1.type() != Event::CallEvent && e1.freeText() != e2.freeText()) {
        qWarning() << Q_FUNC_INFO << "freeText:" << e1.freeText() << e2.freeText();
        return false;
    }
    if (e1.groupId() != e2.groupId()) {
        qWarning() << Q_FUNC_INFO << "groupId:" << e1.groupId() << e2.groupId();
        return false;
    }
    if (e1.type() != Event::CallEvent && e1.fromVCardFileName() != e2.fromVCardFileName()) {
        qWarning() << Q_FUNC_INFO << "vcardFileName:" << e1.fromVCardFileName() << e2.fromVCardFileName();
        return false;
    }
    if (e1.type() != Event::CallEvent && e1.fromVCardLabel() != e2.fromVCardLabel()) {
        qWarning() << Q_FUNC_INFO << "vcardLabel:" << e1.fromVCardLabel() << e2.fromVCardLabel();
        return false;
    }
    if (e1.type() != Event::CallEvent && e1.status() != e2.status()) {
        qWarning() << Q_FUNC_INFO << "status:" << e1.status() << e2.status();
        return false;
    }
    if (e1.headers() != e2.headers()) {
        qWarning() << Q_FUNC_INFO << "headers:" << e1.headers() << e2.headers();
        return false;
    }
    if (e1.toList() != e2.toList()) {
        qWarning() << Q_FUNC_INFO << "toList:" << e1.toList() << e2.toList();
        return false;
    }

//    QCOMPARE(e1.messageToken(), e2.messageToken());
    return true;
}

void deleteAll()
{
    qDebug() << Q_FUNC_INFO << "- Deleting all";

    cleanUpTestContacts();
    cleanupTestGroups();
    cleanupTestEvents();
}

QString randomMessage(int words)
{
    QString msg;
    QTextStream msgStream(&msg, QIODevice::WriteOnly);
    for(int j = 0; j < words; j++) {
        msgStream << msgWords[qrand() % numWords] << " ";
    }
    return msg;
}

/*
 * Returns the average system load since last time this function was called (or
 * since boot if this first time this function is called). The scale is [1, 0],
 * where 1 is busy and 0 is idle system. In case of errors, -1 is returned.
 */
double getSystemLoad()
{
    // Parsing assumes that first line of /proc/stat looks something like this:
    // cpu  110807 325 23491 425840 37435 1367 32 0 0

    QFile file("/proc/stat");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << Q_FUNC_INFO << "Failed to open /proc/stat";
        return -1;
    }

    QTextStream in(&file);
    QString line = in.readLine();
    file.close();

    QStringList parts = line.split(" ", QString::SkipEmptyParts);
    if (parts.size() < 10) {
        qWarning() << Q_FUNC_INFO << "Invalid input from /proc/stat:" << line;
        return -1;
    }
    if (parts.at(0) != QStringLiteral("cpu")) {
        qWarning() << Q_FUNC_INFO << "Invalid input from /proc/stat:" << line;
        return -1;
    }

    int newIdleTicks = parts.at(4).toInt();
    int newAllTicks = 0;
    for (int i = 1; i < parts.count(); i++) {
        newAllTicks += parts.at(i).toInt();
    }

    quint64 idleTickDelta = newIdleTicks - idleTicks;
    quint64 allTickDelta = newAllTicks - allTicks;
    idleTicks = newIdleTicks;
    allTicks = newAllTicks;

    return 1.0 - ((double)idleTickDelta / allTickDelta);
}

/*
 * Wait in semi-busy loop until system load drops below IDLE_TRESHOLD
 */
void waitForIdle(int pollInterval) {
    double load = 1.0;
    QDateTime startTime = QDateTime::currentDateTime();
    getSystemLoad();
    while (load > IDLE_TRESHOLD) {
        qDebug() << Q_FUNC_INFO << "Waiting system to calm down. Wait time:"
            << startTime.secsTo(QDateTime::currentDateTime()) << "seconds. Load:"
            << load * 100 << "\%";
        QTest::qWait(pollInterval);
        load = getSystemLoad();
    }
    qDebug() << Q_FUNC_INFO << "Done. Wait time:"
        << startTime.secsTo(QDateTime::currentDateTime()) << "seconds. Load:"
        << load * 100 << "\%";
}

bool waitSignal(QSignalSpy &spy, int msec)
{
    QTime timer;
    timer.start();
    while (timer.elapsed() < msec && spy.isEmpty()) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::sendPostedEvents(0, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }

    return !spy.isEmpty();
}

void waitWithDeletes(int msec)
{
    QTime timer;
    timer.start();
    while (timer.elapsed() < msec) {
        QCoreApplication::sendPostedEvents();
        QCoreApplication::sendPostedEvents(0, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
}

void summarizeResults(const QString &className, QList<int> &times, QFile *logFile, int testSecs)
{
    int sum = 0;
    QStringList timeList;
    foreach (int time, times) {
        sum += time;
        timeList.append(QString::number(time));
    }

    qSort(times);
    float median = 0.0;
    const int iterations(times.count());
    if (iterations % 2 > 0) {
        median = times[(int)(iterations / 2)];
    } else {
        median = (times[iterations / 2] + times[iterations / 2 - 1]) / 2.0f;
    }

    const float mean = sum / (float)iterations;
    qDebug("##### Mean: %.1f; Median: %.1f; Min: %d; Max: %d; Test time: %dsec", mean, median, times[0], times[iterations-1], testSecs);

    if (logFile) {
        QString descriptor(className + "::" + QTest::currentTestFunction());
        if (QTest::currentDataTag())
            descriptor.append(":").append(QTest::currentDataTag());

        QTextStream out(logFile);
        out << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") << ": "
            << descriptor << " (" << iterations << " iterations)"
            << "\n"
            << timeList.join(" ")
            << "\n"
            << "Median average: " << (int)median << " ms. Min: " << times[0] << "ms. Max: " << times[iterations-1] << " ms. Test time: ";
        if (testSecs > 3600)
            out << (testSecs / 3600) << "h ";
        if (testSecs > 60)
            out << ((testSecs % 3600) / 60) << "m ";
        out << ((testSecs % 3600) % 60) << "s"
            << "\n";
    }
}

