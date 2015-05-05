/*
 * ntest2.c
 *  Simulate sphone6 packets
 *
 *  Created on: May 04 2015
 *      Author: amyznikov
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <execinfo.h>
#include <netdb.h>
#include <limits.h>

#define INET_ADDR(a,b,c,d) \
  (uint32_t)((((uint32_t)(a))<<24)|((b)<<16)|((c)<<8)|(d))


enum {
  invalid_mode,
  client_mode,
  server_mode,
};

enum {
  dup_none = 0,
  dup_separate = 1,
  dup_inline = 2,
  dup_xor = 3,
};

/** Size must be 36 bytes as in libsphone6 */
struct pkt {
  int32_t sn;         // 4
  int32_t n;          // 8
  int64_t sndtick;    // 16
  int64_t rcvtick;    // 24
  uint8_t fill[12];   // 36
};

static char listen_address[256];
static uint16_t listen_port;

static char send_address[256];
static uint16_t send_port;

static int64_t send_interval = 40000; /* usec */
static int pktsize = -1;
static volatile int stop_sender_thread = 0;
static volatile int stop_receiver_thread = 0;

static int maxpkts = -1;
static struct pkt * pkts;


static int dup_mode = dup_none;
static int dup_offset = 0;

static char output_filename[PATH_MAX] = "ntest2.dat";

static int loss_emu_ratio = 0; // percent
static int loss_emu_srand = 12345;


static int64_t t0;

static int64_t gettime(void)
{
  struct timespec tm;
  clock_gettime(CLOCK_MONOTONIC, &tm);
  return ((int64_t) (tm.tv_sec * 1000000 + tm.tv_nsec / 1000) - t0);
}


static void us_sleep( int64_t usec )
{
  struct timespec rqtp;
  rqtp.tv_sec = usec / 1000000;
  rqtp.tv_nsec = (usec - rqtp.tv_sec * 1000000) * 1000;
  clock_nanosleep(CLOCK_MONOTONIC, 0, &rqtp, NULL);
}



