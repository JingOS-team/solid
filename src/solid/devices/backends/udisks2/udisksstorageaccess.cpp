/*
    SPDX-FileCopyrightText: 2009 Pino Toscano <pino@kde.org>
    SPDX-FileCopyrightText: 2009-2012 Lukáš Tinkl <ltinkl@redhat.com>
    SPDX-FileCopyrightText: 2021 Yan Zhao <zhaoyan@jingos.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#include "udisksstorageaccess.h"
#include "udisks_debug.h"
#include "udisks2.h"

#include <QDomDocument>
#include <QDBusConnection>
#include <QDir>
#include <QDBusInterface>
#include <QGuiApplication>
#include <QWindow>

using namespace Solid::Backends::UDisks2;

StorageAccess::StorageAccess(Device *device)
    : DeviceInterface(device), m_setupInProgress(false), m_teardownInProgress(false), m_passphraseRequested(false)
{
    connect(device, SIGNAL(changed()), this, SLOT(checkAccessibility()));
    updateCache();

    // Delay connecting to DBus signals to avoid the related time penalty
    // in hot paths such as predicate matching
    QTimer::singleShot(0, this, SLOT(connectDBusSignals()));
}

StorageAccess::~StorageAccess()
{
}

void StorageAccess::connectDBusSignals()
{
    m_device->registerAction("setup", this,
                             SLOT(slotSetupRequested()),
                             SLOT(slotSetupDone(int,QString)));

    m_device->registerAction("teardown", this,
                             SLOT(slotTeardownRequested()),
                             SLOT(slotTeardownDone(int,QString)));
}

bool StorageAccess::isLuksDevice() const
{
    return m_device->isEncryptedContainer(); // encrypted device
}

bool StorageAccess::isAccessible() const
{
    if (isLuksDevice()) { // check if the cleartext slave is mounted
        const QString path = clearTextPath();
        //qDebug() << Q_FUNC_INFO << "CLEARTEXT device path: " << path;
        if (path.isEmpty() || path == "/") {
            return false;
        }
        Device holderDevice(path);
        return holderDevice.isMounted();
    }

    return m_device->isMounted();
}

static inline QString get_shortest(QByteArrayList& mntPoints)
{
    // We return the shortest filePath to avoid errors like:
    // https://bugs.debian.org/762950

    if (mntPoints.isEmpty()) {
        return QString();
    }
    QString shortest = QFile::decodeName(mntPoints.first());
    for (int i = 1; i < mntPoints.count(); i++) {
        QString current = QFile::decodeName(mntPoints.at(i));
        if (shortest.length() > current.length()) {
            shortest = current;
        }
    }
    return shortest;
}

QString StorageAccess::filePath() const
{
    QByteArrayList mntPoints;

    if (isLuksDevice()) {  // encrypted (and unlocked) device
        const QString path = clearTextPath();
        if (path.isEmpty() || path == "/") {
            return QString();
        }
        Device holderDevice(path);
        mntPoints = qdbus_cast<QByteArrayList>(holderDevice.prop("MountPoints"));
        // FIXME Solid doesn't support multiple mount points
        return get_shortest(mntPoints);
    }

    mntPoints = qdbus_cast<QByteArrayList>(m_device->prop("MountPoints"));
    // FIXME Solid doesn't support multiple mount points
    return get_shortest(mntPoints);
}

bool StorageAccess::isIgnored() const
{
    if (m_device->prop("HintIgnore").toBool()) {
        return true;
    }

    const QString path = filePath();

    bool inUserPath = path.startsWith(QLatin1String("/media/")) ||
                      path.startsWith(QLatin1String("/run/media/")) ||
                      path.startsWith(QDir::homePath());
    return !inUserPath;
}

bool StorageAccess::setup()
{
    if (m_teardownInProgress || m_setupInProgress) {
        return false;
    }
    m_setupInProgress = true;
    m_device->broadcastActionRequested("setup");

    if (m_device->isEncryptedContainer() && clearTextPath().isEmpty()) {
        return requestPassphrase();
    } else {
        return mount();
    }
}

bool StorageAccess::teardown()
{
    if (m_teardownInProgress || m_setupInProgress) {
        return false;
    }
    m_teardownInProgress = true;
    m_device->broadcastActionRequested("teardown");

    return unmount();
}

void StorageAccess::updateCache()
{
    m_isAccessible = isAccessible();
}

void StorageAccess::checkAccessibility()
{
    const bool old_isAccessible = m_isAccessible;
    updateCache();

    if (old_isAccessible != m_isAccessible) {
        Q_EMIT accessibilityChanged(m_isAccessible, m_device->udi());
    }
}

void StorageAccess::slotDBusReply(const QDBusMessage & /*reply*/)
{
    if (m_setupInProgress) {
        if (isLuksDevice() && !isAccessible()) { // unlocked device, now mount it
            mount();
        } else { // Don't broadcast setupDone unless the setup is really done. (Fix kde#271156)
            m_setupInProgress = false;
            m_device->invalidateCache();
            m_device->broadcastActionDone("setup");

            checkAccessibility();
        }
    } else if (m_teardownInProgress) { // FIXME
        const QString ctPath = clearTextPath();
        qCDebug(UDISKS2) << "Successfully unmounted " << m_device->udi();
        if (isLuksDevice() && !ctPath.isEmpty() && ctPath != "/") { // unlocked device, lock it
            callCryptoTeardown();
        } else if (!ctPath.isEmpty() && ctPath != "/") {
            callCryptoTeardown(true); // Lock crypted parent
        } else {
            // try to "eject" (aka safely remove) from the (parent) drive, e.g. SD card from a reader
            QString drivePath = m_device->drivePath();
            if (!drivePath.isEmpty() || drivePath != "/") {
                Device drive(drivePath);
                QDBusConnection c = QDBusConnection::systemBus();

                if (drive.prop("MediaRemovable").toBool() &&
                        drive.prop("MediaAvailable").toBool() &&
                        !m_device->isOpticalDisc()) { // optical drives have their Eject method
                    QDBusMessage msg = QDBusMessage::createMethodCall(UD2_DBUS_SERVICE, drivePath, UD2_DBUS_INTERFACE_DRIVE, "Eject");
                    msg << QVariantMap();   // options, unused now
                    c.call(msg, QDBus::NoBlock);
                } else if (drive.prop("CanPowerOff").toBool() &&
                        !m_device->isOpticalDisc()) { // avoid disconnecting optical drives from the bus
                    qCDebug(UDISKS2) << "Drive can power off:" << drivePath;
                    QDBusMessage msg = QDBusMessage::createMethodCall(UD2_DBUS_SERVICE, drivePath, UD2_DBUS_INTERFACE_DRIVE, "PowerOff");
                    msg << QVariantMap();   // options, unused now
                    c.call(msg, QDBus::NoBlock);
                }
            }

            m_teardownInProgress = false;
            m_device->invalidateCache();
            m_device->broadcastActionDone("teardown");

            checkAccessibility();
        }
    }
}

