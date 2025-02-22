#ifndef SYSTEMAPI_H
#define SYSTEMAPI_H

#include <QObject>
#include <QMetaType>
#include <QMutableListIterator>
#include <QTimer>
#include <QMutex>
#include <liboxide.h>

#include "apibase.h"
#include "buttonhandler.h"
#include "application.h"
#include "screenapi.h"
#include "digitizerhandler.h"
#include "login1_interface.h"

#define systemAPI SystemAPI::singleton()

typedef org::freedesktop::login1::Manager Manager;

struct Inhibitor {
    int fd;
    QString who;
    QString what;
    QString why;
    bool block;
    Inhibitor(Manager* systemd, QString what, QString who, QString why, bool block = false)
     : who(who), what(what), why(why), block(block) {
        QDBusUnixFileDescriptor reply = systemd->Inhibit(what, who, why, block ? "block" : "delay");
        fd = reply.takeFileDescriptor();
    }
    void release(){
        if(released()){
            return;
        }
        close(fd);
        fd = -1;
    }
    bool released() { return fd == -1; }
};

#define NULL_TOUCH_COORD -1

struct Touch {
    int slot = 0;
    int id = -1;
    int x = NULL_TOUCH_COORD;
    int y = NULL_TOUCH_COORD;
    bool active = false;
    bool existing = false;
    bool modified = true;
    int pressure = 0;
    int major = 0;
    int minor = 0;
    int orientation = 0;
    string debugString() const{
        return "<Touch " + to_string(id) + " (" + to_string(x) + ", " + to_string(y) + ") " + (active ? "pressed" : "released") + ">";
    }
};
QDebug operator<<(QDebug debug, const Touch& touch);
QDebug operator<<(QDebug debug, Touch* touch);
Q_DECLARE_METATYPE(Touch)
Q_DECLARE_METATYPE(input_event)

