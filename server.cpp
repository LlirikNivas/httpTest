#include "picohttpparser.h"

#include <netinet/in.h>
#include <sys/wait.h>
#include <algorithm>
#include <csignal>
#include <iostream>
#include <istream>
#include <memory>
#include <ostream>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <wait.h>

#define PORT "80"
#define BACKLOG 32

inline int guard(int n, const char *msg) {
    if (n < 0) perror(msg);

    return n;
}

class HTTPClient {
   public:
    HTTPClient(int clientSocket, sockaddr_in clientAdress,
               socklen_t adressLength)
        : cliSock(clientSocket),
          clientAdr(clientAdress),
          clientAdrLen(adressLength) {
         std::cout << "got connection from " << inet_ntoa(clientAdr.sin_addr) << std::endl;
    }

    int getRequest() {
        while (true) {
            int len;
            while ((len = recv(cliSock, buff + ofs, sizeof(buff) - ofs,
                               MSG_NOSIGNAL)) == -1 &&
                   errno == EINTR) {
                continue;
            }

            if (len == 0) {
                std::cout << "cient disconnected" << std::endl;
                return -1;
            }
            if (len < 0) {
                std::cout << "recv error " << strerror(errno) << std::endl;
                return -1;
            }

            lastofs = ofs;
            ofs += len;

            headersCnt = sizeof(headers) / sizeof(headers[0]);

            int parseRet;
            guard(parseRet = phr_parse_request(buff, ofs, &method, &methodLen,
                                               &path, &pathLen, &version,
                                               headers, &headersCnt, lastofs),
                  "phr_parse_req_error");

            if (parseRet > 0) {
                return ofs - 1;
            }

            if (parseRet == -2) {
                continue;
            }

            if (ofs == sizeof(buff)) {
                std::cerr << "request is too big" << std::endl;
                return -1;
            }
        }

        // printf("buffer is %s\n", buff);
        // printf("method is %.*s\n", (int)methodLen, method);
        // printf("path is %.*s\n", (int)pathLen, path);
        // printf("HTTP version is 1.%d\n", version);
        // printf("headers:\n");
        // for (int i = 0; i != headersCnt; ++i) {
        // printf("%.*s: %.*s\n", (int)headers[i].name_len, headers[i].name,
        //(int)headers[i].value_len, headers[i].value);
        //}
    }

    void giveResponse() {
        std::ostringstream resp;

        int status = 200;
        if (version != 0) {
            status = 505;
            resp << "HTTP/1.0" << status << "\n";
            resp << "Content-length: " << 0 << " \n";
            resp << "Content-Type: text/html\n";
            resp << "\n";

            sendStr(resp.str());
            return;
        }

        // std::string met(method, methodLen);
        std::string_view met(method, methodLen);

        if (met == "GET") {
            httpGET();
        }
    }

    HTTPClient(HTTPClient &&) = default;

    ~HTTPClient() {
        std::cout << "response given" << std::endl;
        close(cliSock);
    }

   private:
    void httpGET() {
        struct stat fileStat;
        std::ostringstream resp;

        int status = 200;

        char *pathBuf = new char[pathLen + 1];
        if (pathBuf == nullptr) return;

        memcpy(pathBuf, path, pathLen);
        pathBuf[pathLen] = '\0';
        std::replace(pathBuf, pathBuf + pathLen + 1, '?', '\0');
        std::unique_ptr<char> realPath(pathBuf);

        int filefd;
        guard(filefd = open(realPath.get(), O_RDONLY), "open Error");

        guard(fstat(filefd, &fileStat), "fstat error");

        if (filefd < 0 || !S_ISREG(fileStat.st_mode)) {
            status = 404;

            resp << "HTTP/1.0 " << status << "\n";
            resp << "Content-length: " << 0 << " \n";
        } else {
            resp << "HTTP/1.0 " << status << "\n";
            resp << "Content-length: " << size_t(fileStat.st_size) << " \n";
        }

        resp << "Content-Type: text/html\n";
        resp << "\n";

        sendStr(resp.str());
        sendFile(filefd, fileStat);

        close(filefd);
    }

    void httpPOST();

