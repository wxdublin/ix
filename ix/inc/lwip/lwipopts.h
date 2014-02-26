#define	LWIP_STATS	0
#define	LWIP_TCP	1
#define	NO_SYS		1

#define	LWIP_DEBUG		LWIP_DBG_OFF
#define	TCP_CWND_DEBUG		LWIP_DBG_OFF
#define	TCP_DEBUG		LWIP_DBG_OFF
#define	TCP_FR_DEBUG		LWIP_DBG_OFF
#define	TCP_INPUT_DEBUG		LWIP_DBG_OFF
#define	TCP_OUTPUT_DEBUG	LWIP_DBG_OFF
#define	TCP_QLEN_DEBUG		LWIP_DBG_OFF
#define	TCP_RST_DEBUG		LWIP_DBG_OFF
#define	TCP_RTO_DEBUG		LWIP_DBG_OFF
#define	TCP_WND_DEBUG		LWIP_DBG_OFF

#include <ix/stddef.h>
#include <ix/byteorder.h>

#define LWIP_PLATFORM_BYTESWAP	1
#define LWIP_PLATFORM_HTONS(x) hton16(x)
#define LWIP_PLATFORM_NTOHS(x) ntoh16(x)
#define LWIP_PLATFORM_HTONL(x) hton32(x)
#define LWIP_PLATFORM_NTOHL(x) ntoh32(x)
