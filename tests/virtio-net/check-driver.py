#!/usr/bin/env python3
"""Exercise the actual XIF C code with a device that withholds TX completions.

Only the kernel and MMIO boundary is substituted. The queue, ownership,
packet-length checks, open/close and output functions are the driver source.
Pass --expect-failure with an unfixed source to check the regression control.
"""
import argparse
import os
import pathlib
import re
import subprocess
import tempfile

SUPPORT = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "virtio_net.h"
#define _cdecl
#define BUF_NORMAL 0
#define BUF_ATOMIC 1
#define SIOCSIFNETMASK 1
#define SIOCSIFFLAGS 2
#define SIOCSIFADDR 3
#define SIOCSIFMTU 4
#define ENOSYS 38
#define IFF_BROADCAST 2
#define ETH_ALEN 6
#define ETH_MAX_DLEN 1500
#define HWTYPE_ETH 1
#define IF_MAXQ 60
typedef struct proc { int dummy; } PROC;
typedef struct timeout { int dummy; } TIMEOUT;
typedef struct buf { unsigned char *dstart, *dend; short info; struct buf *next; } BUF;
struct ifq { int qlen, maxqlen; BUF *first, *last; };
struct hw { short len; struct { unsigned char bytes[6]; } adr; };
struct netif {
 char name[16]; short unit, flags, hwtype, timer, metric, mtu, maxpackets;
 struct hw hwlocal, hwbrcst; struct ifq snd, rcv; void *bpf, *data;
 unsigned long in_packets, in_errors, out_packets, out_errors;
 long (*open)(struct netif *); long (*close)(struct netif *);
 long (*output)(struct netif *, BUF *, const char *, short, short);
 long (*ioctl)(struct netif *, short, long); void (*timeout)(struct netif *);
};
static int alloc_calls, feature_writes, failures;
static unsigned long device_status, feature_select, driver_select, pic[8];
static TIMEOUT timer;
static void *kmalloc(unsigned long n) { return calloc(1, n); }
static void kfree(void *p) { free(p); }
static unsigned short spl7(void) { return 0; }
static void spl(unsigned short sr) { (void)sr; }
static void c_conws(const char *s) { (void)s; }
static int ksprintf(char *s, const char *f, ...) {
 int n; va_list a; va_start(a,f); n=vsprintf(s,f,a); va_end(a); return n;
}
static long Setexc(short n, long handler) { (void)n; (void)handler; return 0; }
static TIMEOUT *addroottimeout(long delay, void (*f)(PROC *,long), unsigned short flags) {
 (void)delay; (void)f; (void)flags; return &timer;
}
static void cancelroottimeout(TIMEOUT *t) { (void)t; }
static void buf_deref(BUF *b, int mode) { (void)mode; free(b->dstart); free(b); }
static BUF *buf_alloc(long n, long reserve, int mode) {
 BUF *b=calloc(1,sizeof(*b)); (void)reserve; (void)mode;
 b->dstart=calloc(1,n); b->dend=b->dstart; ++alloc_calls; return b;
}
static long if_enqueue(struct ifq *q, BUF *b, short priority) {
 (void)priority;
 if (q->qlen>=q->maxqlen) { buf_deref(b,0); return ENOMEM; }
 b->next=NULL; if(q->last) q->last->next=b; else q->first=b;
 q->last=b; ++q->qlen; return 0;
}
static BUF *if_dequeue(struct ifq *q) {
 BUF *b=q->first; if(!b)return NULL; q->first=b->next;
 if(!q->first)q->last=NULL; --q->qlen; return b;
}
static void if_flushq(struct ifq *q) { BUF *b; while((b=if_dequeue(q)))buf_deref(b,0); }
static long if_input(struct netif *n, BUF *b, long delay, short type) {
 (void)n; (void)delay; (void)type; buf_deref(b,0); return 0;
}
static BUF *eth_build_hdr(BUF *b, struct netif *n, const char *a, short type) {
 (void)n; (void)a; (void)type; return b;
}
static short eth_remove_hdr(BUF *b) { (void)b; return 0; }
static void bpf_input(struct netif *n, BUF *b) { (void)n; (void)b; }
static short if_getfreeunit(const char *s) { (void)s; return 0; }
static void if_register(struct netif *n) { (void)n; }
void (*virtio_net_old_interrupt)(void);
void virtio_net_irq_entry(void) {}
'''

MMIO = r'''
static unsigned long vio_read32(short slot, short reg) {
 if(slot!=127)return 0;
 switch(reg) {
 case VIRTIO_MMIO_MAGIC:return VIRTIO_MAGIC;
 case VIRTIO_MMIO_VERSION:return 2;
 case VIRTIO_MMIO_DEVICE_ID:return VIRTIO_ID_NET;
 case VIRTIO_MMIO_DEVICE_FEATURES:return feature_select ? 1 : (1UL<<VIRTIO_NET_F_MAC);
 case VIRTIO_MMIO_QUEUE_NUM_MAX:return VN_QSIZE;
 case VIRTIO_MMIO_STATUS:return device_status;
 default:return 0;
 }
}
static void vio_write32(short slot, short reg, unsigned long value) {
 (void)slot;
 if(reg==VIRTIO_MMIO_STATUS)device_status=value;
 if(reg==VIRTIO_MMIO_DEVICE_FEATURES_SEL)feature_select=value;
 if(reg==VIRTIO_MMIO_DRIVER_FEATURES_SEL)driver_select=value;
 if(reg==VIRTIO_MMIO_DRIVER_FEATURES)++feature_writes;
}
static unsigned char vio_cfg8(short slot, short off) { (void)slot; return off+1; }
'''

TEST = r'''
static void check(int pass, const char *what) {
 if(!pass) { ++failures; printf("FAIL %s\n",what); }
}
static BUF *packet(unsigned char value) {
 BUF *b=buf_alloc(64,0,0); memset(b->dstart,value,64); b->dend+=64; return b;
}
int main(void) {
 int i, before; unsigned long errors;
 check(driver_init()==0,"device initializes");
 check(virtio_net_open(&if_virtio)==0,"interface opens");
 for(i=0;i<VN_QSIZE;i++)
  check(virtio_net_output(&if_virtio,packet(i),"",0,0)==0,"TX submit");
 check(virtio_net_output(&if_virtio,packet(99),"",0,0)==0,"TX queues while device busy");
 check(vq_tx.bufs[VIRTIO_NET_HDR_LEN]==0,"device-owned TX buffer stays unchanged");
 check(if_virtio.snd.qlen==1,"one packet waits for a real completion");
 /* Return descriptor 7, proving reclamation follows IDs, not FIFO guesses. */
 st32(vq_tx.used+4,7); st16(vq_tx.used+2,1);
 virtio_net_poll_timeout(NULL,0);
 check(vq_tx.bufs[7*VN_BUFSIZE+VIRTIO_NET_HDR_LEN]==99,"returned descriptor can be reused");
 check(if_virtio.snd.qlen==0,"queued packet drains after completion");
 before=alloc_calls; errors=if_virtio.in_errors;
 st32(vq_rx.used+4,0); st32(vq_rx.used+8,VN_BUFSIZE+64); st16(vq_rx.used+2,1);
 virtio_net_poll(&if_virtio);
 check(alloc_calls==before && if_virtio.in_errors==errors+1,"oversized RX completion is rejected before copy");
 before=feature_writes;
 check(virtio_net_close(&if_virtio)==0,"interface closes");
 check(virtio_net_open(&if_virtio)==0,"interface reopens");
 check(feature_writes>before,"reopen renegotiates device features after reset");
 virtio_net_close(&if_virtio);
 if(failures)printf("%d driver checks failed\n",failures);
 else puts("Driver ownership, receive bounds and reopen checks passed");
 return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("--expect-failure", action="store_true")
    args = parser.parse_args()
    source = args.source.read_text()
    source = re.sub(r'^#\s*include[^\n]*\n', '', source, flags=re.MULTILINE)
    a = source.index('static inline volatile unsigned long *vio_reg')
    b = source.index('static struct netif if_virtio;', a)
    source = source[:a] + MMIO + source[b:]
    # Substitute the PIC register boundary when present in the new driver.
    source = re.sub(r'(static volatile unsigned long \*pic_reg\(short offset\)\n)\{.*?\n\}',
                    r'\1{ return &pic[offset / 4]; }', source, flags=re.DOTALL)
    work = pathlib.Path(tempfile.mkdtemp(prefix="virtio-net-unit-"))
    harness = work / "driver.c"
    harness.write_text(SUPPORT + source + TEST)
    binary = work / "test"
    subprocess.run(['cc', '-std=gnu99', '-O1', '-g', '-Wno-int-to-pointer-cast',
                    '-fsanitize=address,undefined', '-I', str(args.source.parent),
                    str(harness), '-o', str(binary)], check=True)
    result = subprocess.run([str(binary)], env={**os.environ,
                                               'ASAN_OPTIONS': 'detect_leaks=0'})
    return 0 if bool(result.returncode) == args.expect_failure else 1


if __name__ == '__main__':
    raise SystemExit(main())