void StorageAccess::slotDBusError(const QDBusError &error)
{
    //qDebug() << Q_FUNC_INFO << "DBUS ERROR:" << error.name() << error.message();

    if (m_setupInProgress) {
        m_setupInProgress = false;
        m_device->broadcastActionDone("setup", m_device->errorToSolidError(error.name()),
                                      m_device->errorToString(error.name()) + ": " + error.message());

        checkAccessibility();
    } else if (m_teardownInProgress) {
        m_teardownInProgress = false;
        m_device->broadcastActionDone("teardown", m_device->errorToSolidError(error.name()),
                                      m_device->errorToString(error.name()) + ": " + error.message());
        checkAccessibility();
    }
}

void StorageAccess::slotSetupRequested()
{
    m_setupInProgress = true;
    //qDebug() << "SETUP REQUESTED:" << m_device->udi();
    Q_EMIT setupRequested(m_device->udi());
}

void StorageAccess::slotSetupDone(int error, const QString &errorString)
{
    m_setupInProgress = false;
    //qDebug() << "SETUP DONE:" << m_device->udi();
    checkAccessibility();
    Q_EMIT setupDone(static_cast<Solid::ErrorType>(error), errorString, m_device->udi());
}

void StorageAccess::slotTeardownRequested()
{
    m_teardownInProgress = true;
    Q_EMIT teardownRequested(m_device->udi());
}

void StorageAccess::slotTeardownDone(int error, const QString &errorString)
{
    m_teardownInProgress = false;
    checkAccessibility();
    Q_EMIT teardownDone(static_cast<Solid::ErrorType>(error), errorString, m_device->udi());
}

bool StorageAccess::mount()
{
    QString path = m_device->udi();

    if (isLuksDevice()) { // mount options for the cleartext volume
        const QString ctPath = clearTextPath();
        if (!ctPath.isEmpty()) {
            path = ctPath;
        }
    }

    QDBusConnection c = QDBusConnection::systemBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(UD2_DBUS_SERVICE, path, UD2_DBUS_INTERFACE_FILESYSTEM, "Mount");
    QVariantMap options;

    if (m_device->prop("IdType").toString() == "vfat") {
        options.insert("options", "flush");
    }

    msg << options;

    return c.callWithCallback(msg, this,
                              SLOT(slotDBusReply(QDBusMessage)),
                              SLOT(slotDBusError(QDBusError)));
}

