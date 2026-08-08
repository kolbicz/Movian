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
#include "main.h"
#include "htsmsg/htsmsg_store.h"
#include "ui/glw/glw_settings.h"
#include "settings.h"
#include "video_settings.h"
#include "misc/str.h"


struct video_settings video_settings;


void
video_settings_init(void)
{
  prop_t *s;

  s = settings_add_dir(NULL, _p("Video"), "video", NULL,
		       _p("Video acceleration and display behaviour"),
		       "settings:video", "01");

  settings_create_separator(s, _p("Accelerated Decoding"));

#if ENABLE_WSL2

  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("OpenGL Driver")),
                 SETTING_STORE("videoplayback", "mesa_driver_2"),
                 SETTING_WRITE_INT(&video_settings.mesa_driver),
				 SETTING_OPTION     ("0", _p("Default")),
                 SETTING_OPTION_CSTR("1", "D3D12"),
				 SETTING_OPTION     ("2", _p("Off")),
                 NULL);

  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("OpenGL Adapter")),
                 SETTING_STORE("videoplayback", "mesa_adapter"),
                 SETTING_WRITE_INT(&video_settings.mesa_adapter),
				 SETTING_OPTION     ("1", _p("Default")),
                 SETTING_OPTION_CSTR("2", "NVIDIA"),
                 SETTING_OPTION_CSTR("3", "AMD"),
				 SETTING_OPTION_CSTR("4", "INTEL"),
				 SETTING_OPTION     ("0", _p("Off")),
                 NULL);
  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("UI Refresh")),
                 SETTING_STORE("videoplayback", "mesa_fps"),
                 SETTING_WRITE_INT(&video_settings.mesa_double_fps),

				 SETTING_VALUE("2"),

				 SETTING_OPTION     ("1", _p("Match video framerate")),
				 SETTING_OPTION     ("2", _p("Double video framerate")),
				 SETTING_OPTION_CSTR("48", "48 Hz"),
                 SETTING_OPTION_CSTR("50", "50 Hz"),
				 SETTING_OPTION_CSTR("60", "60 Hz"),
				 SETTING_OPTION_CSTR("75", "75 Hz"),
				 SETTING_OPTION_CSTR("90", "90 Hz"),
				 SETTING_OPTION_CSTR("100", "100 Hz"),
				 SETTING_OPTION_CSTR("120", "120 Hz"),
                 SETTING_OPTION_CSTR("144", "144 Hz"),
                 NULL);
#endif

#if ENABLE_VDPAU
  setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Enable VDPAU")),
                 SETTING_VALUE(1),
                 SETTING_WRITE_BOOL(&video_settings.vdpau),
                 SETTING_STORE("videoplayback", "vdpau"),
                 NULL);

  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Preferred VDPAU deinterlacer method")),
                 SETTING_STORE("videoplayback", "vdpau_deinterlace"),
                 SETTING_WRITE_INT(&video_settings.vdpau_deinterlace),
                 SETTING_OPTION("2", _p("Temporal/Spatial")),
                 SETTING_OPTION("1", _p("Temporal")),
                 SETTING_OPTION("0", _p("Off")),
                 NULL);

  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Maximum resolution for deinterlacer")),
                 SETTING_STORE("videoplayback",
                                "vdpau_deinterlace_resolution_limit"),
                 SETTING_WRITE_INT(&video_settings.
                                   vdpau_deinterlace_resolution_limit),
                 SETTING_OPTION     ("0", _p("No limit")),
                 SETTING_OPTION_CSTR("576",  "576"),
                 SETTING_OPTION_CSTR("720",  "720"),
                 SETTING_OPTION_CSTR("1080", "1080"),
                 NULL);
#endif

#if defined(__APPLE__)
  // The shared separator above already labels this section.
  // settings_create_separator(s, _p("Accelerated Decoding"));

  setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("H264/AVC & H265/HEVC")),
                 SETTING_STORE("hw_videoplayback", "avc_hevc"),
                 SETTING_VALUE(1),
                 SETTING_WRITE_BOOL(&video_settings.video_accel),
                 NULL);

  setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Assume untagged HEVC Main10 is SDR")),
                 SETTING_STORE("hw_videoplayback", "untagged_main10_sdr"),
                 SETTING_VALUE(1),
                 SETTING_WRITE_BOOL(&video_settings.video_accel_untagged_main10),
                 NULL);

  setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Probe 10-bit P010 HDR output")),
                 SETTING_STORE("hw_videoplayback", "probe_p010_hdr"),
                 SETTING_VALUE(1),
                 SETTING_WRITE_BOOL(&video_settings.video_accel_probe_p010),
                 NULL);

#if TARGET_OS_OSX
  setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Use P010 10-bit decode with SDR output")),
                 SETTING_STORE("hw_videoplayback", "p010_sdr_playback"),
                 SETTING_VALUE(1),
                 SETTING_WRITE_BOOL(&video_settings.video_accel_p010_playback),
                 NULL);

  setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Direct P010 IOSurface rendering (Test 3)")),
                 SETTING_STORE("hw_videoplayback", "p010_direct_rendering"),
                 SETTING_VALUE(1),
                 SETTING_WRITE_BOOL(&video_settings.video_accel_p010_direct),
                 NULL);