class SystemAPI : public APIBase {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", OXIDE_SYSTEM_INTERFACE)
    Q_PROPERTY(int autoSleep READ autoSleep WRITE setAutoSleep NOTIFY autoSleepChanged)
    Q_PROPERTY(int autoLock READ autoLock WRITE setAutoLock NOTIFY autoLockChanged)
    Q_PROPERTY(bool lockOnSuspend READ lockOnSuspend WRITE setLockOnSuspend NOTIFY lockOnSuspendChanged)
    Q_PROPERTY(bool sleepInhibited READ sleepInhibited NOTIFY sleepInhibitedChanged)
    Q_PROPERTY(bool powerOffInhibited READ powerOffInhibited NOTIFY powerOffInhibitedChanged)

public:
    enum SwipeDirection { None, Right, Left, Up, Down };
    Q_ENUM(SwipeDirection)
    static SystemAPI* singleton(SystemAPI* self = nullptr){
        static SystemAPI* instance;
        if(self != nullptr){
            instance = self;
        }
        return instance;
    }
    SystemAPI(QObject* parent)
     : APIBase(parent),
       suspendTimer(this),
       lockTimer(this),
       settings(this),
       sleepInhibitors(),
       powerOffInhibitors(),
       mutex(),
       touches(),
       swipeStates(),
       swipeLengths() {
        Oxide::Sentry::sentry_transaction("system", "init", [this](Oxide::Sentry::Transaction* t){
            Oxide::Sentry::sentry_span(t, "settings", "Sync settings", [this](Oxide::Sentry::Span* s){
                Oxide::Sentry::sentry_span(s, "swipes", "Default swipe values", [this]{
                    for(short i = Right; i <= Down; i++){
                        swipeStates[(SwipeDirection)i] = true;
                    }
                });
                Oxide::Sentry::sentry_span(s, "sync", "Sync settings", [this]{
                    settings.sync();
                });
                Oxide::Sentry::sentry_span(s, "singleton", "Instantiate singleton", [this]{
                    singleton(this);
                });
                resumeApp = nullptr;
            });
            Oxide::Sentry::sentry_span(t, "systemd", "Connect to SystemD DBus", [this](Oxide::Sentry::Span* s){
                Oxide::Sentry::sentry_span(s, "manager", "Create manager object", [this]{
                    systemd = new Manager("org.freedesktop.login1", "/org/freedesktop/login1", QDBusConnection::systemBus(), this);
                });
                Oxide::Sentry::sentry_span(s, "connect", "Connect to signals", [this]{
                    // Handle Systemd signals
                    connect(systemd, &Manager::PrepareForSleep, this, &SystemAPI::PrepareForSleep);
                    connect(&suspendTimer, &QTimer::timeout, this, &SystemAPI::suspendTimeout);
                    connect(&lockTimer, &QTimer::timeout, this, &SystemAPI::lockTimeout);
                });
            });
            Oxide::Sentry::sentry_span(t, "autoSleep", "Setup automatic sleep", [this](Oxide::Sentry::Span* s){
                QSettings settings;
                if(QFile::exists(settings.fileName())){
                    qDebug() << "Importing old settings";
                    settings.sync();
                    if(settings.contains("autoSleep")){
                        qDebug() << "Importing old autoSleep";
                        sharedSettings.set_autoSleep(settings.value("autoSleep").toInt());
                    }
                    int size = settings.beginReadArray("swipes");
                    if(size){
                        sharedSettings.beginWriteArray("swipes");
                        for(short i = Right; i <= Down && i < size; i++){
                            settings.setArrayIndex(i);
                            sharedSettings.setArrayIndex(i);
                            qDebug() << QString("Importing old swipe[%1]").arg(i);
                            sharedSettings.setValue("enabled", settings.value("enabled", true));
                            sharedSettings.setValue("length", settings.value("length", 30));
                        }
                        sharedSettings.endArray();
                    }
                    settings.endArray();
                    settings.remove("swipes");
                    settings.sync();
                    sharedSettings.sync();
                }
                if(autoSleep() < 0) {
                    sharedSettings.set_autoSleep(0);
                }else if(autoSleep() > 10){
                    sharedSettings.set_autoSleep(10);
                }
                qDebug() << "Auto Sleep" << autoSleep();
                Oxide::Sentry::sentry_span(s, "timer", "Setup timers", [this]{
                    if(autoSleep()){
                        suspendTimer.start(autoSleep() * 60 * 1000);
                    }else{
                        suspendTimer.stop();
                    }
                    if(autoLock()){
                        lockTimer.start(autoLock() * 60 * 1000);
                    }else{
                        lockTimer.stop();
                    }
                });
                connect(&sharedSettings, &Oxide::SharedSettings::autoSleepChanged, [=](int _autoSleep){
                    emit autoSleepChanged(_autoSleep);
                });
                connect(&sharedSettings, &Oxide::SharedSettings::changed, [=](){
                    sharedSettings.beginReadArray("swipes");
                    for(short i = Right; i <= Down; i++){
                        sharedSettings.setArrayIndex(i);
                        swipeStates[(SwipeDirection)i] = sharedSettings.value("enabled", true).toBool();
                        int length = sharedSettings.value("length", 30).toInt();
                        swipeLengths[(SwipeDirection)i] = length;
                        emit swipeLengthChanged(i, length);
                    }
                    sharedSettings.endArray();
                });
            });
            Oxide::Sentry::sentry_span(t, "swipes", "Load swipe settings", [=](){
                sharedSettings.beginReadArray("swipes");
                for(short i = Right; i <= Down; i++){
                    sharedSettings.setArrayIndex(i);
                    swipeStates[(SwipeDirection)i] = sharedSettings.value("enabled", true).toBool();
                    swipeLengths[(SwipeDirection)i] = sharedSettings.value("length", 30).toInt();
                }
                sharedSettings.endArray();
            });
            // Ask Systemd to tell us nicely when we are about to suspend or resume
            Oxide::Sentry::sentry_span(t, "inhibit", "Inhibit sleep and power off", [this](Oxide::Sentry::Span* s){
                Oxide::Sentry::sentry_span(s, "inhibitSleep", "Inhibit sleep", [this]{
                    inhibitSleep();
                });
                Oxide::Sentry::sentry_span(s, "inhibitPowerOff", "Inhibit power off", [this]{
                    inhibitPowerOff();
                });
            });
            qRegisterMetaType<input_event>();
            Oxide::Sentry::sentry_span(t, "input", "Connect input events", [this]{
                connect(touchHandler, &DigitizerHandler::activity, this, &SystemAPI::activity);
                connect(touchHandler, &DigitizerHandler::inputEvent, this, &SystemAPI::touchEvent);
                connect(wacomHandler, &DigitizerHandler::activity, this, &SystemAPI::activity);
                connect(wacomHandler, &DigitizerHandler::inputEvent, this, &SystemAPI::penEvent);
            });
            qDebug() << "System API ready to use";
        });
    }
    ~SystemAPI(){
        qDebug() << "Removing all inhibitors";
        rguard(false);
        QMutableListIterator<Inhibitor> i(inhibitors);
        while(i.hasNext()){
            auto inhibitor = i.next();
            inhibitor.release();
            i.remove();
        }
        delete systemd;
    }
    void setEnabled(bool enabled){
        qDebug() << "System API" << enabled;
    }
    int autoSleep(){return sharedSettings.autoSleep(); }
    void setAutoSleep(int _autoSleep);
    int autoLock(){return sharedSettings.autoLock(); }
    void setAutoLock(int _autoLock);
    bool lockOnSuspend(){return sharedSettings.lockOnSuspend(); }
    void setLockOnSuspend(bool _lockOnSuspend);
    bool sleepInhibited(){ return sleepInhibitors.length(); }
    bool powerOffInhibited(){ return powerOffInhibitors.length(); }
    void uninhibitAll(QString name);
    void stopSuspendTimer(){
        qDebug() << "Suspend timer disabled";
        suspendTimer.stop();
    }
    void stopLockTimer(){
        qDebug() << "Lock timer disabled";
        lockTimer.stop();
    }
    void startSuspendTimer();
    void startLockTimer();
    void lock(){ mutex.lock(); }
    void unlock() { mutex.unlock(); }
    Q_INVOKABLE void setSwipeEnabled(int direction, bool enabled){
        if(!hasPermission("system")){
            return;
        }
        if(direction <= SwipeDirection::None || direction > SwipeDirection::Down){
            qDebug() << "Invalid swipe direction: " << direction;
            return;
        }
        setSwipeEnabled((SwipeDirection)direction, enabled);
    }
    void setSwipeEnabled(SwipeDirection direction, bool enabled){
        if(direction == None){
            return;
        }
        switch(direction){
            case Left:
                qDebug() << "Swipe Left: " << enabled;
                break;
            case Right:
                qDebug() << "Swipe Right: " << enabled;
                break;
            case Up:
                qDebug() << "Swipe Up: " << enabled;
                break;
            case Down:
                qDebug() << "Swipe Down: " << enabled;
                break;
            default:
                return;
        }
        swipeStates[direction] = enabled;
        sharedSettings.beginWriteArray("swipes");
        for(short i = Right; i <= Down; i++){
            sharedSettings.setArrayIndex(i);
            sharedSettings.setValue("enabled", swipeStates[(SwipeDirection)i]);
            sharedSettings.setValue("length", swipeLengths[(SwipeDirection)i]);
        }
        sharedSettings.endArray();
        sharedSettings.sync();
    }
    Q_INVOKABLE bool getSwipeEnabled(int direction){
        if(!hasPermission("system")){
            return false;
        }
        if(direction <= SwipeDirection::None || direction > SwipeDirection::Down){
            qDebug() << "Invalid swipe direction: " << direction;
            return false;
        }
        return getSwipeEnabled(direction);
    }
    bool getSwipeEnabled(SwipeDirection direction){ return swipeStates[direction]; }
    Q_INVOKABLE void toggleSwipeEnabled(int direction){
        if(!hasPermission("system")){
            return;
        }
        if(direction <= SwipeDirection::None || direction > SwipeDirection::Down){
            qDebug() << "Invalid swipe direction: " << direction;
            return;
        }
        toggleSwipeEnabled((SwipeDirection)direction);
    }
    void toggleSwipeEnabled(SwipeDirection direction){ setSwipeEnabled(direction, !getSwipeEnabled(direction)); }
    Q_INVOKABLE void setSwipeLength(int direction, int length){
        int maxLength;
        if(!hasPermission("system")){
            return;
        }
        if(direction <= SwipeDirection::None || direction > SwipeDirection::Down){
            qDebug() << "Invalid swipe direction: " << direction;
            return;
        }
        if(direction == SwipeDirection::Up || direction == SwipeDirection::Down){
            maxLength = deviceSettings.getTouchHeight();
        }else{
            maxLength = deviceSettings.getTouchWidth();
        }
        if(length < 0 || length > maxLength){
            qDebug() << "Invalid swipe length: " << direction;
            return;
        }
        setSwipeLength((SwipeDirection)direction, length);
    }
    void setSwipeLength(SwipeDirection direction, int length){
        if(direction == None){
            return;
        }
        switch(direction){
            case Left:
                qDebug() << "Swipe Left Length: " << length;
                break;
            case Right:
                qDebug() << "Swipe Right Length: " << length;
                break;
            case Up:
                qDebug() << "Swipe Up Length: " << length;
                break;
            case Down:
                qDebug() << "Swipe Down Length: " << length;
                break;
            default:
                return;
        }
        swipeLengths[direction] = length;
        sharedSettings.beginWriteArray("swipes");
        for(short i = Right; i <= Down; i++){
            sharedSettings.setArrayIndex(i);
            sharedSettings.setValue("enabled", swipeStates[(SwipeDirection)i]);
            sharedSettings.setValue("length", swipeLengths[(SwipeDirection)i]);
        }
        sharedSettings.endArray();
        sharedSettings.sync();
        emit swipeLengthChanged(direction, length);
    }
    Q_INVOKABLE int getSwipeLength(int direction){
        if(!hasPermission("system")){
            return -1;
        }
        if(direction <= SwipeDirection::None || direction > SwipeDirection::Down){
            qDebug() << "Invalid swipe direction: " << direction;
            return -1;
        }
        return getSwipeLength((SwipeDirection)direction);
    }
    int getSwipeLength(SwipeDirection direction){ return swipeLengths[direction]; }
public slots:
    void suspend(){
        if(sleepInhibited()){
            qDebug() << "Unable to suspend. Action is currently inhibited.";
            return;
        }
        qDebug() << "Requesting Suspend...";
        systemd->Suspend(false).waitForFinished();
        qDebug() << "Suspend requested.";
    }
    void powerOff() {
        if(powerOffInhibited()){
            qDebug() << "Unable to power off. Action is currently inhibited.";
            return;
        }
        qDebug() << "Requesting Power off...";
        releasePowerOffInhibitors(true);
        rguard(false);
        systemd->PowerOff(false).waitForFinished();
        qDebug() << "Power off requested";
    }
    void reboot() {
        if(powerOffInhibited()){
            qDebug() << "Unable to reboot. Action is currently inhibited.";
            return;
        }
        qDebug() << "Requesting Reboot...";
        releasePowerOffInhibitors(true);
        rguard(false);
        systemd->Reboot(false).waitForFinished();
        qDebug() << "Reboot requested";
    }
    void activity();
    void inhibitSleep(QDBusMessage message){
        if(!sleepInhibited()){
            emit sleepInhibitedChanged(true);
        }
        suspendTimer.stop();
        sleepInhibitors.append(message.service());
        inhibitors.append(Inhibitor(systemd, "sleep:handle-suspend-key:idle", message.service(), "Application requested block", true));
    }
    void uninhibitSleep(QDBusMessage message);
    void inhibitPowerOff(QDBusMessage message){
        if(!powerOffInhibited()){
            emit powerOffInhibitedChanged(true);
        }
        powerOffInhibitors.append(message.service());
        inhibitors.append(Inhibitor(systemd, "shutdown:handle-power-key", message.service(), "Application requested block", true));
    }
    void uninhibitPowerOff(QDBusMessage message){
        if(!powerOffInhibited()){
            return;
        }
        powerOffInhibitors.removeAll(message.service());
        if(!powerOffInhibited()){
            emit powerOffInhibitedChanged(false);
        }
    }
    void toggleSwipes();
signals:
    void leftAction();
    void homeAction();
    void rightAction();
    void powerAction();
    void bottomAction();
    void topAction();
    void sleepInhibitedChanged(bool);
    void powerOffInhibitedChanged(bool);
    void autoSleepChanged(int);
    void autoLockChanged(int);
    void lockOnSuspendChanged(bool);
    void swipeLengthChanged(int, int);
    void deviceSuspending();
    void deviceResuming();

private slots:
    void PrepareForSleep(bool suspending);
    void suspendTimeout();
    void lockTimeout();
    void touchEvent(const input_event& event){
        switch(event.type){
            case EV_SYN:
                switch(event.code){
                    case SYN_REPORT:
                        // Always mark the current slot as modified
                        auto touch = getEvent(currentSlot);
                        touch->modified = true;
                        QList<Touch*> released;
                        QList<Touch*> pressed;
                        QList<Touch*> moved;
                        for(auto touch : touches.values()){
                            if(touch->id == -1){
                                touch->active = false;
                                released.append(touch);
                            }else if(!touch->active){
                                released.append(touch);
                            }else if(!touch->existing){
                                pressed.append(touch);
                            }else if(touch->modified){
                                moved.append(touch);
                            }
                        }
                        if(!penActive){
                            if(pressed.length()){
                                touchDown(pressed);
                            }
                            if(moved.length()){
                                touchMove(moved);
                            }
                            if(released.length()){
                                touchUp(released);
                            }
                        }else if(swipeDirection != None){
                            if(Oxide::debugEnabled()){
                                qDebug() << "Swiping cancelled due to pen activity";
                            }
                            swipeDirection = None;
                        }
                        // Cleanup released touches
                        for(auto touch : released){
                            if(!touch->active){
                                touches.remove(touch->slot);
                                delete touch;
                            }
                        }
                        // Setup touches for next event set
                        for(auto touch : touches.values()){
                            touch->modified = false;
                            touch->existing = touch->existing || (touch->x != NULL_TOUCH_COORD && touch->y != NULL_TOUCH_COORD);
                        }
                    break;
                }
            break;
            case EV_ABS:
                if(currentSlot == -1 && event.code != ABS_MT_SLOT){
                    return;
                }
                switch(event.code){
                    case ABS_MT_SLOT:{
                        currentSlot = event.value;
                        auto touch = getEvent(currentSlot);
                        touch->modified = true;
                    }break;
                    case ABS_MT_TRACKING_ID:{
                        auto touch = getEvent(currentSlot);
                        touch->active = event.value != -1;
                        if(touch->active){
                            touch->id = event.value;
                        }
                    }break;
                    case ABS_MT_POSITION_X:{
                        auto touch = getEvent(currentSlot);
                        touch->x = event.value;
                    }break;
                    case ABS_MT_POSITION_Y:{
                        auto touch = getEvent(currentSlot);
                        touch->y = event.value;
                    }break;
                    case ABS_MT_PRESSURE:{
                        auto touch = getEvent(currentSlot);
                        touch->pressure = event.value;
                    }break;
                    case ABS_MT_TOUCH_MAJOR:{
                        auto touch = getEvent(currentSlot);
                        touch->major = event.value;
                    }break;
                    case ABS_MT_TOUCH_MINOR:{
                        auto touch = getEvent(currentSlot);
                        touch->minor = event.value;
                    }break;
                    case ABS_MT_ORIENTATION:{
                        auto touch = getEvent(currentSlot);
                        touch->orientation = event.value;
                    }break;
                }
            break;
        }
    }
    void penEvent(const input_event& event){
        if(event.type != EV_KEY || event.code != BTN_TOOL_PEN){
            return;
        }
        penActive = event.value;
        if(Oxide::debugEnabled()){
            qDebug() << "Pen state: " << (penActive ? "Active" : "Inactive");
        }
    }

private:
    Manager* systemd;
    QList<Inhibitor> inhibitors;
    Application* resumeApp;
    qint64 lockTimestamp = 0;
    QTimer suspendTimer;
    QTimer lockTimer;
    QSettings settings;
    QStringList sleepInhibitors;
    QStringList powerOffInhibitors;
    QMutex mutex;
    QMap<int, Touch*> touches;
    int currentSlot = 0;
    bool wifiWasOn = false;
    bool penActive = false;
    int swipeDirection = None;
    QPoint location;
    QPoint startLocation;
    QMap<SwipeDirection, bool> swipeStates;
    QMap<SwipeDirection, int> swipeLengths;

