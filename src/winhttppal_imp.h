/***
 * Copyright (C) Microsoft. All rights reserved.
 * Licensed under the MIT license. See LICENSE.txt file in the project root for full license information.
 *
 * =+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+=+
 *
 * HTTP Library: Client-side APIs.
 *
 * This file contains WinHttpPAL class declarations.
 *
 * For the latest on this and related APIs, please see: https://github.com/microsoft/WinHttpPAL
 *
 * =-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-
 ****/

extern "C"
{
#include <curl/curl.h>
#include <curl/curlver.h>
}

#ifdef WIN32
#define THREAD_ID                               GetCurrentThreadId()
#define THREADPARAM                             LPVOID
#define CREATETHREAD(func, param, id) \
                                                CreateThread(                         \
                                                        NULL,                         \
                                                        0,                            \
                                                        (LPTHREAD_START_ROUTINE)func, \
                                                        param,                        \
                                                        0,                             \
                                                        id)

#define THREADJOIN(h)                           WaitForSingleObject(h, INFINITE);
#define THREADRETURN                            DWORD
#define THREAD_HANDLE                           HANDLE
#else
#define THREADPARAM                             void*
#define THREADRETURN                            void*
#define THREAD_ID                               pthread_self()
#define THREADJOIN(x)                           pthread_join(x, NULL)
#define THREAD_HANDLE                           pthread_t

typedef void* (*LPTHREAD_START_ROUTINE)(void *);
static inline THREAD_HANDLE CREATETHREAD(LPTHREAD_START_ROUTINE func, LPVOID param, pthread_t *id)
{
    pthread_t inc_x_thread;
    pthread_create(&inc_x_thread, NULL, func, param);
    return inc_x_thread;
}
#endif

class WinHttpSessionImp;

class WinHttpBase
{
public:
    virtual ~WinHttpBase() {}
};

class WinHttpSessionImp :public WinHttpBase
{
    std::string m_ServerName;
    std::string m_Referrer;
    std::vector<std::string> m_Proxies;
    std::string m_Proxy;
    WINHTTP_STATUS_CALLBACK m_InternetCallback = NULL;
    DWORD m_NotificationFlags = 0;

    int m_ServerPort = 0;
    long m_Timeout = 15000;
    BOOL m_Async = false;

    bool m_closing = false;
    DWORD m_MaxConnections = 0;
    DWORD m_SecureProtocol = 0;
    void *m_UserBuffer = NULL;

public:

    BOOL SetUserData(void **data)
    {
        if (!data)
            return FALSE;

        m_UserBuffer = *data;
        return TRUE;
    }
    void *GetUserData() { return m_UserBuffer; }

    BOOL SetSecureProtocol(DWORD *data)
    {
        if (!data)
            return FALSE;

        m_SecureProtocol = *data;
        return TRUE;
    }
    DWORD GetSecureProtocol() const { return m_SecureProtocol; }

    BOOL SetMaxConnections(DWORD *data)
    {
        if (!data)
            return FALSE;

        m_MaxConnections = *data;
        return TRUE;
    }
    DWORD GetMaxConnections() const { return m_MaxConnections; }

    void SetAsync() { m_Async = TRUE; }
    BOOL GetAsync() const { return m_Async; }

    BOOL GetThreadClosing() const { return m_closing; }

    std::vector<std::string> &GetProxies() { return m_Proxies; }
    void SetProxies(std::vector<std::string> &proxies) { m_Proxies = proxies; }

    std::string &GetProxy() { return m_Proxy; }

    int GetServerPort() const { return m_ServerPort; }
    void SetServerPort(int port) { m_ServerPort = port; }

    long GetTimeout() const;
    void SetTimeout(long timeout) { m_Timeout = timeout; }

    std::string &GetServerName() { return m_ServerName; }

    std::string &GetReferrer() { return m_Referrer; }

    void SetCallback(WINHTTP_STATUS_CALLBACK lpfnInternetCallback, DWORD dwNotificationFlags) {
        m_InternetCallback = lpfnInternetCallback;
        m_NotificationFlags = dwNotificationFlags;
    }
    WINHTTP_STATUS_CALLBACK GetCallback(DWORD *dwNotificationFlags)
    {
        if (dwNotificationFlags)
            *dwNotificationFlags = m_NotificationFlags;
        return m_InternetCallback;
    }

