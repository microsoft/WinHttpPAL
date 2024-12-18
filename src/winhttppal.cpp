/***
 * Copyright (C) Microsoft. All rights reserved.
 * Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
 *
 * =+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
 *
 * HTTP Library: Client-side APIs.
 *
 * This file contains WinHttp client implementation using CURL.
 *
 * For the latest on this and related APIs, please see: https://github.com/microsoft/WinHttpPAL
 *
 * =-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
 ****/

#include <iostream>
#include <sstream>
#include <vector>
#include <mutex>
#include <thread>
#include <algorithm>
#include <map>
#include <condition_variable>
#include <future>
#include <queue>
#include <chrono>
#include <ctime>
#include <string.h>
#include <time.h>
#include <regex>
#include <cstdlib>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <memory>
#include <iomanip>
#include <chrono>
#include <openssl/err.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <assert.h>

#ifdef UNICODE
#include <codecvt>
#endif

#ifdef WIN32
#define localtime_r(_Time, _Tm) localtime_s(_Tm, _Time)
#endif

#define _WINHTTP_INTERNAL_
#ifdef _MSC_VER
#include <windows.h>
#include "winhttp.h"
#else
#include <pthread.h>
#include <unistd.h>
#include "winhttppal.h"
#endif

#include "winhttppal_imp.h"

#ifndef WINHTTP_CURL_MAX_WRITE_SIZE
#define WINHTTP_CURL_MAX_WRITE_SIZE CURL_MAX_WRITE_SIZE
#endif

class WinHttpSessionImp;
class WinHttpRequestImp;

#ifdef min
#define MIN min
#define MAX max
#else
#define MIN std::min
#define MAX std::max
#endif

#ifdef UNICODE
#define WCTLEN wcslen
#define TSTRINGSTREAM std::wstringstream
#define TSTRING std::wstring
#define STRING_LITERAL "%S"
#define TO_STRING std::to_wstring
#define TREGEX std::wregex
#define TREGEX_SEARCH std::regex_search
#define TREGEX_MATCH std::wsmatch
#else
#define WCTLEN strlen
#define TSTRINGSTREAM std::stringstream
#define TEXT(T) T
#define TSTRING std::string
#define STRING_LITERAL "%s"
#define TO_STRING std::to_string
#define TREGEX std::regex
#define TREGEX_SEARCH std::regex_search
#define TREGEX_MATCH std::smatch
#endif

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

static const std::map<DWORD, TSTRING> StatusCodeMap = {
{ 100, TEXT("Continue") },
{ 101, TEXT("Switching Protocols") },
{ 200, TEXT("OK") },
{ 201, TEXT("Created") },
{ 202, TEXT("Accepted") },
{ 203, TEXT("Non-Authoritative Information") },
{ 204, TEXT("No Content") },
{ 205, TEXT("Reset Content") },
{ 206, TEXT("Partial Content") },
{ 300, TEXT("Multiple Choices") },
{ 301, TEXT("Moved Permanently") },
{ 302, TEXT("Found") },
{ 303, TEXT("See Other") },
{ 304, TEXT("Not Modified") },
{ 305, TEXT("Use Proxy") },
{ 306, TEXT("(Unused)") },
{ 307, TEXT("Temporary Redirect") },
{ 400, TEXT("Bad Request") },
{ 401, TEXT("Unauthorized") },
{ 402, TEXT("Payment Required") },
{ 403, TEXT("Forbidden") },
{ 404, TEXT("Not Found") },
{ 405, TEXT("Method Not Allowed") },
{ 406, TEXT("Not Acceptable") },
{ 407, TEXT("Proxy Authentication Required") },
{ 408, TEXT("Request Timeout") },
{ 409, TEXT("Conflict") },
{ 410, TEXT("Gone") },
{ 411, TEXT("Length Required") },
{ 412, TEXT("Precondition Failed") },
{ 413, TEXT("Request Entity Too Large") },
{ 414, TEXT("Request-URI Too Long") },
{ 415, TEXT("Unsupported Media Type") },
{ 416, TEXT("Requested Range Not Satisfiable") },
{ 417, TEXT("Expectation Failed") },
{ 500, TEXT("Internal Server Error") },
{ 501, TEXT("Not Implemented") },
{ 502, TEXT("Bad Gateway") },
{ 503, TEXT("Service Unavailable") },
{ 504, TEXT("Gateway Timeout") },
{ 505, TEXT("HTTP Version Not Supported") },
};

enum
{
    WINHTTP_CLASS_SESSION,
    WINHTTP_CLASS_REQUEST,
    WINHTTP_CLASS_IMP,
};

static int winhttp_tracing = false;
static int winhttp_tracing_verbose = false;

#ifdef _MSC_VER
int gettimeofday(struct timeval * tp, struct timezone * tzp);
#endif

static std::mutex trcmtx;

#ifndef _MSC_VER
static void TRACE_INTERNAL(const char *fmt, ...) __attribute__ ((format (printf, 1, 2)));
#endif

static TSTRING FindRegex(const TSTRING &subject, const TSTRING &regstr);
static std::vector<std::string> FindRegexA(const std::string &str, const std::string &regstr);

static void TRACE_INTERNAL(const char *fmt, ...)
{
    std::lock_guard<std::mutex> lck(trcmtx);
    va_list args;
    va_start(args, fmt);

    tm localTime;
    std::chrono::system_clock::time_point t = std::chrono::system_clock::now();
    time_t now = std::chrono::system_clock::to_time_t(t);
    localtime_r(&now, &localTime);

    const std::chrono::duration<double> tse = t.time_since_epoch();
    std::chrono::seconds::rep milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(tse).count() % 1000;

    char szBuffer[512];
    vsnprintf(szBuffer, sizeof szBuffer - 1, fmt, args);
    szBuffer[sizeof(szBuffer) / sizeof(szBuffer[0]) - 1] = '\0';

    std::cout << (1900 + localTime.tm_year) << '-'
        << std::setfill('0') << std::setw(2) << (localTime.tm_mon + 1) << '-'
        << std::setfill('0') << std::setw(2) << localTime.tm_mday << ' '
        << std::setfill('0') << std::setw(2) << localTime.tm_hour << ':'
        << std::setfill('0') << std::setw(2) << localTime.tm_min << ':'
        << std::setfill('0') << std::setw(2) << localTime.tm_sec << '.'
        << std::setfill('0') << std::setw(3) << milliseconds << " " << szBuffer;

    va_end(args);
}

#define TRACE(fmt, ...) \
            do { if (winhttp_tracing) TRACE_INTERNAL(fmt, __VA_ARGS__); } while (0)

#define TRACE_VERBOSE(fmt, ...) \
            do { if (winhttp_tracing_verbose) TRACE_INTERNAL(fmt, __VA_ARGS__); } while (0)

#define CURL_BAILOUT_ONERROR(res, request, retval)                                         \
    if ((res) != CURLE_OK)                                                                 \
    {                                                                                      \
        TRACE("%-35s:%-8d:%-16p res:%d\n", __func__, __LINE__, (void*)(request), (res));   \
        return (retval);                                                                   \
    }

#define MUTEX_TYPE                              std::mutex
#define MUTEX_SETUP(x)
#define MUTEX_CLEANUP(x)
#define MUTEX_LOCK(x)                           x.lock()
#define MUTEX_UNLOCK(x)                         x.unlock()

class WinHttpBase;
class WinHttpConnectImp;

#if OPENSSL_VERSION_NUMBER < 0x10100000L
/* This array will store all of the mutexes available to OpenSSL. */
static MUTEX_TYPE *mutex_buf = NULL;

static void locking_function(int mode, int n, const char *file, int line)
{
    if (mode & CRYPTO_LOCK)
        MUTEX_LOCK(mutex_buf[n]);
    else
        MUTEX_UNLOCK(mutex_buf[n]);
}

static unsigned long id_function()
{
    return static_cast<unsigned long>(THREAD_ID);
}
#endif

static int thread_setup()
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    int i;

    mutex_buf = new MUTEX_TYPE[CRYPTO_num_locks()];
    if (!mutex_buf)
        return 0;
    for (i = 0; i < CRYPTO_num_locks(); i++)
        MUTEX_SETUP(mutex_buf[i]);
    CRYPTO_set_id_callback(id_function);
    CRYPTO_set_locking_callback(locking_function);
#endif
    return 1;
}

static int thread_cleanup()
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    int i;

    if (!mutex_buf)
        return 0;
    CRYPTO_set_id_callback(NULL);
    CRYPTO_set_locking_callback(NULL);
    for (i = 0; i < CRYPTO_num_locks(); i++)
        MUTEX_CLEANUP(mutex_buf[i]);
    delete [] mutex_buf;
    mutex_buf = NULL;
#endif
    return 1;
}

static void ConvertCstrAssign(const TCHAR *lpstr, size_t cLen, std::string &target)
{
#ifdef UNICODE
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    target = conv.to_bytes(std::wstring(lpstr, cLen));
#else
    target.assign(lpstr, cLen);
#endif
}

static std::vector<std::string> Split(std::string &str, char delimiter) {
    std::vector<std::string> internal;
    std::stringstream ss(str); // Turn the string into a stream.
    std::string tok;

    while (getline(ss, tok, delimiter)) {
        internal.push_back(tok);
    }

    return internal;
}

