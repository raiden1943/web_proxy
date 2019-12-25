#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <resolv.h>
#include <netdb.h>
#include <pthread.h>
#include <string>
#include <list>
#define FAIL	-1
#define BUFSIZE 2048

using namespace std;

const string HTTPMETHOD[6] = {"GET", "POST", "HEAD", "PUT", "DELETE", "OPTIONS"};
const string HOSTPREFIX = "Host: ";

struct Info
{
	string host;
	int clientfd;
	int serverfd;
	Info(){}
	Info(string host, int clientfd, int serverfd)
	{
		this->host = host;
		this->clientfd = clientfd;
		this->serverfd = serverfd;
	}
};
struct Client
{
	pthread_t client_th, server_th;
	Info info;
	Client(pthread_t client_th, pthread_t server_th, Info info)
	{
		this->client_th = client_th;
		this->server_th = server_th;
		this->info = info;
	}
};

pthread_mutex_t mutex_lock;
bool broadcast_mode = false;
list<Client> clientList;

void* client_func(void *data)
{
	string host = (*(Info *) data).host;
	int clientfd = (*(Info *) data).clientfd;
	int serverfd = (*(Info *) data).serverfd;
	
	while (1)
	{
		char buf[BUFSIZE];
		ssize_t received = recv(clientfd, buf, BUFSIZE - 1, 0);
		if (received <= 0) 
		{
			perror("client recv failed");
			break;
		}
		buf[received] = '\0';
		printf("Client(%s) : %s\n", host.c_str(), buf);
		
		ssize_t sent = send(serverfd, buf, received, 0);
		if(sent == 0)
		{
			perror("client send failed");
			break;
		}
	}
	
	close(clientfd);
	close(serverfd);
	
	pthread_mutex_lock(&mutex_lock);
	for(auto it = clientList.begin(); it != clientList.end(); it++)
	{
		if((*it).info.clientfd == clientfd)
		{
			pthread_cancel((*it).server_th);
			clientList.erase(it);
			break;
		}
	}
	pthread_mutex_unlock(&mutex_lock);
}

void* server_func(void *data)
{
	string host = (*(Info *) data).host;
	int clientfd = (*(Info *) data).clientfd;
	int serverfd = (*(Info *) data).serverfd;
	
	while (1)
	{
		char buf[BUFSIZE];
		ssize_t received = recv(serverfd, buf, BUFSIZE - 1, 0);
		if (received <= 0) 
		{
			perror("server recv failed");
			break;
		}
		buf[received] = '\0';
		printf("Server(%s) : %s\n", host.c_str(), buf);
		
		ssize_t sent = send(clientfd, buf, received, 0);
		if(sent == 0)
		{
			perror("server send failed");
			break;
		}
	}

	close(clientfd);
	close(serverfd);
	
	pthread_mutex_lock(&mutex_lock);
	for(auto it = clientList.begin(); it != clientList.end(); it++)
	{
		if((*it).info.clientfd == clientfd)
		{
			pthread_cancel((*it).client_th);
			clientList.erase(it);
			break;
		}
	}
	pthread_mutex_unlock(&mutex_lock);
}

int OpenListener(int port)
{
	int sd;
	struct sockaddr_in addr;
	sd = socket(PF_INET, SOCK_STREAM, 0);
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0 )
	{
		perror("can't bind port");
		abort();
	}
	if ( listen(sd, 10) != 0 )
	{
		perror("Can't configure listening port");
		abort();
	}
	return sd;
}

int OpenConnection(const char *hostname)
{
	int sd;
	struct hostent *host;
	struct sockaddr_in addr;
	if ( (host = gethostbyname(hostname)) == NULL )
	{
		perror(hostname);
		abort();
	}
	sd = socket(PF_INET, SOCK_STREAM, 0);
	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(80);
	addr.sin_addr.s_addr = *(long*)(host->h_addr);
	//printf("%s\n",inet_ntoa(addr.sin_addr));
	if ( connect(sd, (struct sockaddr*)&addr, sizeof(addr)) != 0 )
	{
		close(sd);
		perror(hostname);
		abort();
	}
	return sd;
}

string getdomain(char data[BUFSIZE], int len)
{
	bool isStart = false, isWrite = false;
	for(auto method : HTTPMETHOD) {
		if(len < method.size()) continue;
		string str = "";
		for(int i = 0; i < method.size(); i++) str += data[i];
		if(method == str) isStart = true;
	}
	
	if(!isStart) return "";
	string host = "";
	for(int i = 0; i < len - HOSTPREFIX.size() + 1; i++) {
		bool cmp = true;
		for(int j = 0; j < HOSTPREFIX.size(); j++) {
			if(data[i + j] != HOSTPREFIX[j]) cmp = false;
		}
		if(cmp == true) isWrite = true, i+=HOSTPREFIX.size();
		if(isWrite && data[i] == 0x0d && data[i + 1] == 0x0a) break;
		if(isWrite) host += data[i];
	}
	
	return host;
}

bool isHttps(char buf[BUFSIZE], int len)
{
	string con = "CONNECT";
	if(len < 7) return true;
	for(int i = 0; i < 7; i++)
	{
		if(con[i] != buf[i]) return false;
	}
	return true;
}

void usage() {
	printf("syntax : web_proxy <tcp port>\n");
	printf("sample : web_proxy 8080\n");
}

int main(int argc, char **argv)
{
	if(getuid() != 0)
	{
		printf("This program must be run as root/sudo user!!");
		exit(-1);
	}
	if(argc != 2)
	{
		usage();
		exit(-1);
	}
	
	int client = OpenListener(atoi(argv[1]));
	
	int num = 1;
	while (1)
	{
		struct sockaddr_in client_addr;
		socklen_t clientlen = sizeof(client_addr);
		int clientfd = accept(client, (struct sockaddr*)&client_addr, &clientlen);
		if (clientfd < 0)
		{
			perror("ERROR on accept");
			break;
		}
		
		char buf[BUFSIZE];
		ssize_t received = recv(clientfd, buf, BUFSIZE - 1, 0);
		if (received <= 0) 
		{
			perror("recv failed");
			break;
		}
		if(isHttps(buf, received)) continue;
		
		string host = getdomain(buf, received);
		//printf("host : \"%s\"\n", host.c_str());
		
		int serverfd = OpenConnection(host.c_str());
		if (serverfd < 0)
		{
			perror("ERROR on connect");
			break;
		}
		
		Info info(host, clientfd, serverfd);
		
		pthread_t client_th;
		if(pthread_create(&client_th, NULL, client_func, (void *)&info) < 0)
		{
			perror("thread create error : ");
			break;
		}
		pthread_detach(client_th);
		
		pthread_t server_th;
		if(pthread_create(&server_th, NULL, server_func, (void *)&info) < 0)
		{
			perror("thread create error : ");
			break;
		}
		pthread_detach(server_th);
		
		printf("Client(%s) : %s\n", host.c_str(), buf);
		buf[received] = '\0';
		ssize_t sent = send(serverfd, buf, received, 0);
		if(sent == 0)
		{
			perror("send failed");
			break;
		}
		
		pthread_mutex_lock(&mutex_lock);
		clientList.push_back(Client(client_th, server_th, info));
		pthread_mutex_unlock(&mutex_lock);
	}
	
	pthread_mutex_lock(&mutex_lock);
	for(auto client : clientList)
	{
		pthread_cancel(client.client_th);
		pthread_cancel(client.server_th);
	}
	pthread_mutex_unlock(&mutex_lock);

	close(client);
}
