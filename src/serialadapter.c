/*
 * AVRDUDE - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2023 Stefan Rueger <stefan.rueger@urclocks.com>
 * Copyright (C) 2023 Hans Eirik Bull
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
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ac_cfg.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "avrdude.h"
#include "libavrdude.h"

#ifdef HAVE_LIBSERIALPORT

#include <libserialport.h>
#include <ctype.h>

typedef struct {
  int vid, pid;
  char *sernum, *port;
} SERPORT;

// Set new port string freeing any previously set one
static int sa_setport(char **portp, const char *sp_port) {
  if(!sp_port) {
    pmsg_warning("port string to be assigned is NULL\n");
    return -1;
  }

  if(portp) {
    if(*portp)
      free(*portp);
    *portp = cfg_strdup(__func__, sp_port);
  }

  return 0;
}

// Is the actual serial number sn matched by the query q?
static int sa_snmatch(const char *sn, const char *q) {
  return sn && (str_starts(sn, q) || (str_starts(q , "...") && str_ends(sn, q+3)));
}

// Order two SERPORTs port strings: base first then trailing numbers, if any
static int sa_portcmp(const void *p, const void *q) {
  int ret;
  const char *a = ((SERPORT *) p)->port, *b = ((SERPORT *) q)->port;
  const char *na = str_endnumber(a), *nb = str_endnumber(b);
  size_t la = strlen(a) - (na? strlen(na): 0), lb = strlen(b) - (nb? strlen(nb): 0);

  // Compare string bases first
  if(la && lb && (ret = strncasecmp(a, b, la < lb? la: lb)))
    return ret;
  if((ret = la-lb))
    return ret;

  // If string bases are the same then compare trailing numbers
  if(na && nb) {
    long long d;
    if((d = atoll(na)-atoll(nb)))
      return d < 0? -1: 1;
  } else if(na)
    return 1;
  else if(nb)
    return -1;

  // Ports are the same (this should not happen) but still compare vid, pid and sn
  if((ret = ((SERPORT *) p)->vid - ((SERPORT *) q)->vid))
    return ret;
  if((ret = ((SERPORT *) p)->pid - ((SERPORT *) q)->pid))
    return ret;

  return strcmp(((SERPORT *) p)->sernum, ((SERPORT *) q)->sernum);
}

// Get serial port data; allocate a SERPORT array sp, store data and return it
static SERPORT *get_libserialport_data(int *np) {
  struct sp_port **port_list = NULL;
  enum sp_return result = sp_list_ports(&port_list);

  if(result != SP_OK) {
    pmsg_error("sp_list_ports() failed!\n");
    sp_free_port_list(port_list);
    return NULL;
  }

  int i, j, n;
  // Count the number of available ports and allocate space according to the needed size
  for(n = 0; port_list[n]; n++)
    continue;
  SERPORT *sp = cfg_malloc(__func__, n*sizeof*sp);

  for(j = 0, i = 0; i < n; i++) { // j counts the number of valid ports
    struct sp_port *p = port_list[i];
    char *q;
    // Fill sp struct with port information
    if((q = sp_get_port_name(p))) {
      sp[j].port = cfg_strdup(__func__, q);
      if(sp_get_port_usb_vid_pid(p, &sp[j].vid, &sp[j].pid) != SP_OK)
        sp[j].vid = sp[j].pid = 0;
      sp[j].sernum = cfg_strdup(__func__, (q = sp_get_port_usb_serial(p))? q: "");
      j++;
    }
  }

  if(j > 0)
    qsort(sp, j, sizeof*sp, sa_portcmp);
  else
    free(sp), sp = NULL;

  sp_free_port_list(port_list);

  if(np)
    *np = j;
  return sp;
}

// Returns a NULL-terminated malloc'd list of items in SERPORT list spa that are not in spb
SERPORT **sa_spa_not_spb(SERPORT *spa, int na, SERPORT *spb, int nb) {
  SERPORT **ret = cfg_malloc(__func__, (na+1)*sizeof*ret);
  int ia = 0, ib = 0, ir = 0;

  // Use the comm algorithm on two sorted SERPORT lists
  while(ia < na && ib < nb) {
    int d = sa_portcmp(spa+ia, spb+ib);
    if(d < 0)
      ret[ir++] = spa+ia++;
    else if(d > 0)
      ib++;
    else
      ia++, ib++;
  }
  while(ia < na)
    ret[ir++] = spa+ia++;

  ret[ir] = NULL;
  return ret;
}

// Return number of SERPORTs that a serial adapter matches
static int sa_num_matches_by_sea(const SERIALADAPTER *sea, const char *sernum, const SERPORT *sp, int n) {
  const char *sn = *sernum? sernum: sea->usbsn;
  int matches = 0;

  for(int i = 0; i < n; i++)
    if(sp[i].vid == sea->usbvid)
      for(LNODEID usbpid = lfirst(sea->usbpid); usbpid; usbpid = lnext(usbpid))
        if(sp[i].pid == *(int *) ldata(usbpid) && sa_snmatch(sp[i].sernum, sn)) {
          matches++;
          break;
        }

  return matches;
}

// Return number of SERPORTs that a (vid, pid, sernum) triple matches
static int sa_num_matches_by_ids(int vid, int pid, const char *sernum, const SERPORT *sp, int n) {
  int matches = 0;

  for(int i = 0; i < n; i++)
    if(sp[i].vid == vid && sp[i].pid == pid && sa_snmatch(sp[i].sernum, sernum))
      matches++;

  return matches;
}

// Is the i-th SERPORT the only match with the serial adapter wrt all plugged-in ones?
static int sa_unique_by_sea(const SERIALADAPTER *sea, const char *sn, const SERPORT *sp, int n, int i) {
  return sa_num_matches_by_sea(sea, sn, sp, n) == 1 && sa_num_matches_by_sea(sea, sn, sp+i, 1);
}

// Is the i-th SERPORT the only match with (vid, pid, sn) wrt all plugged-in ones?
static int sa_unique_by_ids(int vid, int pid, const char *sn, const SERPORT *sp, int n, int i) {
  return sa_num_matches_by_ids(vid, pid, sn, sp, n) == 1 && sa_num_matches_by_ids(vid, pid, sn, sp+i, 1);
}

// Return a malloc'd list of -P specifications that uniquely address sp[i]
static char **sa_list_specs(const SERPORT *sp, int n, int i) {
  int Pn = 4, Pi = 0;
  char **Plist = cfg_malloc(__func__, Pn*sizeof*Plist);
  const char *sn = sp[i].sernum, *via = NULL;

  // Loop though all serial adapters in avrdude.conf
  for(LNODEID ln1 = lfirst(programmers); ln1; ln1=lnext(ln1)) {
    SERIALADAPTER *sea = ldata(ln1);
    if(!is_serialadapter(sea))
      continue;
    for(LNODEID sid = lfirst(sea->id); sid; sid = lnext(sid)) {
      char *id = ldata(sid);
      // Put id or id:sn into list if it uniquely matches sp[i]
      if(sa_unique_by_sea(sea, "", sp, n, i))
        Plist[Pi++] = cfg_strdup(__func__, id);
      else if(*sn && sa_unique_by_sea(sea, sn, sp, n, i))
        Plist[Pi++] = str_sprintf("%s:%s", id, sn);
      else if(!via && sa_num_matches_by_sea(sea, "", sp+i, 1))
        via = id;

      if(Pi >= Pn-1) {            // Ensure there is space for one more and NULL
        if(Pn >= INT_MAX/2)
          break;
        Pn *= 2;
        Plist = cfg_realloc(__func__, Plist, Pn*sizeof*Plist);
      }
    }
  }

  if(Pi == 0 && sp[i].vid) {    // No unique serial adapter, so maybe vid:pid[:sn] works?
    if(sa_unique_by_ids(sp[i].vid, sp[i].pid, "", sp, n, i))
      Plist[Pi++] = str_sprintf("usb:%04x:%04x", sp[i].vid, sp[i].pid);
    else if(*sn && sa_unique_by_ids(sp[i].vid, sp[i].pid, sn, sp, n, i))
      Plist[Pi++] = str_sprintf("usb:%04x:%04x:%s", sp[i].vid, sp[i].pid, sn);
    else if(via && Pi == 0)
      Plist[Pi++] = str_sprintf("(via %s serial adapter)", via);
  }

  Plist[Pi] = NULL;
  return Plist;
}

// Print possible ways SERPORT sp[i] might be specified
static void sa_print_specs(const SERPORT *sp, int n, int i) {
  char **Pspecs = sa_list_specs(sp, n, i);

  msg_warning("  -P %s", sp[i].port);
  for(char **Ps = Pspecs; *Ps; Ps++) {
    msg_warning("%s %s", str_starts(*Ps, "(via ")? "": Ps[1]? ", -P": " or -P", *Ps);
    free(*Ps);
  }
  msg_warning("\n");

  free(Pspecs);
}

// Set the port specs to the port iff sea matches one and only one of the connected SERPORTs
int setport_from_serialadapter(char **portp, const SERIALADAPTER *sea, const char *sernum) {
  int rv, m, n;
  SERPORT *sp = get_libserialport_data(&n);
  if(!sp || n <= 0)
    return -1;

  m = sa_num_matches_by_sea(sea, sernum, sp, n);
  if(m == 1) {                  // Unique result, set port string
    rv = -1;
    for(int i = 0; i < n; i++)
      if(sa_num_matches_by_sea(sea, sernum, sp+i, 1))
        rv = sa_setport(portp, sp[i].port);
  } else {
    rv = -2;
    pmsg_warning("-P %s is %s; consider\n", *portp, m? "ambiguous": "not connected");
    for(int i = 0; i < n; i++)
      if(m == 0 || sa_num_matches_by_sea(sea, sernum, sp+i, 1) == 1)
        sa_print_specs(sp, n, i);
  }

  for(int k = 0; k < n; k++)
    free(sp[k].sernum), free(sp[k].port);
  free(sp);

  return rv;
}

// Set the port specs to the port iff the ids match one and only one of the connected SERPORTs
int setport_from_vid_pid(char **portp, int vid, int pid, const char *sernum) {
  int rv, m, n;
  SERPORT *sp = get_libserialport_data(&n);
  if(!sp || n <= 0)
    return -1;

  m = sa_num_matches_by_ids(vid, pid, sernum, sp, n);
  if(m == 1) {                  // Unique result, set port string
    rv = -1;
    for(int i = 0; i < n; i++)
      if(sa_num_matches_by_ids(vid, pid, sernum, sp+i, 1))
        rv = sa_setport(portp, sp[i].port);
  } else {
    rv = -2;
    pmsg_warning("-P %s is %s; consider\n", *portp, m? "ambiguous": "not connected");
    for(int i = 0; i < n; i++)
      if(m == 0 || sa_num_matches_by_ids(vid, pid, sernum, sp+i, 1) == 1)
        sa_print_specs(sp, n, i);
  }

  for(int k = 0; k < n; k++)
    free(sp[k].sernum), free(sp[k].port);
  free(sp);

  return rv;
}

// List available serial ports
int list_available_serialports(LISTID programmers) {
  // Get serial port information from libserialport
  int n;
  SERPORT *sp = get_libserialport_data(&n);
  if(!sp || n <= 0)
    return -1;

  msg_warning("%sossible candidate serial port%s:\n",
    n>1? "P": "A p", n>1? "s are": " is");

  for(int i = 0; i < n; i++)
    sa_print_specs(sp, n, i);

  msg_warning("Note that above port%s might not be connected to a target board or an AVR programmer.\n",
    str_plural(n));
  msg_warning("Also note there may be other direct serial ports not listed above.\n");

  for(int k = 0; k < n; k++)
    free(sp[k].sernum), free(sp[k].port);
  free(sp);

  return 0;
}

#else

int setport_from_serialadapter(char **portp, const SERIALADAPTER *ser, const char *sernum) {
  pmsg_error("avrdude built without libserialport support; please compile again with libserialport installed\n");
  return -1;
}

int setport_from_vid_pid(char **portp, int vid, int pid, const char *sernum) {
  pmsg_error("avrdude built without libserialport support; please compile again with libserialport installed\n");
  return -1;
}

int list_available_serialports(LISTID programmers) {
  pmsg_error("avrdude built without libserialport support; please compile again with libserialport installed\n");
  return -1;
}

#endif

void list_serialadapters(FILE *fp, const char *prefix, LISTID programmers) {
  LNODEID ln1, ln2, ln3;
  SERIALADAPTER *sea;
  int maxlen=0, len;

  sort_programmers(programmers);

  // Compute max length of serial adapter names
  for(ln1 = lfirst(programmers); ln1; ln1 = lnext(ln1)) {
    sea = ldata(ln1);
    if(!is_serialadapter(sea))
      continue;
    for(ln2=lfirst(sea->id); ln2; ln2=lnext(ln2)) {
      const char *id = ldata(ln2);
      if(*id == 0 || *id == '.')
        continue;
      if((len = strlen(id)) > maxlen)
        maxlen = len;
    }
  }

  for(ln1 = lfirst(programmers); ln1; ln1 = lnext(ln1)) {
    sea = ldata(ln1);
    if(!is_serialadapter(sea))
      continue;
    for(ln2=lfirst(sea->id); ln2; ln2=lnext(ln2)) {
      const char *id = ldata(ln2);
      if(*id == 0 || *id == '.')
        continue;
      fprintf(fp, "%s%-*s = [usbvid 0x%04x, usbpid", prefix, maxlen, id, sea->usbvid);
      for(ln3=lfirst(sea->usbpid); ln3; ln3=lnext(ln3))
        fprintf(fp, " 0x%04x", *(int *) ldata(ln3));
      if(sea->usbsn && *sea->usbsn)
        fprintf(fp, ", usbsn %s", sea->usbsn);
      fprintf(fp, "]\n");
    }
  }
}

void serialadapter_not_found(const char *sea_id) {
  msg_error("\v");
  if(sea_id && *sea_id)
    pmsg_error("cannot find serial adapter id %s\n", sea_id);

  msg_error("\nValid serial adapters are:\n");
  list_serialadapters(stderr, "  ", programmers);
  msg_error("\n");
}
