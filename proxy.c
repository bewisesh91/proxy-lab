#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_OBJS_COUNT 10
#define LRU_MAGIC_NUMBER 9999 // LRU, Least Recently Used : 가장 오랫동안 참조되지 않은 페이지를 교체하는 기법

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 ""Firefox/10.0.3\r\n";
static const char *requestline_hdr_format = "GET %s HTTP/1.0\r\n";
static const char *endof_hdr = "\r\n";
static const char *host_hdr_format = "Host: %s\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";

static const char *host_key = "Host";
static const char *connection_key = "Connection";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *user_agent_key = "User-Agent";

void doit(int connfd);
void parse_uri(char *uri, char *hostname, char *path, int *port);
void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
int connect_endServer(char *hostname, int port, char *http_header);

// 쓰레드 생성 관련 함수
void *thread(void *vargsp);

// 캐시 관련 함수
void cache_init();
int cache_find(char *url);
void cache_uri(char *uri, char *buf);
void readerPre(int i);
void readerAfter(int i);

// 캐시 블럭 구조체
typedef struct{
  char cache_obj[MAX_OBJECT_SIZE];
  char cache_url[MAXLINE];
  int LRU;
  int isEmpty;

  int readCnt;      // count of readers
  sem_t wmutex;     // protects accesses to cache
  sem_t rdcntmutex; // protects accesses to readcnt
} cache_block;

// 캐시 구조체
typedef struct{
  cache_block cacheobjs[CACHE_OBJS_COUNT]; // ten cache blocks
  int cache_num;
} Cache;

Cache cache;


int main(int argc, char **argv){ // argc : 인자의 개수, argv : 인자의 배열
  int listenfd, *connfd;
  socklen_t clientlen;
  char hostname[MAXLINE], port[MAXLINE];
  pthread_t tid;
  struct sockaddr_storage clientaddr;

  // cache_init : 
  cache_init();

  /* Check command line args */
  if (argc != 2){ // 입력된 인자의 갯수가 2개가 아니라면 표준 오류 출력
    fprintf(stderr, "usage: %s <port> \n", argv[0]);
    exit(1); // exit(1): 에러 시 강제 종료
  }

  // Signal(SIGPIPE, SIG_IGN) : 비정상 종료된 소켓에 접근 시 발생하는 프로세스 종료 시그널을 무시하는 함수
  // 이를 통해 나머지 클라이언트들과이 연결 중인 전체 프로세스를 유지할 수 있음
  Signal(SIGPIPE, SIG_IGN);
  
  // Open_listenfd 함수를 호출해서 듣기 소켓을 오픈한다. 인자로 포트 번호를 넘겨준다.
  listenfd = Open_listenfd(argv[1]);
  while (1){
    clientlen = sizeof(clientaddr);
    connfd = Malloc(sizeof(int));

    // 반복적으로 연결 요청을 접수한다. accept 함수는 1. 듣기 식별자, 2. 소켓 주소 구조체의 주소, 3. 주소(소켓 구조체)의 길이를 인자로 받는다.
    *connfd = Accept(listenfd,(SA *)&clientaddr,&clientlen);

    // Getaddrinfo는 호스트 이름(호스트 주소), 서비스 이름(포트 번호)의 스트링 표시를 소켓 주소 구조체로 변환한다.
    // Getnameinfo는 위를 반대로 소켓 주소 구조체에서 스트링 표시로 변환한다.
    // 연결이 성공했다는 메시지를 위해 Getnameinfo 함수를 호출한다.
    Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
    printf("Accepted connection from (%s %s).\n", hostname, port);

    // Pthread_create : 쓰레드 생성 함수
    // 첫 번째 인자 : 쓰레드 식별자
    // 두 번째 인자 : 쓰레드 특성 지정 (기본: NULL)
    // 세 번째 인자 : 쓰레드 함수
    // 네 번째 인자 : 쓰레드 함수의 매개변수
    Pthread_create(&tid, NULL, thread, connfd);
  }
  return 0;
}

// thread : 쓰레드를 통해 수행할 기능을 담은 함수
void *thread(void *vargsp){
  // 인자로 받은 vargsp(connfd)를 connfd에 담아준다. 
  int connfd = *((int *)vargsp);
  
  // Pthread_detach : 쓰레드를 분리하는 함수
  // 이를 통해 해당 쓰레드가 종료되면 시스템에서 자동으로 메모리를 반환시킬 수 있다.
  Pthread_detach(pthread_self());
  Free(vargsp);

  // doit 함수 호출 후, connfd를 닫는다.
  doit(connfd);
  Close(connfd);
}

