/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */

/**
 * WS-Discovery (WSD) client.
 *
 * Modern Windows (10/11) dropped SMB1/NetBIOS browsing and does not
 * advertise SMB over mDNS, so neither nmb.c's NetBIOS master-browser
 * discovery nor avahi.c's "_smb._tcp" browser finds it. WS-Discovery
 * (SOAP over UDP multicast, port 3702) is what Windows uses instead --
 * File Explorer's "Network" view is a WSD client.
 *
 * Protocol shape (all verified against a live Windows 11 host,
 * DESKTOP-IF89D8H @ 192.168.1.144, on 2026-07-05 -- see Buksa/movian#67):
 *
 *  1. We multicast a Probe (SOAP 1.2 envelope, action .../Probe) to
 *     239.255.255.250:3702, scoped to Types "pub:Computer".
 *  2. Matching hosts reply with a *unicast* ProbeMatch (action
 *     .../ProbeMatches) sent back to the source port of our Probe --
 *     no multicast group membership is needed to receive it, we just
 *     need to keep the sending socket around. The reply's UDP source
 *     address is the reachable address (same trick as the NBT fix,
 *     #66): trust that over anything embedded in the XML.
 *  3. The ProbeMatch itself carries no computer name -- only a stable
 *     per-device EndpointReference (urn:uuid:...) and an XAddrs URL
 *     for the device's WS-Transfer metadata endpoint. To get the
 *     machine name we issue a follow-up unicast WS-Transfer Get (plain
 *     HTTP POST, SOAP 1.2) to that XAddr; the response's
 *     ".../Relationship" metadata section carries
 *     <pub:Computer>HOSTNAME/Workgroup:WORKGROUP</pub:Computer>, which
 *     is what File Explorer's own name resolution is built on. If that
 *     follow-up fails for any reason we fall back to the bare address.
 */

#include <string.h>
#include <stdio.h>

#include "config.h"
#include "main.h"
#include "arch/arch.h"
#include "networking/asyncio.h"
#include "task.h"
#include "misc/str.h"
#include "htsmsg/htsmsg_xml.h"
#include "fileaccess/http_client.h"
#include "service.h"

#define WSD_TRACE(x, ...) do {                                 \
    if(gconf.enable_smb_debug)                                 \
      tracelog(0, TRACE_DEBUG, "WSD", x, ##__VA_ARGS__); \
  } while(0)

#define WSD_PROBE_PERIOD 20 /* seconds between probes */

static const net_addr_t wsd_mcast_addr = {4, 3702, {239, 255, 255, 250}};

static hts_mutex_t wsd_mutex;
static asyncio_fd_t *wsd_udp_fd;
static struct asyncio_timer wsd_probe_timer;

LIST_HEAD(wsd_host_list, wsd_host);

typedef struct wsd_host {
  LIST_ENTRY(wsd_host) wh_link;
  char *wh_epr;          /* wsa:EndpointReference/wsa:Address, our stable key */
  char *wh_addr;         /* dotted-quad reachable address (UDP reply source) */
  char *wh_xaddr;        /* WS-Transfer metadata URL, valid while wh_pending */
  int wh_mark;           /* sweep counter, like nmb_server_t::ns_mark */
  int wh_pending;        /* a resolve task is in flight for this host */
  service_t *wh_service; /* NULL until the resolve task completes */
} wsd_host_t;

static struct wsd_host_list wsd_hosts;

/**
 *
 */
static wsd_host_t *
wsd_host_find(const char *epr)
{
  wsd_host_t *wh;
  LIST_FOREACH(wh, &wsd_hosts, wh_link)
    if(!strcmp(wh->wh_epr, epr))
      return wh;
  return NULL;
}


/**
 *
 */
static void
wsd_host_destroy(wsd_host_t *wh)
{
  LIST_REMOVE(wh, wh_link);
  if(wh->wh_service != NULL)
    service_destroy(wh->wh_service);
  free(wh->wh_epr);
  free(wh->wh_addr);
  free(wh->wh_xaddr);
  free(wh);
}


/**
 * Split the WS-Transfer "pub:Computer" value ("HOSTNAME/Workgroup:WG"
 * or "HOSTNAME/Domain:DOM") into hostname + group, so we can build the
 * exact same "group/host" service id that nmb.c's NetBIOS discovery
 * uses -- letting the two sources address the same identity even
 * though the framework itself does not dedup services by id.
 */
