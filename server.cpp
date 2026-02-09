#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <sstream>
#include <algorithm>

#pragma comment(lib, "Ws2_32.lib")

using namespace std;

struct Client {
    SOCKET sock;
    char name[1024];
};

vector<Client*> clients;
CRITICAL_SECTION cs;

static bool sendAll(SOCKET s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n == SOCKET_ERROR || n == 0) return false;
        sent += n;
    }
    return true;
}

static bool startsWith(const string& s, const string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

static vector<string> splitByChar(const string& s, char delim) {
    vector<string> out;
    string cur;
    for (char c : s) {
        if (c == delim) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

static void trimCRLF(string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

static Client* findClientByNameUnsafe(const string& name) {
    for (auto* c : clients) {
        if (strcmp(c->name, name.c_str()) == 0) return c;
    }
    return nullptr;
}

/* Server -> Client framing
   Always send 1024 bytes name + 1024 bytes message*/

static void sendFrame(SOCKET s, const string& name, const string& msg) {
    char nameBuf[1024] = {0};
    char msgBuf[1024]  = {0};

    strncpy_s(nameBuf, name.c_str(), sizeof(nameBuf) - 1);
    strncpy_s(msgBuf,  msg.c_str(),  sizeof(msgBuf) - 1);

    sendAll(s, nameBuf, 1024);
    sendAll(s, msgBuf,  1024);
}

static void sendSystem(SOCKET s, const string& msg) {
    string m = msg;
    if (m.empty() || m.back() != '\n') m += "\n";
    sendFrame(s, "SYSTEM", m);
}

/* File transfer state */

struct TransferInfo {
    int id;
    SOCKET senderSock;
    SOCKET receiverSock;
    string senderName;
    string receiverName;
    string filename;
    int fileSize;
    bool active;
};

static unordered_map<int, TransferInfo> gTransfers;

static void sendOfferToReceiver(const TransferInfo& t) {
    // FILE_OFFER|id|from|filename|size
    stringstream ss;
    ss << "FILE_OFFER|" << t.id << "|" << t.senderName << "|" << t.filename << "|" << t.fileSize;
    sendSystem(t.receiverSock, ss.str());
}

static void notifySenderAccepted(const TransferInfo& t) {
    // FILE_ACCEPTED|id
    stringstream ss;
    ss << "FILE_ACCEPTED|" << t.id;
    sendSystem(t.senderSock, ss.str());
}

// include who declined + filename
static void notifySenderDeclined(const TransferInfo& t) {
    // FILE_DECLINED|id|declinedBy|filename
    stringstream ss;
    ss << "FILE_DECLINED|" << t.id << "|" << t.receiverName << "|" << t.filename;
    sendSystem(t.senderSock, ss.str());
}

static void notifySenderSent(const TransferInfo& t) {
    // FILE_SENT|id|receiver|filename
    stringstream ss;
    ss << "FILE_SENT|" << t.id << "|" << t.receiverName << "|" << t.filename;
    sendSystem(t.senderSock, ss.str());
}

static void notifyReceiverDownloaded(const TransferInfo& t) {
    // FILE_DOWNLOADED|id|filename
    stringstream ss;
    ss << "FILE_DOWNLOADED|" << t.id << "|" << t.filename;
    sendSystem(t.receiverSock, ss.str());
}

/* Relay file bytes with support for already-buffered bytes*/

static bool relayFileBytesWithPrefetch(
    SOCKET sender,
    SOCKET receiver,
    int fileSize,
    string& inBufAfterHeader
) {
    int remaining = fileSize;

    if (!inBufAfterHeader.empty()) {
        int take = min((int)inBufAfterHeader.size(), remaining);
        if (take > 0) {
            if (!sendAll(receiver, inBufAfterHeader.data(), take)) return false;
            inBufAfterHeader.erase(0, take);
            remaining -= take;
        }
    }

    const int CHUNK = 4096;
    vector<char> buf(CHUNK);

    while (remaining > 0) {
        int toRead = min(CHUNK, remaining);
        int n = recv(sender, buf.data(), toRead, 0);
        if (n <= 0) return false;
        if (!sendAll(receiver, buf.data(), n)) return false;
        remaining -= n;
    }

    return true;
}

/* Per-client incoming line parser */

static bool extractLine(string& inBuf, string& outLine) {
    size_t pos = inBuf.find('\n');
    if (pos == string::npos) return false;
    outLine = inBuf.substr(0, pos + 1);
    inBuf.erase(0, pos + 1);
    trimCRLF(outLine);
    return true;
}

DWORD WINAPI ClientHandler(LPVOID lpParam) {
    Client* cl = (Client*)lpParam;
    SOCKET client = cl->sock;

    string inBuf;
    const int CHUNK = 4096;
    vector<char> temp(CHUNK);

    while (true) {
        int n = recv(client, temp.data(), CHUNK, 0);
        if (n <= 0) {
            cout << cl->name << " disconnected.\n";

            char bufdis[1024];
            sprintf_s(bufdis, "%s has disconnected.", cl->name);

            EnterCriticalSection(&cs);
            for (auto* other : clients) {
                if (other->sock != cl->sock) {
                    sendSystem(other->sock, bufdis);
                }
            }
            LeaveCriticalSection(&cs);
            break;
        }

        inBuf.append(temp.data(), n);

        string line;
        while (extractLine(inBuf, line)) {
            if (line.empty()) continue;

            // /send|baseId|filename|size (broadcast offer)
            if (startsWith(line, "/send|")) {
                auto parts = splitByChar(line, '|');
                if (parts.size() == 4) {
                    int baseId = atoi(parts[1].c_str());
                    string filename = parts[2];
                    int fsize = atoi(parts[3].c_str());

                    EnterCriticalSection(&cs);
                    string senderName = cl->name;

                    for (auto* other : clients) {
                        if (other->sock == cl->sock) continue;

                        int rid = baseId ^ (int)(uintptr_t)other->sock;

                        TransferInfo t;
                        t.id = rid;
                        t.senderSock = cl->sock;
                        t.receiverSock = other->sock;
                        t.senderName = senderName;
                        t.receiverName = other->name;
                        t.filename = filename;
                        t.fileSize = fsize;
                        t.active = false;

                        gTransfers[rid] = t;
                        sendOfferToReceiver(t);

                        // FILE_IDMAP|baseId|rid|receiverName
                        {
                            stringstream ss;
                            ss << "FILE_IDMAP|" << baseId << "|" << rid << "|" << other->name;
                            sendSystem(cl->sock, ss.str());
                        }
                    }
                    LeaveCriticalSection(&cs);
                } else {
                    sendSystem(cl->sock, "Invalid /send format.");
                }
                continue;
            }

            // /sendto|username|id|filename|size (private offer)
            if (startsWith(line, "/sendto|")) {
                auto parts = splitByChar(line, '|');
                if (parts.size() == 5) {
                    string targetName = parts[1];
                    int id = atoi(parts[2].c_str());
                    string filename = parts[3];
                    int fsize = atoi(parts[4].c_str());

                    EnterCriticalSection(&cs);
                    Client* target = findClientByNameUnsafe(targetName);
                    string senderName = cl->name;

                    if (!target) {
                        LeaveCriticalSection(&cs);
                        sendSystem(cl->sock, "User not found.");
                    } else if (target->sock == cl->sock) {
                        LeaveCriticalSection(&cs);
                        sendSystem(cl->sock, "Cannot send file to yourself.");
                    } else {
                        TransferInfo t;
                        t.id = id;
                        t.senderSock = cl->sock;
                        t.receiverSock = target->sock;
                        t.senderName = senderName;
                        t.receiverName = target->name;
                        t.filename = filename;
                        t.fileSize = fsize;
                        t.active = false;

                        gTransfers[id] = t;
                        sendOfferToReceiver(t);

                        LeaveCriticalSection(&cs);
                    }
                } else {
                    sendSystem(cl->sock, "Invalid private send format.");
                }
                continue;
            }

            // /accept|id
            if (startsWith(line, "/accept|")) {
                auto parts = splitByChar(line, '|');
                if (parts.size() == 2) {
                    int id = atoi(parts[1].c_str());

                    EnterCriticalSection(&cs);
                    auto it = gTransfers.find(id);
                    if (it == gTransfers.end()) {
                        LeaveCriticalSection(&cs);
                        sendSystem(cl->sock, "No such transfer.");
                    } else {
                        TransferInfo& t = it->second;
                        if (t.receiverSock != cl->sock) {
                            LeaveCriticalSection(&cs);
                            sendSystem(cl->sock, "This transfer is not for you.");
                        } else {
                            t.active = true;
                            LeaveCriticalSection(&cs);
                            sendSystem(cl->sock, "Accepted.");
                            notifySenderAccepted(t);
                        }
                    }
                } else {
                    sendSystem(cl->sock, "Invalid /accept format.");
                }
                continue;
            }

            // /decline|id
            if (startsWith(line, "/decline|")) {
                auto parts = splitByChar(line, '|');
                if (parts.size() == 2) {
                    int id = atoi(parts[1].c_str());

                    EnterCriticalSection(&cs);
                    auto it = gTransfers.find(id);
                    if (it == gTransfers.end()) {
                        LeaveCriticalSection(&cs);
                        sendSystem(cl->sock, "No such transfer.");
                    } else {
                        TransferInfo t = it->second;
                        if (t.receiverSock != cl->sock) {
                            LeaveCriticalSection(&cs);
                            sendSystem(cl->sock, "This transfer is not for you.");
                        } else {
                            gTransfers.erase(it);
                            LeaveCriticalSection(&cs);
                            sendSystem(cl->sock, "Declined.");
                            notifySenderDeclined(t); // now includes receiver name
                        }
                    }
                } else {
                    sendSystem(cl->sock, "Invalid /decline format.");
                }
                continue;
            }

            // FILE_BEGIN|id|filename|size then raw bytes
            if (startsWith(line, "FILE_BEGIN|")) {
                auto parts = splitByChar(line, '|');
                if (parts.size() == 4) {
                    int id = atoi(parts[1].c_str());
                    string filename = parts[2];
                    int fsize = atoi(parts[3].c_str());

                    TransferInfo tCopy;
                    bool ok = false;

                    EnterCriticalSection(&cs);
                    auto it = gTransfers.find(id);
                    if (it != gTransfers.end()) {
                        TransferInfo& t = it->second;
                        if (t.senderSock == cl->sock && t.active) {
                            t.filename = filename;
                            t.fileSize = fsize;
                            tCopy = t;
                            ok = true;
                        }
                    }
                    LeaveCriticalSection(&cs);

                    if (!ok) {
                        sendSystem(cl->sock, "Transfer not active or invalid id.");
                        continue;
                    }

                    // Tell receiver to start reading bytes
                    {
                        stringstream ss;
                        ss << "FILE_START|" << tCopy.id << "|" << tCopy.senderName << "|" << tCopy.filename << "|" << tCopy.fileSize;
                        sendSystem(tCopy.receiverSock, ss.str());
                    }

                    bool relayed = relayFileBytesWithPrefetch(
                        tCopy.senderSock,
                        tCopy.receiverSock,
                        tCopy.fileSize,
                        inBuf
                    );

                    EnterCriticalSection(&cs);
                    gTransfers.erase(tCopy.id);
                    LeaveCriticalSection(&cs);

                    if (relayed) {
                        notifySenderSent(tCopy);
                        notifyReceiverDownloaded(tCopy);
                    } else {
                        sendSystem(tCopy.senderSock, "File transfer failed.");
                        sendSystem(tCopy.receiverSock, "File transfer failed.");
                    }
                } else {
                    sendSystem(cl->sock, "Invalid FILE_BEGIN format.");
                }
                continue;
            }

            // Private message: @username message
            if (!line.empty() && line[0] == '@') {
                string str = line;
                str.erase(0, 1);
                int pos = (int)str.find(' ');

                if (pos == (int)string::npos || pos == 0) {
                    sendSystem(cl->sock, "Invalid format. Use: @username message");
                } else {
                    string name = str.substr(0, pos);
                    string message = str.substr(pos + 1);

                    bool found = false;

                    EnterCriticalSection(&cs);
                    for (auto* c : clients) {
                        if (strcmp(c->name, name.c_str()) == 0) {
                            if (c->sock == cl->sock) {
                                sendSystem(cl->sock, "Cannot message yourself.");
                            } else {
                                char toRecipient[1024];
                                sprintf_s(toRecipient, "%s -> You", cl->name);
                                sendFrame(c->sock, toRecipient, message);

                                char toSender[1024];
                                sprintf_s(toSender, "You -> %s", c->name);
                                sendFrame(cl->sock, toSender, message);

                                cout << "[" << cl->name << " -> " << c->name << "]: " << message << "\n";
                            }
                            found = true;
                            break;
                        }
                    }
                    LeaveCriticalSection(&cs);

                    if (!found) sendSystem(cl->sock, "User not found.");
                }

                continue;
            }

            // Public message
            cout << "[" << cl->name << "]: " << line << "\n";

            EnterCriticalSection(&cs);
            for (auto* other : clients) {
                if (other->sock != cl->sock) {
                    sendFrame(other->sock, cl->name, line);
                }
            }
            LeaveCriticalSection(&cs);
        }
    }

    EnterCriticalSection(&cs);
    for (int i = 0; i < (int)clients.size(); i++) {
        if (clients[i]->sock == cl->sock) {
            clients.erase(clients.begin() + i);
            break;
        }
    }
    LeaveCriticalSection(&cs);

    closesocket(client);
    delete cl;
    return 0;
}

int main() {
    WSADATA wsa;
    SOCKET serverSock, clientSock;
    sockaddr_in serverAddr, clientAddr;
    int clientSize = sizeof(clientAddr);

    char buffName[1025];

    InitializeCriticalSection(&cs);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        cout << "WSAStartup failed\n";
        return 1;
    }

    serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock == INVALID_SOCKET) {
        cout << "Could not create socket\n";
        return 1;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(9000);

    if (bind(serverSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cout << "bind() failed: " << WSAGetLastError() << "\n";
        closesocket(serverSock);
        WSACleanup();
        return 0;
    }

    if (listen(serverSock, SOMAXCONN) == SOCKET_ERROR) {
        cout << "Listen() error: " << WSAGetLastError() << "\n";
    }

    cout << "Server started. Waiting for clients...\n";

    while (true) {
        clientSock = accept(serverSock, (sockaddr*)&clientAddr, &clientSize);

        if (clientSock == INVALID_SOCKET) {
            cout << "Accept failed\n";
            continue;
        }

        int namebytes = recv(clientSock, buffName, 1024, 0);
        if (namebytes <= 0) {
            closesocket(clientSock);
            continue;
        }
        buffName[namebytes] = '\0';

        cout << buffName << " joined.\n";

        Client* cl = new Client;
        cl->sock = clientSock;
        strcpy_s(cl->name, buffName);

        EnterCriticalSection(&cs);
        clients.push_back(cl);
        LeaveCriticalSection(&cs);

        char buffConName[1024];
        sprintf_s(buffConName, "%s has connected.", buffName);

        EnterCriticalSection(&cs);
        for (auto* c : clients) {
            sendSystem(c->sock, buffConName);
        }
        LeaveCriticalSection(&cs);

        HANDLE hThread = CreateThread(NULL, 0, ClientHandler, (LPVOID)cl, 0, NULL);
        CloseHandle(hThread);
    }

    DeleteCriticalSection(&cs);
    closesocket(serverSock);
    WSACleanup();
    return 0;
}