#endif
#endif

  settings_create_separator(s, _p("Info Banner"));

  glw_settings.gs_setting_info_timer =
    setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Duration")),
                   SETTING_VALUE(5),
                   SETTING_RANGE(1, 10),
                   SETTING_UNIT_CSTR("sec"),
                   SETTING_WRITE_INT(&glw_settings.gs_info_timer),
				   SETTING_WRITE_PROP(prop_create(prop_create(prop_get_global(),
                                                            "osd"),
                                                "infoTimer")),
                   SETTING_STORE("glw", "infotimer"),
                   NULL);

  glw_settings.gs_setting_info_title =
    setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Title")),
                   SETTING_VALUE(1),
                   SETTING_WRITE_BOOL(&glw_settings.gs_info_title),
					SETTING_WRITE_PROP(prop_create(prop_create(prop_get_global(),
                                                            "osd"),
                                                "infoTitle")),
                   SETTING_STORE("glw", "infotitle"),
                   NULL);

  glw_settings.gs_setting_info_description =
    setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Description")),
                   SETTING_VALUE(1),
                   SETTING_WRITE_BOOL(&glw_settings.gs_info_description),
					SETTING_WRITE_PROP(prop_create(prop_create(prop_get_global(),
                                                            "osd"),
                                                "infoDescription")),
                   SETTING_STORE("glw", "infodescription"),
                   NULL);

  glw_settings.gs_setting_info_tracks =
    setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Audio & Subtitles")),
                   SETTING_VALUE(0),
                   SETTING_WRITE_BOOL(&glw_settings.gs_info_tracks),
					SETTING_WRITE_PROP(prop_create(prop_create(prop_get_global(),
                                                            "osd"),
                                                "infoTracks")),
                   SETTING_STORE("glw", "infotracks"),
                   NULL);


  settings_create_separator(s, _p("Visuals & Actions"));

  setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Show clock during playback")),
                 SETTING_STORE("videoplayback", "show_clock"),
                 SETTING_WRITE_PROP(prop_create(prop_create(prop_get_global(),
                                                            "clock"),
                                                "showDuringVideo")),
                 NULL);

  setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Automatically play next video in list")),
                 SETTING_STORE("videoplayback", "continuous_playback"),
                 SETTING_WRITE_BOOL(&video_settings.continuous_playback),
                 NULL);

  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Resume video playback")),
                 SETTING_WRITE_INT(&video_settings.resume_mode),
                 SETTING_STORE("videoplayback", "resumemode2"),
                 SETTING_OPTION("1", _p("Always")),
                 SETTING_OPTION("0", _p("Never")),
                 SETTING_OPTION("2", _p("Ask")),
                 NULL);

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Count video as played when reaching")),
                 SETTING_VALUE(90),
                 SETTING_RANGE(1, 100),
                 SETTING_UNIT_CSTR("%"),
                 SETTING_WRITE_INT(&video_settings.played_threshold),
                 SETTING_STORE("videoplayback", "played_threshold"),
                 NULL);

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Step when seeking backward")),
                 SETTING_VALUE(20),
                 SETTING_RANGE(3, 60),
                 SETTING_UNIT_CSTR("s"),
                 SETTING_WRITE_INT(&video_settings.seek_back_step),
                 SETTING_STORE("videoplayback", "seekbackstep"),
                 NULL);

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Step when seeking forward")),
                 SETTING_VALUE(60),
                 SETTING_RANGE(3, 60),
                 SETTING_UNIT_CSTR("s"),
                 SETTING_WRITE_INT(&video_settings.seek_fwd_step),
                 SETTING_STORE("videoplayback", "seekfwdstep"),
                 NULL);

  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Up / Down during video playback controls")),
                 SETTING_WRITE_INT(&video_settings.dpad_up_down_mode),
                 SETTING_STORE("videoplayback", "dpad_up_down_mode"),
                 SETTING_OPTION("0", _p("Master volume")),
                 SETTING_OPTION("1", _p("Per-file volume")),
                 SETTING_OPTION("2", _p("Channel +/-")),
                 NULL);

  settings_create_separator(s, _p("Zoom & Scale"));
  video_settings.vzoom_setting =
    setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Video zoom")),
                   SETTING_UNIT_CSTR("%"),
                   SETTING_RANGE(50, 200),
                   SETTING_VALUE(100),
                   SETTING_STORE("videoplayback", "vzoom"),
                   SETTING_VALUE_ORIGIN("global"),
                   NULL);

  video_settings.scale_vertical_setting =
    setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Vertical scale")),
                   SETTING_UNIT_CSTR("%"),
                   SETTING_RANGE(10, 300),
                   SETTING_VALUE(100),
                   SETTING_STORE("videoplayback", "verticalscale"),
                   SETTING_VALUE_ORIGIN("global"),
                   NULL);

  video_settings.pan_vertical_setting =
    setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Vertical pan")),
                   SETTING_UNIT_CSTR("%"),
                   SETTING_RANGE(-100, 100),
                   SETTING_VALUE(0),
                   SETTING_STORE("videoplayback", "verticalpan"),
                   SETTING_VALUE_ORIGIN("global"),
                   NULL);

  video_settings.scale_horizontal_setting =
    setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Horizontal scale")),
                   SETTING_UNIT_CSTR("%"),
                   SETTING_RANGE(10, 300),
                   SETTING_VALUE(100),
                   SETTING_STORE("videoplayback", "horizontalscale"),
                   SETTING_VALUE_ORIGIN("global"),
                   NULL);

  video_settings.pan_horizontal_setting =
    setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Horizontal pan")),
                   SETTING_UNIT_CSTR("%"),
                   SETTING_RANGE(-100, 100),
                   SETTING_VALUE(0),
                   SETTING_STORE("videoplayback", "horizontalpan"),
                   SETTING_VALUE_ORIGIN("global"),
                   NULL);

  video_settings.stretch_horizontal_setting =
    setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Stretch video to widescreen")),
                   SETTING_STORE("videoplayback", "stretch_horizontal"),
                   SETTING_VALUE_ORIGIN("global"),
                   NULL);

  video_settings.stretch_fullscreen_setting =
    setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Stretch video to fullscreen")),
                   SETTING_STORE("videoplayback", "stretch_fullscreen"),
                   SETTING_VALUE_ORIGIN("global"),
                   NULL);

  video_settings.vinterpolate_setting =
    setting_create(SETTING_BOOL, s, SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Video frame interpolation")),
                   SETTING_STORE("videoplayback", "vinterpolate"),
                   SETTING_VALUE_ORIGIN("global"),
                   SETTING_VALUE(1),
                   NULL);


  settings_create_separator(s, _p("Buffer & Adaptive Playback"));

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Buffer Size")),
                 SETTING_VALUE(112),
                 SETTING_RANGE(112, gconf.max_video_buffer_size ?: 768),
                 SETTING_UNIT_CSTR("MB"),
                 SETTING_STORE("videoplayback", "videobuffersize_154"),
                 SETTING_WRITE_INT(&video_settings.video_buffer_size),
                 NULL);

  setting_create(SETTING_INT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Pre-buffer")),
                 SETTING_VALUE(5),
                 SETTING_RANGE(0, 15),
                 SETTING_UNIT_CSTR("s"),
                 SETTING_STORE("videoplayback", "liveprebuffer"),
                 SETTING_WRITE_INT(&video_settings.video_prebuffer_size),
                 NULL);

