/*
 * Environment mocks shared by the Ethernet harnesses (doip_soad_fuzz, someip_sd_fuzz):
 * Det/log level, critical sections, and an in-memory TcpIp socket layer that SoAd polls.
 * The including harness defines NUM_TCP_CONN and a tick() that runs its MainFunctions.
 * Not a header in the usual sense: it defines functions and is included by exactly one .c file.
 */
#ifndef NET_MOCK_H
#define NET_MOCK_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "Std_Types.h"
#include "ComStack_Types.h"
#include "TcpIp.h"
#include "Std_Critical.h"

static void tick(void);

/* ---------------- environment mocks: Det / log level ---------------- */
Std_ReturnType Det_ReportError(uint16_t ModuleId, uint8_t InstanceId, uint8_t ApiId,
                               uint8_t ErrorId) {
  (void)ModuleId;
  (void)InstanceId;
  (void)ApiId;
  (void)ErrorId;
  return E_OK;
}
int std_get_log_level(void) {
  return 1000; /* quiet: no ASLOG output */
}
int std_get_as_log_level(const char *name, int dft) {
  (void)name;
  return dft;
}

/* ---------------- environment mocks: critical sections (single-threaded harness) ------------ */
imask_t Std_EnterCritical(void) {
  return 0;
}
void Std_ExitCritical(imask_t mask) {
  (void)mask;
}

/* ---------------- environment mocks: TcpIp (in-memory sockets) ---------------- */
#define MAX_SOCKS 16
#define SOCK_BUF 8192
typedef struct {
  int used;
  TcpIp_ProtocolType proto;
  int listening;
  int closedByPeer;
  uint8_t rx[SOCK_BUF];
  uint32_t rxHead;
  uint32_t rxLen;
} SockType;
static SockType socks[MAX_SOCKS];
static int pendingAccept[MAX_SOCKS]; /* queue of sockets waiting in accept() */
static int numPendingAccept;
static int tcpConn[NUM_TCP_CONN]; /* peer connection index -> socket id (+1), 0 = none */
static uint32_t txBytes;
static uint8_t lastTx[256]; /* read by mayhem/tests */
static uint32_t lastTxLen;
static int lastTxSock;

static int sock_new(TcpIp_ProtocolType proto) {
  int i;
  for (i = 0; i < MAX_SOCKS; i++) {
    if (!socks[i].used) {
      memset(&socks[i], 0, sizeof(socks[i]));
      socks[i].used = 1;
      socks[i].proto = proto;
      return i;
    }
  }
  return -1;
}
static SockType *sock_get(TcpIp_SocketIdType id) {
  if ((id < 0) || (id >= MAX_SOCKS) || !socks[id].used) {
    return NULL;
  }
  return &socks[id];
}
static void sock_record_tx(TcpIp_SocketIdType id, const uint8_t *buf, uint32_t len) {
  uint32_t i;
  for (i = 0; i < len; i++) {
    txBytes += buf[i]; /* the stack hands the whole buffer to the network */
  }
  lastTxSock = id;
  lastTxLen = len;
  memcpy(lastTx, buf, (len < sizeof(lastTx)) ? len : sizeof(lastTx));
}

