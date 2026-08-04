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
#include "media.h"
#include "video/video_settings.h"
#include "misc/minmax.h"
#include "misc/cancellable.h"


#if 1


static void
update_epoch_in_queue(struct media_buf_queue *q, int epoch)
{
  media_buf_t *mb;
  TAILQ_FOREACH(mb, q, mb_link)
    mb->mb_epoch = epoch;
}


static int
mp_seek_in_queues(media_pipe_t *mp, int64_t user_time)
{
  media_buf_t *abuf, *vbuf, *mb; //*vk,
  int rval = 1;

#if 0
  if(mp->mp_hls_source == 2)
  {
	  TRACE(TRACE_ERROR, "Media", "mp_hls_source=%i", mp->mp_hls_source);
	  //mp->mp_audio_need_resync = 2;
	  return rval;
  }
#endif

#if 0
  int64_t vts = -1;
#endif

  TAILQ_FOREACH(abuf, &mp->mp_audio.mq_q_data, mb_link)
  {
    if(abuf->mb_user_time != PTS_UNSET && abuf->mb_user_time >= user_time - 000000)
	{
		//TRACE(TRACE_ERROR, "Media", "A-USER: %li | USER: %li | DELTA: %li", abuf->mb_user_time/1000000, user_time/1000000, (abuf->mb_user_time - user_time)/1000000);
		break;
	}

#if 0
	//if(gconf.enable_android_seek_in_queues)
	{
		if(abuf->mb_pts != PTS_UNSET && abuf->mb_pts >= user_time - 000000)
		{
			//TRACE(TRACE_ERROR, "Media", "A-PTS: %lli | USER: %lli | DELTA: %lli", abuf->mb_pts/1000000, user_time/1000000, (abuf->mb_pts - user_time)/1000000);
			if((abuf->mb_pts - user_time) < 3000000)
			{
				//TRACE(TRACE_INFO, "Media", "A-PTS: %li | USER: %li | DELTA: %li", abuf->mb_pts/1000000, user_time/1000000, (abuf->mb_pts - user_time)/1000000);
				break;
			}
			//break;
		}
	}
#endif
  }

int found = 0;
  if(abuf != NULL)
	  {
//TRACE(TRACE_ERROR, "Media", "Seek in queues a-user-time: %lli", abuf->mb_pts);
    //vk = NULL;

    TAILQ_FOREACH(vbuf, &mp->mp_video.mq_q_data, mb_link) {
		//if(vbuf->mb_keyframe)
			//vk = vbuf;

		//if(vbuf->mb_keyframe && vbuf->mb_pts != PTS_UNSET && (vbuf->mb_user_time >= user_time))// || vbuf->mb_pts >= user_time))
		if(vbuf->mb_keyframe && vbuf->mb_pts != PTS_UNSET && (vbuf->mb_user_time >= abuf->mb_user_time-50000))// || vbuf->mb_pts >= user_time))
		{
			found = 1;
			//if(vk != NULL) vts = vk->mb_user_time;
			//TRACE(TRACE_ERROR, "Media", "V-USER: %li | USER: %li | DELTA: %li", vbuf->mb_user_time/1000000, user_time/1000000, (vbuf->mb_user_time - user_time)/1000000);
			break;
		}


#if 0
		//if(gconf.enable_android_seek_in_queues)
		{
			if(vbuf->mb_keyframe && vbuf->mb_pts != PTS_UNSET && (vbuf->mb_pts >= user_time))
			{
				//TRACE(TRACE_ERROR, "Media", "V-PTS: %lli | USER: %lli | DELTA: %lli", vbuf->mb_pts/1000000, user_time/1000000, (vbuf->mb_pts - user_time)/1000000);
				if((vbuf->mb_pts - user_time) < 3000000)
				{
					vts = vbuf->mb_pts;
					//TRACE(TRACE_ERROR, "Media", "V-PTS: %li | USER: %li | DELTA: %li", vbuf->mb_pts/1000000, user_time/1000000, (vbuf->mb_pts - user_time)/1000000);
					break;
				}
				//break;
			}
		}
#endif

	}

    if(found && vbuf != NULL /*&& vk != NULL*/) {
//TRACE(TRACE_ERROR, "Media", "Seek in queues v-user-time: %lli", vbuf->mb_pts);
      int adrop = 0, vdrop = 0; //, vskip = 0;
      while(1) {
	mb = TAILQ_FIRST(&mp->mp_audio.mq_q_data);
	if(mb == abuf)
	  break;
	TAILQ_REMOVE(&mp->mp_audio.mq_q_data, mb, mb_link);
	mp->mp_audio.mq_packets_current--;
	mp->mp_buffer_current -= mb_buffered_size(mb);
	media_buf_free_locked(mp, mb);
	adrop++;
      }
      mq_update_stats(mp, &mp->mp_audio, 1);

      while(1) {
	mb = TAILQ_FIRST(&mp->mp_video.mq_q_data);
	if(mb == vbuf)
	  break;
	TAILQ_REMOVE(&mp->mp_video.mq_q_data, mb, mb_link);
	mp->mp_video.mq_packets_current--;
	mp->mp_buffer_current -= mb_buffered_size(mb);
	media_buf_free_locked(mp, mb);
	vdrop++;
      }
      mq_update_stats(mp, &mp->mp_video, 1);



	while(mb != vbuf) {
	mb->mb_skip = 1;
	mb = TAILQ_NEXT(mb, mb_link);
	//vskip++;
      }

      rval = 0;
#if 0
	  if(vts>=0)
		prop_set(mp->mp_prop_root, "seektime", PROP_SET_FLOAT, vts / 1000000.0);
	  else
#endif
		prop_set(mp->mp_prop_root, "seektime", PROP_SET_FLOAT, user_time / 1000000.0);

	  update_epoch_in_queue(&mp->mp_audio.mq_q_data, mp->mp_epoch);
      update_epoch_in_queue(&mp->mp_video.mq_q_data, mp->mp_epoch);
      update_epoch_in_queue(&mp->mp_video.mq_q_aux, mp->mp_epoch);

      mb = media_buf_alloc_locked(mp, 0);
      mb->mb_data_type = MB_CTRL_FLUSH;
      mb->mb_data32 = 0;
      mb_enq(mp, &mp->mp_audio, mb);


      mb = media_buf_alloc_locked(mp, 0);
      mb->mb_data_type = MB_CTRL_FLUSH;
      mb->mb_data32 = 0;
      mb_enq(mp, &mp->mp_video, mb);

      mp_check_underrun(mp);

      //TRACE(TRACE_DEBUG, "Media", "Seek by dropping %d audio packets and %d+%d video packets from queue", adrop, vdrop, vskip);
    }
  }



  return rval;
}
#endif