static BOOL SizeCheck(LPVOID lpBuffer, LPDWORD lpdwBufferLength, DWORD Required)
{
    if (!lpBuffer)
    {
        if (lpdwBufferLength)
            *lpdwBufferLength = Required;
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    if (lpdwBufferLength && (*lpdwBufferLength < Required)) {
        *lpdwBufferLength = Required;

        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }

    if ((Required == 0) || (lpdwBufferLength && (*lpdwBufferLength == 0)))
        return FALSE;

    if (!lpBuffer)
        return FALSE;

    if (lpdwBufferLength && (*lpdwBufferLength < Required))
        return FALSE;

    return TRUE;
}

static DWORD ConvertSecurityProtocol(DWORD offered)
{
    DWORD min = 0;
    DWORD max = 0;

    if (offered & WINHTTP_FLAG_SECURE_PROTOCOL_SSL2) {
        min = CURL_SSLVERSION_SSLv2;
    }
    else if (offered & WINHTTP_FLAG_SECURE_PROTOCOL_SSL3) {
        min = CURL_SSLVERSION_SSLv3;
    }
    else
        min = CURL_SSLVERSION_DEFAULT;

    if (offered & WINHTTP_FLAG_SECURE_PROTOCOL_TLS1) {
        min = CURL_SSLVERSION_TLSv1_0;
    }
    else if (offered & WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1) {
        min = CURL_SSLVERSION_TLSv1_1;
    }
    else if (offered & WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2) {
        min = CURL_SSLVERSION_TLSv1_2;
    }

#if (LIBCURL_VERSION_MAJOR>=7) && (LIBCURL_VERSION_MINOR >= 61)
    if (offered & WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2) {
        max = CURL_SSLVERSION_MAX_TLSv1_2;
    }
    else if (offered & WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1) {
        max = CURL_SSLVERSION_MAX_TLSv1_1;
    }
    else if (offered & WINHTTP_FLAG_SECURE_PROTOCOL_TLS1) {
        max = CURL_SSLVERSION_MAX_TLSv1_0;
    }
    else if (offered & WINHTTP_FLAG_SECURE_PROTOCOL_SSL3) {
        max = CURL_SSLVERSION_SSLv3;
    }
    else if (offered & WINHTTP_FLAG_SECURE_PROTOCOL_SSL2) {
        max = CURL_SSLVERSION_SSLv2;
    }
    else
        max = CURL_SSLVERSION_MAX_DEFAULT;
#endif

    return min | max;
}

template <class T, typename prmtype>
static BOOL CallMemberFunction(WinHttpBase *base, std::function<BOOL(T*, prmtype *data)> fn, LPVOID    lpBuffer)
{
    T *obj;

    if (!lpBuffer)
        return FALSE;

    if ((obj = dynamic_cast<T *>(base)))
    {
        if (fn(obj, static_cast<prmtype*>(lpBuffer)))
            return TRUE;
    }
    return FALSE;
}

WinHttpSessionImp *GetImp(WinHttpBase *base)
{
    WinHttpConnectImp *connect;
    WinHttpSessionImp *session;
    WinHttpRequestImp *request;

    if ((connect = dynamic_cast<WinHttpConnectImp *>(base)))
    {
        session = connect->GetHandle();
    }
    else if ((request = dynamic_cast<WinHttpRequestImp *>(base)))
    {
        connect = request->GetSession();
        if (!connect)
            return NULL;
        session = connect->GetHandle();
    }
    else
    {
        session = dynamic_cast<WinHttpSessionImp *>(base);
    }

    return session;
}

static void QueueBufferRequest(std::vector<BufferRequest> &store, LPVOID buffer, size_t length)
{
    BufferRequest shr;
    shr.m_Buffer = buffer;
    shr.m_Length = length;
    shr.m_Used = 0;
    store.push_back(shr);
}

static BufferRequest GetBufferRequest(std::vector<BufferRequest> &store)
{
    if (store.empty())
        return BufferRequest();
    BufferRequest shr = store.front();
    store.erase(store.begin());
    return shr;
}

static BufferRequest &PeekBufferRequest(std::vector<BufferRequest> &store)
{
    return store.front();
}

EnvInit::EnvInit()
{
    if (const char* env_p = std::getenv("WINHTTP_PAL_DEBUG"))
        winhttp_tracing = std::stoi(std::string(env_p));

    if (const char* env_p = std::getenv("WINHTTP_PAL_DEBUG_VERBOSE"))
        winhttp_tracing_verbose = std::stoi(std::string(env_p));
}

static EnvInit envinit;

THREADRETURN UserCallbackContainer::UserCallbackThreadFunction(LPVOID lpThreadParameter)
{
    UserCallbackContainer *cbContainer = static_cast<UserCallbackContainer *>(lpThreadParameter);

    while (true)
    {
        {
            std::unique_lock<std::mutex> GetEventHndlMtx(cbContainer->m_hEventMtx);
            while (!cbContainer->m_EventCounter)
                cbContainer->m_hEvent.wait(GetEventHndlMtx);
            GetEventHndlMtx.unlock();

            cbContainer->m_EventCounter--;
        }

        if (cbContainer->GetClosing())
        {
            TRACE("%s", "exiting\n");
            break;
        }

        while (true)
        {
            cbContainer->m_MapMutex.lock();
            UserCallbackContext *ctx = NULL;
            ctx = cbContainer->GetNext();
            cbContainer->m_MapMutex.unlock();
            if (!ctx)
                break;

            void *statusInformation = NULL;

            if (ctx->GetStatusInformationValid())
                statusInformation = ctx->GetStatusInformation();

            if (!ctx->GetRequestRef()->GetClosed() && ctx->GetCb()) {
                TRACE_VERBOSE("%-35s:%-8d:%-16p ctx = %p cb = %p ctx->GetUserdata() = %p dwInternetStatus:0x%lx statusInformation=%p refcount:%lu\n",
                    __func__, __LINE__, (void*)ctx->GetRequest(), reinterpret_cast<void*>(ctx), reinterpret_cast<void*>(ctx->GetCb()),
                    ctx->GetUserdata(), ctx->GetInternetStatus(), statusInformation, ctx->GetRequestRef().use_count());

                ctx->GetCb()(ctx->GetRequest(),
                    (DWORD_PTR)(ctx->GetUserdata()),
                    ctx->GetInternetStatus(),
                    statusInformation,
                    ctx->GetStatusInformationLength());

                TRACE_VERBOSE("%-35s:%-8d:%-16p ctx = %p cb = %p ctx->GetUserdata() = %p dwInternetStatus:0x%lx statusInformation=%p refcount:%lu\n",
                    __func__, __LINE__, (void*)ctx->GetRequest(), reinterpret_cast<void*>(ctx), reinterpret_cast<void*>(ctx->GetCb()),
                    ctx->GetUserdata(), ctx->GetInternetStatus(), statusInformation, ctx->GetRequestRef().use_count());
            }
            ctx->GetRequestCompletionCb()(ctx->GetRequestRef(), ctx->GetInternetStatus());
            delete ctx;
        }
    }

#if OPENSSL_VERSION_NUMBER >= 0x10000000L && OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_remove_thread_state(NULL);
#elif OPENSSL_VERSION_NUMBER < 0x10000000L
    ERR_remove_state(0);
#endif

    return 0;
}

UserCallbackContext* UserCallbackContainer::GetNext()
{
    UserCallbackContext *ctx = NULL;
    // show content:
    if (!GetCallbackQueue().empty())
    {
        ctx = GetCallbackQueue().front();
        GetCallbackQueue().erase(GetCallbackQueue().begin());
    }

    return ctx;
}

BOOL UserCallbackContainer::Queue(UserCallbackContext *ctx)
{
    if (!ctx)
        return FALSE;

    {
        std::lock_guard<std::mutex> lck(m_MapMutex);

        TRACE_VERBOSE("%-35s:%-8d:%-16p ctx = %p cb = %p userdata = %p dwInternetStatus = %p\n",
            __func__, __LINE__, (void*)ctx->GetRequest(), reinterpret_cast<void*>(ctx), reinterpret_cast<void*>(ctx->GetCb()),
            ctx->GetUserdata(), ctx->GetStatusInformation());
        GetCallbackQueue().push_back(ctx);
    }
    {
        std::lock_guard<std::mutex> lck(m_hEventMtx);
        m_EventCounter++;
        m_hEvent.notify_all();
    }
    return TRUE;
}

void UserCallbackContainer::DrainQueue()
{
    while (true)
    {
        m_MapMutex.lock();
        UserCallbackContext *ctx = GetNext();
        m_MapMutex.unlock();

        if (!ctx)
            break;

        delete ctx;
    }
}

CURL *ComContainer::AllocCURL()
{
    CURL *ptr;

    m_MultiMutex.lock();
    ptr = curl_easy_init();
    m_MultiMutex.unlock();

    return ptr;
}

void ComContainer::FreeCURL(CURL *ptr)
{
    m_MultiMutex.lock();
    curl_easy_cleanup(ptr);
    m_MultiMutex.unlock();
}

ComContainer &ComContainer::GetInstance()
{
    static ComContainer *the_instance = new ComContainer();
    return *the_instance;
}

void ComContainer::ResumeTransfer(CURL *handle, int bitmask)
{
    int still_running;

    m_MultiMutex.lock();
    curl_easy_pause(handle, bitmask);
    curl_multi_perform(m_curlm, &still_running);
    m_MultiMutex.unlock();
}

BOOL ComContainer::AddHandle(std::shared_ptr<WinHttpRequestImp> &srequest, CURL *handle)
{
    CURLMcode mres = CURLM_OK;

    m_MultiMutex.lock();
    if (std::find(m_ActiveCurl.begin(), m_ActiveCurl.end(), handle) != m_ActiveCurl.end()) {
        mres = curl_multi_remove_handle(m_curlm, handle);
        m_ActiveCurl.erase(std::remove(m_ActiveCurl.begin(), m_ActiveCurl.end(), handle), m_ActiveCurl.end());
    }
    if (std::find(m_ActiveRequests.begin(), m_ActiveRequests.end(), srequest) != m_ActiveRequests.end()) {
        m_ActiveRequests.erase(std::remove(m_ActiveRequests.begin(), m_ActiveRequests.end(), srequest), m_ActiveRequests.end());
    }
    m_MultiMutex.unlock();
    if (mres != CURLM_OK)
    {
        TRACE("curl_multi_remove_handle() failed: %s\n", curl_multi_strerror(mres));
        return FALSE;
    }

    m_MultiMutex.lock();
    mres = curl_multi_add_handle(m_curlm, handle);
    m_ActiveCurl.push_back(handle);
    m_ActiveRequests.push_back(srequest);
    m_MultiMutex.unlock();
    if (mres != CURLM_OK)
    {
        TRACE("curl_multi_add_handle() failed: %s\n", curl_multi_strerror(mres));
        return FALSE;
    }

    return TRUE;
}

BOOL ComContainer::RemoveHandle(std::shared_ptr<WinHttpRequestImp> &srequest, CURL *handle, bool clearPrivate)
{
    CURLMcode mres;

    m_MultiMutex.lock();
    mres = curl_multi_remove_handle(m_curlm, handle);

    if (clearPrivate)
        curl_easy_getinfo(handle, CURLINFO_PRIVATE, NULL);

    m_ActiveCurl.erase(std::remove(m_ActiveCurl.begin(), m_ActiveCurl.end(), handle), m_ActiveCurl.end());
    m_ActiveRequests.erase(std::remove(m_ActiveRequests.begin(), m_ActiveRequests.end(), srequest), m_ActiveRequests.end());
    m_MultiMutex.unlock();
    if (mres != CURLM_OK)
    {
        TRACE("curl_multi_add_handle() failed: %s\n", curl_multi_strerror(mres));
        return FALSE;
    }

    return TRUE;
}

void ComContainer::KickStart()
{
    std::lock_guard<std::mutex> lck(m_hAsyncEventMtx);
    m_hAsyncEventCounter++;
    m_hAsyncEvent.notify_all();
}

ComContainer::ComContainer(): m_hAsyncEventCounter(0)
{
    curl_global_init(CURL_GLOBAL_ALL);
    thread_setup();

    m_curlm = curl_multi_init();

    DWORD thread_id;
    m_hAsyncThread = CREATETHREAD(
        AsyncThreadFunction,       // thread function name
        this, &thread_id);          // argument to thread function
}

ComContainer::~ComContainer()
{
    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)this);
    m_closing = true;
    {
        std::lock_guard<std::mutex> lck(m_hAsyncEventMtx);
        m_hAsyncEventCounter++;
        m_hAsyncEvent.notify_all();
    }
    THREADJOIN(m_hAsyncThread);
    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)this);

    curl_multi_cleanup(m_curlm);
    curl_global_cleanup();
    thread_cleanup();
}

template<class T>
void WinHttpHandleContainer<T>::UnRegister(T *val)
{
    typedef std::shared_ptr<T> t_shared;
    typename std::vector<t_shared>::iterator findit;
    bool found = false;
    std::lock_guard<std::mutex> lck(m_ActiveRequestMtx);

    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)val);

    for (auto it = m_ActiveRequests.begin(); it != m_ActiveRequests.end(); ++it)
    {
        auto v = (*it);
        if (v.get() == val)
        {
            findit = it;
            found = true;
            break;
        }
    }
    if (found)
        m_ActiveRequests.erase(findit);
}

template<class T>
bool WinHttpHandleContainer<T>::IsRegistered(T *val)
{
    bool found = false;
    std::lock_guard<std::mutex> lck(m_ActiveRequestMtx);

    for (auto it = m_ActiveRequests.begin(); it != m_ActiveRequests.end(); ++it)
    {
        auto v = (*it);
        if (v.get() == val)
        {
            found = true;
            break;
        }
    }
    TRACE("%-35s:%-8d:%-16p found:%d\n", __func__, __LINE__, (void*)val, found);

    return found;
}

template<class T>
void WinHttpHandleContainer<T>::Register(std::shared_ptr<T> rqst)
{
    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)(rqst.get()));
    std::lock_guard<std::mutex> lck(m_ActiveRequestMtx);
    m_ActiveRequests.push_back(rqst);
}

WinHttpSessionImp::~WinHttpSessionImp()
{
    TRACE("%-35s:%-8d:%-16p sesion\n", __func__, __LINE__, (void*)this);
    SetCallback(NULL, 0);
    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)this);
}

THREADRETURN ComContainer::AsyncThreadFunction(LPVOID lpThreadParameter)
{
    ComContainer *comContainer = static_cast<ComContainer *>(lpThreadParameter);

    while (true)
    {
        {
            std::unique_lock<std::mutex> getAsyncEventHndlMtx(comContainer->m_hAsyncEventMtx);
            while (!comContainer->m_hAsyncEventCounter)
                comContainer->m_hAsyncEvent.wait(getAsyncEventHndlMtx);
            getAsyncEventHndlMtx.unlock();

            comContainer->m_hAsyncEventCounter--;
        }

        int still_running = 1;

        while (still_running && !comContainer->GetThreadClosing())
        {
            WinHttpRequestImp *request = NULL;

            comContainer->QueryData(&still_running);

            //TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)request);
            struct CURLMsg *m;

            /* call curl_multi_perform or curl_multi_socket_action first, then loop
               through and check if there are any transfers that have completed */

            do {
                int msgq = 0;
                request = NULL;
                std::shared_ptr<WinHttpRequestImp> srequest;

                comContainer->m_MultiMutex.lock();
                m = curl_multi_info_read(comContainer->m_curlm, &msgq);
                if (m)
                    curl_easy_getinfo(m->easy_handle, CURLINFO_PRIVATE, &request);
                if (request)
                {
                    srequest = request->shared_from_this();
                }
                comContainer->m_MultiMutex.unlock();

                if (m && (m->msg == CURLMSG_DONE) && request && srequest) {
                    WINHTTP_ASYNC_RESULT result = { 0, 0 };
                    DWORD dwInternetStatus;

                    TRACE("%-35s:%-8d:%-16p type:%s result:%d\n", __func__, __LINE__, (void*)request, request->GetType().c_str(), m->data.result);
                    request->GetCompletionCode() = m->data.result;

                    if (m->data.result == CURLE_OK)
                    {
                        if (request->HandleQueryDataNotifications(srequest, 0))
                        {
                            TRACE("%-35s:%-8d:%-16p GetQueryDataEvent().notify_all\n", __func__, __LINE__, (void*)request);
                        }
                        {
                            std::lock_guard<std::mutex> lck(request->GetQueryDataEventMtx());
                            request->GetQueryDataEventState() = true;
                        }
                        request->HandleQueryDataNotifications(srequest, 0);
                    }

                    request->GetBodyStringMutex().lock();

                    request->GetCompletionStatus() = true;

                    if (m->data.result == CURLE_OK)
                    {
                        void *ptr = request->GetResponseString().data();
                        size_t available = request->GetResponseString().size();
                        size_t totalread = 0;
                        request->ConsumeIncoming(srequest, ptr, available, totalread);
                        if (totalread)
                        {
                            TRACE("%-35s:%-8d:%-16p consumed length:%lu\n", __func__, __LINE__, (void*)srequest.get(), totalread);
                        }
                        request->FlushIncoming(srequest);
                    }
                    else if (m->data.result == CURLE_OPERATION_TIMEDOUT)
                    {
                        result.dwError = ERROR_WINHTTP_TIMEOUT;
                        dwInternetStatus = WINHTTP_CALLBACK_STATUS_REQUEST_ERROR;
                        request->AsyncQueue(srequest, dwInternetStatus, 0, &result, sizeof(result), true);
                        TRACE("%-35s:%-8d:%-16p request done type = %s CURLE_OPERATION_TIMEDOUT\n",
                              __func__, __LINE__, (void*)request, request->GetType().c_str());
                    }
                    else
                    {
                        result.dwError = ERROR_WINHTTP_OPERATION_CANCELLED;
                        dwInternetStatus = WINHTTP_CALLBACK_STATUS_REQUEST_ERROR;
                        TRACE("%-35s:%-8d:%-16p unknown async request done m->data.result = %d\n",
                              __func__, __LINE__, (void*)request, m->data.result);
#ifdef _DEBUG
                        assert(0);
#endif
                        request->AsyncQueue(srequest, dwInternetStatus, 0, &result, sizeof(result), true);
                    }

                    request->GetBodyStringMutex().unlock();

                } else if (m && (m->msg != CURLMSG_DONE)) {
                    TRACE("%-35s:%-8d:%-16p unknown async request done\n", __func__, __LINE__, (void*)request);
                    DWORD dwInternetStatus;
                    WINHTTP_ASYNC_RESULT result = { 0, 0 };
                    result.dwError = ERROR_WINHTTP_OPERATION_CANCELLED;
                    dwInternetStatus = WINHTTP_CALLBACK_STATUS_REQUEST_ERROR;
#ifdef _DEBUG
                    assert(0);
#endif
                    if (request)
                        request->AsyncQueue(srequest, dwInternetStatus, 0, &result, sizeof(result), true);
                }

                if (request)
                {
                    comContainer->RemoveHandle(srequest, request->GetCurl(), true);
                    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)request);
                }
            } while (m);
        }
        if (comContainer->GetThreadClosing())
        {
            TRACE("%s:%d exiting\n", __func__, __LINE__);
            break;
        }
    }

