/*
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <devpkey.h>

#include <wine/debug.h>
#include <wine/list.h>

#include "mmdevapi_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(mmdevapi);
WINE_DECLARE_DEBUG_CHANNEL(voicemod);

/* {7f1c6c0e-5d0b-4f3a-9a53-2f4f6d2e8b11} */
static const GUID host_listener_session_guid =
    {0x7f1c6c0e, 0x5d0b, 0x4f3a, {0x9a, 0x53, 0x2f, 0x4f, 0x6d, 0x2e, 0x8b, 0x11}};

BOOL is_voicemod_capture_endpoint(IMMDevice *device)
{
    IPropertyStore *store;
    IMMEndpoint *endpoint;
    EDataFlow flow;
    PROPVARIANT pv;
    BOOL ret = FALSE;
    HRESULT hr;

    if (FAILED(IMMDevice_QueryInterface(device, &IID_IMMEndpoint, (void **)&endpoint)))
        return FALSE;
    hr = IMMEndpoint_GetDataFlow(endpoint, &flow);
    IMMEndpoint_Release(endpoint);
    if (FAILED(hr) || flow != eCapture) return FALSE;

    if (FAILED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store))) return FALSE;
    PropVariantInit(&pv);
    if (SUCCEEDED(IPropertyStore_GetValue(store, (const PROPERTYKEY *)&DEVPKEY_Device_FriendlyName, &pv)) &&
            pv.vt == VT_LPWSTR && pv.pwszVal && wcsstr(pv.pwszVal, L"Voicemod Virtual Audio Device"))
        ret = TRUE;
    PropVariantClear(&pv);
    IPropertyStore_Release(store);
    return ret;
}

/* The listener is a host application without a Windows process.  Name a
 * long-lived process other than the caller so clients which resolve the
 * process image, or which ignore their own sessions, accept the session. */
DWORD get_host_listener_pid(void)
{
    static DWORD cached;
    PROCESSENTRY32W entry = {.dwSize = sizeof(entry)};
    DWORD self = GetCurrentProcessId(), fallback = 0;
    HANDLE snapshot;

    if (cached) return cached;
    if ((snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)) == INVALID_HANDLE_VALUE)
        return self;
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == self || !entry.th32ProcessID) continue;
            if (!wcsicmp(entry.szExeFile, L"explorer.exe"))
            {
                cached = entry.th32ProcessID;
                break;
            }
            if (!fallback && !wcsicmp(entry.szExeFile, L"services.exe"))
                fallback = entry.th32ProcessID;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (!cached) cached = fallback ? fallback : self;
    return cached;
}

struct host_listener_created
{
    IAudioSessionNotification *notification;
    IAudioSessionControl *control;
};

static DWORD WINAPI host_listener_created_thread(void *arg)
{
    struct host_listener_created *created = arg;
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    Sleep(300);
    TRACE_(voicemod)("announcing host listener session to %p\n", created->notification);
    IAudioSessionNotification_OnSessionCreated(created->notification, created->control);
    IAudioSessionControl_Release(created->control);
    IAudioSessionNotification_Release(created->notification);
    free(created);
    if (SUCCEEDED(init)) CoUninitialize();
    return 0;
}

static void ensure_host_listener_session(IMMDevice *device)
{
    struct audio_session *session;

    sessions_lock();
    if (SUCCEEDED(get_audio_session(&host_listener_session_guid, device, 2, &session)))
        session->host_listener = TRUE;
    sessions_unlock();
}

static void announce_host_listener(IMMDevice *device, IAudioSessionNotification *notification)
{
    struct host_listener_created *created;
    struct audio_session_wrapper *wrapper;
    HANDLE thread;
    HRESULT hr;

    ensure_host_listener_session(device);

    sessions_lock();
    hr = get_audio_session_wrapper(&host_listener_session_guid, device, &wrapper);
    sessions_unlock();
    if (FAILED(hr)) return;

    if (!(created = malloc(sizeof(*created))))
    {
        IAudioSessionControl2_Release(&wrapper->IAudioSessionControl2_iface);
        return;
    }
    created->notification = notification;
    created->control = (IAudioSessionControl *)&wrapper->IAudioSessionControl2_iface;
    IAudioSessionNotification_AddRef(notification);

    if ((thread = CreateThread(NULL, 0, host_listener_created_thread, created, 0, NULL)))
        CloseHandle(thread);
    else
    {
        IAudioSessionControl_Release(created->control);
        IAudioSessionNotification_Release(notification);
        free(created);
    }
}