    void inhibitSleep(){
        inhibitors.append(Inhibitor(systemd, "sleep", qApp->applicationName(), "Handle sleep screen"));
    }
    void inhibitPowerOff(){
        inhibitors.append(Inhibitor(systemd, "shutdown", qApp->applicationName(), "Block power off from any other method", true));
        rguard(true);
    }
    void releaseSleepInhibitors(bool block = false){
        QMutableListIterator<Inhibitor> i(inhibitors);
        while(i.hasNext()){
            auto inhibitor = i.next();
            if(inhibitor.what.contains("sleep") && inhibitor.block == block){
                inhibitor.release();
            }
            if(inhibitor.released()){
                i.remove();
            }
        }
    }
    void releasePowerOffInhibitors(bool block = false){
        QMutableListIterator<Inhibitor> i(inhibitors);
        while(i.hasNext()){
            auto inhibitor = i.next();
            if(inhibitor.what.contains("shutdown") && inhibitor.block == block){
                inhibitor.release();
            }
            if(inhibitor.released()){
                i.remove();
            }
        }
    }
    void rguard(bool install){
        QProcess::execute("/opt/bin/rguard", QStringList() << (install ? "-1" : "-0"));
    }
    Touch* getEvent(int slot){
        if(slot == -1){
            return nullptr;
        }
        if(!touches.contains(slot)){
            touches.insert(slot, new Touch{
                .slot = slot
            });
        }
        return touches.value(slot);
    }
    int getCurrentFingers(){
        return std::count_if(touches.begin(), touches.end(), [](Touch* touch){
            return touch->active;
        });
    }