#if OPENSSL_VERSION_NUMBER >= 0x10000000L && OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_remove_thread_state(NULL);
#elif OPENSSL_VERSION_NUMBER < 0x10000000L
    ERR_remove_state(0);
#endif
    return 0;
}

int ComContainer::QueryData(int *still_running)
{
    if (!still_running)
        return 0;

    int rc = 0;
    struct timeval timeout;

    fd_set fdread;
    fd_set fdwrite;
    fd_set fdexcep;
    int maxfd = -1;

    long curl_timeo = -1;

    FD_ZERO(&fdread);
    FD_ZERO(&fdwrite);
    FD_ZERO(&fdexcep);

    /* set a suitable timeout to play around with */
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    m_MultiMutex.lock();
    curl_multi_timeout(m_curlm, &curl_timeo);
    m_MultiMutex.unlock();
    if (curl_timeo < 0)
        /* no set timeout, use a default */
        curl_timeo = 10000;

    if (curl_timeo > 0) {
        timeout.tv_sec = curl_timeo / 1000;
        if (timeout.tv_sec > 1)
            timeout.tv_sec = 1;
        else
            timeout.tv_usec = (curl_timeo % 1000) * 1000;
    }

    /* get file descriptors from the transfers */
    m_MultiMutex.lock();
    rc = curl_multi_fdset(m_curlm, &fdread, &fdwrite, &fdexcep, &maxfd);
    m_MultiMutex.unlock();

    if ((maxfd == -1) || (rc != CURLM_OK)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    else
        rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &timeout);

    switch (rc) {
    case -1:
        /* select error */
        *still_running = 0;
        TRACE("%s\n", "select() returns error, this is badness\n");
        break;
    case 0:
    default:
        /* timeout or readable/writable sockets */
        m_MultiMutex.lock();
        curl_multi_perform(m_curlm, still_running);
        m_MultiMutex.unlock();
        break;
    }

    return rc;
}

THREADRETURN WinHttpRequestImp::UploadThreadFunction(THREADPARAM lpThreadParameter)
{
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(lpThreadParameter);
    if (!request)
        return NULL;

    CURL *curl = request->GetCurl();
    CURLcode res;

    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)request);
    /* Perform the request, res will get the return code */
    res = curl_easy_perform(curl);
    /* Check for errors */
    CURL_BAILOUT_ONERROR(res, request, NULL);

    TRACE("%-35s:%-8d:%-16p res:%d\n", __func__, __LINE__, (void*)request, res);
    request->GetUploadThreadExitStatus() = true;

#if OPENSSL_VERSION_NUMBER >= 0x10000000L && OPENSSL_VERSION_NUMBER < 0x10100000L
    ERR_remove_thread_state(NULL);
#elif OPENSSL_VERSION_NUMBER < 0x10000000L
    ERR_remove_state(0);
#endif
    return 0;
}

BOOL WinHttpRequestImp::SetProxy(std::vector<std::string> &proxies)
{
    std::vector<std::string>::iterator it;

    for (it = proxies.begin(); it != proxies.end(); it++)
    {
        std::string &urlstr = (*it);
        CURLcode res;

        res = curl_easy_setopt(GetCurl(), CURLOPT_PROXY, urlstr.c_str());
        CURL_BAILOUT_ONERROR(res, this, FALSE);
    }

    return TRUE;
}

BOOL WinHttpRequestImp::SetServer(std::string &ServerName, int nServerPort)
{
    CURLcode res;

    res = curl_easy_setopt(GetCurl(), CURLOPT_URL, ServerName.c_str());
    CURL_BAILOUT_ONERROR(res, this, FALSE);

    res = curl_easy_setopt(GetCurl(), CURLOPT_PORT, nServerPort);
    CURL_BAILOUT_ONERROR(res, this, FALSE);

    return TRUE;
}

void WinHttpRequestImp::HandleReceiveNotifications(std::shared_ptr<WinHttpRequestImp> &srequest)
{
    bool expected = true;

    bool result = std::atomic_compare_exchange_strong(&GetReceiveResponsePending(), &expected, false);
    if (result)
    {
        bool redirectPending;
        {
            expected = true;
            redirectPending = GetRedirectPending();
            TRACE("%-35s:%-8d:%-16p redirectPending = %d ResponseCallbackSendCounter = %d\n", __func__, __LINE__, (void*)this,
                    redirectPending, (int)ResponseCallbackSendCounter());
        }
        GetReceiveResponseMutex().lock();
        if (redirectPending)
        {
            if (ResponseCallbackSendCounter() == 0)
            {
                TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_RECEIVING_RESPONSE\n", __func__, __LINE__, (void*)this);
                AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_RECEIVING_RESPONSE, 0, NULL, 0, false);
                ResponseCallbackSendCounter()++;
            }
            if (ResponseCallbackSendCounter() == 1)
            {
                TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED\n", __func__, __LINE__, (void*)this);
                AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED, GetHeaderString().length(), NULL, 0, false);
                ResponseCallbackSendCounter()++;
            }
            AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_REDIRECT, 0, NULL, 0, false);
            ResponseCallbackSendCounter()++;
        }
        if (ResponseCallbackSendCounter() == (0 + redirectPending * 3))
        {
            TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_RECEIVING_RESPONSE\n", __func__, __LINE__, (void*)this);
            AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_RECEIVING_RESPONSE, 0, NULL, 0, false);
            ResponseCallbackSendCounter()++;
        }
        if (ResponseCallbackSendCounter() == (1 + redirectPending * 3))
        {
            TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED\n", __func__, __LINE__, (void*)this);
            AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED, GetHeaderString().length(), NULL, 0, false);
            ResponseCallbackSendCounter()++;
        }
        if ((ResponseCallbackSendCounter() == (2 + redirectPending * 3)) && (GetCompletionCode() == CURLE_OK))
        {
            TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE\n", __func__, __LINE__, (void*)this);
            SetHeaderReceiveComplete();
            AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, 0, NULL, 0, false);
            ResponseCallbackSendCounter()++;
        }
        GetReceiveResponseMutex().unlock();
    }
}

size_t WinHttpRequestImp::WriteHeaderFunction(void *ptr, size_t size, size_t nmemb, void* rqst) {
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(rqst);
    if (!request)
        return 0;

    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return size * nmemb;
    bool EofHeaders = false;

    TRACE("%-35s:%-8d:%-16p %zu\n", __func__, __LINE__, (void*)request, size * nmemb);
    {
        std::lock_guard<std::mutex> lck(request->GetHeaderStringMutex());
        request->GetHeaderString().append(static_cast<char*>(ptr), size * nmemb);

        if (request->GetHeaderString().find("\r\n\r\n") != std::string::npos)
            EofHeaders = true;
        if (request->GetHeaderString().find("\n\n") != std::string::npos)
            EofHeaders = true;
    }
    if (EofHeaders && request->GetAsync())
    {
        std::string regstr;
        DWORD retValue = 501;

        TRACE_VERBOSE("%-35s:%-8d:%-16p Header string:%s\n", __func__, __LINE__, (void*)request, request->GetHeaderString().c_str());
        regstr.append("^HTTP/[0-9.]* [0-9]{3}");
        std::vector<std::string> result = FindRegexA(request->GetHeaderString(), regstr);
        for (auto codestr : result)
        {
            std::string code = codestr.substr(codestr.length() - 3);
            retValue = stoi(code);
        }

        if ((retValue == 302) || (retValue == 301))
        {
            std::lock_guard<std::mutex> lck(request->GetHeaderStringMutex());
            request->GetHeaderString() = "";
            TRACE("%-35s:%-8d:%-16p Redirect \n", __func__, __LINE__, (void*)request);
            request->GetRedirectPending() = true;
            return size * nmemb;
        }

        if (retValue != 100)
        {
            std::lock_guard<std::mutex> lck(request->GetReceiveCompletionEventMtx());
            request->ResponseCallbackEventCounter()++;
            request->HandleReceiveNotifications(srequest);
            TRACE("%-35s:%-8d:%-16p GetReceiveCompletionEvent().notify_all\n", __func__, __LINE__, (void*)request);
        }
        else
        {
            std::lock_guard<std::mutex> lck(request->GetHeaderStringMutex());
            request->GetHeaderString() = "";
            TRACE("%-35s:%-8d:%-16p retValue = %lu \n", __func__, __LINE__, (void*)request, retValue);
        }

    }
    return size * nmemb;
}

size_t WinHttpRequestImp::WriteBodyFunction(void *ptr, size_t size, size_t nmemb, void* rqst) {
    size_t read = 0;
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(rqst);
    if (!request)
        return 0;

    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return 0;

    {
        std::lock_guard<std::mutex> lck(request->GetBodyStringMutex());
        void *buf = request->GetResponseString().data();
        size_t available = request->GetResponseString().size();
        size_t totalread = 0;

        request->ConsumeIncoming(srequest, buf, available, totalread);
        if (totalread)
        {
            TRACE("%-35s:%-8d:%-16p consumed length:%lu\n", __func__, __LINE__, (void*)srequest.get(), totalread);
            request->GetReadLength() += totalread;
            request->GetReadData().erase(request->GetReadData().begin(), request->GetReadData().begin() + totalread);
            request->GetReadData().shrink_to_fit();
        }
    }

    size_t available = size * nmemb;
    {
        std::lock_guard<std::mutex> lck(request->GetBodyStringMutex());
        void *buf = ptr;

        request->ConsumeIncoming(srequest, buf, available, read);

        if (available)
        {
            request->GetResponseString().insert(request->GetResponseString().end(),
                reinterpret_cast<const BYTE*>(buf),
                reinterpret_cast<const BYTE*>(buf) + available);

            read += available;
        }
    }

    if (request->GetAsync()) {
        if (available && request->HandleQueryDataNotifications(srequest, available))
            TRACE("%-35s:%-8d:%-16p GetQueryDataEvent().notify_all\n", __func__, __LINE__, (void*)request);
    }

    return read;
}

void WinHttpRequestImp::ConsumeIncoming(std::shared_ptr<WinHttpRequestImp> &srequest, void* &ptr, size_t &available, size_t &read)
{
    while (available)
    {
        BufferRequest buf = GetBufferRequest(srequest->GetOutstandingReads());
        if (!buf.m_Length)
            break;

        size_t len = MIN(buf.m_Length, available);

        if (len)
        {
            TRACE("%-35s:%-8d:%-16p reading length:%lu written:%lu %p %ld\n",
                __func__, __LINE__, (void*)srequest.get(), len, read, buf.m_Buffer, buf.m_Length);
            memcpy(static_cast<char*>(buf.m_Buffer), ptr, len);
        }

        ptr = static_cast<char*>(ptr) + len;
        available -= len;

        if (len)
        {
            LPVOID StatusInformation = buf.m_Buffer;

            TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_READ_COMPLETE lpBuffer: %p length:%lu\n", __func__, __LINE__, (void*)srequest.get(), buf.m_Buffer, len);
            srequest->AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, len, StatusInformation, sizeof(StatusInformation), false);
        }

        read += len;
    }
}

void WinHttpRequestImp::FlushIncoming(std::shared_ptr<WinHttpRequestImp> &srequest)
{
    while (1)
    {
        BufferRequest buf = GetBufferRequest(srequest->GetOutstandingReads());
        if (!buf.m_Length)
            break;

        LPVOID StatusInformation = buf.m_Buffer;

        TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_READ_COMPLETE lpBuffer: %p length:%d\n", __func__, __LINE__, (void*)srequest.get(), buf.m_Buffer, 0);
        srequest->AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, 0, StatusInformation, sizeof(StatusInformation), false);
    }
}

void WinHttpRequestImp::CleanUp()
{
    m_CompletionCode = CURLE_OK;
    m_ResponseString.clear();
    m_HeaderString.clear();
    m_TotalReceiveSize = 0;
    m_ReadData.clear();
    m_ReadDataEventCounter = 0;
    m_UploadThreadExitStatus = false;
    m_QueryDataPending = false;
    m_ReceiveResponsePending = false;
    m_RedirectPending = false;
    m_ReceiveResponseEventCounter = 0;
    m_ReceiveResponseSendCounter = 0;
    m_OutstandingWrites.clear();
    m_OutstandingReads.clear();
    m_Completion = false;
}

WinHttpRequestImp::WinHttpRequestImp():
            m_UploadCallbackThread(),
            m_QueryDataPending(false),
            m_ReceiveResponsePending(false),
            m_ReceiveResponseEventCounter(0),
            m_RedirectPending(false)
{
    m_curl = ComContainer::GetInstance().AllocCURL();
    if (!m_curl)
        return;
}