static void
wsd_split_computer(const char *computer, char *host, size_t hostsize,
                    char *group, size_t groupsize)
{
  const char *slash = strchr(computer, '/');
  size_t hlen = slash ? (size_t)(slash - computer) : strlen(computer);
  if(hlen >= hostsize)
    hlen = hostsize - 1;
  memcpy(host, computer, hlen);
  host[hlen] = 0;

  group[0] = 0;
  if(slash != NULL) {
    const char *colon = strchr(slash, ':');
    if(colon != NULL) {
      snprintf(group, groupsize, "%s", colon + 1);
    }
  }
}


/**
 * Fill 'out' (>= 37 bytes) with a fresh random "xxxxxxxx-xxxx-...-xxxx"
 * MessageID, suitable for "urn:uuid:%s".
 */
static void
wsd_gen_uuid(char *out, size_t outsize)
{
  uint8_t rnd[16];
  arch_get_random_bytes(rnd, sizeof(rnd));
  snprintf(out, outsize,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7],
           rnd[8], rnd[9], rnd[10], rnd[11], rnd[12], rnd[13], rnd[14], rnd[15]);
}


/**
 * Blocking WS-Transfer Get for the ThisDevice/Relationship metadata,
 * to pull the Windows computer name out of a device that only
 * announced itself via an opaque EndpointReference. Runs on a task
 * worker thread -- never call from the asyncio thread.
 *
 * Returns a malloced "HOSTNAME" (and, if known, *group malloced too)
 * on success, or NULL if the Get failed / didn't carry a pub:Computer.
 */
static char *
wsd_resolve_name(const char *xaddr, const char *epr, char **groupp)
{
  htsbuf_queue_t post;
  buf_t *result;
  char errbuf[256];
  char *ret = NULL;
  char msgid[40];

  *groupp = NULL;

  wsd_gen_uuid(msgid, sizeof(msgid));

  htsbuf_queue_init(&post, 0);
  htsbuf_qprintf(&post,
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\">"
    "<soap:Header>"
    "<wsa:To>%s</wsa:To>"
    "<wsa:Action>http://schemas.xmlsoap.org/ws/2004/09/transfer/Get</wsa:Action>"
    "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
    "<wsa:ReplyTo>"
    "<wsa:Address>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</wsa:Address>"
    "</wsa:ReplyTo>"
    "</soap:Header>"
    "<soap:Body/>"
    "</soap:Envelope>", epr, msgid);

  int r = http_req(xaddr,
                    HTTP_RESULT_PTR(&result),
                    HTTP_ERRBUF(errbuf, sizeof(errbuf)),
                    HTTP_POSTDATA(&post, "application/soap+xml; charset=utf-8"),
                    HTTP_CONNECT_TIMEOUT(3000),
                    HTTP_READ_TIMEOUT(3000),
                    NULL);
  if(r) {
    WSD_TRACE("WS-Transfer Get to %s failed -- %s",
          xaddr, errbuf);
    return NULL;
  }

  htsmsg_t *root = htsmsg_xml_deserialize_buf(result, errbuf, sizeof(errbuf));
  if(root == NULL) {
    WSD_TRACE("Unable to parse GetResponse from %s -- %s",
          xaddr, errbuf);
    return NULL;
  }

  htsmsg_t *body = htsmsg_get_map_multi(root, "Envelope", "Body", NULL);
  htsmsg_t *metadata = body ? htsmsg_get_map(body, "Metadata") : NULL;

  if(metadata != NULL) {
    htsmsg_field_t *f;
    HTSMSG_FOREACH(f, metadata) {
      if(strcmp(f->hmf_name, "MetadataSection"))
        continue;
      htsmsg_t *section = htsmsg_get_map_by_field(f);
      if(section == NULL)
        continue;
      const char *dialect = htsmsg_get_str(section, "Dialect");
      if(dialect == NULL || !strstr(dialect, "/Relationship"))
        continue;

      const char *computer =
        htsmsg_get_str_multi(section, "Relationship", "Host", "Computer",
                             "cdata", NULL);
      if(computer == NULL)
        continue;

      char host[128], group[128];
      wsd_split_computer(computer, host, sizeof(host), group, sizeof(group));
      if(host[0]) {
        ret = strdup(host);
        if(group[0])
          *groupp = strdup(group);
      }
      break;
    }
  }
  htsmsg_release(root);
  return ret;
}


