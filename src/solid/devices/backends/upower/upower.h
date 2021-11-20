/*
    SPDX-FileCopyrightText: 2009 Pino Toscano <pino@kde.org>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#ifndef SOLID_BACKENDS_UPOWER_H
#define SOLID_BACKENDS_UPOWER_H

/* UPower */
#define UP_DBUS_SERVICE           "org.freedesktop.UPower"
#define UP_DBUS_PATH              "/org/freedesktop/UPower"
#define UP_DBUS_INTERFACE         "org.freedesktop.UPower"
#define UP_DBUS_INTERFACE_DEVICE  UP_DBUS_INTERFACE ".Device"
#define UP_UDI_PREFIX             "/org/freedesktop/UPower"

typedef enum {
	UP_DEVICE_KIND_UNKNOWN,
	UP_DEVICE_KIND_LINE_POWER,
	UP_DEVICE_KIND_BATTERY,
	UP_DEVICE_KIND_UPS,
	UP_DEVICE_KIND_MONITOR,
	UP_DEVICE_KIND_MOUSE,
	UP_DEVICE_KIND_KEYBOARD,
	UP_DEVICE_KIND_PDA,
	UP_DEVICE_KIND_PHONE,
	UP_DEVICE_KIND_MEDIA_PLAYER,
	UP_DEVICE_KIND_TABLET,
	UP_DEVICE_KIND_COMPUTER,
	UP_DEVICE_KIND_GAMING_INPUT,
	UP_DEVICE_KIND_LAST
} UpDeviceKind;

#endif // SOLID_BACKENDS_UPOWER_H