WinHttpRequestImp::~WinHttpRequestImp()
{
    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)this);

    m_closed = true;

    if (!GetAsync() && Uploading())
    {
        {
            std::lock_guard<std::mutex> lck(GetReadDataEventMtx());
            GetReadDataEventCounter()++;
        }
        THREADJOIN(m_UploadCallbackThread);
    }

    if (GetAsync()) {
            TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)this);
    }
    else
        curl_easy_getinfo(GetCurl(), CURLINFO_PRIVATE, NULL);

    /* always cleanup */
    ComContainer::GetInstance().FreeCURL(m_curl);

    /* free the custom headers */
    if (m_HeaderList)
        curl_slist_free_all(m_HeaderList);

    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)this);
}

static void RequestCompletionCb(std::shared_ptr<WinHttpRequestImp> &requestRef, DWORD status)
{
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING)
    {
        TRACE_VERBOSE("%-35s:%-8d:%-16p status:0x%lx\n", __func__, __LINE__, (void*)(requestRef.get()), status);
        requestRef->GetClosed() = true;
    }
}

BOOL WinHttpRequestImp::AsyncQueue(std::shared_ptr<WinHttpRequestImp> &requestRef,
                                    DWORD dwInternetStatus, size_t statusInformationLength,
                                  LPVOID statusInformation, DWORD statusInformationCopySize,
                                  bool allocate)
{
    UserCallbackContext* ctx = NULL;
    DWORD dwNotificationFlags;
    WINHTTP_STATUS_CALLBACK cb = GetCallback(&dwNotificationFlags);
    LPVOID userdata = NULL;

    if (!requestRef->GetAsync())
        return FALSE;

    userdata = GetUserData();

    ctx = new UserCallbackContext(requestRef, dwInternetStatus, static_cast<DWORD>(statusInformationLength),
                                    dwNotificationFlags, cb, userdata, statusInformation,
                                    statusInformationCopySize, allocate, RequestCompletionCb);
    if (ctx) {
        UserCallbackContainer::GetInstance().Queue(ctx);
    }
    else
        return FALSE;


    return TRUE;
}


size_t WinHttpRequestImp::ReadCallback(void *ptr, size_t size, size_t nmemb, void *userp)
{
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(userp);
    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return size * nmemb;

    size_t len = 0;

    TRACE("request->GetTotalLength(): %lu\n", request->GetTotalLength());
    if (request->GetOptionalData().length() > 0)
    {
        len = MIN(request->GetOptionalData().length(), size * nmemb);
        TRACE("%-35s:%-8d:%-16p writing optional length of %lu\n", __func__, __LINE__, (void*)request, len);
        std::copy(request->GetOptionalData().begin(), request->GetOptionalData().end(), static_cast<char*>(ptr));
        request->GetOptionalData().erase(0, len);
        request->GetReadLength() += len;
        return len;
    }

    if (request->GetClosed())
        return -1;

    TRACE("%-35s:%-8d:%-16p request->GetTotalLength():%lu request->GetReadLength():%lu\n", __func__, __LINE__, (void*)request, request->GetTotalLength(), request->GetReadLength());
    if (((request->GetTotalLength() == 0) && (request->GetOptionalData().length() == 0) && request->Uploading()) ||
        (request->GetTotalLength() != request->GetReadLength()))
    {
        std::unique_lock<std::mutex> getReadDataEventHndlMtx(request->GetReadDataEventMtx());
        if (!request->GetReadDataEventCounter())
        {
            TRACE("%-35s:%-8d:%-16p transfer paused:%lu\n", __func__, __LINE__, (void*)request, size * nmemb);
            return CURL_READFUNC_PAUSE;
        }
        TRACE("%-35s:%-8d:%-16p transfer resumed as already signalled:%lu\n", __func__, __LINE__, (void*)request, size * nmemb);
    }

    if (request->GetAsync())
    {
        std::lock_guard<std::mutex> lck(request->GetReadDataEventMtx());
        if (!request->GetOutstandingWrites().empty())
        {
            BufferRequest &buf = PeekBufferRequest(request->GetOutstandingWrites());
            len = MIN(buf.m_Length - buf.m_Used, size * nmemb);

            if (request->GetTotalLength())
            {
                size_t remaining = request->GetTotalLength() - request->GetReadLength();
                len = MIN(len, remaining);
            }

            request->GetReadLength() += len;
            TRACE("%-35s:%-8d:%-16p writing additional length:%lu  buf.m_Length:%lu buf.m_Buffer:%p buf.m_Used:%lu\n",
                  __func__, __LINE__, (void*)request, len, buf.m_Length, buf.m_Buffer, buf.m_Used);

            if (len)
            {
                memcpy(static_cast<char*>(ptr), static_cast<char*>(buf.m_Buffer) + buf.m_Used, len);
            }

            buf.m_Used += len;

            if (buf.m_Used == buf.m_Length)
            {
                DWORD dwInternetStatus;
                DWORD result;

                result = buf.m_Length;
                dwInternetStatus = WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE;
                request->AsyncQueue(srequest, dwInternetStatus, sizeof(dwInternetStatus), &result, sizeof(result), true);
                GetBufferRequest(request->GetOutstandingWrites());
                TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:%p buf.m_Length:%lu\n", __func__, __LINE__, (void*)request, buf.m_Buffer, buf.m_Length);
                request->GetReadDataEventCounter()--;
            }

            TRACE("%-35s:%-8d:%-16p chunk written:%lu\n", __func__, __LINE__, (void*)request, len);
        }
    }
    else
    {
        request->GetReadDataEventMtx().lock();
        request->GetReadDataEventCounter()--;
        len = MIN(request->GetReadData().size(), size * nmemb);
        TRACE("%-35s:%-8d:%-16p writing additional length:%lu\n", __func__, __LINE__, (void*)request, len);
        std::copy(request->GetReadData().begin(), request->GetReadData().begin() + len, static_cast<char*>(ptr));
        request->GetReadLength() += len;
        request->GetReadData().erase(request->GetReadData().begin(), request->GetReadData().begin() + len);
        request->GetReadData().shrink_to_fit();
        request->GetReadDataEventMtx().unlock();
    }
    return len;
}

int WinHttpRequestImp::SocketCallback(CURL *handle, curl_infotype type,
    char *data, size_t size,
    void *userp)
{
    const char *text = "";

    switch (type) {
    case CURLINFO_TEXT:
        TRACE_VERBOSE("%-35s:%-8d:%-16p == Info: %s", __func__, __LINE__, (void*)userp, data);
        return 0;
    default: /* in case a new one is introduced to shock us */
        TRACE_VERBOSE("%-35s:%-8d:%-16p type:%d\n", __func__, __LINE__, (void*)userp, type);
        return 0;

    case CURLINFO_HEADER_IN:
        text = "=> Receive header";
        break;
    case CURLINFO_HEADER_OUT:
        text = "=> Send header";
        break;
    case CURLINFO_DATA_OUT:
        text = "=> Send data";
        break;
    case CURLINFO_SSL_DATA_OUT:
        text = "=> Send SSL data";
        break;
    case CURLINFO_DATA_IN:
        text = "<= Recv data";
        break;
    case CURLINFO_SSL_DATA_IN:
        text = "<= Recv SSL data";
        break;
    }
    TRACE_VERBOSE("%-35s:%-8d:%-16p %s\n", __func__, __LINE__, (void*)userp, text);

    return 0;
}

bool WinHttpRequestImp::HandleQueryDataNotifications(std::shared_ptr<WinHttpRequestImp> &srequest, size_t available)
{
    bool expected = true;
    bool result = std::atomic_compare_exchange_strong(&GetQueryDataPending(), &expected, false);
    if (result)
    {
        if (!available)
        {
            size_t length;

            GetBodyStringMutex().lock();
            length = GetResponseString().size();
            available = length;
            GetBodyStringMutex().unlock();
        }

        DWORD lpvStatusInformation = static_cast<DWORD>(available);
        TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:%lu\n", __func__, __LINE__, (void*)this, lpvStatusInformation);
        AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, sizeof(lpvStatusInformation),
            (LPVOID)&lpvStatusInformation, sizeof(lpvStatusInformation), true);
    }
    return result;
}

void WinHttpRequestImp::WaitAsyncQueryDataCompletion(std::shared_ptr<WinHttpRequestImp> &srequest)
{
    bool completed;
    GetQueryDataPending() = true;
    TRACE("%-35s:%-8d:%-16p GetQueryDataPending() = %d\n", __func__, __LINE__, (void*)this, (int)GetQueryDataPending());
    {
        std::lock_guard<std::mutex> lck(GetQueryDataEventMtx());
        completed = GetQueryDataEventState();
    }

    if (completed) {
        TRACE("%-35s:%-8d:%-16p transfer already finished\n", __func__, __LINE__, (void*)this);
        HandleQueryDataNotifications(srequest, 0);
    }
}

void WinHttpRequestImp::WaitAsyncReceiveCompletion(std::shared_ptr<WinHttpRequestImp> &srequest)
{
    bool expected = false;
    std::atomic_compare_exchange_strong(&GetReceiveResponsePending(), &expected, true);

    TRACE("%-35s:%-8d:%-16p GetReceiveResponsePending() = %d\n", __func__, __LINE__, (void*)this, (int) GetReceiveResponsePending());
    {
        std::lock_guard<std::mutex> lck(GetReceiveCompletionEventMtx());
        if (ResponseCallbackEventCounter())
        {
            TRACE("%-35s:%-8d:%-16p HandleReceiveNotifications \n", __func__, __LINE__, (void*)this);
            HandleReceiveNotifications(srequest);
        }
    }
}

WINHTTPAPI HINTERNET WINAPI WinHttpOpen
(
    LPCTSTR pszAgentW,
    DWORD dwAccessType,
    LPCTSTR pszProxyW,
    LPCTSTR pszProxyBypassW,
    DWORD dwFlags
)
{
    std::shared_ptr<WinHttpSessionImp> session = std::make_shared<WinHttpSessionImp> ();
    if (!session)
        return NULL;

    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*) (session.get()));
    if (dwAccessType == WINHTTP_ACCESS_TYPE_NAMED_PROXY)
    {
        if (!pszProxyW)
            return NULL;

        TRACE("%-35s:%-8d:%-16p proxy: " STRING_LITERAL " \n", __func__, __LINE__, (void*) (session.get()), pszProxyW);
        ConvertCstrAssign(pszProxyW, WCTLEN(pszProxyW), session->GetProxy());

        std::vector<std::string> proxies = Split(session->GetProxy(), ';');
        if (proxies.empty())
        {
            std::string sproxy;

            ConvertCstrAssign(pszProxyW, WCTLEN(pszProxyW), sproxy);
            proxies.push_back(sproxy);
        }
        session->SetProxies(proxies);
    }

    if (dwFlags & WINHTTP_FLAG_ASYNC) {
        session->SetAsync();
    }

    WinHttpHandleContainer<WinHttpSessionImp>::Instance().Register(session);
    return session.get();
}

WINHTTPAPI HINTERNET WINAPI WinHttpConnect
(
    HINTERNET hSession,
    LPCTSTR pswzServerName,
    INTERNET_PORT nServerPort,
    DWORD dwReserved
)
{
    WinHttpSessionImp *session = static_cast<WinHttpSessionImp *>(hSession);
    if (!session)
        return NULL;

    TRACE("%-35s:%-8d:%-16p pswzServerName: " STRING_LITERAL " nServerPort:%d\n",
          __func__, __LINE__, (void*)session, pswzServerName, nServerPort);
    ConvertCstrAssign(pswzServerName, WCTLEN(pswzServerName), session->GetServerName());

    session->SetServerPort(nServerPort);
    std::shared_ptr<WinHttpConnectImp> connect = std::make_shared<WinHttpConnectImp> ();
    if (!connect)
        return NULL;

    connect->SetHandle(session);
    WinHttpHandleContainer<WinHttpConnectImp>::Instance().Register(connect);
    return connect.get();
}


BOOLAPI WinHttpCloseHandle(
    HINTERNET hInternet
)
{
    WinHttpBase *base = static_cast<WinHttpBase *>(hInternet);

    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)base);
    if (!base)
        return FALSE;

    if (WinHttpHandleContainer<WinHttpRequestImp>::Instance().IsRegistered(static_cast<WinHttpRequestImp *>(hInternet)))
    {
        WinHttpRequestImp *request = dynamic_cast<WinHttpRequestImp *>(base);
        if (!request)
            return FALSE;

        std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
        if (!srequest)
            return FALSE;

        request->GetClosing() = true;
        WinHttpHandleContainer<WinHttpRequestImp>::Instance().UnRegister(request);
        return TRUE;
    }

    if (WinHttpHandleContainer<WinHttpSessionImp>::Instance().IsRegistered(static_cast<WinHttpSessionImp *>(hInternet)))
    {
        WinHttpSessionImp *session = dynamic_cast<WinHttpSessionImp *>(base);
        if (session)
        {
            WinHttpHandleContainer<WinHttpSessionImp>::Instance().UnRegister(session);
            return TRUE;
        }
        return TRUE;
    }

    if (WinHttpHandleContainer<WinHttpConnectImp>::Instance().IsRegistered(static_cast<WinHttpConnectImp *>(hInternet)))
    {
        WinHttpConnectImp *connect = dynamic_cast<WinHttpConnectImp *>(base);
        if (connect)
        {
            WinHttpHandleContainer<WinHttpConnectImp>::Instance().UnRegister(connect);
            return TRUE;
        }
    }

    return TRUE;
}