/**
 * Runs on a task worker thread. Resolves the computer name for a
 * freshly-seen EndpointReference and registers (or refreshes) its
 * smb2:// service.
 */
static void
wsd_resolve_and_register(void *aux)
{
  wsd_host_t *self = aux;
  char *epr = strdup(self->wh_epr);
  char *addr = strdup(self->wh_addr);
  char *xaddr = self->wh_xaddr; /* set + owned by caller before task_run */

  char *group = NULL;
  char *host = wsd_resolve_name(xaddr, epr, &group);

  char id[128];
  char url[128];
  const char *title;

  /* The connect address is always the reachable address the ProbeMatch
   * arrived from -- never the resolved name. That name resolution is
   * exactly the thing modern Windows dropped (no NetBIOS, no mDNS
   * advertisement of SMB), so a URL that depended on it working would
   * quietly defeat WSD's whole reason to exist. */
  snprintf(url, sizeof(url), "smb2://%s", addr);

  if(host != NULL && group != NULL) {
    /* Same "workgroup/name" shape nmb.c uses, so the two sources name
     * the same host identically even though nothing currently merges
     * services by id (see dedup note in Buksa/movian#67). */
    snprintf(id, sizeof(id), "%s/%s", group, host);
    title = host;
  } else if(host != NULL) {
    snprintf(id, sizeof(id), "wsd/%s", host);
    title = host;
  } else {
    snprintf(id, sizeof(id), "wsd/%s", addr);
    title = addr;
  }

  hts_mutex_lock(&wsd_mutex);
  wsd_host_t *wh = wsd_host_find(epr);
  if(wh != NULL) {
    if(wh->wh_service == NULL) {
      wh->wh_service = service_create_managed(id, title, url, "server", NULL,
                                              0, 0, SVC_ORIGIN_DISCOVERED);
      WSD_TRACE("Registered %s as %s (id '%s')",
            addr, url, id);
    }
    wh->wh_pending = 0;
    wh->wh_mark = 0;
    free(wh->wh_xaddr);
    wh->wh_xaddr = NULL;
  }
  hts_mutex_unlock(&wsd_mutex);

  free(host);
  free(group);
  free(epr);
  free(addr);
  /* self == wh: still linked into wsd_hosts, do not free it here --
   * wsd_send_probe()'s sweep owns its lifetime from this point on. */
}


/**
 * Parse one <wsd:ProbeMatch> element: pull out the EndpointReference
 * (our stable per-host key) and the first XAddrs URL. remote_addr is
 * the UDP reply's source address -- the reachable address, per the
 * same reasoning as the NBT multi-address fix (#66): don't trust
 * anything embedded in the XML over where the packet actually came
 * from.
 */
static void
wsd_handle_probematch(htsmsg_t *pm, const net_addr_t *remote_addr)
{
  const char *epr =
    htsmsg_get_str_multi(pm, "EndpointReference", "Address", "cdata", NULL);
  const char *xaddrs =
    htsmsg_get_str_multi(pm, "XAddrs", "cdata", NULL);
  const char *types =
    htsmsg_get_str_multi(pm, "Types", "cdata", NULL);

  if(epr == NULL || xaddrs == NULL)
    return;

  if(types != NULL && !strstr(types, "Computer"))
    return;

  char firstxaddr[256];
  snprintf(firstxaddr, sizeof(firstxaddr), "%s", xaddrs);
  char *sp = strchr(firstxaddr, ' ');
  if(sp != NULL)
    *sp = 0;

  /* net_addr_str() appends ":port" when na_port != 0 -- remote_addr's
   * port is wherever the host's WSD stack happened to send the reply
   * from (3702 here), not anything we want baked into an smb2:// URL,
   * so format the bare address ourselves. */
  net_addr_t addr_only = *remote_addr;
  addr_only.na_port = 0;

  hts_mutex_lock(&wsd_mutex);

  wsd_host_t *wh = wsd_host_find(epr);
  if(wh != NULL) {
    if(wh->wh_pending || !strcmp(wh->wh_addr, net_addr_str(&addr_only))) {
      wh->wh_mark = 0;
      hts_mutex_unlock(&wsd_mutex);
      return;
    }
    /* Same endpoint, new source address (DHCP move / other NIC): the
     * registered smb2:// URL still points at the old address, and every
     * ProbeMatch from the new one would keep that stale entry alive.
     * Drop it and rebuild below so the service follows the host. While
     * a resolve is pending we must not destroy (the task holds the
     * pointer); the next probe round re-checks once it clears. */
    WSD_TRACE("%s moved to %s, re-registering",
          wh->wh_addr, net_addr_str(&addr_only));
    wsd_host_destroy(wh);
  }

  wh = calloc(1, sizeof(wsd_host_t));
  wh->wh_epr = strdup(epr);
  wh->wh_addr = strdup(net_addr_str(&addr_only));
  wh->wh_pending = 1;
  LIST_INSERT_HEAD(&wsd_hosts, wh, wh_link);

  hts_mutex_unlock(&wsd_mutex);

  WSD_TRACE("New host %s (%s), resolving name via %s",
        wh->wh_addr, epr, firstxaddr);

  wh->wh_xaddr = strdup(firstxaddr);
  task_run(wsd_resolve_and_register, wh);
}


