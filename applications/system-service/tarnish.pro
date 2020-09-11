QT -= gui
QT += dbus

CONFIG += c++11 console
CONFIG -= app_bundle

SOURCES += main.cpp \
    buttonhandler.cpp \
    fb2png.cpp \
    sysobject.cpp

TARGET=tarnish

target.path = /opt/bin
INSTALLS += target

HEADERS += \
    buttonhandler.h \
    dbusservice.h \
    dbussettings.h \
    fb2png.h \
    inputmanager.h \
    mxcfb.h \
    powerapi.h \
    sysobject.h

linux-oe-g++ {
    LIBS += -lpng16
}

QMAKE_POST_LINK += sh $$_PRO_FILE_PWD_/generate_xml.sh

DISTFILES += \
    generate_xml.sh

RESOURCES += \
    qml.qrc