    void touchDown(QList<Touch*> touches){
        if(penActive){
            return;
        }
        if(Oxide::debugEnabled()){
            qDebug() << "DOWN" << touches;
        }
        if(getCurrentFingers() != 1){
            return;
        }
        auto touch = touches.first();
        if(swipeDirection != None || touch->x == NULL_TOUCH_COORD || touch->y == NULL_TOUCH_COORD){
            return;
        }
        int offset = 20;
        if(deviceSettings.getDeviceType() == Oxide::DeviceSettings::RM2){
            offset = 40;
        }
        if(touch->y <= offset){
            swipeDirection = Up;
        }else if(touch->y > (deviceSettings.getTouchHeight() - offset)){
            swipeDirection = Down;
        }else if(touch->x <= offset){
            if(deviceSettings.getDeviceType() == Oxide::DeviceSettings::RM2){
                swipeDirection = Right;
            }else{
                swipeDirection = Left;
            }
        }else if(touch->x > (deviceSettings.getTouchWidth() - offset)){
            if(deviceSettings.getDeviceType() == Oxide::DeviceSettings::RM2){
                swipeDirection = Left;
            }else{
                swipeDirection = Right;
            }
        }else{
            return;
        }
        if(Oxide::debugEnabled()){
            qDebug() << "Swipe started" << swipeDirection;
        }
        startLocation = location = QPoint(touch->x, touch->y);
    }
    void touchUp(QList<Touch*> touches){
        if(Oxide::debugEnabled()){
            qDebug() << "UP" << touches;
        }
        if(swipeDirection == None){
            if(Oxide::debugEnabled()){
                qDebug() << "Not swiping";
            }
            if(touchHandler->grabbed()){
                for(auto touch : touches){
                    writeTouchUp(touch);
                }
                touchHandler->ungrab();
            }
            return;
        }
        if(getCurrentFingers()){
            if(Oxide::debugEnabled()){
                qDebug() << "Still swiping";
            }
            if(touchHandler->grabbed()){
                for(auto touch : touches){
                    writeTouchUp(touch);
                }
            }
            return;
        }
        if(touches.length() > 1){
            if(Oxide::debugEnabled()){
                qDebug() << "Too many fingers";
            }
            if(touchHandler->grabbed()){
                for(auto touch : touches){
                    writeTouchUp(touch);
                }
                touchHandler->ungrab();
            }
            swipeDirection = None;
            return;
        }
        auto touch = touches.first();
        if(touch->x == NULL_TOUCH_COORD || touch->y == NULL_TOUCH_COORD){
            if(Oxide::debugEnabled()){
                qDebug() << "Invalid touch event";
            }
            swipeDirection = None;
            return;
        }
        if(swipeDirection == Up){
            if(!swipeStates[Up] || touch->y < location.y() || touch->y - startLocation.y() < swipeLengths[Up]){
                // Must end swiping up and having gone far enough
                cancelSwipe(touch);
                return;
            }
            emit bottomAction();
        }else if(swipeDirection == Down){
            if(!swipeStates[Down] || touch->y > location.y() || startLocation.y() - touch->y < swipeLengths[Down]){
                // Must end swiping down and having gone far enough
                cancelSwipe(touch);
                return;
            }
            emit topAction();
        }else if(swipeDirection == Right || swipeDirection == Left){
            auto isRM2 = deviceSettings.getDeviceType() == Oxide::DeviceSettings::RM2;
            auto invalidLeft = !swipeStates[Left] || touch->x < location.x() || touch->x - startLocation.x() < swipeLengths[Left];
            auto invalidRight = !swipeStates[Right] || touch->x > location.x() || startLocation.x() - touch->x < swipeLengths[Right];
            if(swipeDirection == Right && (isRM2 ? invalidLeft : invalidRight)){
                // Must end swiping right and having gone far enough
                cancelSwipe(touch);
                return;
            }else if(swipeDirection == Left && (isRM2 ? invalidRight : invalidLeft)){
                // Must end swiping left and having gone far enough
                cancelSwipe(touch);
                return;
            }
            if(swipeDirection == Left){
                emit rightAction();
            }else{
                emit leftAction();
            }
        }
        swipeDirection = None;
        touchHandler->ungrab();
        touch->x = -1;
        touch->y = -1;
        writeTouchUp(touch);
        if(Oxide::debugEnabled()){
            qDebug() << "Swipe direction" << swipeDirection;
        }
    }
    void touchMove(QList<Touch*> touches){
        if(Oxide::debugEnabled()){
            qDebug() << "MOVE" << touches;
        }
        if(swipeDirection == None){
            if(touchHandler->grabbed()){
                for(auto touch : touches){
                    writeTouchMove(touch);
                }
                touchHandler->ungrab();
            }
            return;
        }
        if(touches.length() > 1){
            if(Oxide::debugEnabled()){
                qDebug() << "Too many fingers";
            }
            if(touchHandler->grabbed()){
                for(auto touch : touches){
                    writeTouchMove(touch);
                }
                touchHandler->ungrab();
            }
            swipeDirection = None;
            return;
        }
        auto touch = touches.first();
        if(touch->y > location.y()){
            location = QPoint(touch->x, touch->y);
        }
    }
    void cancelSwipe(Touch* touch){
        if(Oxide::debugEnabled()){
            qDebug() << "Swipe Cancelled";
        }
        swipeDirection = None;
        touchHandler->ungrab();
        writeTouchUp(touch);
    }
    void writeTouchUp(Touch* touch){
        bool grabbed = touchHandler->grabbed();
        if(grabbed){
            touchHandler->ungrab();
        }
        writeTouchMove(touch);
        if(Oxide::debugEnabled()){
            qDebug() << "Write touch up" << touch;
        }
        int size = sizeof(input_event) * 3;
        input_event* events = (input_event*)malloc(size);
        events[0] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_SLOT, touch->slot);
        events[1] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_TRACKING_ID, -1);
        events[2] = DigitizerHandler::createEvent(EV_SYN, 0, 0);
        touchHandler->write(events, size);
        free(events);
        if(grabbed){
            touchHandler->grab();
        }
    }
    void writeTouchMove(Touch* touch){
        bool grabbed = touchHandler->grabbed();
        if(grabbed){
            touchHandler->ungrab();
        }
        if(Oxide::debugEnabled()){
            qDebug() << "Write touch move" << touch;
        }
        int count = 8;
        if(touch->x == NULL_TOUCH_COORD){
            count--;
        }
        if(touch->y == NULL_TOUCH_COORD){
            count--;
        }
        int size = sizeof(input_event) * count;
        input_event* events = (input_event*)malloc(size);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_SLOT, touch->slot);
        if(touch->x != NULL_TOUCH_COORD){
            events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_POSITION_X, touch->x);
        }
        if(touch->y != NULL_TOUCH_COORD){
            events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_POSITION_Y, touch->y);
        }
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_PRESSURE, touch->pressure);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_TOUCH_MAJOR, touch->major);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_TOUCH_MINOR, touch->minor);
        events[2] = DigitizerHandler::createEvent(EV_ABS, ABS_MT_ORIENTATION, touch->orientation);
        events[2] = DigitizerHandler::createEvent(EV_SYN, 0, 0);
        touchHandler->write(events, size);
        free(events);
        if(grabbed){
            touchHandler->grab();
        }
    }
    void fn(){
        auto n = 512 * 8;
        auto num_inst = 4;
        input_event* ev = (input_event *)malloc(sizeof(struct input_event) * n * num_inst);
        memset(ev, 0, sizeof(input_event) * n * num_inst);
        auto i = 0;
        while (i < n) {
            ev[i++] = DigitizerHandler::createEvent(EV_ABS, ABS_DISTANCE, 1);
            ev[i++] = DigitizerHandler::createEvent(EV_SYN, 0, 0);
            ev[i++] = DigitizerHandler::createEvent(EV_ABS, ABS_DISTANCE, 2);
            ev[i++] = DigitizerHandler::createEvent(EV_SYN, 0, 0);
        }
    }
};
#endif // SYSTEMAPI_H
