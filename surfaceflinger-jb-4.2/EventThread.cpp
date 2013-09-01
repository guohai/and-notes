/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include <stdint.h>
#include <sys/types.h>

#include <cutils/compiler.h>

#include <gui/BitTube.h>
#include <gui/IDisplayEventConnection.h>
#include <gui/DisplayEventReceiver.h>

#include <utils/Errors.h>
#include <utils/String8.h>
#include <utils/Trace.h>

#include "EventThread.h"
#include "SurfaceFlinger.h"

// ---------------------------------------------------------------------------
namespace android {
// ---------------------------------------------------------------------------

EventThread::EventThread(const sp<SurfaceFlinger>& flinger)
    : mFlinger(flinger),
      mUseSoftwareVSync(false),
      mDebugVsyncEnabled(false) {

    for (int32_t i = 0; i < HWC_DISPLAY_TYPES_SUPPORTED; i++) {
        mVSyncEvent[i].header.type = DisplayEventReceiver::DISPLAY_EVENT_VSYNC;
        mVSyncEvent[i].header.id = 0;
        mVSyncEvent[i].header.timestamp = 0;
        mVSyncEvent[i].vsync.count =  0;
    }
}

void EventThread::onFirstRef() {
    run("EventThread", PRIORITY_URGENT_DISPLAY + PRIORITY_MORE_FAVORABLE); // 第一次引用的时候开始运行
}

sp<EventThread::Connection> EventThread::createEventConnection() const {
    return new Connection(const_cast<EventThread*>(this));
}

status_t EventThread::registerDisplayEventConnection( // 每个Connection被调用一次，但是目前可能会有很多Connection连接过来，很多侦测VSYNC
        const sp<EventThread::Connection>& connection) {
    Mutex::Autolock _l(mLock);
    mDisplayEventConnections.add(connection);
    mCondition.broadcast();
    return NO_ERROR;
}

void EventThread::removeDisplayEventConnection(
        const wp<EventThread::Connection>& connection) {
    Mutex::Autolock _l(mLock);
    mDisplayEventConnections.remove(connection);
}

void EventThread::setVsyncRate(uint32_t count,
        const sp<EventThread::Connection>& connection) {
    if (int32_t(count) >= 0) { // server must protect against bad params
        Mutex::Autolock _l(mLock);
        const int32_t new_count = (count == 0) ? -1 : count; // 0 没有事件返回
        if (connection->count != new_count) {
            connection->count = new_count;
            mCondition.broadcast();
        }
    }
}

void EventThread::requestNextVsync(
        const sp<EventThread::Connection>& connection) {
    Mutex::Autolock _l(mLock);
    if (connection->count < 0) { // 如果是大于等于0就表示continuous，所以这个方法就不需要作用
        connection->count = 0; // -1是个表示要requestNextVsync操作有返回的状态，所以这里有进行转换
        mCondition.broadcast();
    }
}

void EventThread::onScreenReleased() {
    Mutex::Autolock _l(mLock);
    if (!mUseSoftwareVSync) {
        // disable reliance on h/w vsync
        mUseSoftwareVSync = true;
        mCondition.broadcast();
    }
}

void EventThread::onScreenAcquired() {
    Mutex::Autolock _l(mLock);
    if (mUseSoftwareVSync) {
        // resume use of h/w vsync
        mUseSoftwareVSync = false;
        mCondition.broadcast();
    }
}


void EventThread::onVSyncReceived(int type, nsecs_t timestamp) { // 不管是硬件还是软件触发的VSYNC事件都会到这里
    ALOGE_IF(type >= HWC_DISPLAY_TYPES_SUPPORTED,
            "received event for an invalid display (id=%d)", type);

    Mutex::Autolock _l(mLock);
    if (type < HWC_DISPLAY_TYPES_SUPPORTED) {
        mVSyncEvent[type].header.type = DisplayEventReceiver::DISPLAY_EVENT_VSYNC;
        mVSyncEvent[type].header.id = type;
        mVSyncEvent[type].header.timestamp = timestamp; // 不为0就表示有一个VSYNC事件过来了，并且type会直到是哪个display过来的
        mVSyncEvent[type].vsync.count++;
        mCondition.broadcast();
    }
}

void EventThread::onHotplugReceived(int type, bool connected) {
    ALOGE_IF(type >= HWC_DISPLAY_TYPES_SUPPORTED,
            "received event for an invalid display (id=%d)", type);

    Mutex::Autolock _l(mLock);
    if (type < HWC_DISPLAY_TYPES_SUPPORTED) {
        DisplayEventReceiver::Event event;
        event.header.type = DisplayEventReceiver::DISPLAY_EVENT_HOTPLUG;
        event.header.id = type;
        event.header.timestamp = systemTime();
        event.hotplug.connected = connected;
        mPendingEvents.add(event);
        mCondition.broadcast();
    }
}

bool EventThread::threadLoop() {
    DisplayEventReceiver::Event event;
    Vector< sp<EventThread::Connection> > signalConnections; // 比如会有多个组件或者应用注册这个Connection
    signalConnections = waitForEvent(&event); // 获取到一个Evente(vsync/hotplug)

    // dispatch events to listeners...
    const size_t count = signalConnections.size(); // 有多少路Layer/Display连接过来？
												   // 如果每路都是对不同的Event感兴趣怎么办？
    for (size_t i = 0 ; i < count ; i++) {
        const sp<Connection>& conn(signalConnections[i]);
        // now see if we still need to report this event
        status_t err = conn->postEvent(event); // 通过BitTube把收到的Event散播出去，每次就一个Event，如果有多个display和不同的event
											   // 岂不很繁忙？
        if (err == -EAGAIN || err == -EWOULDBLOCK) {
            // The destination doesn't accept events anymore, it's probably
            // full. For now, we just drop the events on the floor.
            // FIXME: Note that some events cannot be dropped and would have
            // to be re-sent later.
            // Right-now we don't have the ability to do this.
            ALOGW("EventThread: dropping event (%08x) for connection %p",
                    event.header.type, conn.get());
        } else if (err < 0) {
            // handle any other error on the pipe as fatal. the only
            // reasonable thing to do is to clean-up this connection.
            // The most common error we'll get here is -EPIPE.
            removeDisplayEventConnection(signalConnections[i]);
        }
    }
    
    // Derived class must implement threadLoop(). The thread starts its life
    // here. There are two ways of using the Thread object:
    // 1) loop: if threadLoop() returns true, it will be called again if
    //          requestExit() wasn't called.
    // 2) once: if threadLoop() returns false, the thread will exit upon return.
    // virtual bool        threadLoop() = 0;

    return true;
}

// This will return when (1) a vsync event has been received, and (2) there was
// at least one connection interested in receiving it when we started waiting.
Vector< sp<EventThread::Connection> > EventThread::waitForEvent(
        DisplayEventReceiver::Event* event) // 这个东西会block住
{
    Mutex::Autolock _l(mLock);
    Vector< sp<EventThread::Connection> > signalConnections;

    do {
        bool eventPending = false;
        bool waitForVSync = false;

        size_t vsyncCount = 0; // 回来的VSYNC有多少
        nsecs_t timestamp = 0;
        for (int32_t i = 0; i < HWC_DISPLAY_TYPES_SUPPORTED; i++) {
            timestamp = mVSyncEvent[i].header.timestamp; // timestamp会被新的事件的timestamp给覆盖掉
            if (timestamp) {
                // we have a vsync event to dispatch
                *event = mVSyncEvent[i];
                mVSyncEvent[i].header.timestamp = 0;
                vsyncCount = mVSyncEvent[i].vsync.count;
                break;
            }
        }

        if (!timestamp) {
            // no vsync event, see if there are some other event
            eventPending = !mPendingEvents.isEmpty(); // 目前这里主要就是HOT-PLUG事件
            if (eventPending) {
                // we have some other event to dispatch
                *event = mPendingEvents[0]; // Good, we got one finally
                mPendingEvents.removeAt(0);
            }
        }

        // find out connections waiting for events
        size_t count = mDisplayEventConnections.size(); // 多个请求display事件的连接
        for (size_t i = 0; i < count; i++) {
            sp<Connection> connection(mDisplayEventConnections[i].promote());
            if (connection != NULL) {
                bool added = false;
                if (connection->count >= 0) {
                    // we need vsync events because at least
                    // one connection is waiting for it
                    waitForVSync = true;
                    if (timestamp) {
                        // we consume the event only if it's time
                        // (ie: we received a vsync event)
                        if (connection->count == 0) {
                            // fired this time around
                            connection->count = -1;
                            signalConnections.add(connection);
                            added = true;
                        } else if (connection->count == 1 ||
                                (vsyncCount % connection->count) == 0) { // 这个vsyncCount什么意义？ connection->count可能很大吗？
																		 // 0 / 5
																		 // 这是相对于已有的VSYNC比较的，比如每间隔5个VSYNC返回一个
																		 // 给客户端， 1就是每个都返回
                            // continuous event, and time to report it
                            signalConnections.add(connection);
                            added = true;
                        }
                    }
                } // 针对-1的情况，都不触发，如果变成0了，肯定是有人执行了requestNextVsync

                if (eventPending && !timestamp && !added) { // 没有VSYNC，但是有其他事件
                    // we don't have a vsync event to process
                    // (timestamp == 0), but we have some pending
                    // messages.
                    signalConnections.add(connection);
                }
            } else {
                // we couldn't promote this reference, the connection has
                // died, so clean-up!
                mDisplayEventConnections.removeAt(i);
                --i; --count; // Good
            }
        }

        // Here we figure out if we need to enable or disable vsyncs
        if (timestamp && !waitForVSync) { // Cool，没有人请求就不用report，但是目前看来至少有一个吧，就是我们SurfaceFlinger用的
            // we received a VSYNC but we have no clients
            // don't report it, and disable VSYNC events
            disableVSyncLocked();
        } else if (!timestamp && waitForVSync) {
            // we have at least one client, so we want vsync enabled
            // (TODO: this function is called right after we finish
            // notifying clients of a vsync, so this call will be made
            // at the vsync rate, e.g. 60fps.  If we can accurately
            // track the current state we could avoid making this call
            // so often.)
            enableVSyncLocked(); // 这个是同步的吗？
        }

        // note: !timestamp implies signalConnections.isEmpty(), because we
        // don't populate signalConnections if there's no vsync pending
        if (!timestamp && !eventPending) {
            // wait for something to happen
            if (waitForVSync) {
                // This is where we spend most of our time, waiting
                // for vsync events and new client registrations.
                //
                // If the screen is off, we can't use h/w vsync, so we
                // use a 16ms timeout instead.  It doesn't need to be
                // precise, we just need to keep feeding our clients.
                //
                // We don't want to stall if there's a driver bug, so we
                // use a (long) timeout when waiting for h/w vsync, and
                // generate fake events when necessary.
                bool softwareSync = mUseSoftwareVSync;
                nsecs_t timeout = softwareSync ? ms2ns(16) : ms2ns(1000); // 如果DRV有bug，我们还是可以继续进行，只是时间不准
                if (mCondition.waitRelative(mLock, timeout) == TIMED_OUT) {
                    if (!softwareSync) {
                        ALOGW("Timed out waiting for hw vsync; faking it");
                    }
                    // FIXME: how do we decide which display id the fake
                    // vsync came from ?
					// 好问题
                    mVSyncEvent[0].header.type = DisplayEventReceiver::DISPLAY_EVENT_VSYNC;
                    mVSyncEvent[0].header.id = HWC_DISPLAY_PRIMARY;
                    mVSyncEvent[0].header.timestamp = systemTime(SYSTEM_TIME_MONOTONIC);
                    mVSyncEvent[0].vsync.count++;
                }
            } else {
                // Nobody is interested in vsync, so we just want to sleep.
                // h/w vsync should be disabled, so this will wait until we
                // get a new connection, or an existing connection becomes
                // interested in receiving vsync again.
                mCondition.wait(mLock); // 没有人对VSYNC感兴趣，所以我们需要关闭硬件VSYNC，然后在此睡眠
            }
        }
    } while (signalConnections.isEmpty());

    // here we're guaranteed to have a timestamp and some connections to signal
    // (The connections might have dropped out of mDisplayEventConnections
    // while we were asleep, but we'll still have strong references to them.)
    return signalConnections;
}

void EventThread::enableVSyncLocked() {
    if (!mUseSoftwareVSync) {
        // never enable h/w VSYNC when screen is off
        mFlinger->eventControl(HWC_DISPLAY_PRIMARY, SurfaceFlinger::EVENT_VSYNC, true);
        mPowerHAL.vsyncHint(true);
    }
    mDebugVsyncEnabled = true;
}

void EventThread::disableVSyncLocked() {
    mFlinger->eventControl(HWC_DISPLAY_PRIMARY, SurfaceFlinger::EVENT_VSYNC, false);
    mPowerHAL.vsyncHint(false);
    mDebugVsyncEnabled = false;
}

void EventThread::dump(String8& result, char* buffer, size_t SIZE) const {
    Mutex::Autolock _l(mLock);
    result.appendFormat("VSYNC state: %s\n",
            mDebugVsyncEnabled?"enabled":"disabled");
    result.appendFormat("  soft-vsync: %s\n",
            mUseSoftwareVSync?"enabled":"disabled");
    result.appendFormat("  numListeners=%u,\n  events-delivered: %u\n",
            mDisplayEventConnections.size(),
            mVSyncEvent[HWC_DISPLAY_PRIMARY].vsync.count);
    for (size_t i=0 ; i<mDisplayEventConnections.size() ; i++) {
        sp<Connection> connection =
                mDisplayEventConnections.itemAt(i).promote();
        result.appendFormat("    %p: count=%d\n",
                connection.get(), connection!=NULL ? connection->count : 0);
    }
}

// ---------------------------------------------------------------------------

EventThread::Connection::Connection(
        const sp<EventThread>& eventThread)
    : count(-1), mEventThread(eventThread), mChannel(new BitTube()) // 服务端创建
{
}

EventThread::Connection::~Connection() {
    // do nothing here -- clean-up will happen automatically
    // when the main thread wakes up
}

void EventThread::Connection::onFirstRef() {
    // NOTE: mEventThread doesn't hold a strong reference on us
    mEventThread->registerDisplayEventConnection(this);
}

sp<BitTube> EventThread::Connection::getDataChannel() const {
    return mChannel;
}

void EventThread::Connection::setVsyncRate(uint32_t count) {
    mEventThread->setVsyncRate(count, this);
}

void EventThread::Connection::requestNextVsync() {
    mEventThread->requestNextVsync(this); // 如果VsyncRate大于等于0，这个方法就不起作用
}

status_t EventThread::Connection::postEvent(
        const DisplayEventReceiver::Event& event) {
    ssize_t size = DisplayEventReceiver::sendEvents(mChannel, &event, 1); // 看这里，看这里
    return size < 0 ? status_t(size) : status_t(NO_ERROR);
}

// ---------------------------------------------------------------------------

}; // namespace android