//extern volatile int f_torrent_cancel_piece;
/**
 *
 */
static void
mp_direct_seek(media_pipe_t *mp, int64_t ts, int in_queues)
{
  event_t *e;
  event_ts_t *ets;

  if(!(mp->mp_flags & MP_CAN_SEEK))
    return;

  ts = MAX(ts, 0);

  prop_set_float_ex(mp->mp_prop_currenttime, mp->mp_sub_currenttime,
		    ts / 1000000.0);

/*
  if(mp->mp_seek_initiate != NULL) // OMX/RPi only
    mp->mp_seek_initiate(mp);
*/
  //mp->mp_seek_base = ts;
  //mp->mp_av_in_sync = 0;
  //mp->mp_seeking = 1;
  mp->mp_epoch++;

#if 1
  if(in_queues)
  {
	//TRACE(TRACE_DEBUG, "SEEK", "Seek in queues try...");
	if(!mp_seek_in_queues(mp, ts/*+1500000*/))// && !mp_seek_in_queues_pts(mp, ts))
	{
		//TRACE(TRACE_DEBUG, "SEEK", "Seek in queues finished");
		TAILQ_FOREACH(e, &mp->mp_eq, e_link)
		{
			if(!event_is_type(e, EVENT_SEEK))
				continue;

			if(e != NULL)
			{
				//TRACE(TRACE_DEBUG, "SEEK", "Seek deduped");
				TAILQ_REMOVE(&mp->mp_eq, e, e_link);
				event_release(e);
			}
		}

		//prop_set(mp->mp_prop_root, "seektime", PROP_SET_FLOAT, ts / 1000000.0);
		mp->mp_seek_base = ts;
		//mp->mp_seeking = 0;
		return;
	}
	//TRACE(TRACE_DEBUG, "SEEK", "Seek in queues failed...");
  }
#endif

//f_torrent_cancel_piece = 1;

  /* If there already is a seek event enqueued, update it */
  TAILQ_FOREACH(e, &mp->mp_eq, e_link) {
    if(!event_is_type(e, EVENT_SEEK))
      continue;

/*
    ets = (event_ts_t *)e;
    ets->ts = ts;
	mp->mp_seek_base = ts;
    return;
*/
	//TRACE(TRACE_DEBUG, "SEEK", "Seek deduped");
	TAILQ_REMOVE(&mp->mp_eq, e, e_link);
	event_release(e);
  }

  prop_set(mp->mp_prop_root, "seektime", PROP_SET_FLOAT, ts / 1000000.0);

  ets = event_create(EVENT_SEEK, sizeof(event_ts_t));
  ets->ts = ts;

  e = &ets->h;
  mp->mp_seek_base = ts;
  mp_event_dispatch(mp, e);
}