void TcpIp_Init(const TcpIp_ConfigType *configPtr) {
  (void)configPtr;
}
Std_ReturnType TcpIp_SetupAddrFrom(TcpIp_SockAddrType *remoteAddrPtr, uint32_t ipv4Addr,
                                   uint16_t port) {
  remoteAddrPtr->Addr[0] = (ipv4Addr >> 24) & 0xFF;
  remoteAddrPtr->Addr[1] = (ipv4Addr >> 16) & 0xFF;
  remoteAddrPtr->Addr[2] = (ipv4Addr >> 8) & 0xFF;
  remoteAddrPtr->Addr[3] = ipv4Addr & 0xFF;
  remoteAddrPtr->Port = port;
  return E_OK;
}
Std_ReturnType TcpIp_GetIpAddr(TcpIp_LocalAddrIdType localAddrId, TcpIp_SockAddrType *ipAddrPtr,
                               uint8_t *netmaskPtr, TcpIp_SockAddrType *defaultRouterPtr) {
  (void)localAddrId;
  if (NULL != ipAddrPtr) {
    TcpIp_SetupAddrFrom(ipAddrPtr, TCPIP_IPV4_ADDR(192, 168, 1, 10), 13400);
  }
  if (NULL != netmaskPtr) {
    *netmaskPtr = 24;
  }
  if (NULL != defaultRouterPtr) {
    TcpIp_SetupAddrFrom(defaultRouterPtr, TCPIP_IPV4_ADDR(192, 168, 1, 1), 0);
  }
  return E_OK;
}
Std_ReturnType TcpIp_GetLocalAddr(TcpIp_SocketIdType socketId, TcpIp_SockAddrType *addr) {
  (void)socketId;
  return TcpIp_SetupAddrFrom(addr, TCPIP_IPV4_ADDR(192, 168, 1, 10), 13400);
}
TcpIp_SocketIdType TcpIp_Create(TcpIp_ProtocolType protocol) {
  return sock_new(protocol);
}
Std_ReturnType TcpIp_SetNonBlock(TcpIp_SocketIdType socketId, boolean nonBlocked) {
  (void)nonBlocked;
  return sock_get(socketId) ? E_OK : E_NOT_OK;
}
Std_ReturnType TcpIp_SetTimeout(TcpIp_SocketIdType socketId, uint32_t timeoutMs) {
  (void)timeoutMs;
  return sock_get(socketId) ? E_OK : E_NOT_OK;
}
Std_ReturnType TcpIp_Close(TcpIp_SocketIdType socketId, boolean abort) {
  SockType *s = sock_get(socketId);
  int i;
  (void)abort;
  if (NULL == s) {
    return E_NOT_OK;
  }
  s->used = 0;
  for (i = 0; i < NUM_TCP_CONN; i++) {
    if (tcpConn[i] == socketId + 1) {
      tcpConn[i] = 0;
    }
  }
  return E_OK;
}
Std_ReturnType TcpIp_Bind(TcpIp_SocketIdType socketId, TcpIp_LocalAddrIdType localAddrId,
                          uint16_t *portPtr) {
  (void)localAddrId;
  (void)portPtr;
  return sock_get(socketId) ? E_OK : E_NOT_OK;
}
Std_ReturnType TcpIp_AddToMulticast(TcpIp_SocketIdType socketId, TcpIp_SockAddrType *ipv4Addr) {
  (void)ipv4Addr;
  return sock_get(socketId) ? E_OK : E_NOT_OK;
}
Std_ReturnType TcpIp_SetMulticastIF(TcpIp_SocketIdType socketId, TcpIp_LocalAddrIdType localAddrId) {
  (void)localAddrId;
  return sock_get(socketId) ? E_OK : E_NOT_OK;
}
Std_ReturnType TcpIp_TcpConnect(TcpIp_SocketIdType socketId,
                                const TcpIp_SockAddrType *remoteAddrPtr) {
  (void)remoteAddrPtr;
  return sock_get(socketId) ? E_OK : E_NOT_OK;
}
Std_ReturnType TcpIp_TcpListen(TcpIp_SocketIdType socketId, uint16_t maxChannels) {
  SockType *s = sock_get(socketId);
  (void)maxChannels;
  if (NULL == s) {
    return E_NOT_OK;
  }
  s->listening = 1;
  return E_OK;
}
Std_ReturnType TcpIp_TcpAccept(TcpIp_SocketIdType socketId, TcpIp_SocketIdType *acceptSock,
                               TcpIp_SockAddrType *remoteAddrPtr) {
  SockType *s = sock_get(socketId);
  if ((NULL == s) || !s->listening || (0 == numPendingAccept)) {
    return E_NOT_OK;
  }
  *acceptSock = pendingAccept[0];
  memmove(&pendingAccept[0], &pendingAccept[1], sizeof(int) * (MAX_SOCKS - 1));
  numPendingAccept--;
  TcpIp_SetupAddrFrom(remoteAddrPtr, TCPIP_IPV4_ADDR(192, 168, 1, 20), 50000);
  return E_OK;
}
Std_ReturnType TcpIp_IsTcpStatusOK(TcpIp_SocketIdType socketId) {
  SockType *s = sock_get(socketId);
  if ((NULL == s) || (s->closedByPeer && (0 == s->rxLen))) {
    return E_NOT_OK;
  }
  return E_OK;
}
static Std_ReturnType sock_read(TcpIp_SocketIdType socketId, uint8_t *bufPtr, uint32_t *length) {
  SockType *s = sock_get(socketId);
  uint32_t n;
  if (NULL == s) {
    return E_NOT_OK;
  }
  n = (*length < s->rxLen) ? *length : s->rxLen;
  memcpy(bufPtr, &s->rx[s->rxHead], n);
  s->rxHead += n;
  s->rxLen -= n;
  *length = n;
  return E_OK;
}
Std_ReturnType TcpIp_Recv(TcpIp_SocketIdType socketId, uint8_t *bufPtr, uint32_t *length) {
  return sock_read(socketId, bufPtr, length);
}
Std_ReturnType TcpIp_RecvFrom(TcpIp_SocketIdType socketId, TcpIp_SockAddrType *remoteAddrPtr,
                              uint8_t *bufPtr, uint32_t *length) {
  TcpIp_SetupAddrFrom(remoteAddrPtr, TCPIP_IPV4_ADDR(192, 168, 1, 20), 13400);
  return sock_read(socketId, bufPtr, length);
}
Std_ReturnType TcpIp_SendTo(TcpIp_SocketIdType socketId, const TcpIp_SockAddrType *remoteAddrPtr,
                            const uint8_t *bufPtr, uint32_t length) {
  (void)remoteAddrPtr;
  if (NULL == sock_get(socketId)) {
    return E_NOT_OK;
  }
  sock_record_tx(socketId, bufPtr, length);
  return E_OK;
}
Std_ReturnType TcpIp_Send(TcpIp_SocketIdType socketId, const uint8_t *bufPtr, uint32_t length) {
  if (NULL == sock_get(socketId)) {
    return E_NOT_OK;
  }
  sock_record_tx(socketId, bufPtr, length);
  return E_OK;
}
Std_ReturnType TcpIp_TcpKeepAlive(TcpIp_SocketIdType socketId, uint32_t idel, uint32_t interval,
                                  uint32_t count) {
  (void)idel;
  (void)interval;
  (void)count;
  return sock_get(socketId) ? E_OK : E_NOT_OK;
}
uint16_t TcpIp_Tell(TcpIp_SocketIdType socketId) {
  SockType *s = sock_get(socketId);
  if (NULL == s) {
    return 0;
  }
  return (s->rxLen > 0xFFFF) ? 0xFFFF : (uint16_t)s->rxLen;
}
boolean TcpIp_IsLinkedUp(void) {
  return TRUE;
}

