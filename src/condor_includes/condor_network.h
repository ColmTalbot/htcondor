#ifndef _NETWORK
#define _NETWORK

#include "_condor_fix_types.h"

#define SCHED_PORT			9605
#define START_PORT			9611
#define COLLECTOR_PORT		9612
#define COLLECTOR_UDP_PORT	9613
#define NEGOTIATOR_PORT		9614
#define START_UDP_PORT		9615
#define ACCOUNTANT_PORT		9616

#if defined(__cplusplus)
extern "C" {
#endif

#if defined( __STDC__) || defined(__cplusplus)
int do_connect ( char *host, char *service, u_int port );
#else
int do_connect ();
#endif

#if defined(__cplusplus)
}
#endif

#endif /* _NETWORK */