    ~WinHttpSessionImp();
};

class WinHttpConnectImp :public WinHttpBase
{
    WinHttpSessionImp *m_Handle = NULL;
    void *m_UserBuffer = NULL;

public:
    void SetHandle(WinHttpSessionImp *session) { m_Handle = session; }
    WinHttpSessionImp *GetHandle() { return m_Handle; }

    BOOL SetUserData(void **data)
    {
        if (!data)
            return FALSE;

        m_UserBuffer = *data;
        return TRUE;
    }
    void *GetUserData() { return m_UserBuffer; }
};

struct BufferRequest
{
    LPVOID m_Buffer = NULL;
    size_t  m_Length = 0;
    size_t  m_Used = 0;
};

class WinHttpRequestImp :public WinHttpBase, public std::enable_shared_from_this<WinHttpRequestImp>
{
    CURL *m_curl = NULL;
    std::vector<BYTE> m_ResponseString;
    std::string m_HeaderString;
    std::string m_Header;
    std::string m_FullPath;
    std::string m_OptionalData;
    size_t m_TotalSize = 0;
    size_t m_TotalReceiveSize = 0;
    std::vector<BYTE> m_ReadData;

    std::mutex m_ReadDataEventMtx;
    DWORD m_ReadDataEventCounter = 0;

    THREAD_HANDLE m_UploadCallbackThread;
    bool m_UploadThreadExitStatus = false;

    DWORD m_SecureProtocol = 0;
    DWORD m_MaxConnections = 0;

    std::string m_Type;
    LPVOID m_UserBuffer = NULL;
    bool m_HeaderReceiveComplete = false;

    struct curl_slist *m_HeaderList = NULL;
    WinHttpConnectImp *m_Session = NULL;
    std::mutex m_HeaderStringMutex;
    std::mutex m_BodyStringMutex;

    std::mutex m_QueryDataEventMtx;
    bool m_QueryDataEventState = false;
    std::atomic<bool> m_QueryDataPending;

    std::mutex m_ReceiveCompletionEventMtx;

    std::mutex m_ReceiveResponseMutex;
    std::atomic<bool> m_ReceiveResponsePending;
    std::atomic<int> m_ReceiveResponseEventCounter;
    int m_ReceiveResponseSendCounter = 0;

    std::atomic<bool> m_RedirectPending;

    bool m_closing = false;
    bool m_closed = false;
    bool m_Completion = false;
    bool m_Async = false;
    CURLcode m_CompletionCode = CURLE_OK;
    long m_VerifyPeer = 1;
    long m_VerifyHost = 2;
    bool m_Uploading = false;

    WINHTTP_STATUS_CALLBACK m_InternetCallback = NULL;
    DWORD m_NotificationFlags = 0;
    std::vector<BufferRequest> m_OutstandingWrites;
    std::vector<BufferRequest> m_OutstandingReads;
    bool m_Secure = false;

public:
    bool &GetSecure() { return m_Secure; }
    std::vector<BufferRequest> &GetOutstandingWrites() { return m_OutstandingWrites; }
    std::vector<BufferRequest> &GetOutstandingReads() { return m_OutstandingReads; }

    long &VerifyPeer() { return m_VerifyPeer; }
    long &VerifyHost() { return m_VerifyHost; }

    static size_t WriteHeaderFunction(void *ptr, size_t size, size_t nmemb, void* rqst);
    static size_t WriteBodyFunction(void *ptr, size_t size, size_t nmemb, void* rqst);
    void ConsumeIncoming(std::shared_ptr<WinHttpRequestImp> &srequest, void* &ptr, size_t &available, size_t &read);
    void FlushIncoming(std::shared_ptr<WinHttpRequestImp> &srequest);
    void SetCallback(WINHTTP_STATUS_CALLBACK lpfnInternetCallback, DWORD dwNotificationFlags) {
        m_InternetCallback = lpfnInternetCallback;
        m_NotificationFlags = dwNotificationFlags;
    }
    WINHTTP_STATUS_CALLBACK GetCallback(DWORD *dwNotificationFlags)
    {
        if (dwNotificationFlags)
            *dwNotificationFlags = m_NotificationFlags;
        return m_InternetCallback;
    }

    void SetAsync() { m_Async = TRUE; }
    BOOL GetAsync() { return m_Async; }