WINHTTPAPI HINTERNET WINAPI WinHttpOpenRequest(
    HINTERNET hConnect,
    LPCTSTR pwszVerb,  /* include "GET", "POST", and "HEAD" */
    LPCTSTR pwszObjectName,
    LPCTSTR pwszVersion,
    LPCTSTR pwszReferrer,
    LPCTSTR * ppwszAcceptTypes,
    DWORD dwFlags /* isSecurePort ? (WINHTTP_FLAG_REFRESH | WINHTTP_FLAG_SECURE) : WINHTTP_FLAG_REFRESH */
)
{
    WinHttpConnectImp *connect = static_cast<WinHttpConnectImp *>(hConnect);
    if (!connect)
        return NULL;

    WinHttpSessionImp *session = connect->GetHandle();
    if (!session)
        return NULL;

    CURLcode res;

    if (!pwszVerb)
        pwszVerb = TEXT("GET");

    TRACE("%-35s:%-8d:%-16p pwszVerb = " STRING_LITERAL " pwszObjectName: " STRING_LITERAL " pwszVersion = " STRING_LITERAL " pwszReferrer = " STRING_LITERAL "\n",
        __func__, __LINE__, (void*)session, pwszVerb, pwszObjectName, pwszVersion, pwszReferrer);

    std::shared_ptr<WinHttpRequestImp> srequest(new WinHttpRequestImp, [](WinHttpRequestImp *request) {

        std::shared_ptr<WinHttpRequestImp> close_request(request);
        TRACE("%-35s:%-8d:%-16p reference count reached to 0\n", __func__, __LINE__, (void*)request);

        TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING\n", __func__, __LINE__, (void*)request);
        request->AsyncQueue(close_request, WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING, 0, NULL, 0, false);
    });

    if (!srequest)
        return NULL;
    WinHttpRequestImp *request = srequest.get();
    if (!request)
        return NULL;

    if (session->GetAsync())
    {
        DWORD flag;
        WINHTTP_STATUS_CALLBACK cb;
        LPVOID userdata = NULL;

        cb = session->GetCallback(&flag);
        request->SetAsync();
        request->SetCallback(cb, flag);

        if (connect->GetUserData())
            userdata = (LPVOID)connect->GetUserData();
        else if (session->GetUserData())
            userdata = (LPVOID)session->GetUserData();

        if (userdata)
            request->SetUserData(&userdata);

    }

    const char *prefix;
    std::string server = session->GetServerName();
    if (dwFlags & WINHTTP_FLAG_SECURE)
    {
        prefix = "https://";
        request->GetSecure() = true;
    }
    else
    {
        prefix = "http://";
        request->GetSecure() = false;
    }

    if (server.find("http://") == std::string::npos)
        server = prefix + server;
    if (pwszObjectName)
    {
        std::string objectname;

        ConvertCstrAssign(pwszObjectName, WCTLEN(pwszObjectName), objectname);

        size_t index = 0;
        // convert # to %23 to support links to fragments
        while (true) {
            /* Locate the substring to replace. */
            index = objectname.find('#', index);
            if (index == std::string::npos) break;
            /* Make the replacement. */
            objectname.replace(index, 1, "%23");
            /* Advance index forward so the next iteration doesn't pick it up as well. */
            index += 3;
        }
        request->SetFullPath(server, objectname);
        if (!request->SetServer(request->GetFullPath(), session->GetServerPort())) {
            return NULL;
        }
    }
    else
    {
        std::string nullstr;
        request->SetFullPath(server, nullstr);
        if (!request->SetServer(request->GetFullPath(), session->GetServerPort())) {
            return NULL;
        }
    }

    if (session->GetConnectionTimeoutMs() > 0)
    {
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_CONNECTTIMEOUT, session->GetConnectionTimeoutMs()/1000);
        CURL_BAILOUT_ONERROR(res, request, NULL);
    }

    if (session->GetReceiveTimeoutMs() > 0)
    {
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_TIMEOUT_MS, session->GetReceiveTimeoutMs());
        CURL_BAILOUT_ONERROR(res, request, NULL);
    }

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_LOW_SPEED_TIME, 60L);
    CURL_BAILOUT_ONERROR(res, request, NULL);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_LOW_SPEED_LIMIT, 1L);
    CURL_BAILOUT_ONERROR(res, request, NULL);

    request->SetProxy(session->GetProxies());
    TSTRING verb;
    verb.assign(pwszVerb);
    if (verb == TEXT("PUT"))
    {
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_PUT, 1L);
        CURL_BAILOUT_ONERROR(res, request, NULL);

        res = curl_easy_setopt(request->GetCurl(), CURLOPT_UPLOAD, 1L);
        CURL_BAILOUT_ONERROR(res, request, NULL);

        request->Uploading() = true;
    }
    else if (verb == TEXT("GET"))
    {
    }
    else if (verb == TEXT("POST"))
    {
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_POST, 1L);
        CURL_BAILOUT_ONERROR(res, request, NULL);
    }
    else
    {
        std::string verbcst;

        ConvertCstrAssign(pwszVerb, WCTLEN(pwszVerb), verbcst);
        TRACE("%-35s:%-8d:%-16p setting custom header %s\n", __func__, __LINE__, (void*)request, verbcst.c_str());
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_CUSTOMREQUEST, verbcst.c_str());
        CURL_BAILOUT_ONERROR(res, request, NULL);

        res = curl_easy_setopt(request->GetCurl(), CURLOPT_UPLOAD, 1L);
        CURL_BAILOUT_ONERROR(res, request, NULL);

        request->Uploading() = true;
    }

#if (LIBCURL_VERSION_MAJOR>=7) && (LIBCURL_VERSION_MINOR >= 50)
    if (pwszVersion)
    {
        int ver;
        TSTRING version;
        version.assign(pwszVersion);

        if (version == TEXT("1.0"))
            ver = CURL_HTTP_VERSION_1_0;
        else if (version == TEXT("1.1"))
            ver = CURL_HTTP_VERSION_1_1;
        else if (version == TEXT("2.0"))
            ver = CURL_HTTP_VERSION_2_0;
        else
            return NULL;
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_HTTP_VERSION, ver);
        CURL_BAILOUT_ONERROR(res, request, NULL);
    }
#endif

    if (pwszReferrer)
    {
        ConvertCstrAssign(pwszReferrer, WCTLEN(pwszReferrer), session->GetReferrer());

        res = curl_easy_setopt(request->GetCurl(), CURLOPT_REFERER, session->GetReferrer().c_str());
        CURL_BAILOUT_ONERROR(res, request, NULL);
    }

    if (dwFlags & WINHTTP_FLAG_SECURE)
    {
        const char *pKeyName;
        const char *pKeyType;
        const char *pEngine;
        static const char *pCertFile = "testcert.pem";
        static const char *pCACertFile = "cacert.pem";
        //static const char *pHeaderFile = "dumpit";
        const char *pPassphrase = NULL;

        pKeyName = NULL;
        pKeyType = "PEM";
        pCertFile = NULL;
        pCACertFile = NULL;
        pEngine = NULL;

        if (const char* env_p = std::getenv("WINHTTP_PAL_KEYNAME"))
            pKeyName = env_p;

        if (const char* env_p = std::getenv("WINHTTP_PAL_KEYTYPE"))
            pKeyType = env_p;

        if (const char* env_p = std::getenv("WINHTTP_PAL_CERTFILE"))
            pCertFile = env_p;

        if (const char* env_p = std::getenv("WINHTTP_PAL_CACERTFILE"))
            pCACertFile = env_p;

        if (const char* env_p = std::getenv("WINHTTP_PAL_ENGINE"))
            pEngine = env_p;

        if (const char* env_p = std::getenv("WINHTTP_PAL_KEY_PASSPHRASE"))
            pPassphrase = env_p;

        if (pEngine) {
            res = curl_easy_setopt(request->GetCurl(), CURLOPT_SSLENGINE, pEngine);
            CURL_BAILOUT_ONERROR(res, request, NULL);

            res = curl_easy_setopt(request->GetCurl(), CURLOPT_SSLENGINE_DEFAULT, 1L);
            CURL_BAILOUT_ONERROR(res, request, NULL);
        }
        /* cert is stored PEM coded in file... */
        /* since PEM is default, we needn't set it for PEM */
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_SSLCERTTYPE, "PEM");
        CURL_BAILOUT_ONERROR(res, request, NULL);

        /* set the cert for client authentication */
        if (pCertFile) {
            res = curl_easy_setopt(request->GetCurl(), CURLOPT_SSLCERT, pCertFile);
            CURL_BAILOUT_ONERROR(res, request, NULL);
        }

        /* sorry, for engine we must set the passphrase
           (if the key has one...) */
        if (pPassphrase) {
            res = curl_easy_setopt(request->GetCurl(), CURLOPT_KEYPASSWD, pPassphrase);
            CURL_BAILOUT_ONERROR(res, request, NULL);
        }

        /* if we use a key stored in a crypto engine,
           we must set the key type to "ENG" */
        if (pKeyType) {
            res = curl_easy_setopt(request->GetCurl(), CURLOPT_SSLKEYTYPE, pKeyType);
            CURL_BAILOUT_ONERROR(res, request, NULL);
        }

        /* set the private key (file or ID in engine) */
        if (pKeyName) {
            res = curl_easy_setopt(request->GetCurl(), CURLOPT_SSLKEY, pKeyName);
            CURL_BAILOUT_ONERROR(res, request, NULL);
        }

        /* set the file with the certs vaildating the server */
        if (pCACertFile) {
            res = curl_easy_setopt(request->GetCurl(), CURLOPT_CAINFO, pCACertFile);
            CURL_BAILOUT_ONERROR(res, request, NULL);
        }
    }
    if (pwszVerb)
    {
        ConvertCstrAssign(pwszVerb, WCTLEN(pwszVerb), request->GetType());
    }
    request->SetSession(connect);
    WinHttpHandleContainer<WinHttpRequestImp>::Instance().Register(srequest);

    return request;
}

BOOLAPI
WinHttpAddRequestHeaders
(
    HINTERNET hRequest,
    LPCTSTR lpszHeaders,
    DWORD dwHeadersLength,
    DWORD dwModifiers
)
{
    CURLcode res;
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(hRequest);
    if (!request)
        return FALSE;

    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return FALSE;

    TRACE("%-35s:%-8d:%-16p lpszHeaders = " STRING_LITERAL " dwModifiers: %lu\n", __func__, __LINE__, (void*)request,
        lpszHeaders ? lpszHeaders: TEXT(""), dwModifiers);

    if (dwHeadersLength == (DWORD)-1)
    {
        dwHeadersLength = lpszHeaders ? WCTLEN(lpszHeaders): 0;
    }

    if (lpszHeaders)
    {
        std::string &headers = request->GetOutgoingHeaderList();

        ConvertCstrAssign(lpszHeaders, dwHeadersLength, headers);
        request->AddHeader(headers);

        res = curl_easy_setopt(request->GetCurl(), CURLOPT_HTTPHEADER, request->GetHeaderList());
        CURL_BAILOUT_ONERROR(res, request, FALSE);
    }

    return TRUE;
}

