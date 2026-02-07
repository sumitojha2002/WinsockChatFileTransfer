#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>

#pragma comment(lib,"ws2_32.lib")

using namespace std;

struct ClientData{
    SOCKET sock;
    char name[1024];
    bool isRunning;
};

DWORD WINAPI RecvHandler(LPVOID lpParam){
    ClientData* client = (ClientData*)lpParam;
    
    
     char recvName[1025]; 
     char recvBuffer[1025];

    while(client->isRunning){
  
        
        int recvNameSize = recv(client->sock,recvName,1024,0);
        if(recvNameSize <= 0){
            cout << "\nServer disconnected.\n";
            client->isRunning = false;  
            break;  
        }
        recvName[recvNameSize] = '\0';
        
        int recvBufferSize = recv(client->sock, recvBuffer, 1024, 0);
        if(recvBufferSize <= 0){
            cout << "\nServer disconnected.\n";
            client->isRunning = false;  
            break;  
        }
        recvBuffer[recvBufferSize] = '\0';
        
        cout << "\r";  
        cout << "[" << recvName << "]: " << recvBuffer << endl;
        cout << client->name << ": ";  
        cout.flush();
       
    }
    
    return 0;
}

int main(){
    WSADATA wsa;
    sockaddr_in serverAddr;

    char sendBuffer[1024];
    
    ClientData client;
    client.isRunning = true;


    // Start WinSock
    WSAStartup(MAKEWORD(2,2),&wsa);

    // Create socket
    client.sock = socket(AF_INET,SOCK_STREAM,0);
    if(client.sock == INVALID_SOCKET){
        cout << "Could not create socket\n";
        return 1;
    }

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(9000);
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");


    if(connect(client.sock,(sockaddr *)&serverAddr,sizeof(serverAddr))==SOCKET_ERROR){
        cout << "connect() failed:"<<WSAGetLastError()<<endl;
        closesocket(client.sock);
        WSACleanup();
    }else{
        cout << "connect(): connection made."<<endl;
    }

    cout << "Enter your name: ";
    cin.getline(client.name,1024);
    send(client.sock,client.name,strlen(client.name),0);

    HANDLE hThread = CreateThread(
        NULL,
        0,
        RecvHandler,
        (LPVOID)&client,
        0,
        NULL
    );

    while(client.isRunning){
        cout << client.name << ": ";
        cin.getline(sendBuffer, 1024);

        if(send(client.sock, sendBuffer, strlen(sendBuffer), 0) == SOCKET_ERROR){
            cout << "Send failed.\n";
            client.isRunning = false;
            break;
        }  
    }
    // Wait for receive thread to finish
    WaitForSingleObject(hThread, INFINITE);
    CloseHandle(hThread);
    
    // Close
    closesocket(client.sock);
    WSACleanup();
    return 0;
}