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
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "main.h"
#include "bittorrent.h"
#include "settings.h"
#include "htsmsg/htsmsg_store.h"

static int allow_update = 0;


static void
set_torrent_cache_path(void *opaque, const char *str)
{
  rstr_release(btg.btg_cache_path);
  btg.btg_cache_path = rstr_alloc(str);
  if(allow_update)
    torrent_diskio_scan(0);
}




static void
set_torrent_max_connections(void *opaque, int v)
{
  btg.btg_max_connections = v;
  btg.btg_max_peers_global = v;
  btg.btg_max_peers_torrent = v * 4 / 5;
}

static void
set_torrent_max_connections_peer(void *opaque, int v)
{
  btg.btg_peer_requests = v;
}

static void
set_torrent_max_mb_active(void *opaque, int v)
{
  btg.btg_max_mb_active = v*1024*1024;
}

static void
set_torrent_max_mb_cache_per_torrent(void *opaque, int v)
{
  btg.btg_max_mb_cache_per_torrent = v*1024*1024;
}

static void
set_torrent_free_percentage(void *opaque, int v)
{
  btg.btg_free_space_percentage = v;
  if(allow_update)
    torrent_diskio_scan(0);
}

static void
set_torrent_upload_speed(void *opaque, int v)
{
  // How many bytes we should refill send limiter for every tenth second
  btg.btg_max_send_speed = v * (1000000 / 8 / 10);
}


void
torrent_settings_init(void)
{
  prop_t *dir = setting_get_dir("settings"); // general:filebrowse
  prop_t *s = settings_add_dir(dir, _p("BitTorrent"),
                               "bittorrent", NULL, NULL, "settings:bittorrent", "10");

  char defpath[1024];
  int freespace = 66;

#ifdef STOS
  freespace = 75;
#endif

  snprintf(defpath, sizeof(defpath), "%s/bittorrentcache", gconf.cache_path);

/*
  setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Enable bittorrent")),
                 SETTING_MUTEX(&bittorrent_mutex),
                 SETTING_WRITE_BOOL(&btg.btg_enabled),
                 SETTING_VALUE(1),
                 SETTING_STORE("bittorrent", "enable"),
                 NULL);
*/

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Max upload speed")),
                 SETTING_MUTEX(&bittorrent_mutex),
                 SETTING_CALLBACK(set_torrent_upload_speed, NULL),
                 SETTING_VALUE(20),
				 SETTING_STEP(5),
                 SETTING_RANGE(5, 500),
                 SETTING_UNIT_CSTR("Mbit/s"),
                 SETTING_STORE("bittorrent", "uploadspeed_104"),
                 NULL);

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Max usage of free space for caching torrents")),
                 SETTING_MUTEX(&bittorrent_mutex),
                 SETTING_CALLBACK(set_torrent_free_percentage, NULL),
                 SETTING_VALUE(freespace),
                 SETTING_RANGE(0, 95),
                 SETTING_UNIT_CSTR("%"),
                 SETTING_STORE("bittorrent", "freepercentage_104"),
                 NULL);

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Max number of connections")),
                 SETTING_MUTEX(&bittorrent_mutex),
                 SETTING_CALLBACK(set_torrent_max_connections, NULL),
                 SETTING_VALUE(1000),
				 SETTING_STEP(100),
                 SETTING_RANGE(100, 2000),
                 SETTING_UNIT_CSTR(" peers"),
                 SETTING_STORE("bittorrent", "maxconnections_102"),
                 NULL);

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Max concurrent requests per peer")),
                 SETTING_MUTEX(&bittorrent_mutex),
                 SETTING_CALLBACK(set_torrent_max_connections_peer, NULL),
                 SETTING_VALUE(15),
				 SETTING_STEP(1),
                 SETTING_RANGE(5, 50),
                 //SETTING_UNIT_CSTR(" peers"),
                 SETTING_STORE("bittorrent", "maxconnectionspeer_110"),
                 NULL);

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Max memory for active transfers")),
                 SETTING_MUTEX(&bittorrent_mutex),
                 SETTING_CALLBACK(set_torrent_max_mb_active, NULL),
                 SETTING_VALUE(112),
				 SETTING_STEP(4),
                 SETTING_RANGE(96, 384),
                 SETTING_UNIT_CSTR(" MB"),
                 SETTING_STORE("bittorrent", "maxactivemb_115"),
                 NULL);

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Max cache size per torrent")),
                 SETTING_MUTEX(&bittorrent_mutex),
                 SETTING_CALLBACK(set_torrent_max_mb_cache_per_torrent, NULL),
                 SETTING_VALUE(512),
				 SETTING_STEP(16),
                 SETTING_RANGE(160, 4000),
                 SETTING_UNIT_CSTR(" MB"),
                 SETTING_STORE("bittorrent", "maxcachepertorrent_107"),
                 NULL);


  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Inject additional trackers")),
                 SETTING_MUTEX(&bittorrent_mutex),
                 SETTING_WRITE_INT(&btg.btg_add_trackers),
                 SETTING_VALUE("1"),
                 SETTING_OPTION("0",  _p("Off")),
				 SETTING_OPTION("1",  _p("Best")),
				 SETTING_OPTION("2",  _p("Best (IP)")),
				 SETTING_OPTION("3",  _p("All")),
				 SETTING_OPTION("4",  _p("All (IP)")),
                 SETTING_STORE("bittorrent", "injecttrackers_120"),
                 NULL);

  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Tracker protocols")),
				 SETTING_MUTEX(&bittorrent_mutex),
				 SETTING_VALUE("0"),
                 SETTING_STORE("bittorrent", "tcpudp"),
                 SETTING_WRITE_INT(&btg.btg_tcpudp),
                 SETTING_OPTION_CSTR("0",  "TCP & UDP"),
                 SETTING_OPTION_CSTR("1",  "TCP"),
                 SETTING_OPTION_CSTR("2",  "UDP"),
                 NULL);

  settings_create_separator(s, _p("Status"));

  btg.btg_torrent_status = prop_create_root(NULL);
  settings_create_info(s, NULL, btg.btg_torrent_status);

  btg.btg_disk_status = prop_create_root(NULL);
  settings_create_info(s, NULL, btg.btg_disk_status);

  setting_create(SETTING_STRING, s, SETTINGS_INITIAL_UPDATE | SETTINGS_DIR,
                 SETTING_TITLE(_p("Torrent cache path")),
                 SETTING_MUTEX(&bittorrent_mutex),
                 SETTING_CALLBACK(set_torrent_cache_path, NULL),
                 SETTING_VALUE(defpath),
                 SETTING_STORE("bittorrent", "path"),
                 NULL);

  setting_create(SETTING_ACTION, s, 0,
                 SETTING_TITLE(_p("Clear cache")),
                 SETTING_MUTEX(&bittorrent_mutex),
                 SETTING_CALLBACK(torrent_diskio_cache_clear, NULL),
                 NULL);



  allow_update = 1;
  torrent_diskio_scan(0);
}
