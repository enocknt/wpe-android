/**
 * Copyright (C) 2022 Igalia S.L. <info@igalia.com>
 *   Author: Jani Hautakangas <jani@igalia.com>
 *   Author: Loïc Le Page <llepage@igalia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "MessagePump.h"

#include <sys/eventfd.h>
#include <unistd.h>

namespace {
uint glibEventsToLooperEvents(gushort events) noexcept
{
    uint looperEvents = 0;
    if ((events & G_IO_IN) != 0)
        looperEvents |= ALOOPER_EVENT_INPUT;
    if ((events & G_IO_OUT) != 0)
        looperEvents |= ALOOPER_EVENT_OUTPUT;
    if ((events & G_IO_ERR) != 0)
        looperEvents |= ALOOPER_EVENT_ERROR;
    if ((events & G_IO_HUP) != 0)
        looperEvents |= ALOOPER_EVENT_HANGUP;
    if ((events & G_IO_NVAL) != 0)
        looperEvents |= ALOOPER_EVENT_INVALID;
    return looperEvents;
}

gushort looperEventsToGLibEvents(uint events) noexcept
{
    gushort glibEvents = 0;
    if ((events & ALOOPER_EVENT_INPUT) != 0)
        glibEvents |= G_IO_IN;
    if ((events & ALOOPER_EVENT_OUTPUT) != 0)
        glibEvents |= G_IO_OUT;
    if ((events & ALOOPER_EVENT_ERROR) != 0)
        glibEvents |= G_IO_ERR;
    if ((events & ALOOPER_EVENT_HANGUP) != 0)
        glibEvents |= G_IO_HUP;
    if ((events & ALOOPER_EVENT_INVALID) != 0)
        glibEvents |= G_IO_NVAL;
    return glibEvents;
}
} // namespace

/*
 * Message pump implements integration between the GLib main loop
 * and the Android native ALooper run loop and events handling..
 *
 * Message pump "pumps" events and messages file descriptors from GLib context
 * and pushes them back to an Android looper for polling. When an event occurs,
 * the MessagePump is called from the Android looper and initiates GLib main
 * loop cycle steps (prepare, check, dispatch) and makes the appropriate calls
 * into GLib.
 *
 * This allows to run WPE UI on Android within the Android main UI thread.
 */
MessagePump::MessagePump()
{
    // The Android native ALooper uses epoll to poll file descriptors.
    // We use eventfd to inform GLib that it can start dispatching.
    m_dispatchFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

    m_looper = ALooper_prepare(0);
    ALooper_acquire(m_looper);

    ALooper_addFd(
        m_looper, m_dispatchFd, ALOOPER_POLL_CALLBACK, ALOOPER_EVENT_INPUT,
        +[](int fileDesc, int /*events*/, void* userData) -> int {
            // Clear the eventfd and reset its counter to 0
            uint64_t value = 0;
            read(fileDesc, &value, sizeof(value));

            auto* pump = reinterpret_cast<MessagePump*>(userData);
            pump->m_pendingDispatch = false;
            pump->dispatch();
            pump->prepare();

            return 1; // Continue listening for events
        },
        reinterpret_cast<void*>(this));

    m_context = g_main_context_ref(g_main_context_default());
    g_main_context_acquire(m_context);
    prepare();
}

MessagePump::~MessagePump()
{
    flush();

    for (const auto& pollFD : m_looperFdEvents)
        ALooper_removeFd(m_looper, pollFD.first);
    m_looperFdEvents.clear();

    m_pollFdsSize = 0;
    m_pollFdsCapacity = 0;
    if (m_pollFds != nullptr) {
        g_free(m_pollFds);
        m_pollFds = nullptr;
    }
    m_maxPriority = 0;

    g_main_context_release(m_context);
    g_main_context_unref(m_context);

    ALooper_release(m_looper);
    m_looper = nullptr;

    close(m_dispatchFd);
    m_dispatchFd = 0;
}

void MessagePump::flush() const noexcept
{
    // Clear the eventfd and reset its counter to 0
    int64_t value = 0;
    read(m_dispatchFd, &value, sizeof(value));
    dispatch();
}

