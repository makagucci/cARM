/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2003-2004  Theodore A. Roth  <troth@openavr.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* $Id$ */

/* serjtag -- Serial Jtag Cable (using AVR) */

#include "ac_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <unistd.h>

#include "avr.h"
#include "pgm.h"
#include "serjtag.h"
#include "serial.h"

#define SERJTAG_DEBUG	0

extern char * progname;
extern int do_cycles;
extern int ovsigck;
extern int verbose;

static int serjtag_send(PROGRAMMER * pgm, char * buf, size_t len)
{
  return serial_send(&pgm->fd, (unsigned char *)buf, len);
}


static int serjtag_recv(PROGRAMMER * pgm, char * buf, size_t len)
{
  int rv;

  rv = serial_recv(&pgm->fd, (unsigned char *)buf, len);
  if (rv < 0) {
    fprintf(stderr,
	    "%s: serjtag_recv(): programmer is not responding\n",
	    progname);
    exit(1);
  }
  return 0;
}


static int serjtag_drain(PROGRAMMER * pgm, int display)
{
  return serial_drain(&pgm->fd, display);
}

static int serjtag_chip_erase(PROGRAMMER * pgm, AVRPART * p)
{
  unsigned char cmd[4];
  unsigned char res[4];

  if (p->op[AVR_OP_CHIP_ERASE] == NULL) {
    fprintf(stderr, "chip erase instruction not defined for part \"%s\"\n",
            p->desc);
    return -1;
  }

  memset(cmd, 0, sizeof(cmd));

  avr_set_bits(p->op[AVR_OP_CHIP_ERASE], cmd);
  pgm->cmd(pgm, cmd, res);
  usleep(p->chip_erase_delay);
  pgm->initialize(pgm, p);

  return 0;
}

#if SERJTAG_DEBUG == 1
static void  serjtag_check(PROGRAMMER * pgm) {
  int i,bytes;
  char buf[128];

fprintf(stderr,"check response\n");
fflush(stderr);
  i=0;
  buf[i++] = 'r'; 
  buf[i++] = 0;
  buf[i++] = 0;
  serjtag_send(pgm, buf, i);
  serjtag_recv(pgm, buf, 1);
fprintf(stderr,"\tCommonad = 0x%02x %c\n",buf[0],buf[0]);
  serjtag_recv(pgm, buf+1, 1);
fprintf(stderr,"\tFlags = 0x%02x\n",buf[1]);
  serjtag_recv(pgm, buf+2, 1);
fprintf(stderr,"\tBytes = 0x%02x\n",buf[2]);
  bytes = buf[2];
  if (buf[1] & JTAG_BITS) bytes++;
  if (bytes) {
	fprintf(stderr,"\t");
  	for (i=0; i<bytes; i++) {
  		serjtag_recv(pgm, buf+2+i, 1);
		fprintf(stderr,"%02x ",buf[2+i]);
  	}
	fprintf(stderr,"\n");
  }
}
#endif

static int serjtag_recv_j(PROGRAMMER * pgm, char * buf, size_t len, int verbose)
{
  int bytes;
  serjtag_recv(pgm, buf, 3);	/* header */
#if 0
  if (verbose) {
	fprintf(stderr,"recv_j(0) %c 0x%02x %d :",buf[0],buf[1],buf[2]);
  }
#endif
  bytes = buf[2];
  if (buf[1] & JTAG_BITS) {
	  bytes++;
  }
  len -= 3;
  if (len >= bytes) {
    serjtag_recv(pgm, buf+3, bytes);
  } else {
	char tmp[1];
	int i;
  	serjtag_recv(pgm, buf+3, len);
	for (i=0; i< bytes - len; i++) {
  		serjtag_recv(pgm, tmp, 1);
	}
  }
  if (verbose) {
	int i;
	fprintf(stderr,"recv_j %c 0x%02x %d :",buf[0],buf[1],buf[2]);
	for (i=0; i<bytes; i++) {
		fprintf(stderr,"%02x ",buf[3+i]&0xff);
	}
	fprintf(stderr,"\n");
  }
  return 0;
}

static int delay_param = 3;
static int use_delay = JTAG_USE_DELAY;
static unsigned char saved_signature[3];

