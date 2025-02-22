#pragma once
#ifndef DIGITIZERHANDLER_H
#define DIGITIZERHANDLER_H

#include <QThread>
#include <QException>

#include <ext/stdio_filebuf.h>
#include <sstream>
#include <linux/input.h>
#include <iostream>
#include <string>
#include <vector>
#include <liboxide.h>
#include <liboxide/event_device.h>

using namespace std;
using namespace Oxide;

#define touchHandler DigitizerHandler::singleton_touchScreen()
#define wacomHandler DigitizerHandler::singleton_wacom()

class DigitizerHandler : public QThread {
    Q_OBJECT

public:
    static DigitizerHandler* singleton_touchScreen(){
        static DigitizerHandler* instance;
        if(instance != nullptr){
            return instance;
        }
        // Get event devices
        event_device touchScreen_device(deviceSettings.getTouchDevicePath(), O_RDWR);
        if(touchScreen_device.fd == -1){
            qDebug() << "Failed to open event device: " << touchScreen_device.device.c_str();
            throw QException();
        }
        instance = new DigitizerHandler(touchScreen_device);
        instance->start();
        return instance;
    }
    static DigitizerHandler* singleton_wacom(){
        static DigitizerHandler* instance;
        if(instance != nullptr){
            return instance;
        }
        // Get event devices
        event_device wacom_device(deviceSettings.getWacomDevicePath(), O_RDWR);
        if(wacom_device.fd == -1){
            qDebug() << "Failed to open event device: " << wacom_device.device.c_str();
            throw QException();
        }
        instance = new DigitizerHandler(wacom_device);
        instance->start();
        return instance;
    }
    static string exec(const char* cmd);
    static vector<std::string> split_string_by_newline(const std::string& str);
    static int is_uint(string input);

    DigitizerHandler(event_device& device)
     : QThread(),
       filebuf(device.fd, ios::in),
       stream(&filebuf),
       m_enabled(true),
       device(device) {
        flood = build_flood();
    }
    ~DigitizerHandler(){
        if(device.fd == -1){
            return;
        }
        if(device.locked){
            ungrab();
        }
        close(device.fd);
    }
    void setEnabled(bool enabled){
        m_enabled = enabled;
    }
    void grab(){
        if(!grabbed()){
            device.lock();
        }
    }
    void ungrab(){
        if(grabbed()){
            device.unlock();
        }
    }
    bool grabbed() { return device.locked; }
    void write(ushort type, ushort code, int value){
        auto event = createEvent(type, code, value);
        ::write(device.fd, &event, sizeof(input_event));
#ifdef DEBUG
        qDebug() << "Emitted event " << event.time.tv_sec << event.time.tv_usec << type << code << value;
#endif
    }
    void write(input_event* events, size_t size){
        ::write(device.fd, events, size);
    }
    void syn(){
        write(EV_SYN, SYN_REPORT, 0);
    }
    void clear_buffer(){
        if(device.fd == -1){
            return;
        }
#ifdef DEBUG
        qDebug() << "Clearing event buffer on" << device.device.c_str();
#endif
        write(flood, 512 * 8 * 4 * sizeof(input_event));
    }
    static inline input_event createEvent(ushort type, ushort code, int value){
        struct input_event event;
        event.type = type;
        event.code = code;
        event.value = value;
        return event;
    }

signals:
    void activity();
    void inputEvent(const input_event& event);

protected:
    input_event* flood;
    input_event* build_flood(){
        auto n = 512 * 8;
        auto num_inst = 4;
        input_event* ev = (input_event*)malloc(sizeof(struct input_event) * n * num_inst);
        memset(ev, 0, sizeof(input_event) * n * num_inst);
        auto i = 0;
        while (i < n) {
            ev[i++] = createEvent(EV_ABS, ABS_DISTANCE, 1);
            ev[i++] = createEvent(EV_SYN, 0, 0);
            ev[i++] = createEvent(EV_ABS, ABS_DISTANCE, 2);
            ev[i++] = createEvent(EV_SYN, 0, 0);
        }
        return ev;
    }
    void run(){
        char name[256];
        memset(name, 0, sizeof(name));
        ioctl(device.fd, EVIOCGNAME(sizeof(name)), name);
        qDebug() << "Reading From : " << device.device.c_str() << " (" << name << ")";
        qDebug() << "Listening for events...";
        while(handle_events()){
            qApp->processEvents(QEventLoop::AllEvents, 100);
            QThread::yieldCurrentThread();
        }
    }
    bool handle_events(){
        vector<input_event> event_buffer;
        bool success = true;
        while(true){
            input_event event;
            if(!read(&event)){
                success = false;
                goto exitLoop;
            }
            event_buffer.push_back(event);
            switch(event.type){
                case EV_SYN:
                    switch(event.code){
                        case SYN_DROPPED:
                            event_buffer.clear();
                            skip_event();
                        case SYN_REPORT:
                        case SYN_MT_REPORT:
                            goto exitLoop;
                        break;
                    }
                break;
            }
        }
        exitLoop:
        if(event_buffer.size()){
            for(input_event event : event_buffer){
                emit inputEvent(event);
            }
            emit activity();
        }
        return success;
    }
    void skip_event(){
        input_event event;
        while(read(&event)){
            if(event.type != EV_SYN){
                continue;
            }
            if(event.code == SYN_REPORT || event.code == SYN_MT_REPORT){
                break;
            }
        }
    }
    __gnu_cxx::stdio_filebuf<char> filebuf;
    istream stream;
    bool m_enabled;
    event_device device;
    bool read(input_event* ie){
        return (bool)stream.read((char*)ie, static_cast<streamsize>(sizeof(struct input_event)));
    }
};

#endif // DIGITIZERHANDLER_H