/* the n-th open UDP socket (in creation order), wrapping around */
static SockType *udp_sock(int n) {
  int i, count = 0;
  for (i = 0; i < MAX_SOCKS; i++) {
    if (socks[i].used && (TCPIP_IPPROTO_UDP == socks[i].proto) && !socks[i].listening) {
      count++;
    }
  }
  if (0 == count) {
    return NULL;
  }
  n %= count;
  for (i = 0; i < MAX_SOCKS; i++) {
    if (socks[i].used && (TCPIP_IPPROTO_UDP == socks[i].proto) && !socks[i].listening) {
      if (0 == n--) {
        return &socks[i];
      }
    }
  }
  return NULL;
}

/* the TCP peer connection conn (0..NUM_TCP_CONN-1), connected (accepted) on first use */
static SockType *tcp_sock(int conn) {
  int id;
  SockType *s;
  if (0 == tcpConn[conn]) {
    id = sock_new(TCPIP_IPPROTO_TCP);
    if ((id < 0) || (numPendingAccept >= MAX_SOCKS)) {
      return NULL;
    }
    pendingAccept[numPendingAccept++] = id;
    tcpConn[conn] = id + 1;
    tick(); /* SoAd accepts it */
  }
  s = sock_get(tcpConn[conn] - 1);
  return s;
}

/* sel bit 0: 0 = UDP datagram, 1 = TCP bytes; bits 1..6 pick the socket/connection; bit 7 on
 * a TCP chunk: the peer closes the connection after it. The stack runs until it is drained. */
static void deliver(uint8_t sel, const uint8_t *data, uint32_t len) {
  SockType *s;
  int i;
  if (0 == (sel & 0x01)) {
    s = udp_sock((sel >> 1) & 0x3F);
  } else {
    s = tcp_sock((sel >> 1) % NUM_TCP_CONN);
  }
  if (NULL == s) {
    return;
  }
  if (s->rxLen > 0) {
    memmove(s->rx, &s->rx[s->rxHead], s->rxLen);
  }
  s->rxHead = 0;
  if (len > SOCK_BUF - s->rxLen) {
    len = SOCK_BUF - s->rxLen;
  }
  memcpy(&s->rx[s->rxLen], data, len);
  s->rxLen += len;
  if ((sel & 0x81) == 0x81) {
    s->closedByPeer = 1;
  }
  for (i = 0; (i < 8) && s->used && (s->rxLen > 0); i++) {
    tick();
  }
  tick();
  if (s->used && (TCPIP_IPPROTO_UDP == s->proto)) {
    s->rxLen = 0; /* the rest of a datagram SoAd did not read is gone */
  }
}

#endif /* NET_MOCK_H */