static void serjtag_set_delay(PROGRAMMER * pgm) {
   use_delay = JTAG_USE_DELAY;
  if (pgm->bitclock == 0.0) { // using default
	delay_param = 3;
  } else if (pgm->bitclock >= 4.0) {
	delay_param = 0;
	use_delay = 0;
  } else {
  	delay_param =  (int)((2.26/3.0) / pgm->bitclock);
  	if (delay_param > 15) delay_param = 15;
  }
  if ((verbose>=1) || SERJTAG_DEBUG) {
	fprintf(stderr," serjtag:delay %d (%s) ( bitclk %.3f )\n"
		,delay_param, use_delay? "enabled":"disabled", pgm->bitclock);
  }
}

/*
 * issue the 'program enable' command to the AVR device
 */
static int serjtag_program_enable(PROGRAMMER * pgm, AVRPART * p)
{
  char buf[128];
  int i;
  int retry_count = 0;

   serjtag_set_delay(pgm);

  serjtag_send(pgm, "j", 1); // enter jtag mode
  serjtag_recv(pgm, buf, 1);
  if (buf[0] != 'Y') {
	  return -1;
  }
#if SERJTAG_DEBUG == 1
fprintf(stderr," Enter jtag mode .. success \n");
#endif

retry:
  i=0;
  buf[i++] = 's';		/* Set Port */
  buf[i++] = 0;			/* flags */
  buf[i++] = 1;			/* bytes */
  buf[i++] = 0;  /* TDI(MOSI) = 0, TMS(RESET) = 0, TCK(SCK) = 0 */
  serjtag_send(pgm, buf, i);
  usleep(5000); // 5ms
  i=0;
  buf[i++] = 's'; /* Set Port */
  buf[i++] = 0;  /* flags */
  buf[i++] = 1;   /* bytes */
  buf[i++] = JTAG_SET_TMS;  /* TDI(MOSI) = 0, TMS(RESET) = 1, TCK(SCK) = 0 */
  serjtag_send(pgm, buf, i);
  usleep(5000); // 5ms
  i=0;
  buf[i++] = 's';		/* Set Port */
  buf[i++] = 0;			/* flags */
  buf[i++] = 1;			/* bytes */
  buf[i++] = delay_param & JTAG_SET_DELAY;  /* TMS(RESET) = 0, set delay */
  serjtag_send(pgm, buf, i);
  usleep(5000); // 5ms

  i = 0;
  buf[i++] = 'd';               /* PUT TDI Stream */
  buf[i++] = JTAG_RECIEVE | JTAG_USE_DELAY;
  buf[i++] = 0;			/* bytes : set after*/
  /* check */
  buf[i++] = 0xAC;
  buf[i++] = 0x53;
  buf[i++] = 0;
  buf[i++] = 0;
  /* signature[0] */
  buf[i++] = 0x30;
  buf[i++] = 0;
  buf[i++] = 0;
  buf[i++] = 0;
  /* signature[1] */
  buf[i++] = 0x30;
  buf[i++] = 0;
  buf[i++] = 1;
  buf[i++] = 0;
  /* signature[2] */
  buf[i++] = 0x30;
  buf[i++] = 0;
  buf[i++] = 2;
  buf[i++] = 0;
  buf[2] = i - 3; /* set bytes */
  buf[i++] = 'r';                /* Request Recieved Data */
  buf[i++] = 0; 		 /* flags */
  buf[i++] = 0;			 /* bytes */
  serjtag_send(pgm, buf, i);
#if SERJTAG_DEBUG == 1
  fprintf(stderr,"enabling program delay %d retry %d\n",delay_param,retry_count);
#endif
  serjtag_recv_j(pgm, buf, 3+4*4, SERJTAG_DEBUG?1:0);

  saved_signature[0] = buf[3+4*1 +3];
  saved_signature[1] = buf[3+4*2 +3];
  saved_signature[2] = buf[3+4*3 +3];
#if SERJTAG_DEBUG == 1
  fprintf(stderr,"saved_signature %02x %02x %02x\n"
		  , saved_signature[0]
		  , saved_signature[1]
		  , saved_signature[2]);
#endif
  if ((buf[3+2] == 0x53) && (saved_signature[0] == 0x1e)) // success
  {
	  return 0;
  }
  if (retry_count < 5) {
	  retry_count++;
	  goto retry;
  }
  return -1;
}

static int serjtag_read_sig_bytes(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m)
{
   m->buf[0] = saved_signature[0];
   m->buf[1] = saved_signature[1];
   m->buf[2] = saved_signature[2];
   return 3;
}

/*
 * initialize the AVR device and prepare it to accept commands
 */
