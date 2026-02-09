#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")

using namespace std;

struct ClientData {
    SOCKET sock;
    char name[1024];
    bool isRunning;
};

/* Helpers */

static bool sendAll(SOCKET s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n == SOCKET_ERROR || n == 0) return false;
        sent += n;
    }
    return true;
}

static bool recvAll(SOCKET s, char* data, int len) {
    int got = 0;
    while (got < len) {
        int n = recv(s, data + got, len - got, 0);
        if (n <= 0) return false;
        got += n;
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

static string getBaseName(const string& path) {
    size_t p1 = path.find_last_of("\\/");
    if (p1 == string::npos) return path;
    return path.substr(p1 + 1);
}

static bool fileSizeOf(const string& path, int& outSize) {
    ifstream f(path, ios::binary);
    if (!f.is_open()) return false;
    f.seekg(0, ios::end);
    long long sz = (long long)f.tellg();
    f.close();
    if (sz < 0 || sz > INT_MAX) return false;
    outSize = (int)sz;
    return true;
}

static bool ensureDirExists(const string& folder) {
    DWORD attr = GetFileAttributesA(folder.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    if (CreateDirectoryA(folder.c_str(), NULL)) return true;
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return true;
    return false;
}

/* Help text */

static void printHelp(const char* myname) {
    cout << "\nCommands\n";
    cout << "Public chat\n";
    cout << "  type any text and press enter\n\n";
    cout << "Private message\n";
    cout << "  @username message\n";
    cout << "  example: @sumit hello\n\n";
    cout << "Send file (public)\n";
    cout << "  /send C:\\path\\file.txt\n\n";
    cout << "Send file (private)\n";
    cout << "  /send @username C:\\path\\file.txt\n";
    cout << "  example: /send @sumit C:\\Users\\me\\Desktop\\a.txt\n\n";
    cout << "When you receive a file offer\n";
    cout << "  /accept   downloads the next pending offer\n";
    cout << "  /decline  declines the next pending offer\n\n";
    cout << "Help\n";
    cout << "  /help\n\n";
    cout << myname << ": ";
    cout.flush();
}

/*  File transfer support */

static CRITICAL_SECTION gFileCs;
static unordered_map<int, string> gOutgoingFilePath;
static unordered_map<int, string> gIdMapInfo;
static string gSystemLineBuffer;

struct PendingOffer {
    int id;
    string from;
    string filename;
    int size;
};

static CRITICAL_SECTION gOfferCs;
static vector<PendingOffer> gPendingOffers;

static int makeTransferId() {
    return (int)(GetTickCount() ^ GetCurrentThreadId());
}

static bool sendFileBeginAndBytes(SOCKET sock, int id, const string& filename, int size, const string& path) {
    {
        stringstream ss;
        ss << "FILE_BEGIN|" << id << "|" << filename << "|" << size << "\n";
        string header = ss.str();
        if (!sendAll(sock, header.c_str(), (int)header.size())) return false;
    }

    ifstream f(path, ios::binary);
    if (!f.is_open()) return false;

    const int CHUNK = 4096;
    vector<char> buf(CHUNK);
    int remaining = size;

    while (remaining > 0) {
        int toRead = min(CHUNK, remaining);
        f.read(buf.data(), toRead);
        int got = (int)f.gcount();
        if (got <= 0) { f.close(); return false; }
        if (!sendAll(sock, buf.data(), got)) { f.close(); return false; }
        remaining -= got;
    }

    f.close();
    return true;
}

static bool receiveFileBytesToDisk(SOCKET sock, int size, const string& savePath) {
    ofstream out(savePath, ios::binary);
    if (!out.is_open()) return false;

    const int CHUNK = 4096;
    vector<char> buf(CHUNK);
    int remaining = size;

    while (remaining > 0) {
        int toRead = min(CHUNK, remaining);
        int n = recv(sock, buf.data(), toRead, 0);
        if (n <= 0) { out.close(); return false; }
        out.write(buf.data(), n);
        remaining -= n;
    }

    out.close();
    return true;
}

static void printOfferUI(const PendingOffer& o, const char* myname) {
    cout << "\r";
    cout << ">>> Incoming file\n";
    cout << ">>> From: " << o.from << "\n";
    cout << ">>> Name: " << o.filename << "\n";
    cout << ">>> Size: " << o.size << " bytes\n";
    cout << ">>> Type /accept to download, or /decline to reject\n";
    cout << myname << ": ";
    cout.flush();
}

static bool popNextOffer(PendingOffer& out) {
    EnterCriticalSection(&gOfferCs);
    if (gPendingOffers.empty()) {
        LeaveCriticalSection(&gOfferCs);
        return false;
    }
    out = gPendingOffers.front();
    gPendingOffers.erase(gPendingOffers.begin());
    LeaveCriticalSection(&gOfferCs);
    return true;
}

/* Receiver thread */

DWORD WINAPI RecvHandler(LPVOID lpParam) {
    ClientData* client = (ClientData*)lpParam;

    char recvName[1025];
    char recvBuffer[1025];

    while (client->isRunning) {
        if (!recvAll(client->sock, recvName, 1024)) {
            cout << "\nServer disconnected.\n";
            client->isRunning = false;
            break;
        }
        recvName[1024] = '\0';

        if (!recvAll(client->sock, recvBuffer, 1024)) {
            cout << "\nServer disconnected.\n";
            client->isRunning = false;
            break;
        }
        recvBuffer[1024] = '\0';

        if (strcmp(recvName, "SYSTEM") == 0) {
            gSystemLineBuffer += recvBuffer;

            size_t pos;
            while ((pos = gSystemLineBuffer.find('\n')) != string::npos) {
                string sys = gSystemLineBuffer.substr(0, pos);
                gSystemLineBuffer.erase(0, pos + 1);

                if (startsWith(sys, "FILE_OFFER|")) {
                    auto parts = splitByChar(sys, '|');
                    if (parts.size() == 5) {
                        PendingOffer o;
                        o.id = atoi(parts[1].c_str());
                        o.from = parts[2];
                        o.filename = parts[3];
                        o.size = atoi(parts[4].c_str());

                        EnterCriticalSection(&gOfferCs);
                        gPendingOffers.push_back(o);
                        LeaveCriticalSection(&gOfferCs);

                        printOfferUI(o, client->name);
                        continue;
                    }
                }

                if (startsWith(sys, "FILE_IDMAP|")) {
                    auto parts = splitByChar(sys, '|');
                    if (parts.size() == 4) {
                        int baseId = atoi(parts[1].c_str());
                        int rid = atoi(parts[2].c_str());
                        string rname = parts[3];

                        EnterCriticalSection(&gFileCs);
                        auto itBase = gOutgoingFilePath.find(baseId);
                        if (itBase != gOutgoingFilePath.end()) {
                            gOutgoingFilePath[rid] = itBase->second;
                        }
                        gIdMapInfo[rid] = rname;
                        LeaveCriticalSection(&gFileCs);
                        continue;
                    }
                }

                if (startsWith(sys, "FILE_ACCEPTED|")) {
                    auto parts = splitByChar(sys, '|');
                    if (parts.size() == 2) {
                        int id = atoi(parts[1].c_str());
                        string path;

                        EnterCriticalSection(&gFileCs);
                        auto it = gOutgoingFilePath.find(id);
                        if (it != gOutgoingFilePath.end()) path = it->second;
                        LeaveCriticalSection(&gFileCs);

                        if (!path.empty()) {
                            int size = 0;
                            if (!fileSizeOf(path, size)) {
                                cout << "\r>>> Error: Cannot open file.\n";
                                cout << client->name << ": ";
                                cout.flush();
                                continue;
                            }

                            string filename = getBaseName(path);

                            cout << "\r>>> Sending file...\n";
                            bool ok = sendFileBeginAndBytes(client->sock, id, filename, size, path);
                            if (!ok) cout << ">>> Send failed.\n";

                            cout << client->name << ": ";
                            cout.flush();
                            continue;
                        }
                    }
                }

                if (startsWith(sys, "FILE_START|")) {
                    auto parts = splitByChar(sys, '|');
                    if (parts.size() == 5) {
                        string from = parts[2];
                        string filename = parts[3];
                        int size = atoi(parts[4].c_str());

                        string folder = "received_files";
                        ensureDirExists(folder);

                        string savePath = folder + "\\received_" + filename;

                        cout << "\r>>> Downloading " << filename << " from " << from << "...\n";
                        bool ok = receiveFileBytesToDisk(client->sock, size, savePath);
                        if (ok) cout << ">>> File downloaded: " << savePath << "\n";
                        else cout << ">>> Download failed.\n";

                        cout << client->name << ": ";
                        cout.flush();
                        continue;
                    }
                }

                // show who declined
                // FILE_DECLINED|id|declinedBy|filename
                if (startsWith(sys, "FILE_DECLINED|")) {
                    auto parts = splitByChar(sys, '|');
                    if (parts.size() >= 4) {
                        string declinedBy = parts[2];
                        string filename = parts[3];
                        cout << "\r>>> " << declinedBy << " declined the file (" << filename << ").\n";
                    } else {
                        cout << "\r>>> Receiver declined the file.\n";
                    }
                    cout << client->name << ": ";
                    cout.flush();
                    continue;
                }

                if (startsWith(sys, "FILE_SENT|")) {
                    cout << "\r>>> File sent.\n";
                    cout << client->name << ": ";
                    cout.flush();
                    continue;
                }

                if (startsWith(sys, "FILE_DOWNLOADED|")) {
                    continue;
                }

                if (!sys.empty()) {
                    cout << "\r>>> " << sys << "\n";
                    cout << client->name << ": ";
                    cout.flush();
                }
            }

            continue;
        }

        cout << "\r";
        cout << "[" << recvName << "]: " << recvBuffer << "\n";
        cout << client->name << ": ";
        cout.flush();
    }

    return 0;
}

int main() {
    WSADATA wsa;
    sockaddr_in serverAddr;

    char sendBuffer[1024];

    ClientData client;
    client.isRunning = true;

    InitializeCriticalSection(&gFileCs);
    InitializeCriticalSection(&gOfferCs);

    WSAStartup(MAKEWORD(2, 2), &wsa);

    client.sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client.sock == INVALID_SOCKET) {
        cout << "Could not create socket\n";
        return 1;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(9000);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(client.sock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cout << "connect() failed: " << WSAGetLastError() << "\n";
        closesocket(client.sock);
        WSACleanup();
        return 0;
    }

    cout << "connect(): connection made.\n";

    cout << "Enter your name: ";
    cin.getline(client.name, 1024);
    send(client.sock, client.name, (int)strlen(client.name), 0);

    HANDLE hThread = CreateThread(NULL, 0, RecvHandler, (LPVOID)&client, 0, NULL);

    printHelp(client.name);

    while (client.isRunning) {
        cin.getline(sendBuffer, 1024);
        string line = sendBuffer;

        if (line == "/help") {
            printHelp(client.name);
            continue;
        }

        if (startsWith(line, "/send ")) {
            string rest = line.substr(6);

            if (startsWith(rest, "@")) {
                size_t sp = rest.find(' ');
                if (sp == string::npos) {
                    cout << ">>> Invalid. Use: /send @username filepath\n";
                    cout << client.name << ": ";
                    cout.flush();
                    continue;
                }

                string user = rest.substr(1, sp - 1);
                string path = rest.substr(sp + 1);

                int size = 0;
                if (!fileSizeOf(path, size)) {
                    cout << ">>> Error: Cannot open file.\n";
                    cout << client.name << ": ";
                    cout.flush();
                    continue;
                }

                string filename = getBaseName(path);
                int id = makeTransferId();

                EnterCriticalSection(&gFileCs);
                gOutgoingFilePath[id] = path;
                LeaveCriticalSection(&gFileCs);

                stringstream ss;
                ss << "/sendto|" << user << "|" << id << "|" << filename << "|" << size << "\n";
                string cmd = ss.str();

                if (!sendAll(client.sock, cmd.c_str(), (int)cmd.size())) {
                    cout << "Send failed.\n";
                    client.isRunning = false;
                    break;
                }

                cout << ">>> File offer sent.\n";
                cout << client.name << ": ";
                cout.flush();
                continue;
            } else {
                string path = rest;
                int size = 0;
                if (!fileSizeOf(path, size)) {
                    cout << ">>> Error: Cannot open file.\n";
                    cout << client.name << ": ";
                    cout.flush();
                    continue;
                }

                string filename = getBaseName(path);
                int baseId = makeTransferId();

                EnterCriticalSection(&gFileCs);
                gOutgoingFilePath[baseId] = path;
                LeaveCriticalSection(&gFileCs);

                stringstream ss;
                ss << "/send|" << baseId << "|" << filename << "|" << size << "\n";
                string cmd = ss.str();

                if (!sendAll(client.sock, cmd.c_str(), (int)cmd.size())) {
                    cout << "Send failed.\n";
                    client.isRunning = false;
                    break;
                }

                cout << ">>> File offer sent.\n";
                cout << client.name << ": ";
                cout.flush();
                continue;
            }
        }

        if (line == "/accept") {
            PendingOffer o;
            if (!popNextOffer(o)) {
                cout << ">>> No pending file.\n";
                cout << client.name << ": ";
                cout.flush();
                continue;
            }
            stringstream ss;
            ss << "/accept|" << o.id << "\n";
            string cmd = ss.str();

            if (!sendAll(client.sock, cmd.c_str(), (int)cmd.size())) {
                cout << "Send failed.\n";
                client.isRunning = false;
                break;
            }

            cout << client.name << ": ";
            cout.flush();
            continue;
        }

        if (line == "/decline") {
            PendingOffer o;
            if (!popNextOffer(o)) {
                cout << ">>> No pending file.\n";
                cout << client.name << ": ";
                cout.flush();
                continue;
            }
            stringstream ss;
            ss << "/decline|" << o.id << "\n";
            string cmd = ss.str();

            if (!sendAll(client.sock, cmd.c_str(), (int)cmd.size())) {
                cout << "Send failed.\n";
                client.isRunning = false;
                break;
            }

            cout << client.name << ": ";
            cout.flush();
            continue;
        }

        string msg = line + "\n";
        if (!sendAll(client.sock, msg.c_str(), (int)msg.size())) {
            cout << "Send failed.\n";
            client.isRunning = false;
            break;
        }

        cout << client.name << ": ";
        cout.flush();
    }

    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);

    DeleteCriticalSection(&gOfferCs);
    DeleteCriticalSection(&gFileCs);

    closesocket(client.sock);
    WSACleanup();
    return 0;
}