    int sendStr(std::string msg) {
        int sendCode;
        while (guard(sendCode = send(cliSock, msg.c_str(), strlen(msg.c_str()),
                                     MSG_NOSIGNAL),
                     "senderror") == -1 &&
               errno == EINTR) {
            continue;
        }

        return sendCode;
    }

    int sendFile(int filefd, const struct stat &fileStat) {
        if (!S_ISREG(fileStat.st_mode)) return -1;
        if (filefd >= 0) {
            off_t fileOff = 0;
            size_t byteSent = 0, totalLeft = (fileStat.st_size);
            while (totalLeft > 0) {
                guard(byteSent = sendfile(cliSock, filefd, &fileOff, 2e9),
                      "sendfile");  // possible errors
                if (byteSent == -1) {
                    return -1;
                }
                totalLeft -= byteSent;
            }
        }

        return fileStat.st_size;
    }

    int cliSock;

    sockaddr_in clientAdr;
    socklen_t clientAdrLen;

    char buff[4096];
    const char *method, *path;
    size_t buflen = 0, ofs = 0, methodLen = 0, pathLen = 0, headersCnt = 0;
    int lastofs = 0, version;
    phr_header headers[24];
};

class NetworkManager {
   public:
    NetworkManager(const char *node, const char *port) { //check if manager construction's impossible
        addrinfo hints;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        guard(getaddrinfo(node, port, &hints, &myAdr), "getaddrError");

        guard(mSock = socket(myAdr->ai_family, myAdr->ai_socktype,
                             myAdr->ai_protocol),
              "socketError");

        int yes = 1;
        if (setsockopt(mSock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1)
             std::cerr << "setsockopt   " << strerror(errno) << std::endl;

        guard(bind(mSock, myAdr->ai_addr, myAdr->ai_addrlen), "bindError");

        guard(listen(mSock, BACKLOG), "listenError");

        std::cout << "listening on " << node << ":" << port << std::endl;
    }

    HTTPClient waitClient() {
        while (true) {
            sockaddr_in clientAdr;
            socklen_t clientAdrLen = sizeof(clientAdr);
            int cliSock;

            guard(cliSock =
                      accept(mSock, (sockaddr *)(&clientAdr), &clientAdrLen),
                  "acceptError");

            if (cliSock > 0) {
                guard(
                    getpeername(cliSock, (sockaddr *)&clientAdr, &clientAdrLen),
                    "getpeername");
                return HTTPClient(cliSock, clientAdr, clientAdrLen);
            } else {
                continue;
            }
        }
    }

    ~NetworkManager() {
        freeaddrinfo(myAdr);
        close(mSock);
    }

   private:
    int mSock;
    addrinfo *myAdr;
};

void testfunc() { sleep(8); }

void killChildHandler(int signal, siginfo_t *info, void *ptr) {
    if (info->si_code == CLD_EXITED || info->si_code == CLD_DUMPED ||
        info->si_code == CLD_KILLED) {
        waitpid(-1, nullptr, 0);
        // std::cerr << "killed " << std::endl;
    }
}

void setZombieKiller() {
    struct sigaction act;
    act.sa_flags = SA_SIGINFO | SA_RESTART;
    act.sa_sigaction = &killChildHandler;
    sigaction(SIGCHLD, &act, nullptr);
}

int main(int argc, char **argv) {
    setZombieKiller();

    // parse args
    char *h = nullptr, *p = nullptr, *d = nullptr;
    opterr = 0;
    int rez = 0;
    while ((rez = getopt(argc, argv, "p:h:d:")) != -1) {
        switch (rez) {
            case 'h':
                h = optarg;
                break;
            case 'p':
                p = optarg;
                break;
            case 'd':
                d = optarg;
                break;
        }
    }
    if (h == nullptr || p == nullptr || d == nullptr) {
        std::cerr << "usage: server -h <ip> -p <port> -d <directory>\n";
        exit(1);
    }

    chroot(d);
    daemon(1, 0);

    NetworkManager manager(h, p);  // check h and p!!

    while (true) {
        auto client = manager.waitClient();

        if (!fork()) {
            if (client.getRequest() != -1) client.giveResponse();

            return 0;
        }
    }

    return 0;
}