BOOLAPI WinHttpSendRequest
(
    HINTERNET hRequest,
    LPCTSTR lpszHeaders,
    DWORD dwHeadersLength,
    LPVOID lpOptional,
    DWORD dwOptionalLength,
    DWORD dwTotalLength,
    DWORD_PTR dwContext
)
{
    CURLcode res;
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(hRequest);
    if (!request)
        return FALSE;

    WinHttpConnectImp *connect = request->GetSession();
    if (!connect)
        return FALSE;

    WinHttpSessionImp *session = connect->GetHandle();

    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return FALSE;

    TSTRING customHeader;

    if (dwHeadersLength == (DWORD)-1)
    {
        dwHeadersLength = lpszHeaders ? WCTLEN(lpszHeaders): 0;
    }

    if (lpszHeaders)
        customHeader.assign(lpszHeaders, dwHeadersLength);

    if ((dwTotalLength == 0) && (dwOptionalLength == 0) && request->Uploading())
        customHeader += TEXT("Transfer-Encoding: chunked\r\n");

    TRACE("%-35s:%-8d:%-16p lpszHeaders:%p dwHeadersLength:%lu lpOptional:%p dwOptionalLength:%lu dwTotalLength:%lu\n",
        __func__, __LINE__, (void*)request, (const void*)lpszHeaders, dwHeadersLength, lpOptional, dwOptionalLength, dwTotalLength);

    if (!customHeader.empty() && !WinHttpAddRequestHeaders(hRequest, customHeader.c_str(), customHeader.length(), 0))
        return FALSE;

    if (lpOptional)
    {
        if (dwOptionalLength == 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }

        if (!request->SetOptionalData(lpOptional, dwOptionalLength)) return FALSE;

        if (request->GetType() == "POST")
        {
            /* Now specify the POST data */
            res = curl_easy_setopt(request->GetCurl(), CURLOPT_POSTFIELDS, request->GetOptionalData().c_str());
            CURL_BAILOUT_ONERROR(res, request, FALSE);
        }
        else if (request->GetType() == "PUT")
        {
            res = curl_easy_setopt(request->GetCurl(), CURLOPT_CUSTOMREQUEST, "PUT");
            CURL_BAILOUT_ONERROR(res, request, FALSE);

            res = curl_easy_setopt(request->GetCurl(), CURLOPT_POSTFIELDS, request->GetOptionalData().c_str()); // data goes here 
            CURL_BAILOUT_ONERROR(res, request, FALSE);

            res = curl_easy_setopt(request->GetCurl(), CURLOPT_POSTFIELDSIZE, dwOptionalLength); // length is a must
            CURL_BAILOUT_ONERROR(res, request, FALSE);
        }
    }

    if (request->Uploading() || (request->GetType() == "POST"))
    {
        if (!request->GetAsync() && (dwTotalLength == 0)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
    }

    if (dwOptionalLength == (DWORD)-1)
        dwOptionalLength = request->GetOptionalData().length();

    DWORD totalsize = MAX(dwOptionalLength, dwTotalLength);
    /* provide the size of the upload, we specicially typecast the value
        to curl_off_t since we must be sure to use the correct data size */
    if (request->GetType() == "POST")
    {
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_POSTFIELDSIZE, (long)totalsize);
        CURL_BAILOUT_ONERROR(res, request, FALSE);
    }
    else if (totalsize)
    {
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_INFILESIZE_LARGE, (curl_off_t)totalsize);
        CURL_BAILOUT_ONERROR(res, request, FALSE);
    }

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_BUFFERSIZE, WINHTTP_CURL_MAX_WRITE_SIZE);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    request->GetTotalLength() = dwTotalLength;
    res = curl_easy_setopt(request->GetCurl(), CURLOPT_READDATA, request);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_READFUNCTION, request->ReadCallback);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_DEBUGFUNCTION, request->SocketCallback);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_DEBUGDATA, request);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_WRITEFUNCTION, request->WriteBodyFunction);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_WRITEDATA, request);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_HEADERFUNCTION, request->WriteHeaderFunction);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_HEADERDATA, request);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_PRIVATE, request);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_FOLLOWLOCATION, 1L);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    if (winhttp_tracing_verbose)
    {
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_VERBOSE, 1);
        CURL_BAILOUT_ONERROR(res, request, FALSE);
    }

    /* enable TCP keep-alive for this transfer */
    res = curl_easy_setopt(request->GetCurl(), CURLOPT_TCP_KEEPALIVE, 1L);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    /* keep-alive idle time to 120 seconds */
    res = curl_easy_setopt(request->GetCurl(), CURLOPT_TCP_KEEPIDLE, 120L);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    /* interval time between keep-alive probes: 60 seconds */
    res = curl_easy_setopt(request->GetCurl(), CURLOPT_TCP_KEEPINTVL, 60L);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_SSL_VERIFYPEER, request->VerifyPeer());
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    res = curl_easy_setopt(request->GetCurl(), CURLOPT_SSL_VERIFYHOST, request->VerifyHost());
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    DWORD maxConnections = 0;

    if (request->GetMaxConnections())
        maxConnections = request->GetMaxConnections();
    else if (session->GetMaxConnections())
        maxConnections = session->GetMaxConnections();

    if (maxConnections) {
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_MAXCONNECTS, maxConnections);
        CURL_BAILOUT_ONERROR(res, request, FALSE);
    }

    if (dwContext)
        request->SetUserData(reinterpret_cast<void**>(&dwContext));

    DWORD securityProtocols = 0;

    if (request->GetSecureProtocol())
        securityProtocols = request->GetSecureProtocol();
    else if (session->GetSecureProtocol())
        securityProtocols = session->GetSecureProtocol();

    if (securityProtocols) {
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_SSLVERSION, securityProtocols);
        CURL_BAILOUT_ONERROR(res, request, FALSE);
    }

    if (request->GetAsync())
    {
        if (request->GetClosing())
        {
            TRACE("%-35s:%-8d:%-16p \n", __func__, __LINE__, (void*)request);
            return FALSE;
        }

        request->CleanUp();
        request->AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_SENDING_REQUEST, 0, NULL, 0, false);

        if (!ComContainer::GetInstance().AddHandle(srequest, request->GetCurl()))
            return FALSE;

        ComContainer::GetInstance().KickStart();

        request->AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_REQUEST_SENT, 0, NULL, 0, false);
        request->AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, 0, NULL, 0, false);

        TRACE("%-35s:%-8d:%-16p use_count = %lu\n", __func__, __LINE__, (void*)request, srequest.use_count());
    }
    else
    {
        if (dwTotalLength && (request->GetType() != "POST"))
        {
            DWORD thread_id;
            request->GetUploadThread() = CREATETHREAD(
                WinHttpRequestImp::UploadThreadFunction,       // thread function name
                request, &thread_id);          // argument to thread function
        }
        else
        {
            /* Perform the request, res will get the return code */
            res = curl_easy_perform(request->GetCurl());
            /* Check for errors */
            CURL_BAILOUT_ONERROR(res, request, FALSE);
        }
    }

    return TRUE;
}

WINHTTPAPI
BOOL
WINAPI
WinHttpReceiveResponse
(
    HINTERNET hRequest,
    LPVOID lpReserved
)
{
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(hRequest);
    if (!request)
        return FALSE;

    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return FALSE;

    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)request);

    if ((request->GetTotalLength() == 0) && (request->GetOptionalData().length() == 0) && request->Uploading())
    {
        if (request->GetAsync())
        {
            {
                std::lock_guard<std::mutex> lck(request->GetReadDataEventMtx());
                request->GetReadDataEventCounter()++;
                TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)request);
            }

            ComContainer::GetInstance().ResumeTransfer(request->GetCurl(), CURLPAUSE_CONT);
        }
    }

    if (request->GetAsync())
    {
        TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)request);
        request->WaitAsyncReceiveCompletion(srequest);
    }
    else
    {
        if (request->Uploading())
        {
            while (request->GetTotalLength() != request->GetReadLength()) {
                if (request->GetUploadThreadExitStatus()) {
                    if (request->GetTotalLength() != request->GetReadLength()) {
                        SetLastError(ERROR_WINHTTP_OPERATION_CANCELLED);
                        return FALSE;
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            }

            return TRUE;
        }
        size_t headerLength;
        {
            std::lock_guard<std::mutex> lck(request->GetHeaderStringMutex());
            headerLength = request->GetHeaderString().length();
        }

        return headerLength > 0;
    }
    return TRUE;
}

BOOLAPI
WinHttpQueryDataAvailable
(
    HINTERNET hRequest,
    LPDWORD lpdwNumberOfBytesAvailable
)
{
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(hRequest);
    if (!request)
        return FALSE;

    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return FALSE;

    if (request->GetClosing())
    {
        TRACE("%-35s:%-8d:%-16p \n", __func__, __LINE__, (void*)request);
        return FALSE;
    }

    size_t length;

    request->GetBodyStringMutex().lock();
    length = request->GetResponseString().size();
    size_t available = length;
    request->GetBodyStringMutex().unlock();

    if (request->GetAsync())
    {
        if (available == 0)
        {
            TRACE("%-35s:%-8d:%-16p !!!!!!!\n", __func__, __LINE__, (void*)request);
            request->WaitAsyncQueryDataCompletion(srequest);
            request->GetBodyStringMutex().lock();
            length = request->GetResponseString().size();
            available = length;
            TRACE("%-35s:%-8d:%-16p available = %lu\n", __func__, __LINE__, (void*)request, available);
            request->GetBodyStringMutex().unlock();
        }
        else
        {
            DWORD lpvStatusInformation = available;
            TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:%lu\n", __func__, __LINE__, (void*)request, lpvStatusInformation);
            request->AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, sizeof(lpvStatusInformation),
                (LPVOID)&lpvStatusInformation, sizeof(lpvStatusInformation), true);

        }

    }


    if (lpdwNumberOfBytesAvailable)
        *lpdwNumberOfBytesAvailable = available;

    return TRUE;
}

BOOLAPI
WinHttpReadData
(
    HINTERNET hRequest,
    LPVOID lpBuffer,
    DWORD dwNumberOfBytesToRead,
    LPDWORD lpdwNumberOfBytesRead
)
{
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(hRequest);
    if (!request)
        return FALSE;

    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return FALSE;

    if (request->GetClosing())
    {
        TRACE("%-35s:%-8d:%-16p \n", __func__, __LINE__, (void*)request);
        return FALSE;
    }

    size_t readLength;

    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)request);
    if (dwNumberOfBytesToRead == 0)
    {
        if (lpdwNumberOfBytesRead)
            *lpdwNumberOfBytesRead = 0;

        if (request->GetAsync())
        {
            LPVOID StatusInformation = (LPVOID)lpBuffer;

            TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_READ_COMPLETE lpBuffer: %p length:%d\n", __func__, __LINE__, (void*)request, lpBuffer, 0);
            request->AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, 0, (LPVOID)StatusInformation, sizeof(StatusInformation), false);
        }
        return TRUE;
    }

    request->GetBodyStringMutex().lock();
    readLength = request->GetResponseString().size();
    if (readLength)
    {
        if (readLength > dwNumberOfBytesToRead)
        {
            readLength = dwNumberOfBytesToRead;
        }
        std::copy(request->GetResponseString().begin(), request->GetResponseString().begin() + readLength, static_cast<char *>(lpBuffer));
        request->GetResponseString().erase(request->GetResponseString().begin(), request->GetResponseString().begin() + readLength);
        request->GetResponseString().shrink_to_fit();
    }

    if (request->GetAsync())
    {
        if ((readLength == 0) && (!request->GetCompletionStatus()))
        {
            TRACE("%-35s:%-8d:%-16p Queueing pending reads %p %ld\n", __func__, __LINE__, (void*)request, lpBuffer, dwNumberOfBytesToRead);
            QueueBufferRequest(request->GetOutstandingReads(), lpBuffer, dwNumberOfBytesToRead);
        }
        else
        {
            LPVOID StatusInformation = (LPVOID)lpBuffer;

            TRACE("%-35s:%-8d:%-16p WINHTTP_CALLBACK_STATUS_READ_COMPLETE lpBuffer: %p length:%lu\n", __func__, __LINE__, (void*)request, lpBuffer, readLength);
            request->AsyncQueue(srequest, WINHTTP_CALLBACK_STATUS_READ_COMPLETE, readLength, (LPVOID)StatusInformation, sizeof(StatusInformation), false);
        }
    }
    request->GetBodyStringMutex().unlock();

    if (lpdwNumberOfBytesRead)
        *lpdwNumberOfBytesRead = (DWORD)readLength;
    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)request);
    return TRUE;
}

BOOLAPI
WinHttpSetTimeouts
(
    HINTERNET    hInternet,           // Session/Request handle.
    int          nResolveTimeout,
    int          nConnectTimeout,
    int          nSendTimeout,
    int          nReceiveTimeout
)
{
    WinHttpBase *base = static_cast<WinHttpBase *>(hInternet);
    WinHttpSessionImp *session;
    WinHttpRequestImp *request;
    CURLcode res;

    TRACE("%-35s:%-8d:%-16p nResolveTimeout:%d nConnectTimeout:%d nSendTimeout:%d nReceiveTimeout:%d\n",
          __func__, __LINE__, (void*)base, nResolveTimeout, nConnectTimeout, nSendTimeout, nReceiveTimeout);
    if ((session = dynamic_cast<WinHttpSessionImp *>(base)))
    {
        session->SetReceiveTimeoutMs(nReceiveTimeout);
        session->SetConnectionTimeoutMs(nConnectTimeout);
    }
    else if ((request = dynamic_cast<WinHttpRequestImp *>(base)))
    {
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_TIMEOUT_MS, nReceiveTimeout);
        CURL_BAILOUT_ONERROR(res, request, FALSE);
        res = curl_easy_setopt(request->GetCurl(), CURLOPT_CONNECTTIMEOUT, nConnectTimeout/1000);
        CURL_BAILOUT_ONERROR(res, request, FALSE);
    }
    else
        return FALSE;

    return TRUE;
}

static TSTRING FindRegex(const TSTRING &subject,const TSTRING &regstr)
{
    TSTRING result;
    try {
        TREGEX re(regstr, std::regex_constants::icase);
        TREGEX_MATCH match;
        if (TREGEX_SEARCH(subject, match, re)) {

            result = match.str(0);
        } else {
            result = TEXT("");
            return TEXT("");
        }
    } catch (std::regex_error&) {
        return TEXT("");
    }
    return result;
}

static std::vector<std::string> FindRegexA(const std::string &str,const std::string &regstr)
{
    std::vector<std::string> results;
    try {
        std::regex re(regstr, std::regex_constants::icase);
        std::smatch match;
        std::istringstream f(str);
        std::string line;
        while (std::getline(f, line)) {
            std::string::const_iterator searchStart(line.cbegin());
            while (std::regex_search(searchStart, line.cend(), match, re)) {
                results.push_back(match[0]);
                searchStart = match.suffix().first;
            }
        }
    } catch (std::regex_error&) {
        return results;
    }
    return results;
}

bool is_newline(char i)
{
    return (i == '\n') || (i == '\r');
}

template<class CharT>
std::basic_string<CharT> nullize_newlines(const std::basic_string<CharT>& str) {
    std::basic_string<CharT> result;
    result.reserve(str.size());

    auto cursor = str.begin();
    const auto end = str.end();
    for (;;) {
        cursor = std::find_if_not(cursor, end, is_newline);
        if (cursor == end) {
            return result;
        }

        const auto nextNewline = std::find_if(cursor, end, is_newline);
        result.append(cursor, nextNewline);
        result.push_back(CharT{});
        cursor = nextNewline;
    }
}

static BOOL ReadCurlValue(WinHttpRequestImp *request, LPVOID lpBuffer, LPDWORD lpdwBufferLength,
                          CURLINFO curlparam, bool returnDWORD)
{
    CURLcode res;
    DWORD retValue;

    res = curl_easy_getinfo(request->GetCurl(), curlparam, &retValue);
    CURL_BAILOUT_ONERROR(res, request, FALSE);

    if (!returnDWORD)
    {
        TSTRING str = TO_STRING(retValue);
        if (SizeCheck(lpBuffer, lpdwBufferLength, (str.size() + 1) * sizeof(TCHAR)) == FALSE)
            return FALSE;
    }
    else
    {
        if (SizeCheck(lpBuffer, lpdwBufferLength, sizeof(DWORD)) == FALSE)
            return FALSE;
    }

    if (returnDWORD)
    {
        memcpy(lpBuffer, &retValue, sizeof(retValue));
    }
    else
    {
        TCHAR *outbuf = static_cast<TCHAR*>(lpBuffer);
        TSTRING str = TO_STRING(retValue);
        std::copy(str.begin(), str.end(), outbuf);
        outbuf[str.size()] = TEXT('\0');

        if (lpdwBufferLength)
            *lpdwBufferLength = (str.size() + 1) * sizeof(TCHAR);
    }
    TRACE("%-35s:%-8d:%-16p curlparam:%d code :%lu\n", __func__, __LINE__, (void*)request, curlparam, retValue);
    return TRUE;
}