bool StorageAccess::unmount()
{
    QString path = m_device->udi();

    if (isLuksDevice()) { // unmount options for the cleartext volume
        const QString ctPath = clearTextPath();
        if (!ctPath.isEmpty()) {
            path = ctPath;
        }
    }

    QDBusConnection c = QDBusConnection::systemBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(UD2_DBUS_SERVICE, path, UD2_DBUS_INTERFACE_FILESYSTEM, "Unmount");

    msg << QVariantMap();   // options, unused now

    qCDebug(UDISKS2) << "Initiating unmount of " << path;
    return c.callWithCallback(msg, this,
                              SLOT(slotDBusReply(QDBusMessage)),
                              SLOT(slotDBusError(QDBusError)),
                              s_unmountTimeout);
}

QString StorageAccess::generateReturnObjectPath()
{
    static QAtomicInt number = 1;

    return "/org/kde/solid/UDisks2StorageAccess_" + QString::number(number++);
}

QString StorageAccess::clearTextPath() const
{
    const QString prefix = "/org/freedesktop/UDisks2/block_devices";
    QDBusMessage call = QDBusMessage::createMethodCall(UD2_DBUS_SERVICE, prefix,
                        DBUS_INTERFACE_INTROSPECT, "Introspect");
    QDBusPendingReply<QString> reply = QDBusConnection::systemBus().asyncCall(call);
    reply.waitForFinished();

    if (reply.isValid()) {
        QDomDocument dom;
        dom.setContent(reply.value());
        QDomNodeList nodeList = dom.documentElement().elementsByTagName("node");
        for (int i = 0; i < nodeList.count(); i++) {
            QDomElement nodeElem = nodeList.item(i).toElement();
            if (!nodeElem.isNull() && nodeElem.hasAttribute("name")) {
                const QString udi = prefix + "/" + nodeElem.attribute("name");
                Device holderDevice(udi);

                if (m_device->udi() == holderDevice.prop("CryptoBackingDevice").value<QDBusObjectPath>().path()) {
                    //qDebug() << Q_FUNC_INFO << "CLEARTEXT device path: " << udi;
                    return udi;
                }
            }
        }
    }

    return QString();
}

bool StorageAccess::requestPassphrase()
{
    QString udi = m_device->udi();
    QString returnService = QDBusConnection::sessionBus().baseService();
    m_lastReturnObject = generateReturnObjectPath();

    QDBusConnection::sessionBus().registerObject(m_lastReturnObject, this, QDBusConnection::ExportScriptableSlots);

    // TODO: this only works on X11, Wayland doesn't have global window ids.
    // Passing ids to other processes doesn't make any sense
    auto activeWindow = QGuiApplication::focusWindow();
    uint wId = 0;
    if (activeWindow != nullptr) {
        wId = (uint)activeWindow->winId();
    }

    QString appId = QCoreApplication::applicationName();

    QDBusInterface soliduiserver("org.kde.kded5", "/modules/soliduiserver", "org.kde.SolidUiServer");
    QDBusReply<void> reply = soliduiserver.call("showPassphraseDialog", udi, returnService,
                             m_lastReturnObject, wId, appId);
    m_passphraseRequested = reply.isValid();
    if (!m_passphraseRequested) {
        qCWarning(UDISKS2) << "Failed to call the SolidUiServer, D-Bus said:" << reply.error();
    }

    return m_passphraseRequested;
}

void StorageAccess::passphraseReply(const QString &passphrase)
{
    if (m_passphraseRequested) {
        QDBusConnection::sessionBus().unregisterObject(m_lastReturnObject);
        m_passphraseRequested = false;
        if (!passphrase.isEmpty()) {
            callCryptoSetup(passphrase);
        } else {
            m_setupInProgress = false;
            m_device->broadcastActionDone("setup", Solid::UserCanceled);
        }
    }
}

void StorageAccess::callCryptoSetup(const QString &passphrase)
{
    QDBusConnection c = QDBusConnection::systemBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(UD2_DBUS_SERVICE, m_device->udi(), UD2_DBUS_INTERFACE_ENCRYPTED, "Unlock");

    msg << passphrase;
    msg << QVariantMap();   // options, unused now

    c.callWithCallback(msg, this,
                       SLOT(slotDBusReply(QDBusMessage)),
                       SLOT(slotDBusError(QDBusError)));
}

bool StorageAccess::callCryptoTeardown(bool actOnParent)
{
    QDBusConnection c = QDBusConnection::systemBus();
    QDBusMessage msg = QDBusMessage::createMethodCall(UD2_DBUS_SERVICE,
                       actOnParent ? (m_device->prop("CryptoBackingDevice").value<QDBusObjectPath>().path()) : m_device->udi(),
                       UD2_DBUS_INTERFACE_ENCRYPTED, "Lock");
    msg << QVariantMap();   // options, unused now

    return c.callWithCallback(msg, this,
                              SLOT(slotDBusReply(QDBusMessage)),
                              SLOT(slotDBusError(QDBusError)));
}