static int serjtag_initialize(PROGRAMMER * pgm, AVRPART * p)
{
  char id[8];
  char sw[3];

  /* Get the programmer identifier. Programmer returns exactly 7 chars
     _without_ the null.*/

  serjtag_send(pgm, "S", 1);
  serjtag_recv(pgm, id, 7);
  id[7] = 0;

  /* Get the HW and SW versions to see if the programmer is present. */

  serjtag_send(pgm, "V", 1);
  serjtag_recv(pgm, sw, 2);
  sw[2] = 0;

  fprintf(stderr, "Found programmer: Id = \"%s\"; Revison = %s\n", id, sw);

  if (strncmp(id, "SERJTAG", 7) && strncmp(id, "USB910A", 7)
       && strncmp(id, "USB910A", 7) )
  {
     fprintf(stderr, "\tserjtag protocol not supported.\n");
     exit(1);
  }
  return serjtag_program_enable(pgm, p);
}

static void serjtag_disable(PROGRAMMER * pgm)
{
  char buf[2];
  serjtag_send(pgm, "V", 1);
  serjtag_recv(pgm, buf, 2);

  return;
}


static void serjtag_enable(PROGRAMMER * pgm)
{
  /* Do nothing. */

  return;
}


/*
 * transmit an AVR device command and return the results; 'cmd' and
 * 'res' must point to at least a 4 byte data buffer
 */
static int serjtag_cmd(PROGRAMMER * pgm, unsigned char cmd[4], 
                      unsigned char res[4])
{
  char buf[10];

  /* FIXME: Insert version check here */

  buf[0] = 'd';                 /* PUT TDI Stream */
  buf[1] = JTAG_RECIEVE | use_delay;
  buf[2] = 4;			/* bytes */
  buf[3] = cmd[0];
  buf[4] = cmd[1];
  buf[5] = cmd[2];
  buf[6] = cmd[3];
  buf[7] = 'r';                 /* Request Recieved Data */
  buf[8] = 0;
  buf[9] = 0;

  serjtag_send (pgm, buf, 10);
  serjtag_recv (pgm, buf, 7);

  res[0] = 0x00;                /* Dummy value */
  res[1] = cmd[0];
  res[2] = cmd[1];
  res[3] = buf[6];

  return 0;
}


static int serjtag_open(PROGRAMMER * pgm, char * port)
{
  /*
   *  If baudrate was not specified use 19.200 Baud
   */
  if(pgm->baudrate == 0) {
    pgm->baudrate = 19200;
  }

  strcpy(pgm->port, port);
  serial_open(port, pgm->baudrate, &pgm->fd);

  /*
   * drain any extraneous input
   */
  serjtag_drain (pgm, 0);
	
  return 0;
}

static void serjtag_close(PROGRAMMER * pgm)
{
  serial_close(&pgm->fd);
  pgm->fd.ifd = -1;
}


static void serjtag_display(PROGRAMMER * pgm, char * p)
{
  return;
}

static int serjtag_paged_write_gen(PROGRAMMER * pgm, AVRPART * p,
                                     AVRMEM * m, int page_size, int n_bytes)
{
  unsigned long    i;
  int rc;
  for (i=0; i<n_bytes; i++) {
    report_progress(i, n_bytes, NULL);

    rc = avr_write_byte_default(pgm, p, m, i, m->buf[i]);
    if (rc != 0) {
      return -2;
    }

    if (m->paged) {
      /*
       * check to see if it is time to flush the page with a page
       * write
       */
      if (((i % m->page_size) == m->page_size-1) || (i == n_bytes-1)) {
        rc = avr_write_page(pgm, p, m, i);
        if (rc != 0) {
	  return -2;
        }
      }
    }
  }
  return i;
}

#define SERJTAG_BUFFERD_WRITE

#ifdef SERJTAG_BUFFERD_WRITE
#define SERJTAG_BUF_SIZE	1024
unsigned char *serjtag_buf;
int serjtag_buf_len;

static void serjtag_flush(PROGRAMMER * pgm) {
   if (!serjtag_buf || (serjtag_buf_len == 0)) return;
   serjtag_send(pgm, serjtag_buf, serjtag_buf_len);
   serjtag_buf_len = 0;
}

static void serjtag_write(PROGRAMMER * pgm, unsigned char *buf, int size) {
  if (!serjtag_buf) {
     serjtag_buf = malloc(SERJTAG_BUF_SIZE);
     if (!serjtag_buf) {
       fprintf(stderr, "can't alloc memory\n");
       exit(1);
     }
  }
  if (SERJTAG_BUF_SIZE < serjtag_buf_len + size) {
      serjtag_flush(pgm);
  }
  memcpy(serjtag_buf + serjtag_buf_len, buf, size);
  serjtag_buf_len += size;
}
#else
static void serjtag_flush(PROGRAMMER * pgm) {
}
static void serjtag_write(PROGRAMMER * pgm, unsigned char *buf, int size) {
   serjtag_send(pgm, buf, size);
}
#endif