BOOLAPI WinHttpQueryHeaders(
    HINTERNET   hRequest,
    DWORD       dwInfoLevel,
    LPCTSTR     pwszName,
    LPVOID         lpBuffer,
    LPDWORD lpdwBufferLength,
    LPDWORD lpdwIndex
)
{
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(hRequest);
    if (!request)
        return FALSE;

    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return FALSE;

    bool returnDWORD = false;

    if (pwszName != WINHTTP_HEADER_NAME_BY_INDEX)
        return FALSE;

    if (lpdwIndex != WINHTTP_NO_HEADER_INDEX)
        return FALSE;

    if (request->GetHeaderString().length() == 0)
        return FALSE;

    if (dwInfoLevel & WINHTTP_QUERY_FLAG_NUMBER)
    {
        dwInfoLevel &= ~WINHTTP_QUERY_FLAG_NUMBER;
        returnDWORD = true;
    }
    TRACE("%-35s:%-8d:%-16p dwInfoLevel = 0x%lx\n", __func__, __LINE__, (void*)request, dwInfoLevel);

    if (returnDWORD && SizeCheck(lpBuffer, lpdwBufferLength, sizeof(DWORD)) == FALSE)
        return FALSE;

#if (LIBCURL_VERSION_MAJOR>=7) && (LIBCURL_VERSION_MINOR >= 50)
    if (dwInfoLevel == WINHTTP_QUERY_VERSION)
    {
        return ReadCurlValue(request, lpBuffer, lpdwBufferLength, CURLINFO_HTTP_VERSION, returnDWORD);
    }
#endif
    if (dwInfoLevel == WINHTTP_QUERY_STATUS_CODE)
    {
        WinHttpSessionImp *session;

        session = GetImp(request);
        if (!session->GetProxies().empty() && request->GetSecure())
            return ReadCurlValue(request, lpBuffer, lpdwBufferLength, CURLINFO_HTTP_CONNECTCODE, returnDWORD);
        else
            return ReadCurlValue(request, lpBuffer, lpdwBufferLength, CURLINFO_RESPONSE_CODE, returnDWORD);
    }

    if (dwInfoLevel == WINHTTP_QUERY_STATUS_TEXT)
    {
        CURLcode res;
        DWORD responseCode;

        res = curl_easy_getinfo(request->GetCurl(), CURLINFO_RESPONSE_CODE, &responseCode);
        CURL_BAILOUT_ONERROR(res, request, FALSE);

        std::lock_guard<std::mutex> lck(request->GetHeaderStringMutex());

#ifdef UNICODE
        TSTRING subject;

        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        subject = conv.from_bytes(request->GetHeaderString());
#else
        TSTRING &subject = request->GetHeaderString();
#endif

        TSTRING regstr;

        regstr.append(TEXT("HTTP.*"));
        regstr.append(TO_STRING(responseCode));

        TSTRING result = FindRegex(subject, regstr);
        if (!result.empty())
        {
            size_t offset = subject.find(result);
            if (offset == std::string::npos)
                return FALSE;

            size_t offsetendofline = subject.find(TEXT("\r\n"), offset);
            if (offsetendofline == std::string::npos)
                return FALSE;

            size_t startpos = offset + result.length() + 1;
            if (offsetendofline <= startpos)
                return FALSE;

            size_t linelength = offsetendofline - startpos;
            if (SizeCheck(lpBuffer, lpdwBufferLength, (linelength + 1) * sizeof(TCHAR)) == FALSE)
                return FALSE;

            std::copy(subject.begin() + startpos, subject.begin() + offsetendofline,
                      (TCHAR*)lpBuffer);
            ((TCHAR*)lpBuffer)[linelength] = TEXT('\0');
            return TRUE;
        }
        else
        {
            TSTRING retStr = StatusCodeMap.at(responseCode);
            if (retStr.empty())
                return FALSE;

            if (SizeCheck(lpBuffer, lpdwBufferLength, (retStr.size() + 1) * sizeof(TCHAR)) == FALSE)
                return FALSE;

            std::copy(retStr.begin(), retStr.end(), (TCHAR*)lpBuffer);
            ((TCHAR*)lpBuffer)[retStr.size()] = TEXT('\0');
        }
        return TRUE;
    }

    if (dwInfoLevel == WINHTTP_QUERY_RAW_HEADERS)
    {
        std::string header;
        TCHAR *wbuffer = static_cast<TCHAR*>(lpBuffer);

        std::lock_guard<std::mutex> lck(request->GetHeaderStringMutex());
        header.append(request->GetHeaderString());

        header = nullize_newlines(header);
        header.resize(header.size() + 1);

        if (SizeCheck(lpBuffer, lpdwBufferLength, (header.length() + 1) * sizeof(TCHAR)) == FALSE)
            return FALSE;

#ifdef UNICODE
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        std::wstring wc = conv.from_bytes(header);

        if (lpdwBufferLength && (*lpdwBufferLength < ((wc.length() + 1) * sizeof(TCHAR))))
            return FALSE;

        std::copy(wc.begin(), wc.end(), wbuffer);
#else
        std::copy(header.begin(), header.end(), wbuffer);
#endif
        wbuffer[header.length()] = TEXT('\0');
        if (lpdwBufferLength)
            *lpdwBufferLength = (DWORD)header.length();
        return TRUE;
    }

    if (dwInfoLevel == WINHTTP_QUERY_RAW_HEADERS_CRLF)
    {
        std::lock_guard<std::mutex> lck(request->GetHeaderStringMutex());
        size_t length;
        TCHAR *wbuffer = static_cast<TCHAR*>(lpBuffer);

        length = request->GetHeaderString().length();

        if (SizeCheck(lpBuffer, lpdwBufferLength, (length + 1) * sizeof(TCHAR)) == FALSE)
            return FALSE;

#ifdef UNICODE
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        std::wstring wc = conv.from_bytes(request->GetHeaderString());
        if (lpdwBufferLength && (*lpdwBufferLength < ((wc.length() + 1) * sizeof(TCHAR))))
            return FALSE;

        std::copy(wc.begin(), wc.end(), wbuffer);
#else
        std::copy(request->GetHeaderString().begin(), request->GetHeaderString().end(), wbuffer);
#endif
        wbuffer[length] = TEXT('\0');
        if (lpdwBufferLength)
            *lpdwBufferLength = (DWORD)(length * sizeof(TCHAR));
        return TRUE;
    }


    return FALSE;
}

BOOLAPI WinHttpSetOption(
    HINTERNET hInternet,
    DWORD     dwOption,
    LPVOID    lpBuffer,
    DWORD     dwBufferLength
)
{
    WinHttpBase *base = static_cast<WinHttpBase *>(hInternet);

    TRACE("%-35s:%-8d:%-16p dwOption:%lu\n", __func__, __LINE__, (void*)base, dwOption);

    if (dwOption == WINHTTP_OPTION_MAX_CONNS_PER_SERVER)
    {
        if (dwBufferLength != sizeof(DWORD))
            return FALSE;

        if (CallMemberFunction<WinHttpSessionImp, DWORD>(base, &WinHttpSessionImp::SetMaxConnections, lpBuffer))
            return TRUE;

        if (CallMemberFunction<WinHttpRequestImp, DWORD>(base, &WinHttpRequestImp::SetMaxConnections, lpBuffer))
            return TRUE;

        return FALSE;
    }
    else if (dwOption == WINHTTP_OPTION_CONTEXT_VALUE)
    {
        if (dwBufferLength != sizeof(void*))
            return FALSE;

        if (CallMemberFunction<WinHttpConnectImp, void*>(base, &WinHttpConnectImp::SetUserData, lpBuffer))
            return TRUE;

        if (CallMemberFunction<WinHttpSessionImp, void*>(base, &WinHttpSessionImp::SetUserData, lpBuffer))
            return TRUE;

        if (CallMemberFunction<WinHttpRequestImp, void*>(base, &WinHttpRequestImp::SetUserData, lpBuffer))
            return TRUE;

        return FALSE;
    }
    else if (dwOption == WINHTTP_OPTION_SECURE_PROTOCOLS)
    {
        if (dwBufferLength != sizeof(DWORD))
            return FALSE;

        DWORD curlOffered = ConvertSecurityProtocol(*static_cast<DWORD*>(lpBuffer));
        if (CallMemberFunction<WinHttpSessionImp, DWORD>(base, &WinHttpSessionImp::SetSecureProtocol, &curlOffered))
            return TRUE;

        if (CallMemberFunction<WinHttpRequestImp, DWORD>(base, &WinHttpRequestImp::SetSecureProtocol, &curlOffered))
            return TRUE;

        return FALSE;
    }
    else if (dwOption == WINHTTP_OPTION_ENABLE_FEATURE)
    {
        if (dwBufferLength != sizeof(DWORD))
            return FALSE;

        DWORD dwEnableSSLRevocationOpt = *static_cast<DWORD*>(lpBuffer);

        if (dwEnableSSLRevocationOpt == WINHTTP_ENABLE_SSL_REVOCATION)
            return TRUE;

        return FALSE;
    }
    else if (dwOption == WINHTTP_OPTION_SECURITY_FLAGS)
    {
        WinHttpRequestImp *request;

        if (dwBufferLength != sizeof(DWORD))
            return FALSE;

        if ((request = dynamic_cast<WinHttpRequestImp *>(base)))
        {
            if (!lpBuffer)
                return FALSE;

            DWORD value = *static_cast<DWORD*>(lpBuffer);
            if (value == (SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE))
            {
                request->VerifyPeer() = 0L;
                request->VerifyHost() = 0L;
            }
            else if (value == SECURITY_FLAG_IGNORE_CERT_CN_INVALID)
            {
                request->VerifyPeer() = 1L;
                request->VerifyHost() = 0L;
            }
            else if (!value)
            {
                request->VerifyPeer() = 1L;
                request->VerifyHost() = 2L;
            }
            else
                return FALSE;

            return TRUE;
        }
        else
            return FALSE;
    }

    return FALSE;
}

WINHTTPAPI
WINHTTP_STATUS_CALLBACK
WINAPI
WinHttpSetStatusCallback
(
    HINTERNET hInternet,
    WINHTTP_STATUS_CALLBACK lpfnInternetCallback,
    DWORD dwNotificationFlags,
    DWORD_PTR dwReserved
)
{
  if (hInternet == NULL)
        return WINHTTP_INVALID_STATUS_CALLBACK;

    WinHttpBase *base = static_cast<WinHttpBase *>(hInternet);
    WinHttpSessionImp *session;
    WinHttpRequestImp *request;

    WINHTTP_STATUS_CALLBACK oldcb;
    DWORD olddwNotificationFlags;
    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)hInternet);

    if ((session = dynamic_cast<WinHttpSessionImp *>(base)))
    {
        oldcb = session->GetCallback(&olddwNotificationFlags);
        session->SetCallback(lpfnInternetCallback, dwNotificationFlags);
    }
    else if ((request = dynamic_cast<WinHttpRequestImp *>(base)))
    {
        oldcb = request->GetCallback(&olddwNotificationFlags);
        request->SetCallback(lpfnInternetCallback, dwNotificationFlags);
    }
    else
        return FALSE;

    return oldcb;
}

BOOLAPI
WinHttpQueryOption
(
    HINTERNET hInternet,
    DWORD dwOption,
    LPVOID lpBuffer,
    LPDWORD lpdwBufferLength
)
{
    WinHttpBase *base = static_cast<WinHttpBase *>(hInternet);
    WinHttpSessionImp *session;

    TRACE("%-35s:%-8d:%-16p\n", __func__, __LINE__, (void*)base);

    if (!base)
        return FALSE;

    if (WINHTTP_OPTION_CONNECT_TIMEOUT == dwOption)
    {
        if (SizeCheck(lpBuffer, lpdwBufferLength, sizeof(DWORD)) == FALSE)
            return FALSE;

        session = GetImp(base);
        if (!session)
            return FALSE;

        *static_cast<DWORD *>(lpBuffer) = session->GetConnectionTimeoutMs();
    }
    if (WINHTTP_OPTION_CALLBACK == dwOption)
    {
        if (SizeCheck(lpBuffer, lpdwBufferLength, sizeof(LPVOID)) == FALSE)
            return FALSE;

        session = GetImp(base);
        if (!session)
            return FALSE;

        DWORD dwNotificationFlags;
        WINHTTP_STATUS_CALLBACK cb = session->GetCallback(&dwNotificationFlags);
        *static_cast<WINHTTP_STATUS_CALLBACK *>(lpBuffer) = cb;
    }
    else if (WINHTTP_OPTION_URL == dwOption)
    {
        WinHttpRequestImp *request;

        if (!(request = dynamic_cast<WinHttpRequestImp *>(base)))
            return FALSE;

        char *url = NULL;
        curl_easy_getinfo(request->GetCurl(), CURLINFO_EFFECTIVE_URL, &url);
        if (!url)
            return FALSE;

        if (SizeCheck(lpBuffer, lpdwBufferLength, (strlen(url) + 1) * sizeof(TCHAR)) == FALSE)
            return FALSE;

        TCHAR *wbuffer = static_cast<TCHAR*>(lpBuffer);
        size_t length = strlen(url);
#ifdef UNICODE
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        std::wstring wc = conv.from_bytes(std::string(url));
        if (lpdwBufferLength && (*lpdwBufferLength < ((wc.length() + 1) * sizeof(TCHAR))))
            return FALSE;

        std::copy(wc.begin(), wc.end(), wbuffer);
#else
        std::string urlstr;
        urlstr.assign(url);
        std::copy(urlstr.begin(), urlstr.end(), wbuffer);
#endif
        wbuffer[strlen(url)] = TEXT('\0');
        if (lpdwBufferLength)
            *lpdwBufferLength = (DWORD)length;
    }
    else if (WINHTTP_OPTION_HTTP_VERSION == dwOption)
    {
        WinHttpRequestImp *request;

        if (!(request = dynamic_cast<WinHttpRequestImp *>(base)))
            return FALSE;

        if (SizeCheck(lpBuffer, lpdwBufferLength, sizeof(HTTP_VERSION_INFO)) == FALSE)
            return FALSE;

        HTTP_VERSION_INFO version;

#if (LIBCURL_VERSION_MAJOR>=7) && (LIBCURL_VERSION_MINOR >= 50)
        long curlversion;
        CURLcode rc;

        rc = curl_easy_getinfo(request->GetCurl(), CURLINFO_HTTP_VERSION, &curlversion);
        if (rc != CURLE_OK)
            return FALSE;


        if (curlversion == CURL_HTTP_VERSION_1_0)
        {
            version.dwMinorVersion = 0;
            version.dwMajorVersion = 1;
        }
        else if (curlversion == CURL_HTTP_VERSION_1_1)
        {
            version.dwMinorVersion = 1;
            version.dwMajorVersion = 1;
        }
        else if (curlversion == CURL_HTTP_VERSION_2_0)
        {
            version.dwMinorVersion = 0;
            version.dwMajorVersion = 2;
        }
        else
            return FALSE;
#else
        version.dwMinorVersion = 1;
        version.dwMajorVersion = 1;
#endif

        memcpy(lpBuffer, &version, sizeof(version));
        if (lpdwBufferLength)
            *lpdwBufferLength = (DWORD)sizeof(version);
    }

    return TRUE;
}