    WinHttpRequestImp();

    bool &GetQueryDataEventState() { return m_QueryDataEventState; }
    std::mutex &GetQueryDataEventMtx() { return m_QueryDataEventMtx; }

    bool HandleQueryDataNotifications(std::shared_ptr<WinHttpRequestImp> &, size_t available);
    void WaitAsyncQueryDataCompletion(std::shared_ptr<WinHttpRequestImp> &);

    void HandleReceiveNotifications(std::shared_ptr<WinHttpRequestImp> &srequest);
    void WaitAsyncReceiveCompletion(std::shared_ptr<WinHttpRequestImp> &srequest);

    CURLcode &GetCompletionCode() { return m_CompletionCode; }
    bool &GetCompletionStatus() { return m_Completion; }
    bool &GetClosing() { return m_closing; }
    bool &GetClosed() { return m_closed; }
    void CleanUp();
    ~WinHttpRequestImp();

    bool &GetUploadThreadExitStatus() { return m_UploadThreadExitStatus; }
    std::atomic <bool> &GetQueryDataPending() { return m_QueryDataPending; }

    THREAD_HANDLE &GetUploadThread() {
        return m_UploadCallbackThread;
    }

    static THREADRETURN UploadThreadFunction(THREADPARAM lpThreadParameter);

    // used to wake up CURL ReadCallback triggered on a upload request
    std::mutex &GetReadDataEventMtx() { return m_ReadDataEventMtx; }
    DWORD &GetReadDataEventCounter() { return m_ReadDataEventCounter; }

    std::mutex &GetReceiveCompletionEventMtx() { return m_ReceiveCompletionEventMtx; }

    // counters used to see which one of these events are observed: ResponseCallbackEventCounter
    // counters used to see which one of these events are broadcasted: ResponseCallbackSendCounter
    // WINHTTP_CALLBACK_STATUS_RECEIVING_RESPONSE
    // WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED
    // WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE
    //
    // These counters need to be 3 and identical under normal circumstances
    std::atomic<int> &ResponseCallbackEventCounter() { return m_ReceiveResponseEventCounter; }
    int &ResponseCallbackSendCounter() { return m_ReceiveResponseSendCounter; }
    std::atomic <bool> &GetReceiveResponsePending() { return m_ReceiveResponsePending; }
    std::mutex  &GetReceiveResponseMutex() { return m_ReceiveResponseMutex; }

    std::atomic<bool> &GetRedirectPending() { return m_RedirectPending; }

    std::mutex &GetHeaderStringMutex() { return m_HeaderStringMutex; }
    std::mutex &GetBodyStringMutex() { return m_BodyStringMutex; }

    BOOL AsyncQueue(std::shared_ptr<WinHttpRequestImp> &,
                    DWORD dwInternetStatus, size_t statusInformationLength,
            LPVOID statusInformation, DWORD statusInformationCopySize, bool allocate);

    void SetHeaderReceiveComplete() { m_HeaderReceiveComplete = TRUE; }

    BOOL SetSecureProtocol(DWORD *data)
    {
        if (!data)
            return FALSE;

        m_SecureProtocol = *data;
        return TRUE;
    }
    DWORD GetSecureProtocol() { return m_SecureProtocol; }

    BOOL SetMaxConnections(DWORD *data)
    {
        if (!data)
            return FALSE;

        m_MaxConnections = *data;
        return TRUE;
    }
    DWORD GetMaxConnections() { return m_MaxConnections; }

    BOOL SetUserData(void **data)
    {
        if (!data)
            return FALSE;

        m_UserBuffer = *data;
        return TRUE;
    }
    LPVOID GetUserData() { return m_UserBuffer; }

    void SetFullPath(std::string &server, std::string &relative)
    {
        m_FullPath.append(server);
        m_FullPath.append(relative);
    }
    std::string &GetFullPath() { return m_FullPath; }
    std::string &GetType() { return m_Type; }
    bool &Uploading() { return m_Uploading; }

    void SetSession(WinHttpConnectImp *connect) { m_Session = connect; }
    WinHttpConnectImp *GetSession() { return m_Session; }

    struct curl_slist *GetHeaderList() { return m_HeaderList; }

    void SetHeaderList(struct curl_slist *list) { m_HeaderList = list; }
    std::string &GetOutgoingHeaderList() { return m_Header; }