// doit 함수는 thread 함수에서 호출되며, connfd를 인자로 받는다.
void doit(int connfd){
  int end_serverfd;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char endserver_http_header[MAXLINE];
  char hostname[MAXLINE], path[MAXLINE];
  int port;

  // rio: client's rio / server_rio: endserver's rio
  rio_t rio, server_rio;

  Rio_readinitb(&rio, connfd);
  Rio_readlineb(&rio, buf, MAXLINE);
  sscanf(buf, "%s %s %s", method, uri, version); // 클라이언트 요청 라인을 읽어온다.

  if (strcasecmp(method, "GET")){
    printf("Proxy does not implement the method");
    return;
  }

  // 캐시에서 사용할 url을 저장할 문자열 배열
  char url_store[100];
  // uri를 복사해서 url_store에 담는다.
  strcpy(url_store, uri);

  int cache_index;
  // cache_find : url을 통해 캐시 블럭의 인덱스를 확인하는 함수
  if ((cache_index = cache_find(url_store)) != -1){
    readerPre(cache_index);
    // 캐시의 내용을 connfd에 담아 클라이언트에게 전송한다.
    Rio_writen(connfd, cache.cacheobjs[cache_index].cache_obj, strlen(cache.cacheobjs[cache_index].cache_obj));
    readerAfter(cache_index);
    return;
  }

  // parse the uri to get hostname, file path, port
  parse_uri(uri, hostname, path, &port);

  // build the http header which will send to the end server
  build_http_header(endserver_http_header, hostname, path, port, &rio);

  // connect to the end server
  end_serverfd = connect_endServer(hostname, port, endserver_http_header);
  if (end_serverfd < 0){
    printf("connection failed\n");
    return;
  }

  Rio_readinitb(&server_rio, end_serverfd);

  // write the http header to endserver
  Rio_writen(end_serverfd, endserver_http_header, strlen(endserver_http_header));

  // recieve message from end server and send to the client
  char cachebuf[MAX_OBJECT_SIZE];
  int sizebuf = 0;
  size_t n;
  while ((n = Rio_readlineb(&server_rio, buf, MAXLINE)) != 0){
    // printf("proxy received %ld bytes, then send\n", n);
    sizebuf += n;
    // end server로 부터 받은 메시지를 클라이언트에게 전송한다.
    if (sizebuf < MAX_OBJECT_SIZE)
      strcat(cachebuf, buf);
    Rio_writen(connfd, buf, n);
  }
  Close(end_serverfd);

  // 캐시 버퍼의 내용을 새롭게 캐시 블럭에 담는다.
  if (sizebuf < MAX_OBJECT_SIZE){
    cache_uri(url_store, cachebuf);
  }
}

void build_http_header(char *http_header, char *hostname, char *path, int port, rio_t *client_rio){
  char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE], host_hdr[MAXLINE];

  // request line
  sprintf(request_hdr, requestline_hdr_format, path);

  // get other request header for client rio and change it
  while (Rio_readlineb(client_rio, buf, MAXLINE) > 0){
    if (strcmp(buf, endof_hdr) == 0)
      break; // EOF

    if (!strncasecmp(buf, host_key, strlen(host_key))){
      strcpy(host_hdr, buf);
      continue;
    }

    if (!strncasecmp(buf, connection_key, strlen(connection_key)) && !strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key)) && !strncasecmp(buf, user_agent_key, strlen(user_agent_key))){
      strcat(other_hdr, buf);
    }
  }
  if (strlen(host_hdr) == 0){
    sprintf(host_hdr, host_hdr_format, hostname);
  }
  sprintf(http_header, "%s%s%s%s%s%s%s",
          request_hdr,
          host_hdr,
          conn_hdr,
          prox_hdr,
          user_agent_hdr,
          other_hdr,
          endof_hdr);
  return;
}

// Connect to the end server
inline int connect_endServer(char *hostname, int port, char *http_header){
  char portStr[100];
  sprintf(portStr, "%d", port);
  return Open_clientfd(hostname, portStr);
}

// parse the uri to get hostname, file path, port
void parse_uri(char *uri, char *hostname, char *path, int *port){
  *port = 80;
  char *pos = strstr(uri, "//");

  pos = pos != NULL ? pos + 2 : uri;

  char *pos2 = strstr(pos, ":");
  // sscanf(pos, "%s", hostname);
  if (pos2 != NULL){
    *pos2 = '\0';
    sscanf(pos, "%s", hostname);
    sscanf(pos2 + 1, "%d%s", port, path);
  }
  else{
    pos2 = strstr(pos, "/");
    if (pos2 != NULL){
      *pos2 = '\0'; // 중간에 끊으려고
      sscanf(pos, "%s", hostname);
      *pos2 = '/';
      sscanf(pos2, "%s", path);
    }
    else{
      scanf(pos, "%s", hostname);
    }
  }
  return;
}