void MessagePump::invoke(void (*onExec)(void*), void (*onDestroy)(void*), void* userData) const noexcept
{
    struct InvocationInfo {
        void (*m_onExec)(void*);
        void (*m_onDestroy)(void*);
        void* m_userData;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory, bugprone-unhandled-exception-at-new)
    auto* info = new InvocationInfo {onExec, onDestroy, userData};
    g_main_context_invoke_full(
        m_context, G_PRIORITY_DEFAULT,
        +[](gpointer data) -> gboolean {
            auto* invocation = reinterpret_cast<InvocationInfo*>(data);
            if (invocation->m_onExec != nullptr)
                invocation->m_onExec(invocation->m_userData);
            return G_SOURCE_REMOVE;
        },
        info,
        +[](gpointer data) -> void {
            auto* invocation = reinterpret_cast<InvocationInfo*>(data);
            if (invocation->m_onDestroy != nullptr)
                invocation->m_onDestroy(invocation->m_userData);
            delete invocation; // NOLINT(cppcoreguidelines-owning-memory)
        });
}

void MessagePump::prepare() noexcept
{
    g_main_context_prepare(m_context, &m_maxPriority);

    if (m_pollFds == nullptr) {
        m_pollFdsCapacity = 1; // There will be at least one fd in GMainContext
        m_pollFds = g_new(GPollFD, m_pollFdsCapacity);
    }

    gint timeout = 0;
    while ((m_pollFdsSize = g_main_context_query(m_context, m_maxPriority, &timeout, m_pollFds, m_pollFdsCapacity))
        > m_pollFdsCapacity) {
        g_free(m_pollFds);
        m_pollFdsCapacity = m_pollFdsSize;
        m_pollFds = g_new(GPollFD, m_pollFdsCapacity);
    }

    std::vector<GPollFD> changedPollFDs;
    std::vector<int> removedFds;
    collectPollFDChanges(m_pollFds, m_pollFdsSize, changedPollFDs, removedFds);

    for (const auto& pollFD : changedPollFDs) {
        ALooper_addFd(
            m_looper, pollFD.fd, ALOOPER_POLL_CALLBACK,
            static_cast<int>(glibEventsToLooperEvents(pollFD.events) | ALOOPER_EVENT_OUTPUT),
            +[](int fileDesc, int events, void* userData) -> int {
                auto* pump = reinterpret_cast<MessagePump*>(userData);
                for (int j = 0; j < pump->m_pollFdsSize; ++j) {
                    if (pump->m_pollFds[j].fd == fileDesc) {
                        pump->m_pollFds[j].revents = looperEventsToGLibEvents(events);
                        break;
                    }
                }
                pump->scheduleDispatch();
                return 1; // Continue listening for events
            },
            reinterpret_cast<void*>(this));
    }

    for (const auto& fd : removedFds) {
        ALooper_removeFd(m_looper, fd);
    }
}

void MessagePump::collectPollFDChanges(
    const GPollFD* pollFDs, int numPollFDs, std::vector<GPollFD>& changedPollFDs, std::vector<int>& removedFds) noexcept
{
    std::unordered_map<int, int> currFdEvents; // Current fd to mask map

    // Build currFdMask by combining masks for the same fd
    for (int i = 0; i < numPollFDs; ++i) {
        int fd = pollFDs[i].fd;
        gushort events = pollFDs[i].events;
        currFdEvents[fd] |= events;
    }

    // Detect changes and new fds
    for (const auto& kv : currFdEvents) {
        int fd = kv.first;
        gushort events = kv.second;

        auto it = m_looperFdEvents.find(fd);
        if (it == m_looperFdEvents.end()) {
            // New fd detected
            GPollFD e {fd, events, 0};
            changedPollFDs.push_back(e);
        } else if (events != it->second) {
            // Events has changed
            GPollFD e {fd, events, 0};
            changedPollFDs.push_back(e);
        }
    }

    // Detect removed fds
    for (const auto& kv : m_looperFdEvents) {
        int fd = kv.first;
        if (currFdEvents.find(fd) == currFdEvents.end()) {
            // fd was in previous but not in current
            removedFds.push_back(fd);
        }
    }

    // Update m_looperFdEvents for the next call
    m_looperFdEvents = currFdEvents;
}

void MessagePump::scheduleDispatch() noexcept
{
    if (!m_pendingDispatch) {
        uint64_t value = 1;
        write(m_dispatchFd, &value, sizeof(value));
        m_pendingDispatch = true;
    }
}

void MessagePump::dispatch() const noexcept
{
    if (g_main_context_check(m_context, m_maxPriority, m_pollFds, m_pollFdsSize) == TRUE)
        g_main_context_dispatch(m_context);
}