/* echo server mode */
static void run_server_mode()
{
  int so;
  struct sockaddr_in addrs;
  socklen_t addrslen;

  uint8_t pkg[64 * 1024];
  ssize_t size;

  fprintf(stderr, "%s() %d : started in listen mode\n", __func__, __LINE__);


  if ( (so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1 ) {
    fprintf(stderr, "%s() %d : socket() fails: %s\n", __func__, __LINE__, strerror(errno));
    goto end;
  }

  addrs.sin_family = AF_INET;
  addrs.sin_addr.s_addr = *listen_address ? inet_addr(listen_address) : 0;
  addrs.sin_port = htons(listen_port);

  if ( bind(so, (struct sockaddr*) &addrs, sizeof(addrs)) != 0 ) {
    fprintf(stderr, "%s() %d : bind() fails: %s\n", __func__, __LINE__, strerror(errno));
    goto end;
  }


  while ( 42 ) {
    addrslen = sizeof(addrs);
    if ( (size = recvfrom(so, pkg, sizeof(pkg), MSG_NOSIGNAL, (struct sockaddr*) &addrs, &addrslen)) < 0 ) {
      fprintf(stderr, "%s() %d : recvfrom() fails: %s\n", __func__, __LINE__, strerror(errno));
      break;
    }

    if ( sendto(so, pkg, size, MSG_NOSIGNAL, (struct sockaddr*) &addrs, addrslen) != size ) {
      fprintf(stderr, "%s() %d : sendto() fails: %s\n", __func__, __LINE__, strerror(errno));
    }
  }


end:

  if ( so != -1 ) {
    close(so);
  }

  fprintf(stderr, "%s() %d : finished\n", __func__, __LINE__);
}





/* client mode */


static void save_meauremenrs()
{
  FILE * fp;
  int i;

  if ( !pkts ) {
    return;
  }

  fprintf(stderr, "Saving data...\n");

  if ( !(fp = fopen(output_filename, "w")) ) {
    fprintf(stderr, "%s() %d : fopen('%s') fails: %s\n", __func__, __LINE__, output_filename, strerror(errno));
    return;
  }

  fprintf(fp, "sn\tst\trt\tn\n");

  for ( i = 0; i < maxpkts; ++i ) {
    fprintf(fp, "%6d\t%9"PRId64"\t%9"PRId64"\t%3d\n", i + 1, pkts[i].sndtick, pkts[i].rcvtick, pkts[i].n);
  }

  fclose(fp);

  fprintf(stderr, "Saved to %s\n", output_filename);
}





static in_addr_t resolve(const char * nodename)
{
  uint8_t a1, a2, a3, a4;
  struct hostent * host_entry;

  uint32_t address = INADDR_NONE;

  if ( sscanf(nodename, "%hhu.%hhu.%hhu.%hhu", &a1, &a2, &a3, &a4) == 4 ) {
    address = INET_ADDR(a1, a2, a3, a4);
  }
  else if ( (host_entry = gethostbyname(nodename)) && host_entry->h_addr_list && host_entry->h_addr) {
    address = ntohl(*(in_addr_t*) host_entry->h_addr);
  }


  return address == INADDR_NONE ? INADDR_NONE : htonl(address);
}


static void * client_mode_sender_thread(void * arg)
{
  int so;
  int64_t now;
  size_t sndsize;
  int32_t sn;

  struct pkt pkts[2];

  fprintf(stderr, "%s() %d : thread started\n", __func__, __LINE__);

  so = (int) (ssize_t) (arg);

  memset(pkts, 0, sizeof(pkts));

  for ( sn = 1; sn <= maxpkts && !stop_sender_thread; ++sn ) {

    switch ( dup_mode ) {
      case dup_none :
        sndsize = sizeof(pkts[0]);
        pkts[0].sn = sn;
        pkts[0].sndtick = gettime();

        if ( send(so, pkts, sndsize, MSG_NOSIGNAL) != (ssize_t) sndsize ) {
          fprintf(stderr, "%s() %d : send() fails: %s\n", __func__, __LINE__, strerror(errno));
          stop_sender_thread = 1;
        }
      break;

      case dup_separate :
        sndsize = sizeof(pkts[0]);

        pkts[0].sn = sn;
        pkts[0].sndtick = gettime();
        if ( send(so, pkts, sndsize, MSG_NOSIGNAL) != (ssize_t) sndsize ) {
          fprintf(stderr, "%s() %d : send() fails: %s\n", __func__, __LINE__, strerror(errno));
          stop_sender_thread = 1;
          break;
        }

        if ( sn > dup_offset ) {
          pkts[0].sn = sn - dup_offset;
          pkts[0].sndtick = gettime();
          if ( send(so, pkts, sndsize, MSG_NOSIGNAL) != (ssize_t) sndsize ) {
            fprintf(stderr, "%s() %d : send() fails: %s\n", __func__, __LINE__, strerror(errno));
            stop_sender_thread = 1;
            break;
          }
        }
      break;

      case dup_inline :

        if ( sn <= dup_offset ) {
          sndsize = sizeof(pkts[0]);
          pkts[0].sn = sn;
          pkts[0].sndtick = gettime();
        }
        else {
          sndsize = 2 * sizeof(pkts[0]);
          pkts[0].sn = sn;
          pkts[0].sndtick = gettime();
          pkts[1].sn = sn - dup_offset;
          pkts[1].sndtick = pkts[0].sndtick;
        }

        if ( send(so, pkts, sndsize, MSG_NOSIGNAL) != (ssize_t) sndsize ) {
          fprintf(stderr, "%s() %d : send() fails: %s\n", __func__, __LINE__, strerror(errno));
          stop_sender_thread = 1;
          break;
        }
      break;

      case dup_xor :
      break;
    }

    if ( (now = gettime()) < pkts[0].sndtick + send_interval ) {
      us_sleep(pkts[0].sndtick + send_interval - now);
    }
  }

  fprintf(stderr, "%s() %d : thread finished\n", __func__, __LINE__);
  return NULL;
}

static void process_received_packet(const struct pkt * pkt, int64_t rcvtick)
{
  if ( pkt->sn < 1 || pkt->sn > maxpkts ) {
    fprintf(stderr, "%s() %d : Invalid packet sn = %d\n", __func__, __LINE__, pkt->sn);
  }
  else if ( (pkts[pkt->sn - 1].n += 1) == 1 ) {
    pkts[pkt->sn - 1].sn = pkt->sn;
    pkts[pkt->sn - 1].sndtick = pkt->sndtick;
    pkts[pkt->sn - 1].rcvtick = rcvtick;
  }
}

static void * client_mode_receiver_thread( void * arg )
{
  int so;

  struct pkt pkg[2];
  struct pollfd fds;
  ssize_t size;
  int64_t rcvtick;
  int nfds;


  fprintf(stderr, "%s() %d : thread started\n", __func__, __LINE__);

  so = (int) (ssize_t) (arg);

  if ( loss_emu_ratio > 0 ) {
    srand(loss_emu_srand);
  }

  while ( !stop_receiver_thread ) {

    fds.fd = so;
    fds.events = POLLIN | POLLERR | POLLHUP;
    fds.revents = 0;

    if ( (nfds = poll(&fds, 1, 1000)) == 0 ) {
      continue;
    }

    if ( nfds < 0 ) {
      fprintf(stderr, "%s() %d : poll() fails: %s\n", __func__, __LINE__, strerror(errno));
      break;
    }

    if ( !(fds.revents & POLLIN) ) {
      fprintf(stderr, "%s() %d : poll(): revents=0x%0x\n", __func__, __LINE__, fds.revents);
      break;
    }

    if ( (size = recv(so, pkg, sizeof(pkg), MSG_NOSIGNAL)) < 0 ) {
      fprintf(stderr, "%s() %d : recv() fails: %s\n", __func__, __LINE__, strerror(errno));
      break;
    }

    if ( loss_emu_ratio > 0 && rand() % 100 < loss_emu_ratio ) {
      continue;
    }


    rcvtick = gettime();

    if ( size < (ssize_t) sizeof(struct pkt) ) {
      fprintf(stderr, "%s() %d : Too small packet received: size=%zd bytes\n", __func__, __LINE__, size);
      continue;
    }

    if ( size == (ssize_t) sizeof(struct pkt) ) {
      // single packet
      process_received_packet(&pkg[0], rcvtick);
    }
    else if ( size == 2 * (ssize_t) sizeof(struct pkt) ) {
      // double packet
      process_received_packet(&pkg[0], rcvtick);
      process_received_packet(&pkg[1], rcvtick);
    }
    else {
      fprintf(stderr, "%s() %d : Invalid packet size=%zd bytes\n", __func__, __LINE__, size);
      continue;
    }
  }

  save_meauremenrs();

  stop_sender_thread = 1;

  fprintf(stderr, "%s() %d : thread finished\n", __func__, __LINE__);
  return NULL;
}


static void run_client_mode()
{
  int so = -1;
  struct sockaddr_in addrs;
  pthread_t sender_thread_id = 0, receiver_thread_id = 0;

  int status;

  addrs.sin_family = AF_INET;
  addrs.sin_port = htons(send_port);
  addrs.sin_addr.s_addr = resolve(send_address);

  if ( addrs.sin_addr.s_addr == INADDR_NONE ) {
    fprintf(stderr, "%s() %d : can not resolve node name '%s'\n", __func__, __LINE__, send_address);
    goto end;
  }


  if ( (so = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1 ) {
    fprintf(stderr, "%s() %d : socket() fails: %s\n", __func__, __LINE__, strerror(errno));
    goto end;
  }


  if ( connect(so, (struct sockaddr*) &addrs, sizeof(addrs)) != 0 ) {
    fprintf(stderr, "%s() %d : connect() fails: %s\n", __func__, __LINE__, strerror(errno));
    goto end;
  }


  if ( !(pkts = calloc(maxpkts, sizeof(struct pkt))) ) {
    fprintf(stderr, "%s() %d : calloc(maxpkts=%d) fails: %s\n", __func__, __LINE__, maxpkts, strerror(errno));
    goto end;
  }

  t0 = gettime();

  if ( (status = pthread_create(&receiver_thread_id, NULL, client_mode_receiver_thread, (void*) (ssize_t) (so))) ) {
    fprintf(stderr, "%s() %d : pthread_create(receiver_thread) fails: %s\n", __func__, __LINE__, strerror(status));
    goto end;
  }

  if ( (status = pthread_create(&sender_thread_id, NULL, client_mode_sender_thread, (void*)(ssize_t)(so))) ) {
    fprintf(stderr, "%s() %d : pthread_create(sender_thread) fails: %s\n", __func__, __LINE__, strerror(status));
    goto end;
  }

end:

  if ( sender_thread_id != 0 ) {
    pthread_join(sender_thread_id, NULL);
  }

  if ( receiver_thread_id != 0 ) {
    fprintf(stderr, "%s() %d : stopping receiver_thread\n", __func__, __LINE__);
    stop_receiver_thread = 1;
    pthread_join(receiver_thread_id, NULL);
  }

  if ( so != -1 ) {
    close(so);
  }

  free(pkts);
}

static void my_signal_handler(int signum, siginfo_t *si, void * context)
{
  int ignore = 0;


  (void)(si);
  (void)(context);

  switch ( signum ) {
    case SIGINT :
    case SIGQUIT :
    case SIGABRT :
    case SIGTERM :
    case SIGSEGV :
    case SIGSTKFLT :
    case SIGILL :
    case SIGBUS :
    case SIGSYS :
    case SIGFPE :
      save_meauremenrs();
    break;

    default :
      ignore = 1;
    break;
  }

  if ( !ignore ) {
    exit(0);
  }
}

static int set_signal_handler( void (*f)(int, siginfo_t *, void *) )
{
  struct sigaction sa;
  int sig;

  bzero(&sa, sizeof( sa ));
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sa.sa_sigaction = f;

  for ( sig = 1; sig <= SIGUNUSED; ++sig ) {
    /* skip unblockable signals */
    if ( sig != SIGKILL && sig != SIGSTOP && sig != SIGCONT && sigaction(sig, &sa, NULL) != 0  ) {
      return -1;
    }
  }
  return 0;
}


static void usage()
{
  printf("ntest2\n");
  printf("  measure sphone6 packet loss and delay\n");
  printf("\n");
  printf("Run in one of 2 modes:\n");
  printf(" server mode:\n");
  printf("   $ ntest listen=[address]:port\n");
  printf("\n");
  printf(" client mode\n");
  printf("   $ ntest send=address:port [n=<number-packets-to-send>] "
      "[dup=<none|inline|separate|xor>] [do=<dup-offset>] "
      "[loss-emu-ratio=<0..100>] [srand=<integer>] "
      "[-o <output_filename> ]\n");
  printf("\n");
}

int main(int argc, char *argv[])
{
  int mode = invalid_mode;

  int i;

  for ( i = 1; i < argc; ++i ) {

    if ( strcmp(argv[i], "help") == 0 || strcmp(argv[i], "-help") == 0 || strcmp(argv[i], "--help") == 0 ) {
      usage();
      return 0;
    }

    if ( strncmp(argv[i], "listen=", 7) == 0 ) {

      if ( mode != invalid_mode ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      if ( argv[i][7] == ':' ) {
        if ( sscanf(argv[i] + 8, "%hu", &listen_port) != 1 ) {
          fprintf(stderr, "Invalid argument %s\n", argv[i]);
          return 1;
        }
      }
      else if ( sscanf(argv[i] + 7, "%15[^:]:%hu", listen_address, &listen_port) != 2 ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      mode = server_mode;
    }

    else if ( strncmp(argv[i], "send=", 5) == 0 ) {

      if ( mode != invalid_mode ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      if ( sscanf(argv[i] + 5, "%255[^:]:%hu", send_address, &send_port) != 2 || send_port == 0 ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }
      else {
        mode = client_mode;
      }
    }

    else if ( strncmp(argv[i], "pkt-size=", 9) == 0 ) {

      if ( mode != client_mode ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      if ( pktsize > 0 || sscanf(argv[i] + 9, "%d", &pktsize) != 1 || pktsize < (int) sizeof(struct pkt) ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }
    }

    else if ( strncmp(argv[i], "interval=", 9) == 0 ) {

      int interval = -1;

      if ( mode != client_mode ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      if ( send_interval > 0 ) { /* already set */
        fprintf(stderr, "Duplicate argument %s\n", argv[i]);
        return 1;
      }

      if ( sscanf(argv[i] + 9, "%d", &interval) != 1 || interval < 0 ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      send_interval = interval * 1000L;
    }

    else if ( strncmp(argv[i], "n=", 2) == 0 ) {

      if ( mode != client_mode ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      if ( maxpkts > 0 ) { /* already set */
        fprintf(stderr, "Duplicate argument %s\n", argv[i]);
        return 1;
      }

      if ( sscanf(argv[i] + 2, "%d", &maxpkts) != 1 || maxpkts < 1 ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }
    }

    else if ( strncmp(argv[i], "dup=", 4) == 0 ) {

      if ( mode != client_mode ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      if ( strcmp(argv[i] + 4, "none") == 0 ) {
        dup_mode = dup_none;
      }
      else if ( strcmp(argv[i] + 4, "separate") == 0 ) {
        dup_mode = dup_separate;
      }
      else if ( strcmp(argv[i] + 4, "inline") == 0 ) {
        dup_mode = dup_inline;
      }
      else if ( strcmp(argv[i] + 4, "xor") == 0 ) {
        //dup_mode = dup_xor;
        fprintf(stderr, "The 'xor' mode is not implemented\n");
        return 1;
      }
      else {
        fprintf(stderr, "Invalid value %s. must be one of  none, inline, separate, xor\n", argv[i]);
        return 1;
      }
    }
    else if ( strncmp(argv[i], "do=", 3) == 0 ) {

      if ( mode != client_mode ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      if ( sscanf(argv[i] + 3, "%d", &dup_offset) != 1 || dup_offset < 0 ) {
        fprintf(stderr, "Invalid value %s: must be non-negative integer\n", argv[i]);
        return 1;
      }
    }
    else if ( strncmp(argv[i], "loss-emu-ratio=", 15) == 0 ) {

      if ( mode != client_mode ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      if ( sscanf(argv[i] + 15, "%d", &loss_emu_ratio) != 1 || loss_emu_ratio < 0 || loss_emu_ratio > 100 ) {
        fprintf(stderr, "Invalid value %s: must be non-negative integer in range 0..100\n", argv[i]);
        return 1;
      }
    }
    else if ( strncmp(argv[i], "srand=", 6) == 0 ) {

      if ( mode != client_mode ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      if ( sscanf(argv[i] + 6, "%d", &loss_emu_srand) != 1 ) {
        fprintf(stderr, "Invalid value %s: must integer\n", argv[i]);
        return 1;
      }
    }
    else if ( strcmp(argv[i], "-o") == 0 ) {

      if ( mode != client_mode ) {
        fprintf(stderr, "Invalid argument %s\n", argv[i]);
        return 1;
      }

      if ( ++i >= argc ) {
        fprintf(stderr, "Output file nog specified\n");
        return 1;
      }

      strncpy(output_filename, argv[i], sizeof(output_filename)-1);
    }


    else {
      fprintf(stderr, "Invalid argument %s\n", argv[i]);
      return 1;
    }
  }

  if ( mode == invalid_mode ) {
    usage();
    return 1;
  }

  if ( mode == server_mode ) {
    run_server_mode();
  }
  else {

    if ( pktsize < 0 ) {
      pktsize = 36; /* defaults to sphone6 packet size */
    }

    if ( send_interval < 0 ) {
      send_interval = 40 * 1000; ; /* defaults to sphone6 packet interval */
    }

    if ( maxpkts < 0 ) {
      maxpkts = 30 * 1000000 / send_interval; /* defaults to 30 sec test */
    }

    set_signal_handler(my_signal_handler);

    run_client_mode();
  }


  return 0;
}