    void AddHeader(std::string &headers) {
        std::stringstream check1(headers);
        std::string str;

        while(getline(check1, str, '\n'))
        {
            str.erase(std::remove(str.begin(), str.end(), '\r'), str.end());
            SetHeaderList(curl_slist_append(GetHeaderList(), str.c_str()));
        }
    }

    std::vector<BYTE> &GetResponseString() { return m_ResponseString; }
    std::string &GetHeaderString() { return m_HeaderString; }
    CURL *GetCurl() { return m_curl; }

    BOOL SetProxy(std::vector<std::string> &proxies);
    BOOL SetServer(std::string &ServerName, int nServerPort);

    size_t &GetTotalLength() { return m_TotalSize; }
    size_t &GetReadLength() { return m_TotalReceiveSize; }

    std::string &GetOptionalData() { return m_OptionalData; }
    BOOL SetOptionalData(void *lpOptional, size_t dwOptionalLength)
    {
        if (!lpOptional || !dwOptionalLength)
            return FALSE;

        m_OptionalData.assign(&(static_cast<char*>(lpOptional))[0], dwOptionalLength);
        return TRUE;
    }

    std::vector<BYTE> &GetReadData() { return m_ReadData; }

    void AppendReadData(const void *data, size_t len)
    {
        std::lock_guard<std::mutex> lck(GetReadDataEventMtx());
        m_ReadData.insert(m_ReadData.end(), static_cast<const BYTE*>(data), static_cast<const BYTE*>(data) + len);
    }

    static int SocketCallback(CURL *handle, curl_infotype type,
        char *data, size_t size,
        void *userp);

    static size_t ReadCallback(void *ptr, size_t size, size_t nmemb, void *userp);
};

class UserCallbackContext
{
    typedef void (*CompletionCb)(std::shared_ptr<WinHttpRequestImp> &, DWORD status);

    std::shared_ptr<WinHttpRequestImp> m_request;
    DWORD m_dwInternetStatus;
    DWORD m_dwStatusInformationLength;
    DWORD m_dwNotificationFlags;
    WINHTTP_STATUS_CALLBACK m_cb;
    LPVOID m_userdata;
    LPVOID m_StatusInformationVal = NULL;
    BYTE* m_StatusInformation = NULL;
    bool m_allocate = FALSE;
    BOOL m_AsyncResultValid = false;
    CompletionCb m_requestCompletionCb;

    BOOL SetAsyncResult(LPVOID statusInformation, DWORD statusInformationCopySize, bool allocate) {
        if (allocate)
        {
            m_StatusInformation = new BYTE[statusInformationCopySize];
            if (m_StatusInformation)
            {
                memcpy(m_StatusInformation, statusInformation, statusInformationCopySize);
            }
            else
                return FALSE;
        }
        else
        {
            m_StatusInformationVal = statusInformation;
        }
        m_AsyncResultValid = true;
        m_allocate = allocate;

        return TRUE;
    }

public:   
    UserCallbackContext(std::shared_ptr<WinHttpRequestImp> &request,
        DWORD dwInternetStatus,
        DWORD dwStatusInformationLength,
        DWORD dwNotificationFlags,
        WINHTTP_STATUS_CALLBACK cb,
        LPVOID userdata,
        LPVOID statusInformation,
        DWORD statusInformationCopySize, bool allocate,
        CompletionCb completion)
        : m_request(request), m_dwInternetStatus(dwInternetStatus),
        m_dwStatusInformationLength(dwStatusInformationLength),
        m_dwNotificationFlags(dwNotificationFlags), m_cb(cb), m_userdata(userdata),
        m_requestCompletionCb(completion)
    {
        if (statusInformation)
            SetAsyncResult(statusInformation, statusInformationCopySize, allocate);
    }

    ~UserCallbackContext()
    {
        delete [] m_StatusInformation;
    }
    