/*
  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("HLS Live Playback Mode")),
                 SETTING_WRITE_INT(&video_settings.hls_live_mode),
                 SETTING_STORE("videoplayback", "hls_live_mode_154"),
				 SETTING_VALUE("0"),
                 SETTING_OPTION("0", _p("Default")),
                 SETTING_OPTION("1", _p("Large Pre-Buffer")),
                 SETTING_OPTION("2", _p("From Start")),
                 //SETTING_OPTION("3", _p("Standard with Rewind")),
                 NULL);

  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("HLS Initial Bitrate")),
                 SETTING_WRITE_INT(&video_settings.hls_best_quality),
                 SETTING_STORE("videoplayback", "hls_best_quality"),
                 SETTING_OPTION("1", _p("Highest (Best Quality)")),
                 SETTING_OPTION("0", _p("Default")),
                 NULL);
*/
  setting_create(SETTING_MULTIOPT, s, SETTINGS_INITIAL_UPDATE,
                 SETTING_TITLE(_p("Adaptive Format")),
                 SETTING_WRITE_INT(&video_settings.hls_limit),
                 SETTING_STORE("videoplayback", "hls_limit_avc_227"),
				 SETTING_VALUE("15"),
                 SETTING_OPTION("0", _p("No Limit")),
                 SETTING_OPTION("1", _p("SD")),
                 SETTING_OPTION("2", _p("720p")),
                 SETTING_OPTION("3", _p("1080p")),
                 SETTING_OPTION("4", _p("1440p")),
                 SETTING_OPTION("5", _p("4K")),

                 SETTING_OPTION("6", _p("AVC Only")),
                 SETTING_OPTION("7", _p("AVC SD")),
                 SETTING_OPTION("8", _p("AVC 720p")),
                 SETTING_OPTION("9", _p("AVC 1080p")),
                 SETTING_OPTION("10", _p("AVC 1440p")),
                 SETTING_OPTION("11", _p("AVC 4K")),

                 SETTING_OPTION("12", _p("AVC/HEVC Only")),
                 SETTING_OPTION("13", _p("AVC/HEVC SD")),
                 SETTING_OPTION("14", _p("AVC/HEVC 720p")),
                 SETTING_OPTION("15", _p("AVC/HEVC 1080p")),
                 SETTING_OPTION("16", _p("AVC/HEVC 1440p")),
                 SETTING_OPTION("17", _p("AVC/HEVC 4K")),

                 NULL);
  settings_create_separator(s, _p(" "));
  settings_create_separator(s, _p(" "));
}
