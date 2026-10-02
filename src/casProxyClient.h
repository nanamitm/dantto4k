#pragma once
#include <thread>
#include <optional>
#include <memory>
#include <map>
#include <asio.hpp>
#include <winscard.h>
#include "casProxy.h"
#include <deque>

class CasProxyClient {
public:
    CasProxyClient(const std::string& host, uint16_t port);
    ~CasProxyClient();
    void connect();
    void close();
    LONG scardEstablishContext(DWORD dwScope, LPCVOID pvReserved1, LPCVOID pvReserved2, LPSCARDCONTEXT phContext);
    LONG scardReleaseContext(SCARDCONTEXT hContext);
    LONG scardListReaders(SCARDCONTEXT hContext, LPCSTR mszGroups, LPSTR mszReaders, LPDWORD pcchReaders);
    LONG scardConnect(SCARDCONTEXT hContext, LPCSTR szReader, DWORD dwShareMode, DWORD dwPreferredProtocols, LPSCARDHANDLE phCard, LPDWORD pdwActiveProtocol);
    LONG scardReconnect(SCARDHANDLE hCard, DWORD dwShareMode, DWORD dwPreferredProtocols, DWORD dwInitialization, LPDWORD pdwActiveProtocol);
    LONG scardDisconnect(SCARDHANDLE hCard, DWORD dwDisposition);
    LONG scardBeginTransaction(SCARDHANDLE hCard);
    LONG scardEndTransaction(SCARDHANDLE hCard, DWORD dwDisposition);
    LONG scardTransmit(SCARDHANDLE hCard, LPCSCARD_IO_REQUEST pioSendPci, LPCBYTE pbSendBuffer, DWORD cbSendLength, LPSCARD_IO_REQUEST pioRecvPci, LPBYTE pbRecvBuffer, LPDWORD pcbRecvLength);
    LONG scardGetAttrib(SCARDHANDLE hCard, DWORD dwAttrId, LPBYTE pbAttr, LPDWORD pcbAttrLen);
    LONG scardFreeMemory(SCARDCONTEXT hContext, LPCVOID pvMem);

private:
    static constexpr int kConnectionTimeoutMs = 5000;
    static constexpr int kRequestTimeoutMs = 5000;

    void doWrite();
    void doRead();
    void readPacketData(uint32_t length);
    void handleResponse(const std::vector<uint8_t>& data);
    void ensureConnected(std::function<void(bool)> completion);
    void disconnect();
    void onOpen();
    void onFail();
    std::shared_ptr<casproxy::ResponseBase> sendRequest(casproxy::RequestBase& res);

    asio::io_context io_context;
    // Public requests are serialized; all connection/queue/promise state below
    // is owned exclusively by the io_context thread.
    std::mutex mutex;
    std::thread thread;
    std::map<uint32_t, std::shared_ptr<std::promise<std::shared_ptr<casproxy::ResponseBase>>>> mapResponsePromise;
    std::vector<std::function<void(bool)>> connectionWaiters;
    bool connected = false;
    bool connecting{false};
    uint64_t connectionGeneration = 0;
    std::atomic<uint32_t> packetId{0};
    std::string host;
    uint16_t port;
    asio::ip::tcp::resolver resolver;
    std::shared_ptr<asio::ip::tcp::socket> socket;
    asio::steady_timer connectionTimer;
    asio::executor_work_guard<asio::io_context::executor_type> workGuard;
    std::deque<std::shared_ptr<std::vector<uint8_t>>> sendQueue;

};