/**
 *
 */
static void
wsd_udp_input(void *opaque, const void *data, int size,
              const net_addr_t *remote_addr)
{
  char errbuf[256];
  char *xml = malloc(size + 1);
  if(xml == NULL)
    return;
  memcpy(xml, data, size);
  xml[size] = 0;

  htsmsg_t *root = htsmsg_xml_deserialize_cstr(xml, errbuf, sizeof(errbuf));
  free(xml);
  if(root == NULL)
    return;

  htsmsg_t *matches =
    htsmsg_get_map_multi(root, "Envelope", "Body", "ProbeMatches", NULL);

  if(matches != NULL) {
    htsmsg_field_t *f;
    HTSMSG_FOREACH(f, matches) {
      if(strcmp(f->hmf_name, "ProbeMatch"))
        continue;
      htsmsg_t *pm = htsmsg_get_map_by_field(f);
      if(pm != NULL)
        wsd_handle_probematch(pm, remote_addr);
    }
  }
  htsmsg_release(root);
}


/**
 * Sweep hosts that missed two consecutive probe rounds (~2x
 * WSD_PROBE_PERIOD, same shape as nmb.c's ns_mark >= 2 rule), then
 * send a fresh multicast Probe scoped to Windows computers with file
 * sharing (pub:Computer, in the Microsoft "pub" publisher namespace).
 */
static void
wsd_send_probe(void *aux)
{
  asyncio_timer_arm_delta_sec(&wsd_probe_timer, WSD_PROBE_PERIOD);

  hts_mutex_lock(&wsd_mutex);
  wsd_host_t *wh, *next;
  for(wh = LIST_FIRST(&wsd_hosts); wh != NULL; wh = next) {
    next = LIST_NEXT(wh, wh_link);
    if(wh->wh_pending)
      continue;
    if(wh->wh_mark >= 2) {
      WSD_TRACE("%s timed out, removing", wh->wh_addr);
      wsd_host_destroy(wh);
    } else {
      wh->wh_mark++;
    }
  }
  hts_mutex_unlock(&wsd_mutex);

  char msgid[40];
  wsd_gen_uuid(msgid, sizeof(msgid));

  char probe[768];
  snprintf(probe, sizeof(probe),
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:wsa=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
    "xmlns:wsd=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\">"
    "<soap:Header>"
    "<wsa:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</wsa:To>"
    "<wsa:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</wsa:Action>"
    "<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
    "</soap:Header>"
    "<soap:Body>"
    "<wsd:Probe>"
    "<wsd:Types xmlns:pub=\"http://schemas.microsoft.com/windows/pub/2005/07\">"
    "pub:Computer</wsd:Types>"
    "</wsd:Probe>"
    "</soap:Body>"
    "</soap:Envelope>", msgid);

  WSD_TRACE("Sending Probe (MessageID urn:uuid:%s)", msgid);

  asyncio_udp_send(wsd_udp_fd, probe, strlen(probe), &wsd_mcast_addr);
}


/**
 *
 */
static void
wsd_init(void)
{
  hts_mutex_init(&wsd_mutex);

  wsd_udp_fd = asyncio_udp_bind("wsd", NULL, wsd_udp_input, NULL, 1, 0);
  if(wsd_udp_fd == NULL) {
    WSD_TRACE("Unable to bind WS-Discovery UDP socket");
    return;
  }

  asyncio_timer_init(&wsd_probe_timer, wsd_send_probe, NULL);
  wsd_send_probe(NULL);
}

INITME(INIT_GROUP_ASYNCIO, wsd_init, NULL, 0);