static CRITICAL_SECTION g_sessions_lock;
static CRITICAL_SECTION_DEBUG g_sessions_lock_debug =
{
    0, 0, &g_sessions_lock,
    { &g_sessions_lock_debug.ProcessLocksList, &g_sessions_lock_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": g_sessions_lock") }
};
static CRITICAL_SECTION g_sessions_lock = { &g_sessions_lock_debug, -1, 0, 0, 0, 0 };

void sessions_lock(void)
{
    EnterCriticalSection(&g_sessions_lock);
}

void sessions_unlock(void)
{
    LeaveCriticalSection(&g_sessions_lock);
}

struct session_enum
{
    IAudioSessionEnumerator IAudioSessionEnumerator_iface;
    IMMDevice *device;
    GUID *sessions;
    int session_count;
    LONG ref;
};

static struct session_enum *impl_from_IAudioSessionEnumerator(IAudioSessionEnumerator *iface)
{
    return CONTAINING_RECORD(iface, struct session_enum, IAudioSessionEnumerator_iface);
}

static HRESULT WINAPI enumerator_QueryInterface(IAudioSessionEnumerator *iface, REFIID riid, void **ppv)
{
    struct session_enum *enumerator = impl_from_IAudioSessionEnumerator(iface);

    TRACE("(%p)->(%s, %p)\n", enumerator, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IAudioSessionEnumerator))
        *ppv = &enumerator->IAudioSessionEnumerator_iface;
    else {
        WARN("Unknown iface %s.\n", debugstr_guid(riid));
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI enumerator_AddRef(IAudioSessionEnumerator *iface)
{
    struct session_enum *enumerator = impl_from_IAudioSessionEnumerator(iface);
    ULONG ref = InterlockedIncrement(&enumerator->ref);
    TRACE("(%p) new ref %lu\n", enumerator, ref);
    return ref;
}

static ULONG WINAPI enumerator_Release(IAudioSessionEnumerator *iface)
{
    struct session_enum *enumerator = impl_from_IAudioSessionEnumerator(iface);
    ULONG ref = InterlockedDecrement(&enumerator->ref);
    TRACE("(%p) new ref %lu\n", enumerator, ref);

    if (!ref)
    {
        IMMDevice_Release(enumerator->device);
        free(enumerator->sessions);
        free(enumerator);
    }

    return ref;
}

static HRESULT WINAPI enumerator_GetCount(IAudioSessionEnumerator *iface, int *count)
{
    struct session_enum *enumerator = impl_from_IAudioSessionEnumerator(iface);

    TRACE("%p -> %p.\n", iface, count);

    if (!count) return E_POINTER;
    *count = enumerator->session_count;
    return S_OK;
}

static HRESULT WINAPI enumerator_GetSession(IAudioSessionEnumerator *iface, int index, IAudioSessionControl **session)
{
    struct session_enum *enumerator = impl_from_IAudioSessionEnumerator(iface);
    struct audio_session_wrapper *session_wrapper;
    HRESULT hr;

    TRACE("%p -> %d %p.\n", iface, index, session);

    if (!session) return E_POINTER;
    if (index >= enumerator->session_count)
        return E_FAIL;

    *session = NULL;
    sessions_lock();
    hr = get_audio_session_wrapper(&enumerator->sessions[index], enumerator->device, &session_wrapper);
    sessions_unlock();
    if (FAILED(hr))
        return hr;
    *session = (IAudioSessionControl *)&session_wrapper->IAudioSessionControl2_iface;
    return S_OK;
}

static const IAudioSessionEnumeratorVtbl IAudioSessionEnumerator_vtbl =
{
    enumerator_QueryInterface,
    enumerator_AddRef,
    enumerator_Release,
    enumerator_GetCount,
    enumerator_GetSession,
};

static HRESULT create_session_enumerator(IMMDevice *device, IAudioSessionEnumerator **ppv)
{
    struct session_enum *enumerator;
    HRESULT hr;

    if (!(enumerator = calloc(1, sizeof(*enumerator))))
        return E_OUTOFMEMORY;

    if (is_voicemod_capture_endpoint(device)) ensure_host_listener_session(device);

    sessions_lock();
    hr = get_audio_sessions(device, &enumerator->sessions, &enumerator->session_count);
    sessions_unlock();
    if (FAILED(hr))
    {
        free(enumerator);
        return hr;
    }
    enumerator->IAudioSessionEnumerator_iface.lpVtbl = &IAudioSessionEnumerator_vtbl;
    IMMDevice_AddRef(device);
    enumerator->device = device;
    enumerator->ref = 1;
    *ppv = &enumerator->IAudioSessionEnumerator_iface;
    return S_OK;
}

struct session_mgr
{
    IAudioSessionManager2 IAudioSessionManager2_iface;
    IMMDevice *device;
    struct list notifications;
    LONG ref;
};

struct session_notification
{
    struct list entry;
    IAudioSessionNotification *iface;
};

static inline struct session_mgr *impl_from_IAudioSessionManager2(IAudioSessionManager2 *iface)
{
    return CONTAINING_RECORD(iface, struct session_mgr, IAudioSessionManager2_iface);
}

static HRESULT WINAPI ASM_QueryInterface(IAudioSessionManager2 *iface, REFIID riid, void **ppv)
{
    struct session_mgr *This = impl_from_IAudioSessionManager2(iface);
    TRACE("(%p)->(%s, %p)\n", This, debugstr_guid(riid), ppv);

    if (!ppv)
        return E_POINTER;

    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IAudioSessionManager) ||
        IsEqualIID(riid, &IID_IAudioSessionManager2))
        *ppv = &This->IAudioSessionManager2_iface;
    else {
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    IUnknown_AddRef((IUnknown *)*ppv);

    return S_OK;
}