#ifdef CURL_SUPPORTS_URL_API
BOOLAPI WinHttpCrackUrl(
    LPCTSTR          pwszUrl,
    DWORD            dwUrlLength,
    DWORD            dwFlags,
    LPURL_COMPONENTS lpUrlComponents
)
{
    DWORD urlLen;

    if (!pwszUrl || !lpUrlComponents)
        return FALSE;

    if (!lpUrlComponents->dwStructSize)
        return FALSE;

    if (lpUrlComponents->dwStructSize != sizeof(*lpUrlComponents))
        return FALSE;

    if (dwUrlLength == 0)
        urlLen = WCTLEN(pwszUrl);
    else
        urlLen = dwUrlLength;

    std::string urlstr;
    ConvertCstrAssign(pwszUrl, urlLen, urlstr);

    CURLUcode rc;
    CURLU *url = curl_url();
    rc = curl_url_set(url, CURLUPART_URL, urlstr.c_str(), 0);
    if (rc)
        return FALSE;

    char *host;
    rc = curl_url_get(url, CURLUPART_HOST, &host, 0);
    if (!rc) {
        size_t pos = urlstr.find(host);
        if (pos != std::string::npos)
        {
            if (lpUrlComponents->dwHostNameLength != (DWORD)-1)
            {
                if (lpUrlComponents->dwHostNameLength >= (strlen(host) + 1)) {
                    TSTRING hoststr;
                    hoststr.assign((TCHAR*)pwszUrl + pos, strlen(host));

                    std::copy(hoststr.begin(), hoststr.end(), lpUrlComponents->lpszHostName);
                    lpUrlComponents->lpszHostName[strlen(host)] = TEXT('\0');
                    lpUrlComponents->dwHostNameLength = strlen(host);
                }
            }
            else
            {
                lpUrlComponents->lpszHostName = const_cast<TCHAR*>(pwszUrl) + pos;
                lpUrlComponents->dwHostNameLength = strlen(host);
            }
        }
        curl_free(host);
    }

    char *scheme;
    rc = curl_url_get(url, CURLUPART_SCHEME, &scheme, 0);
    if (!rc) {
        size_t pos = urlstr.find(scheme);
        if (pos != std::string::npos)
        {
            if (lpUrlComponents->dwSchemeLength != (DWORD)-1)
            {
                if (lpUrlComponents->dwSchemeLength >= (strlen(scheme) + 1)) {
                    TSTRING schemestr;
                    schemestr.assign((TCHAR*)pwszUrl + pos, strlen(scheme));

                    std::copy(schemestr.begin(), schemestr.end(), lpUrlComponents->lpszScheme);
                    lpUrlComponents->lpszScheme[strlen(scheme)] = TEXT('\0');
                    lpUrlComponents->dwSchemeLength = strlen(scheme);
                }
            }
            else
            {
                lpUrlComponents->lpszScheme = const_cast<TCHAR*>(pwszUrl) + pos;
                lpUrlComponents->dwSchemeLength = strlen(scheme);
            }

            if (strcmp(scheme, "http") == 0) {
                lpUrlComponents->nPort = 80;
                lpUrlComponents->nScheme = INTERNET_SCHEME_HTTP;
            }
            else if (strcmp(scheme, "https") == 0)
            {
                lpUrlComponents->nPort = 443;
                lpUrlComponents->nScheme = INTERNET_SCHEME_HTTPS;
            }
        }
        curl_free(scheme);
    }
    char *path;
    rc = curl_url_get(url, CURLUPART_PATH, &path, 0);
    if (!rc) {
        size_t pos = urlstr.find(path);
        if (pos != std::string::npos)
        {
            if (lpUrlComponents->dwUrlPathLength != (DWORD)-1)
            {
                if (lpUrlComponents->dwUrlPathLength >= (strlen(path) + 1)) {
                    TSTRING urlstr;
                    urlstr.assign((TCHAR*)pwszUrl + pos, strlen(path));

                    std::copy(urlstr.begin(), urlstr.end(), lpUrlComponents->lpszUrlPath);
                    lpUrlComponents->lpszUrlPath[strlen(path)] = TEXT('\0');
                    lpUrlComponents->dwUrlPathLength = strlen(path);
                }
            }
            else
            {
                if (strcmp(path, "/") == 0)
                {
                    lpUrlComponents->lpszUrlPath = (LPWSTR)TEXT("");
                    lpUrlComponents->dwUrlPathLength = 0;
                }
                else
                {
                    lpUrlComponents->lpszUrlPath = const_cast<TCHAR*>(pwszUrl) + pos;
                    lpUrlComponents->dwUrlPathLength = strlen(path);
                }
            }
        }
        curl_free(path);
    }
    char *query;
    rc = curl_url_get(url, CURLUPART_QUERY, &query, 0);
    if (!rc) {
        size_t pos = urlstr.find(query);
        if (pos != std::string::npos)
        {
            if (lpUrlComponents->dwExtraInfoLength != (DWORD)-1)
            {
                if (lpUrlComponents->dwExtraInfoLength >= (strlen(query) + 1)) {
                    TSTRING extrainfo;
                    extrainfo.assign((TCHAR*)pwszUrl + pos - 1, strlen(query));

                    std::copy(extrainfo.begin(), extrainfo.end(), lpUrlComponents->lpszExtraInfo);
                    lpUrlComponents->lpszExtraInfo[strlen(query)] = TEXT('\0');
                    lpUrlComponents->dwExtraInfoLength = strlen(query);
                }
            }
            else
            {
                lpUrlComponents->lpszExtraInfo = const_cast<TCHAR*>(pwszUrl) + pos - 1;
                lpUrlComponents->dwExtraInfoLength = strlen(query) + 1;
            }
        }
        curl_free(query);
    }
    char *user;
    rc = curl_url_get(url, CURLUPART_USER, &user, 0);
    if (!rc) {
        size_t pos = urlstr.find(user);
        if (pos != std::string::npos)
        {
            if (lpUrlComponents->dwUserNameLength != (DWORD)-1)
            {
                if (lpUrlComponents->dwUserNameLength >= (strlen(user) + 1)) {
                    TSTRING userstr;
                    userstr.assign((TCHAR*)pwszUrl + pos - 1, strlen(user));

                    std::copy(userstr.begin(), userstr.end(), lpUrlComponents->lpszUserName);
                    lpUrlComponents->lpszUserName[strlen(user)] = TEXT('\0');
                    lpUrlComponents->dwUserNameLength = strlen(user);
                }
            }
            else
            {
                lpUrlComponents->lpszUserName = const_cast<TCHAR*>(pwszUrl) + pos - 1;
                lpUrlComponents->dwUserNameLength = strlen(user);
            }
        }
        curl_free(user);
    }
    char *pw;
    rc = curl_url_get(url, CURLUPART_PASSWORD, &pw, 0);
    if (!rc) {
        size_t pos = urlstr.find(pw);
        if (pos != std::string::npos)
        {
            if (lpUrlComponents->dwPasswordLength != (DWORD)-1)
            {
                if (lpUrlComponents->dwPasswordLength >= (strlen(pw) + 1)) {
                    TSTRING pwstr;
                    pwstr.assign((TCHAR*)pwszUrl + pos - 1, strlen(pw));

                    std::copy(pwstr.begin(), pwstr.end(), lpUrlComponents->lpszPassword);

                    lpUrlComponents->lpszPassword[strlen(pw)] = TEXT('\0');
                    lpUrlComponents->dwPasswordLength = strlen(pw);
                }
            }
            else
            {
                lpUrlComponents->lpszPassword = const_cast<TCHAR*>(pwszUrl) + pos - 1;
                lpUrlComponents->dwPasswordLength = strlen(pw);
            }
        }
        curl_free(pw);
    }
    curl_url_cleanup(url);

    return TRUE;
}

#endif


BOOLAPI WinHttpWriteData
(
    HINTERNET hRequest,
    LPCVOID lpBuffer,
    DWORD dwNumberOfBytesToWrite,
    LPDWORD lpdwNumberOfBytesWritten
)
{
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(hRequest);
    if (!request)
        return FALSE;

    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return FALSE;

    if (request->GetClosing())
    {
        TRACE("%-35s:%-8d:%-16p \n", __func__, __LINE__, (void*)request);
        return FALSE;
    }

    TRACE("%-35s:%-8d:%-16p dwNumberOfBytesToWrite:%lu\n", __func__, __LINE__, (void*)request, dwNumberOfBytesToWrite);
    if (request->GetAsync())
    {
        {
            std::lock_guard<std::mutex> lck(request->GetReadDataEventMtx());
            QueueBufferRequest(request->GetOutstandingWrites(), const_cast<void*>(lpBuffer), dwNumberOfBytesToWrite);
            request->GetReadDataEventCounter()++;
        }

        ComContainer::GetInstance().ResumeTransfer(request->GetCurl(), CURLPAUSE_CONT);
    }
    else
    {
        request->AppendReadData(lpBuffer, dwNumberOfBytesToWrite);
        {
            std::lock_guard<std::mutex> lck(request->GetReadDataEventMtx());
            request->GetReadDataEventCounter()++;
        }
    }

    if (lpdwNumberOfBytesWritten)
        *lpdwNumberOfBytesWritten = dwNumberOfBytesToWrite;

    return TRUE;
}


BOOL WinHttpGetProxyForUrl(
    HINTERNET                 hSession,
    LPCTSTR                   lpcwszUrl,
    WINHTTP_AUTOPROXY_OPTIONS *pAutoProxyOptions,
    WINHTTP_PROXY_INFO       *pProxyInfo
)
{
    return FALSE;
}

BOOL WinHttpGetDefaultProxyConfiguration(WINHTTP_PROXY_INFO *pProxyInfo)
{
    return FALSE;
}

BOOL WinHttpGetIEProxyConfigForCurrentUser(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG *pProxyConfig)
{
    return FALSE;
}

BOOL WinHttpSetCredentials(
  HINTERNET hRequest,
  DWORD     AuthTargets,
  DWORD     AuthScheme,
  LPCTSTR   pwszUserName,
  LPCTSTR   pwszPassword,
  LPVOID    pAuthParams
)
{
    WinHttpRequestImp *request = static_cast<WinHttpRequestImp *>(hRequest);
    if (!request)
        return FALSE;

    std::shared_ptr<WinHttpRequestImp> srequest = request->shared_from_this();
    if (!srequest)
        return FALSE;

    std::string username;
    std::string password;

    ConvertCstrAssign(pwszUserName, WCTLEN(pwszUserName), username);
    ConvertCstrAssign(pwszPassword, WCTLEN(pwszPassword), password);

    std::string userpwd = username + ":" + password;

    if (WINHTTP_AUTH_TARGET_PROXY == AuthTargets)
    {
        curl_easy_setopt(request->GetCurl(), CURLOPT_PROXYUSERPWD, userpwd.c_str());
        curl_easy_setopt(request->GetCurl(), CURLOPT_PROXYAUTH, CURLAUTH_ANYSAFE);
    }
    else
    {
        curl_easy_setopt(request->GetCurl(), CURLOPT_USERPWD, userpwd.c_str());
        curl_easy_setopt(request->GetCurl(), CURLOPT_HTTPAUTH, CURLAUTH_ANYSAFE);
    }

    return TRUE;
}

BOOL WinHttpQueryAuthSchemes(
    HINTERNET hRequest,
    LPDWORD  lpdwSupportedSchemes,
    LPDWORD  lpdwFirstScheme,
    LPDWORD  pdwAuthTarget
)
{
    *lpdwSupportedSchemes = 0xFF;
    *lpdwFirstScheme = WINHTTP_AUTH_SCHEME_NEGOTIATE;
    *pdwAuthTarget = WINHTTP_AUTH_TARGET_SERVER;
    return TRUE;
}
