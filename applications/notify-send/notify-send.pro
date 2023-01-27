QT -= gui
QT += dbus

CONFIG += c++11 console
CONFIG -= app_bundle

DEFINES += QT_DEPRECATED_WARNINGS
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
        main.cpp

INCLUDEPATH += ../../shared

DBUS_INTERFACES += ../../interfaces/dbusservice.xml
DBUS_INTERFACES += ../../interfaces/notificationapi.xml
DBUS_INTERFACES += ../../interfaces/notification.xml

# Default rules for deployment.
target.path = /opt/bin
!isEmpty(target.path): INSTALLS += target

contains(DEFINES, SENTRY){
    exists($$PWD/../../.build/sentry) {
        LIBS += -L$$PWD/../../.build/sentry/lib -lsentry -ldl -lcurl -lbreakpad_client
        INCLUDEPATH += $$PWD/../../.build/sentry/include
        DEPENDPATH += $$PWD/../../.build/sentry/lib

        library.files = ../../.build/sentry/libsentry.so
        library.path = /opt/lib
        INSTALLS += library
    }else{
        error(You need to build sentry first)
    }
}

LIBS += -L$$PWD/../../.build/liboxide -lliboxide
INCLUDEPATH += $$PWD/../../shared/liboxide
DEPENDPATH += $$PWD/../../shared/liboxide

QMAKE_RPATHDIR += /lib /usr/lib /opt/lib /opt/usr/lib

HEADERS +=

VERSION = 2.5
DEFINES += APP_VERSION=\\\"$$VERSION\\\"