#define USE_INLINE_WRITE_PAGE

static int serjtag_paged_write_flash(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m, 
                                    int page_size, int n_bytes)
{
  unsigned int    i,j;
  int addr,addr_save,buf_pos,do_page_write;
  char buf[128];

  addr = 0;
  for (i=0; i<n_bytes; ) {
     addr_save = addr;
     buf_pos = 0;
     buf[buf_pos++] = 'd';               /* PUT TDI Stream */
     buf[buf_pos++] = use_delay; 
     buf[buf_pos++] = 0; /* bytes : set after */
     do_page_write = 0;
     for (j=0; j<(JTAG_BUFSIZE-3)/4; j++) { // 15 bytes
        buf[buf_pos++] = (addr & 1)?0x48:0x40;
        buf[buf_pos++] = (addr >> 9) & 0xff;
        buf[buf_pos++] = (addr >> 1) & 0xff;
        buf[buf_pos++] = m->buf[i];
	addr ++;
	i++;
	if ( (m->paged) &&
             (((i % m->page_size) == 0) || (i == n_bytes))) {
		do_page_write = 1;
		break;
	}
     }
     buf[2] = buf_pos - 3;
#ifdef USE_INLINE_WRITE_PAGE
     if (do_page_write) {
	int addr_wk = addr_save - (addr_save % m->page_size);
        /* If this device has a "load extended address" command, issue it. */
	if (m->op[AVR_OP_LOAD_EXT_ADDR]) {
	    OPCODE *lext = m->op[AVR_OP_LOAD_EXT_ADDR];

            buf[buf_pos++] = 'd';               /* PUT TDI Stream */
            buf[buf_pos++] = JTAG_RECIEVE | use_delay; 
            buf[buf_pos++] = 4; /* bytes */

	    memset(buf+buf_pos, 0, 4);
	    avr_set_bits(lext, buf+buf_pos);
	    avr_set_addr(lext, buf+buf_pos, addr/2);
            buf_pos += 4;
	}
        buf[buf_pos++] = 'd';               /* PUT TDI Stream */
        buf[buf_pos++] = JTAG_RECIEVE | use_delay; 
        buf[buf_pos++] = 4; /* bytes */
        buf[buf_pos++] = 0x4C; /* Issue Page Write */
        buf[buf_pos++] = (addr_wk >> 9) & 0xff;
        buf[buf_pos++] = (addr_wk >> 1) & 0xff;
        buf[buf_pos++] = 0;
        buf[buf_pos++] = 'r';                /* Request Recieved Data */
        buf[buf_pos++] = 0;
        buf[buf_pos++] = 0;
     }
#endif
     serjtag_write(pgm, buf, buf_pos);
#if 0
fprintf(stderr, "send addr 0x%04x size %d bufsize %d i %d page_write %d\n",
		addr_wk,buf[2],buf_pos,i,do_page_write);
#endif
     if (do_page_write) {
#ifdef USE_INLINE_WRITE_PAGE
        serjtag_flush(pgm);
        usleep(m->max_write_delay);
        serjtag_recv_j(pgm, buf, 4 + 3, 0);
#else
        int rc;
        serjtag_flush(pgm);
	addr_wk = (i-1) / m->page_size * m->page_size;
        rc = avr_write_page(pgm, p, m, addr_wk);
        if (rc != 0) {
	  return -2;
        }
#endif
     }
     report_progress(i, n_bytes, NULL);
  }
  serjtag_flush(pgm);
  return i;
}


static int serjtag_paged_write(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m,
                              int page_size, int n_bytes)
{
  if (strcmp(m->desc, "flash") == 0) {
    return serjtag_paged_write_flash(pgm, p, m, page_size, n_bytes);
  }
  else if (strcmp(m->desc, "eeprom") == 0) {
    return serjtag_paged_write_gen(pgm, p, m, page_size, n_bytes);
  }
  else {
    return -2;
  }
}


static int serjtag_paged_load_gen(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m, 
                             int page_size, int n_bytes)
{
  unsigned char    rbyte;
  unsigned long    i;
  int rc;

  for (i=0; i<n_bytes; i++) {
     rc = avr_read_byte_default(pgm, p, m, i, &rbyte);
     if (rc != 0) {
       return -2;
     }
     m->buf[i] = rbyte;
     report_progress(i, n_bytes, NULL);
  }
  return 0;
}