void cache_init(){
  cache.cache_num = 0;
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++){
    cache.cacheobjs[i].LRU = 0;
    cache.cacheobjs[i].isEmpty = 1;

    // Sem_init 첫 번째 인자: 초기화할 세마포어의 포인터
    // 두 번째: 0 - 쓰레드들끼리 세마포어 공유, 그 외 - 프로세스 간 공유
    // 세 번째: 초기 값
    Sem_init(&cache.cacheobjs[i].wmutex, 0, 1);
    Sem_init(&cache.cacheobjs[i].rdcntmutex, 0, 1);
    // 세마포어란?
    // 공유된 하나의 자원에 여러개의 프로세스가 동시에 접근할대 발생하는 문제를 다루는 방법. 
    // -> 한번에 하나의 프로세스만 접근 가능하도록 만들어줘야함. 
    // -> 이 방법이 프로세스에서 사용될때는 세마포어, 쓰레드에서는 뮤텍스 라고 함. 
    // semaphore 값을 할당하는데 이때 값이 0이면 접근할 수 없고 0보다 크면 해당자원에 접근가능
    // 초기값을 1 로 놓는 이유.
    cache.cacheobjs[i].readCnt = 0;
  }
}

void readerPre(int i){
  P(&cache.cacheobjs[i].rdcntmutex);
  cache.cacheobjs[i].readCnt++;
  if (cache.cacheobjs[i].readCnt == 1)
    P(&cache.cacheobjs[i].wmutex);
  V(&cache.cacheobjs[i].rdcntmutex);
}

void readerAfter(int i){
  P(&cache.cacheobjs[i].rdcntmutex);
  cache.cacheobjs[i].readCnt--;
  if (cache.cacheobjs[i].readCnt == 0)
    V(&cache.cacheobjs[i].wmutex);
  V(&cache.cacheobjs[i].rdcntmutex);
}

int cache_find(char *url){
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++){
    readerPre(i);
    if ((cache.cacheobjs[i].isEmpty == 0) && (strcmp(url, cache.cacheobjs[i].cache_url) == 0))
      break;
    readerAfter(i);
  }
  if (i >= CACHE_OBJS_COUNT)
    return -1;
  return i;
}

int cache_eviction(){
  int min = LRU_MAGIC_NUMBER;
  int minindex = 0;
  int i;
  for (i = 0; i < CACHE_OBJS_COUNT; i++){
    readerPre(i);
    if (cache.cacheobjs[i].isEmpty == 1){
      minindex = i;
      readerAfter(i);
      break;
    }
    
    // cache block 이 다 차있을때는 가장 낮은 LRU를 가진 cache block을 
    // 덮어쓰기위해 minindex로 갱신하고 이 값을 리턴. 
    // -> 'eviction policy'
    if (cache.cacheobjs[i].LRU < min){
      minindex = i;
      min = cache.cacheobjs[i].LRU;
      readerAfter(i);
      continue;
    }
    readerAfter(i);
  }
  return minindex;
}

void writePre(int i){
  P(&cache.cacheobjs[i].wmutex);
}

void writeAfter(int i){
  V(&cache.cacheobjs[i].wmutex);
}

// update the LRU number except the new cache one
// 자신을 제외한 cache block 들의 LRU를 내려주는 함수.
void cache_LRU(int index){
  int i;
  for (i = 0; i < index; i++){
    writePre(i);
    if (cache.cacheobjs[i].isEmpty == 0 && i != index)
      cache.cacheobjs[i].LRU--;
    writeAfter(i);
  }
  i++;
  for (i; i < CACHE_OBJS_COUNT; i++){
    writePre(i);
    if (cache.cacheobjs[i].isEmpty == 0 && i != index)
    {
      cache.cacheobjs[i].LRU--;
    }
    writeAfter(i);
  }
}

// cache the uri and content in cache
void cache_uri(char *uri, char *buf){
  int i = cache_eviction();

  writePre(i);

  strcpy(cache.cacheobjs[i].cache_obj, buf);
  strcpy(cache.cacheobjs[i].cache_url, uri);
  cache.cacheobjs[i].isEmpty = 0;
  cache.cacheobjs[i].LRU = LRU_MAGIC_NUMBER;
  cache_LRU(i);

  writeAfter(i);
}