    std::shared_ptr<WinHttpRequestImp> &GetRequestRef() { return m_request; }
    WinHttpRequestImp *GetRequest() { return m_request.get(); }
    DWORD GetInternetStatus() const { return m_dwInternetStatus; }
    DWORD GetStatusInformationLength() const { return m_dwStatusInformationLength; }
    DWORD GetNotificationFlags() const { return m_dwNotificationFlags; }
    WINHTTP_STATUS_CALLBACK &GetCb() { return m_cb; }
    CompletionCb &GetRequestCompletionCb() { return m_requestCompletionCb; }
    LPVOID GetUserdata() { return m_userdata; }
    LPVOID GetStatusInformation() {
        if (m_allocate)
            return m_StatusInformation;
        else
            return m_StatusInformationVal;
    }
    BOOL GetStatusInformationValid() const { return m_AsyncResultValid; }

private:
    UserCallbackContext(const UserCallbackContext&);
    UserCallbackContext& operator=(const UserCallbackContext&);
};

template<class T>
class WinHttpHandleContainer
{
    std::mutex m_ActiveRequestMtx;
    std::vector<std::shared_ptr<T>> m_ActiveRequests;

public:
    static WinHttpHandleContainer &Instance()
    {
        static WinHttpHandleContainer the_instance;
        return the_instance;
    }

    void UnRegister(T *val);
    bool IsRegistered(T *val);
    void Register(std::shared_ptr<T> rqst);
};


class UserCallbackContainer
{
    typedef std::vector<UserCallbackContext*> UserCallbackQueue;

    UserCallbackQueue m_Queue;

    // used to protect GetCallbackMap()
    std::mutex m_MapMutex;

    THREAD_HANDLE m_hThread;

    // used by UserCallbackThreadFunction as a wake-up signal
    std::mutex m_hEventMtx;
    std::condition_variable m_hEvent;

    // cleared by UserCallbackThreadFunction, set by multiple event providers
    std::atomic<DWORD> m_EventCounter;

    bool m_closing = false;
    UserCallbackQueue &GetCallbackQueue() { return m_Queue; }


public:

    bool GetClosing() const { return m_closing; }

    UserCallbackContext* GetNext();

    static THREADRETURN UserCallbackThreadFunction(LPVOID lpThreadParameter);

    BOOL Queue(UserCallbackContext *ctx);
    void DrainQueue();

    UserCallbackContainer(): m_EventCounter(0)
    {
        DWORD thread_id;
        m_hThread = CREATETHREAD(
            UserCallbackThreadFunction,       // thread function name
            this,          // argument to thread function
            &thread_id
        );
    }

    ~UserCallbackContainer()
    {
        m_closing = true;
        {
            std::lock_guard<std::mutex> lck(m_hEventMtx);
            m_EventCounter++;
            m_hEvent.notify_all();
        }
        THREADJOIN(m_hThread);
        DrainQueue();
    }

    static UserCallbackContainer &GetInstance()
    {
        static UserCallbackContainer the_instance;
        return the_instance;
    }
private:
    UserCallbackContainer(const UserCallbackContainer&);
    UserCallbackContainer& operator=(const UserCallbackContainer&);
};

class EnvInit
{
public:
    EnvInit();
};

class ComContainer
{
    THREAD_HANDLE m_hAsyncThread;
    std::mutex m_hAsyncEventMtx;

    // used to wake up the Async Thread
    // Set by external components, cleared by the Async thread
    std::atomic<DWORD> m_hAsyncEventCounter;
    std::condition_variable m_hAsyncEvent;
    std::mutex m_MultiMutex;
    CURLM *m_curlm = NULL;
    std::vector< CURL *> m_ActiveCurl;
    std::vector<std::shared_ptr<WinHttpRequestImp>> m_ActiveRequests;
    BOOL m_closing = FALSE;

    // used to protect CURLM data structures
    BOOL GetThreadClosing() const { return m_closing; }

public:
    CURL *AllocCURL();
    void FreeCURL(CURL *ptr);
    static ComContainer &GetInstance();
    void ResumeTransfer(CURL *handle, int bitmask);
    BOOL AddHandle(std::shared_ptr<WinHttpRequestImp> &srequest, CURL *handle);
    BOOL RemoveHandle(std::shared_ptr<WinHttpRequestImp> &srequest, CURL *handle, bool clearPrivate);
    long GetTimeout();
    void KickStart();
    ComContainer();
    ~ComContainer();

    static THREADRETURN AsyncThreadFunction(THREADPARAM lpThreadParameter);

    int QueryData(int *still_running);
private:
    ComContainer(const ComContainer&);
    ComContainer& operator=(const ComContainer&);
};
