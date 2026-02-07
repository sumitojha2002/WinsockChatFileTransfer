#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <windows.h>
#include <vector>
#include <string>
#pragma comment(lib, "Ws2_32.lib")

using namespace std;

struct Client {
   SOCKET sock;
   char name[1024];
};

vector<Client *> clients;
CRITICAL_SECTION cs;

DWORD WINAPI ClientHandler(LPVOID lpParam){
    Client* cl = (Client*)lpParam;
    SOCKET client = cl->sock;
    char buf[1025];
    int bytes;
    
    while(true){
        bytes = recv(client, buf, 1024, 0);

        if (bytes <= 0) {
            cout << cl->name << " disconnected.\n";
            
            char bufdis[1024];
            sprintf(bufdis, "%s has disconnected.", cl->name);
 
            EnterCriticalSection(&cs);  
            
            for(int i = 0; i < clients.size(); i++){
                Client* other = clients[i];
                if(other->sock != cl->sock){  
                    send(other->sock, "SYSTEM", 6, 0);
                    send(other->sock, bufdis, strlen(bufdis), 0);
                }
            }
            
            LeaveCriticalSection(&cs);  
            break;  
        }
        
        buf[bytes] = '\0';

        if(buf[0] == '@'){
            // Private message handling
            string str = buf;
            str.erase(0, 1);
            int pos = str.find(" ");
            
            if(pos == string::npos || pos == 0){
                send(cl->sock, "SYSTEM", 6, 0);
                send(cl->sock, "Invalid format. Use: @username message", 38, 0);
            } else {
                string name = str.substr(0, pos);
                string message = str.substr(pos + 1);
                
                bool found = false;

                EnterCriticalSection(&cs);
                for(int i = 0; i < clients.size(); i++){
                    if(strcmp(clients[i]->name, name.c_str()) == 0){
                        if(clients[i]->sock == cl->sock){
                            send(cl->sock, "SYSTEM", 6, 0);
                            send(cl->sock, "Cannot message yourself.", 24, 0);
                        } else {
                            char toRecipient[1024];
                            sprintf(toRecipient, "%s -> You", cl->name);
                            send(clients[i]->sock, toRecipient, strlen(toRecipient), 0);
                            send(clients[i]->sock, message.c_str(), message.length(), 0);
                        
                            char toSender[1024];
                            sprintf(toSender, "You -> %s", clients[i]->name);
                            send(cl->sock, toSender, strlen(toSender), 0);
                            send(cl->sock, message.c_str(), message.length(), 0);

                            cout << "[" << cl->name << " -> " << clients[i]->name << "]: " << message << endl;
                        }
                        found = true;
                        break;
                    }
                }
                LeaveCriticalSection(&cs);
                
                if(!found){
                    send(cl->sock, "SYSTEM", 6, 0);
                    send(cl->sock, "User not found.", 15, 0);
                }
            }
        } else {
            // Public message handling
            cout << "[" << cl->name << "]: " << buf << endl;
            
            EnterCriticalSection(&cs);
            for(int i = 0; i < clients.size(); i++){
                Client* other = clients[i];
                if(other->sock != cl->sock){
                    send(other->sock, cl->name, strlen(cl->name), 0);
                    send(other->sock, buf, bytes, 0);
                }
            }
            LeaveCriticalSection(&cs);
        }
        
    }  
    
    
    EnterCriticalSection(&cs);
    for(int i = 0; i < clients.size(); i++){
        if(clients[i]->sock == cl->sock){
            clients.erase(clients.begin() + i);
            break;
        }
    }
    LeaveCriticalSection(&cs);
    
    closesocket(client);
    delete cl;
    return 0;
}

int main(){
    WSADATA wsa;
    SOCKET server,client;
    sockaddr_in serverAddr,clientAddr;
    int clientSize = sizeof(clientAddr);
   
    char buffName[1025];

    InitializeCriticalSection(&cs);
    // Start WinSock
    if(WSAStartup(MAKEWORD(2,2),&wsa)!=0){
        cout<<"WSAStartup failed\n";
        return 1;
    }   

    // Create socket
    server = socket(AF_INET,SOCK_STREAM,0);
    
    // Check if socket creation was successful
    if(server == INVALID_SOCKET){
        cout<<"Could not create socket\n";
        return 1;
    }

    // Setup server address
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(9000);

    // bind
    if(bind(server,(sockaddr*)&serverAddr,sizeof(serverAddr))==SOCKET_ERROR){
        cout <<"bind() failed:"<<WSAGetLastError()<<endl;
        closesocket(server);
        WSACleanup();
        return 0;
    }else{
        cout<< "bind() is OK"<<endl;
    }

    // Listen
    if(listen(server,SOMAXCONN) == SOCKET_ERROR){
        cout << "Listen() : error listenting on socket"<<WSAGetLastError() <<endl ;
    }

     cout << "Server started. Waiting for clients...\n";

    while(true){

        client = accept(server,(sockaddr*)&clientAddr,&clientSize);
        
            if (client == INVALID_SOCKET) {
            cout << "Accept failed\n";
            continue;
        }
        int namebytes = recv(client,buffName,1024,0);
        buffName[namebytes] = '\0';
        cout << buffName <<" joined.\n";
        
        
        
        Client* cl = new Client;
        cl->sock = client;
        strcpy_s(cl->name, buffName);



        EnterCriticalSection(&cs);
        clients.push_back(cl);
        LeaveCriticalSection(&cs);

        // Broadcast join notification
        char buffConName[1024];  
        sprintf(buffConName, "%s has connected.", buffName);

        EnterCriticalSection(&cs);  

        for(int i = 0; i < clients.size(); i++){
            send(clients[i]->sock, "SYSTEM", 6, 0);
            send(clients[i]->sock, buffConName, strlen(buffConName), 0);
        }

        LeaveCriticalSection(&cs);  

         HANDLE hThread = CreateThread(
            NULL,
            0,
            ClientHandler,
            (LPVOID)cl,
            0,
            NULL
        );

         CloseHandle(hThread);
        
    }

    // Close
    DeleteCriticalSection(&cs);
    closesocket(server);
    WSACleanup();
    return 0;
}