static struct serjtag_request {
	int addr;
	int bytes;
	int n;
	struct serjtag_request *next;
} *req_head,*req_tail,*req_pool;

static void put_request(int addr, int bytes, int n)
{
  struct serjtag_request *p;
  if (req_pool) {
     p = req_pool;
     req_pool = p->next;
  } else {
     p = malloc(sizeof(struct serjtag_request));
     if (!p) {
       fprintf(stderr, "can't alloc memory\n");
       exit(1);
     }
  }
  memset(p, 0, sizeof(struct serjtag_request));
  p->addr = addr;
  p->bytes = bytes;
  p->n = n;
  if (req_tail) {
     req_tail->next = p;
     req_tail = p;
  } else {
     req_head = req_tail = p;
  }
}

static int do_request(PROGRAMMER * pgm, AVRMEM *m)
{
  struct serjtag_request *p;
  int addr, bytes, j, n;
  char buf[128];

  if (!req_head) return 0;
  p = req_head;
  req_head = p->next;
  if (!req_head) req_tail = req_head;

  addr = p->addr;
  bytes = p->bytes;
  n = p->n;
  memset(p, 0, sizeof(struct serjtag_request));
  p->next = req_pool;
  req_pool = p;

  serjtag_recv_j(pgm, buf, bytes + 3, 0);
  for (j=0; j<n; j++) {
     m->buf[addr++] = buf[3 + 4*j + 3];
  }
  return 1;
}

#define REQ_OUTSTANDINGS	10
static int serjtag_paged_load_flash(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m, 
                             int page_size, int n_bytes)
{
  unsigned long    i,j,n;
  //int rc;
  int addr,addr_save,bytes,buf_pos;
  int req_count = 0;
  char buf[128];

  addr = 0;
  for (i=0; i<n_bytes; ) {
     buf_pos = 0;
     buf[buf_pos++] = 'd';               /* PUT TDI Stream */
     buf[buf_pos++] = JTAG_RECIEVE | use_delay; 
     buf[buf_pos++] = 0; /* bytes : set after */
     addr_save = addr;
     for (j=0; j<(JTAG_BUFSIZE-3*2)/4; j++) { // 14 bytes
	if (i >= n_bytes) break;
        buf[buf_pos++] = (addr & 1)?0x28:0x20;
        buf[buf_pos++] = (addr >> 9) & 0xff;
        buf[buf_pos++] = (addr >> 1) & 0xff;
        buf[buf_pos++] = 0;
	addr ++;
	i++;
     }
     n = j;
     buf[2] = bytes =  buf_pos - 3;
     buf[buf_pos++] = 'r';                /* Request Recieved Data */
     buf[buf_pos++] = 0;
     buf[buf_pos++] = 0;
     serjtag_send(pgm, buf, buf_pos);
     put_request(addr_save, bytes, n);
     req_count++;
     if (req_count > REQ_OUTSTANDINGS)
        do_request(pgm, m);
     report_progress(i, n_bytes, NULL);
  }
  while (do_request(pgm, m))
	  ;
  return 0;
}

static int serjtag_paged_load(PROGRAMMER * pgm, AVRPART * p, AVRMEM * m, 
                             int page_size, int n_bytes)
{
  if (strcmp(m->desc, "flash") == 0) {
    return serjtag_paged_load_flash(pgm, p, m, page_size, n_bytes);
  }
  else if (strcmp(m->desc, "eeprom") == 0) {
    return serjtag_paged_load_gen(pgm, p, m, page_size, n_bytes);
  }
  else {
    return -2;
  }
}

void serjtag_initpgm(PROGRAMMER * pgm)
{
  strcpy(pgm->type, "serjtag");

  /*
   * mandatory functions
   */
  pgm->initialize     = serjtag_initialize;
  pgm->display        = serjtag_display;
  pgm->enable         = serjtag_enable;
  pgm->disable        = serjtag_disable;
  pgm->program_enable = serjtag_program_enable;
  pgm->chip_erase     = serjtag_chip_erase;
  pgm->cmd            = serjtag_cmd;
  pgm->open           = serjtag_open;
  pgm->close          = serjtag_close;
  pgm->read_byte      = avr_read_byte_default;
  pgm->write_byte     = avr_write_byte_default;

  /*
   * optional functions
   */

  pgm->paged_write = serjtag_paged_write;
  pgm->paged_load = serjtag_paged_load;
  pgm->read_sig_bytes = serjtag_read_sig_bytes;
}