static ULONG WINAPI ASM_AddRef(IAudioSessionManager2 *iface)
{
    struct session_mgr *This = impl_from_IAudioSessionManager2(iface);
    ULONG ref = InterlockedIncrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);
    return ref;
}

static ULONG WINAPI ASM_Release(IAudioSessionManager2 *iface)
{
    struct session_mgr *This = impl_from_IAudioSessionManager2(iface);
    ULONG ref = InterlockedDecrement(&This->ref);
    TRACE("(%p) new ref %lu\n", This, ref);

    if (!ref)
    {
        struct session_notification *notification, *next;

        LIST_FOR_EACH_ENTRY_SAFE(notification, next, &This->notifications,
                struct session_notification, entry)
        {
            list_remove(&notification->entry);
            IAudioSessionNotification_Release(notification->iface);
            free(notification);
        }
        free(This);
    }

    return ref;
}

static HRESULT WINAPI ASM_GetAudioSessionControl(IAudioSessionManager2 *iface,
                                                 const GUID *guid, DWORD flags,
                                                 IAudioSessionControl **out)
{
    struct session_mgr *This = impl_from_IAudioSessionManager2(iface);
    AudioSessionWrapper *wrapper;
    HRESULT hr;

    TRACE("(%p)->(%s, %lx, %p)\n", This, debugstr_guid(guid), flags, out);

    hr = get_audio_session_wrapper(guid, This->device, &wrapper);
    if (FAILED(hr))
        return hr;

    *out = (IAudioSessionControl*)&wrapper->IAudioSessionControl2_iface;

    return S_OK;
}

static HRESULT WINAPI ASM_GetSimpleAudioVolume(IAudioSessionManager2 *iface,
                                               const GUID *guid, DWORD flags,
                                               ISimpleAudioVolume **out)
{
    struct session_mgr *This = impl_from_IAudioSessionManager2(iface);
    AudioSessionWrapper *wrapper;
    HRESULT hr;

    TRACE("(%p)->(%s, %lx, %p)\n", This, debugstr_guid(guid), flags, out);

    hr = get_audio_session_wrapper(guid, This->device, &wrapper);
    if (FAILED(hr))
        return hr;

    *out = &wrapper->ISimpleAudioVolume_iface;

    return S_OK;
}