/**
 *
 */
void
mp_seek_by_propchange(void *opaque, prop_event_t event, ...)
{
  media_pipe_t *mp = opaque;
  int64_t t;
  va_list ap;
  va_start(ap, event);

  switch(event) {
  case PROP_SET_INT:
    t = va_arg(ap, int) * 1000000LL;
    break;
  case PROP_SET_FLOAT:
    t = va_arg(ap, double) * 1000000.0;
    break;
  default:
    return;
  }

  //if(!mp->mp_audio_created)
	//  mp_direct_seek(mp, t, 0);
  //else
  {
	  if(t > mp->mp_seek_base)
		mp_direct_seek(mp, t/*-2000000*/, 1);
	  else
		mp_direct_seek(mp, t/*-5000000*/, 0);
  }
}

/**
 *
 */
void
mp_event_dispatch(media_pipe_t *mp, event_t *e)
{
  if(mp->mp_handle_event == NULL ||
     !mp->mp_handle_event(mp, mp->mp_handle_event_opaque, e)) {
    TAILQ_INSERT_TAIL(&mp->mp_eq, e, e_link);
    hts_cond_signal(&mp->mp_backpressure);
  } else {
    event_release(e);
  }
}

/**
 *
 */
void
mp_enqueue_event_locked(media_pipe_t *mp, event_t *e)
{
  event_select_track_t *est = (event_select_track_t *)e;
  event_int3_t *ei3;
  int64_t d;
  int dedup_event = 0;

  switch(e->e_type) {
  case EVENT_SELECT_AUDIO_TRACK:
    if(mp_track_mgr_select_track(&mp->mp_audio_track_mgr, est))
	{
	  //mp_flush_locked(mp, 0);
	  //TRACE(TRACE_ERROR, "EVENT", "Changing audio track");
	  if(!mp->mp_hls_source && gconf.f_in_video_playback) { mp_direct_seek(mp, mp->mp_seek_base - 500000, 0);}
      return;
	}
    dedup_event = 1;
    break;

  case EVENT_SELECT_SUBTITLE_TRACK:
    if(mp_track_mgr_select_track(&mp->mp_subtitle_track_mgr, est))
      return;
    dedup_event = 1;
    break;

  case EVENT_DELTA_SEEK_REL:
    // We want to seek thru the entire feature in 3 seconds

#define TOTAL_SEEK_TIME_IN_SECONDS 2

    ei3 = (event_int3_t *)e;

    int pre  = ei3->val1;
    int sign = ei3->val2;
    int rate = ei3->val3;

    d = pre * pre * mp->mp_duration /
      (rate*TOTAL_SEEK_TIME_IN_SECONDS*255*255);

    mp_direct_seek(mp, mp->mp_seek_base += d*sign, 0);
    return;

  case EVENT_PLAYQUEUE_JUMP:
    dedup_event = 1;
    break;

  default:
    break;
  }

  if(dedup_event) {
    event_t *e2;
    TAILQ_FOREACH(e2, &mp->mp_eq, e_link)
      if(e2->e_type == e->e_type)
        break;

    if(e2 != NULL) {
      TAILQ_REMOVE(&mp->mp_eq, e2, e_link);
      event_release(e2);
    }
  }

  if(event_is_action(e, ACTION_PLAYPAUSE ) ||
     event_is_action(e, ACTION_PLAY ) ||
     event_is_action(e, ACTION_PAUSE)) {

    if(action_update_hold_by_event(mp->mp_hold_flags & MP_HOLD_PAUSE, e)) {
      mp->mp_hold_flags |= MP_HOLD_PAUSE;
	  //mp->mp_epoch++;
      //mp_direct_seek(mp, mp->mp_seek_base + 20000);
	  //e = event_create_action(ACTION_SEEK_FORWARD_FRAME);
	  //event_dispatch(e);

    } else {
      mp->mp_hold_flags &= ~MP_HOLD_PAUSE;
    }

    mp_set_playstatus_by_hold_locked(mp, NULL);

  } else if(event_is_type(e, EVENT_INTERNAL_PAUSE)) {

    const event_payload_t *ep = (const event_payload_t *)e;

    mp->mp_hold_flags |= MP_HOLD_PAUSE;
    mp_set_playstatus_by_hold_locked(mp, ep->payload);

  } else if(event_is_action(e, ACTION_SEEK_BACKWARD_FRAME)) {
    mp_direct_seek(mp, mp->mp_seek_base - 500000, 0);

  } else if(event_is_action(e, ACTION_SEEK_FORWARD_FRAME)) {
    mp_direct_seek(mp, mp->mp_seek_base + 500000, 1);

  } else if(event_is_action(e, ACTION_SEEK_BACKWARD)) {
	//mp->mp_allow_prebuffer = 0;
	mp_direct_seek(mp, mp->mp_seek_base /*- (mp->mp_audio_created?5000000:0)*/ - 1000000 *
                   video_settings.seek_back_step, 0);

  } else if(event_is_action(e, ACTION_SEEK_FORWARD)) {
	//mp->mp_allow_prebuffer = 2;
    mp_direct_seek(mp, mp->mp_seek_base /*- (mp->mp_audio_created?1500000:0)*/ + 1000000 *
                   video_settings.seek_fwd_step, 1);

  } else if(event_is_action(e, ACTION_SHUFFLE)) {
    prop_toggle_int(mp->mp_prop_shuffle);
  } else if(event_is_action(e, ACTION_REPEAT)) {
    prop_toggle_int(mp->mp_prop_repeat);
  } else if(event_is_action(e, ACTION_CYCLE_AUDIO)) {
    mp_track_mgr_next_track(&mp->mp_audio_track_mgr);
	if(!mp->mp_hls_source) { mp_direct_seek(mp, mp->mp_seek_base - 500000, 0); }
  } else if(event_is_action(e, ACTION_CYCLE_SUBTITLE)) {
    mp_track_mgr_next_track(&mp->mp_subtitle_track_mgr);
  } else if(event_is_action(e, ACTION_VOLUME_UP) ||
            event_is_action(e, ACTION_VOLUME_DOWN)) {

    switch(video_settings.dpad_up_down_mode) {
    case VIDEO_DPAD_MASTER_VOLUME:
      atomic_inc(&e->e_refcount);
      event_dispatch(e);
      break;
    case VIDEO_DPAD_PER_FILE_VOLUME:
      if(mp->mp_vol_setting == NULL)
        break;
      settings_add_int(mp->mp_vol_setting,
                       event_is_action(e, ACTION_VOLUME_UP) ? 1 : -1);
      break;
    case VIDEO_DPAD_CH_UPDN:
	  //f_torrent_cancel_piece = 1;
      if(event_is_action(e, ACTION_VOLUME_DOWN))
        e = event_create_action(ACTION_SKIP_BACKWARD);
      else
        e = event_create_action(ACTION_SKIP_FORWARD);
      event_dispatch(e);
      break;
    }

  } else {

    // Forward event to player

    if(event_is_action(e, ACTION_SKIP_BACKWARD) &&
       mp->mp_seek_base >= MP_SKIP_LIMIT && !mp->mp_hls_source &&
       mp->mp_flags & MP_CAN_SEEK) {

      // Convert skip previous to track restart
	  //f_torrent_cancel_piece = 1;
      mp_direct_seek(mp, 0, 0);
      return;
    }

    if(event_is_action(e, ACTION_STOP) ||
       event_is_action(e, ACTION_EJECT)) {
      prop_set_string(mp->mp_prop_playstatus, "stop");
    }

    if(event_is_type(e, EVENT_PLAYQUEUE_JUMP) ||
       event_is_type(e, EVENT_EXIT) ||
       event_is_action(e, ACTION_STOP) ||
       event_is_action(e, ACTION_SKIP_FORWARD) ||
       event_is_action(e, ACTION_SKIP_BACKWARD)) {

	  //f_torrent_cancel_piece = 1;
      cancellable_cancel(mp->mp_cancellable);
    }

    atomic_inc(&e->e_refcount);
    mp_event_dispatch(mp, e);
  }
}

/**
 *
 */
void
mp_enqueue_event(media_pipe_t *mp, event_t *e)
{
  hts_mutex_lock(&mp->mp_mutex);
  mp_enqueue_event_locked(mp, e);
  hts_mutex_unlock(&mp->mp_mutex);
}


/**
 *
 */
void
media_eventsink(void *opaque, event_t *e)
{
  mp_enqueue_event_locked(opaque, e);
}


/**
 *
 */
void
mp_event_set_callback(struct media_pipe *mp,
                      int (*mp_callback)(struct media_pipe *mp,
                                         void *opaque,
                                         event_t *e),
                      void *opaque)
{
  hts_mutex_lock(&mp->mp_mutex);
  mp->mp_handle_event = mp_callback;
  mp->mp_handle_event_opaque = opaque;
  hts_mutex_unlock(&mp->mp_mutex);
}