static HRESULT WINAPI ASM_GetSessionEnumerator(IAudioSessionManager2 *iface,
                                               IAudioSessionEnumerator **out)
{
    struct session_mgr *This = impl_from_IAudioSessionManager2(iface);

    TRACE("(%p)->(%p).\n", This, out);

    return create_session_enumerator(This->device, out);
}

static HRESULT WINAPI ASM_RegisterSessionNotification(IAudioSessionManager2 *iface,
                                                      IAudioSessionNotification *notification)
{
    struct session_mgr *This = impl_from_IAudioSessionManager2(iface);
    struct session_notification *entry;

    TRACE("(%p)->(%p)\n", This, notification);
    if (!notification) return E_POINTER;

    LIST_FOR_EACH_ENTRY(entry, &This->notifications, struct session_notification, entry)
        if (entry->iface == notification) return S_OK;

    if (!(entry = malloc(sizeof(*entry)))) return E_OUTOFMEMORY;
    IAudioSessionNotification_AddRef(notification);
    entry->iface = notification;
    list_add_tail(&This->notifications, &entry->entry);

    /* Session creation callbacks are not generated for Wine clients yet, but
     * retaining the callback and accepting registration matches the lifetime
     * contract and lets clients which use notifications opportunistically
     * initialize.
     *
     * Host applications record from the Voicemod bridge through PipeWire, so
     * Wine never sees their sessions.  Voicemod stops processing the
     * microphone when it believes nobody records from its virtual device;
     * report one permanent listener on that endpoint. */
    if (is_voicemod_capture_endpoint(This->device))
    {
        TRACE_(voicemod)("session notification %p registered on the Voicemod capture endpoint\n",
                notification);
        announce_host_listener(This->device, notification);
    }
    else TRACE_(voicemod)("session notification %p registered on device %p\n",
            notification, This->device);
    return S_OK;
}

static HRESULT WINAPI ASM_UnregisterSessionNotification(IAudioSessionManager2 *iface,
                                                        IAudioSessionNotification *notification)
{
    struct session_mgr *This = impl_from_IAudioSessionManager2(iface);
    struct session_notification *entry;

    TRACE("(%p)->(%p)\n", This, notification);
    if (!notification) return E_POINTER;

    LIST_FOR_EACH_ENTRY(entry, &This->notifications, struct session_notification, entry)
    {
        if (entry->iface != notification) continue;
        list_remove(&entry->entry);
        IAudioSessionNotification_Release(entry->iface);
        free(entry);
        return S_OK;
    }
    return E_NOTFOUND;
}

static HRESULT WINAPI ASM_RegisterDuckNotification(IAudioSessionManager2 *iface,
                                                   const WCHAR *session_id,
                                                   IAudioVolumeDuckNotification *notification)
{
    struct session_mgr *This = impl_from_IAudioSessionManager2(iface);
    FIXME("(%p)->(%s, %p) - stub\n", This, debugstr_w(session_id), notification);
    return E_NOTIMPL;
}

static HRESULT WINAPI ASM_UnregisterDuckNotification(IAudioSessionManager2 *iface,
                                                     IAudioVolumeDuckNotification *notification)
{
    struct session_mgr *This = impl_from_IAudioSessionManager2(iface);
    FIXME("(%p)->(%p) - stub\n", This, notification);
    return E_NOTIMPL;
}

static const IAudioSessionManager2Vtbl AudioSessionManager2_Vtbl =
{
    ASM_QueryInterface,
    ASM_AddRef,
    ASM_Release,
    ASM_GetAudioSessionControl,
    ASM_GetSimpleAudioVolume,
    ASM_GetSessionEnumerator,
    ASM_RegisterSessionNotification,
    ASM_UnregisterSessionNotification,
    ASM_RegisterDuckNotification,
    ASM_UnregisterDuckNotification
};

HRESULT AudioSessionManager_Create(IMMDevice *device, IAudioSessionManager2 **ppv)
{
    struct session_mgr *This;

    This = calloc(1, sizeof(*This));
    if (!This)
        return E_OUTOFMEMORY;

    This->IAudioSessionManager2_iface.lpVtbl = &AudioSessionManager2_Vtbl;
    This->device = device;
    list_init(&This->notifications);
    This->ref = 1;

    *ppv = &This->IAudioSessionManager2_iface;

    return S_OK;
}
