/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified extensively to become 
 * UxPlay - An open-souce AirPlay mirroring server.
 * Modifications Copyright (C) 2021-23 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <stddef.h>
#include <cstring>
#include <unistd.h>
#include <ctype.h>
#include <string>
#include <algorithm>
#include <vector>
#include <fstream>
#include <sstream>
#include <iterator>
#include <sys/stat.h>
#include <cstdio>
#include <stdarg.h>
#include <math.h>
#include <cmath>
#include <inttypes.h>
#include <atomic>
#include <mutex>

#ifdef _WIN32  /*modifications for Windows compilation */
#include <glib.h>
#include <unordered_map>
#include <winsock2.h>
#include <iphlpapi.h>
#include <pthread.h>   //for pthreads in MSYS2 UCRT
#else
#include <csignal>
#include <glib-unix.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/types.h>
#include <pwd.h>
# ifdef __linux__
# include <netpacket/packet.h>
# else
# include <net/if_dl.h>
#  ifdef __OpenBSD__
#  include <err.h>
#  endif
# endif
#endif

#include "lib/raop.h"
#include "lib/stream.h"
#include "lib/logger.h"
#include "lib/dnssd.h"
#include "lib/crypto.h"
#include "renderers/video_renderer.h"
#include "renderers/audio_renderer.h"
#include "renderers/mux_renderer.h"
#include "renderers/screen_status.h"
#include "renderers/screen_status_renderer.h"
#include "renderers/playback_trace_file.h"
#ifdef UXPLAY_HAVE_MPV
#include "renderers/mpv_backend.h"
#endif
#ifdef DBUS
#include <dbus/dbus.h>
#endif


#define VERSION "1.73.7"

#define SECOND_IN_USECS 1000000
#define SECOND_IN_NSECS 1000000000UL
#define DEFAULT_NAME "UxPlay"
#define DEFAULT_DEBUG_LOG false
#define LOWEST_ALLOWED_PORT 1024
#define HIGHEST_PORT 65535
#define MISSED_FEEDBACK_LIMIT 15
#define MIN_PASSWORD_LENGTH 4
#define DEFAULT_PLAYBIN_VERSION 3
#define BT709_FIX "capssetter caps=\"video/x-h264, colorimetry=bt709\""
#define SRGB_FIX  " ! video/x-raw,colorimetry=sRGB,format=RGB  ! "
#ifdef FULL_RANGE_RGB_FIX
  #define DEFAULT_SRGB_FIX true
#else
  #define DEFAULT_SRGB_FIX false
#endif

static std::string server_name = DEFAULT_NAME;
static bool server_name_is_utf8 = false;
static dnssd_t *dnssd = NULL;
static raop_t *raop = NULL;
static logger_t *render_logger = NULL;
static bool audio_sync = false;
static bool video_sync = true;
static int64_t audio_delay_alac = 0;
static int64_t audio_delay_aac = 0;
static bool relaunch_video = false;
static bool reset_loop = false;
static unsigned int open_connections= 0;
static std::string videosink = "autovideosink";
static std::string videosink_options = "";
static videoflip_t videoflip[2] = { NONE , NONE };
static bool use_video = true;
static unsigned char compression_type = 0;
static std::string audiosink = "autoaudiosink";
static int  audiodelay = -1;
static bool use_audio = true;
#if __APPLE__
static bool new_window_closing_behavior = false;
#else
static bool new_window_closing_behavior = true;
#endif
static bool close_window;
static bool full_video_reset = true;
static std::string video_parser = "h264parse";
static std::string video_decoder = "decodebin";
static std::string video_converter = "videoconvert";
static bool show_client_FPS_data = false;
static FILE *video_dumpfile = NULL;
static std::string video_dumpfile_name = "videodump";
static int video_dump_limit = 0;
static int video_dumpfile_count = 0;
static int video_dump_count = 0;
static bool dump_video = false;
static unsigned char mark[] = { 0x00, 0x00, 0x00, 0x01 };
static FILE *audio_dumpfile = NULL;
static std::string audio_dumpfile_name = "audiodump";
static int audio_dump_limit = 0;
static int audio_dumpfile_count = 0;
static int audio_dump_count = 0;
static bool dump_audio = false;
static unsigned char audio_type = 0x00;
static unsigned char previous_audio_type = 0x00;
static bool fullscreen = false;
static bool render_coverart = false;
static std::string coverart_filename = "";
static std::string metadata_filename = "";
static bool do_append_hostname = true;
static bool use_random_hw_addr = false;
static unsigned short display[5] = {0}, tcp[3] = {0}, udp[3] = {0};
static bool debug_log = DEFAULT_DEBUG_LOG;
static bool suppress_packet_debug_data = false;
static int log_level = LOGGER_INFO;
static bool bt709_fix = false;
static bool srgb_fix = DEFAULT_SRGB_FIX;
static int nohold = 0;
static bool nofreeze = false;
static unsigned short raop_port;
static unsigned short airplay_port;
static uint64_t remote_clock_offset = 0;
static std::vector<std::string> allowed_clients;
static std::vector<std::string> blocked_clients;
static bool restrict_clients;
static bool setup_legacy_pairing = false;
static unsigned char pin_pw = 0;  /* 0: no client access control; 1: onscreen pin ; 2: require password (same password for all clients)  3: random pw*/
static std::string password = "";
static guint min_password_length = MIN_PASSWORD_LENGTH;
static unsigned short pin = 0;
static std::string keyfile = "";
static std::string mac_address = "";
static std::string dacpfile = "";
static bool registration_list = false;
static std::string pairing_register = "";
static std::vector <std::string> registered_keys;
static double db_low = -30.0;
static double db_high = 0.0;
static bool taper_volume = false;
static double initial_volume = 0.0;
static bool h265_support = false;
static int n_video_renderers = 0;
static int n_audio_renderers = 0;
static bool hls_support = false;
static bool hls_pi4 = false;
static std::string lang = "";
static std::string url = "";
static guint gst_x11_window_id = 0;
static guint video_eos_watch_id = 0;
static guint progress_id = 0;
static guint gst_hls_position_id = 0;
static bool preserve_connections = false;
static guint missed_feedback_limit = MISSED_FEEDBACK_LIMIT;
static guint missed_feedback = 0;
static guint playbin_version = DEFAULT_PLAYBIN_VERSION;
static bool reset_httpd = false;
static bool monitor_progress = false;
static uint32_t rtptime = 0;
static uint32_t rtptime_prev = 0;
static uint32_t rtptime_start = 0;
static uint32_t rtptime_end = 0;
static uint32_t rtptime_coverart_expired = 0;
static std::string artist;
static std::string track_title;
static std::string track_album;
static std::string coverart_artist;
static std::string ble_filename = "";
static std::string rtp_pipeline = "";
static std::string audio_rtp_pipeline = "";
static GMainLoop *gmainloop = NULL;
static bool mux_to_file = false;
static std::string mux_filename = "recording";

/* Only direct AirPlay video is selectable. Mirroring and RAOP stay on GStreamer. */
static bool airplay_video_mpv = false;
static screen_info_mode_t screen_info = SCREEN_INFO_OFF;
static std::atomic<uint64_t> trace_history_failures(0);
static screen_status_renderer_t *status_display = NULL;
static std::mutex display_mutex;
static bool receiver_registered = false;
static std::atomic<bool> mpv_owns_output(false);
static std::atomic<bool> mpv_rebuild_pending(false);
static std::atomic<bool> mirror_waiting(false);
static bool mpv_output_prepared = false;
static std::atomic<bool> playback_output_released(true);
static std::atomic<uint64_t> mpv_generation(0);
static std::atomic<uint64_t> playback_generation(0);
#ifdef UXPLAY_HAVE_MPV
static mpv_backend_t *mpv_player = NULL;
static mpv_backend_config_t mpv_config = {};
static std::string mpv_executable, mpv_vo, mpv_context, mpv_api, mpv_drm_device;
static std::string mpv_connector, mpv_audio_device, mpv_h264_hwdec;
static mpv_decode_policy_t mpv_policy = MPV_DECODE_SOFTWARE;
static bool mpv_fast_rendering = false;
#endif

//Support for D-Bus-based screensaver inhibition (org.freedesktop.ScreenSaver) 
static unsigned int scrsv = 0;
#ifdef DBUS 
/* these strings can be changed at startup if a non-conforming Desktop Environmemt is detected */
static std::string dbus_service = "org.freedesktop.ScreenSaver";
static std::string dbus_path = "/org/freedesktop/ScreenSaver";
static std::string dbus_interface = "org.freedesktop.ScreenSaver";
static std::string dbus_inhibit = "Inhibit";
static std::string dbus_uninhibit = "UnInhibit";
static DBusConnection *dbus_connection = NULL;
static dbus_uint32_t dbus_cookie = 0;
static DBusPendingCall *dbus_pending = NULL;
static bool dbus_last_message = false;
static const char *appname = DEFAULT_NAME;
static const char *reason_always = "mirroring client: inhibit always";
static const char *reason_active = "actively receiving video";
static float previous_hls_position = 0.0f;
#endif

/* logging */

static void log(int level, const char* format, ...) {
    va_list vargs;
    if (level > log_level) return;
    switch (level) {
    case 0:
    case 1:
    case 2:
    case 3:
        printf("*** ERROR: ");
        break;
    case 4:
        printf("*** WARNING: ");
        break;
    default:
        break;
    }
    va_start(vargs, format);
    vprintf(format, vargs);
    printf("\n");
    va_end(vargs);
}

#define LOGD(...) log(LOGGER_DEBUG, __VA_ARGS__)
#define LOGI(...) log(LOGGER_INFO, __VA_ARGS__)
#define LOGW(...) log(LOGGER_WARNING, __VA_ARGS__)
#define LOGE(...) log(LOGGER_ERR, __VA_ARGS__)

#ifdef DBUS
static void dbus_screensaver_inhibiter(bool inhibit) {
    g_assert(inhibit != dbus_last_message);
    g_assert(scrsv);
    /* receive reply from previous request, whenever that was sent
     * (may have been sent hours ago ... !) 
     * (code modeled on vlc/modules/misc/inhibit/dbus.c) */
    if (dbus_pending != NULL) {
        DBusMessage *reply;
        dbus_pending_call_block(dbus_pending);
        reply = dbus_pending_call_steal_reply(dbus_pending);
        dbus_pending_call_unref(dbus_pending);
        dbus_pending = NULL;
        if (reply != NULL) {
            if (!dbus_message_get_args(reply, NULL,
                                       DBUS_TYPE_UINT32, &dbus_cookie,
                                       DBUS_TYPE_INVALID)) {
                dbus_cookie = 0;
            }
            dbus_message_unref(reply);
        }
        LOGD("screen_saver: got D-Bus cookie %" PRIu32, (uint32_t) dbus_cookie);
    }
    
    if (!dbus_cookie && !inhibit) {
        return; /* nothing to do */
    }
	  
    /* send request */
    const char *dbus_method = inhibit ? dbus_inhibit.c_str() : dbus_uninhibit.c_str();
    DBusMessage *dbus_message = dbus_message_new_method_call(dbus_service.c_str(),
                                                             dbus_path.c_str(),
                                                             dbus_interface.c_str(),
                                                             dbus_method);
    g_assert (dbus_message);
    
    if (inhibit) {
        dbus_bool_t ret;
        const char *reason = (scrsv == 1) ? reason_active : reason_always;
	
        ret = dbus_message_append_args(dbus_message,
                                       DBUS_TYPE_STRING, &appname,
                                       DBUS_TYPE_STRING, &reason,
                                       DBUS_TYPE_INVALID);
	g_assert(ret);

        ret =  dbus_connection_send_with_reply(dbus_connection, dbus_message, &dbus_pending, -1);
        if (!ret) {
            dbus_pending = NULL;
        }
    } else {
        g_assert(dbus_cookie);
        LOGD("screen_saver: releasing D-Bus cookie %" PRIu32, (uint32_t) dbus_cookie);
        if (dbus_message_append_args(dbus_message,
                                     DBUS_TYPE_UINT32, &dbus_cookie,
                                     DBUS_TYPE_INVALID)
            && dbus_connection_send(dbus_connection, dbus_message, NULL)) {
            dbus_cookie = 0;
        }
    }
    
    dbus_connection_flush(dbus_connection);
    dbus_message_unref(dbus_message);
    dbus_last_message = inhibit;
}
#endif

static bool file_has_write_access (const char * filename) {
    bool exists = false;
    bool write = false;
#ifdef _WIN32
    if ((exists = _access(filename, 0) != -1)) {
        write = (_access(filename, 2) != -1);
    }
#else
    if ((exists = access(filename, F_OK) != -1)) {
        write = (access(filename, W_OK) != -1);
    }
#endif
    if (!exists) {
        FILE *fp = fopen(filename, "w");
        if (fp) {
            write = true;
	    fclose(fp);
	    remove(filename);
        }
    }
    return write;
}

/* 95 byte png file with a 1x1 white square (single pixel): placeholder for coverart*/
static const unsigned char empty_image[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,  0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  0x01, 0x03, 0x00, 0x00, 0x00, 0x25, 0xdb, 0x56,
    0xca, 0x00, 0x00, 0x00, 0x03, 0x50, 0x4c, 0x54,  0x45, 0x00, 0x00, 0x00, 0xa7, 0x7a, 0x3d, 0xda,
    0x00, 0x00, 0x00, 0x01, 0x74, 0x52, 0x4e, 0x53,  0x00, 0x40, 0xe6, 0xd8, 0x66, 0x00, 0x00, 0x00,
    0x0a, 0x49, 0x44, 0x41, 0x54, 0x08, 0xd7, 0x63,  0x60, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0xe2,
    0x21, 0xbc, 0x33, 0x00, 0x00, 0x00, 0x00, 0x49,  0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };

static size_t write_coverart(const char *filename, const void *image, size_t len) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("Failed to open file %s\n", filename);
        return 0;
    }
    size_t count = fwrite(image, 1, len, fp);
    fclose(fp);
    return count;
}

static size_t write_metadata(const char *filename, const char *text) {
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("Failed to open file %s\n", filename);
        return 0;
    }
    size_t count = fwrite(text, sizeof(char), strlen(text) + 1, fp);
    fclose(fp);
    return count;
}

static int write_bledata( const uint32_t *pid, const char *process_name, const char *filename) {
    char name[16] { 0 };
    size_t len = strlen(process_name);
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        printf("Failed to open file %s\n", filename);
        return 0;
    }
    printf("port %u\n", raop_port);
    size_t count = sizeof(uint16_t) * fwrite(&raop_port, sizeof(uint16_t), 1, fp);
    count += sizeof(uint32_t) * fwrite(pid, sizeof(uint32_t), 1, fp);
    count += sizeof(char) * len * fwrite(process_name, 1, len * sizeof(char), fp);
    fclose(fp);
    return (int) count;
}

static char *create_pin_display(char *pin_str, int margin, int gap) {
    char *ptr;
    char num[2] = { 0 };
    int w = 10;
    int h = 8;
    char digits[10][8][11] = { "0821111380", "2114005113", "1110000111", "1110000111", "1110000111", "1110000111", "5113002114", "0751111470",
                               "0002111000", "0021111000", "0000111000", "0000111000", "0000111000", "0000111000", "0000111000", "0011111110", 
                               "0811112800", "2114005113", "0000000111", "0000082114", "0862111470", "2114700000", "1117000000", "1111111111", 
                               "0821111380", "2114005113", "0000082114", "0000111170", "0000075130", "1110000111", "5113002114", "0751111470", 
                               "0000211110", "0001401110", "0021401110", "0214001110", "2110001110", "1111111111", "0000001110", "0000001110", 
                               "1111111110", "1110000000", "1110000000", "1112111380", "0000075113", "0000000111", "5113002114", "0711114700",  
                               "0821111380", "2114005113", "1110000000", "1112111380", "1114075113", "1110000111", "5113002114", "0751111470", 
                               "1111111111", "0000002114", "0000021140", "0000211400", "0002114000", "0021140000", "0211400000", "2114000000", 
                               "0831111280", "2114002114", "5113802114", "0751111170", "8214775138", "1110000111", "5113002114", "0751111470", 
                               "0821111380", "2114005113", "1110000111", "5113802111", "0751114111", "0000000111", "5113002114", "0751111470"  
                             };

    char pixels[9] = { ' ', '8', 'd', 'b', 'P', 'Y', 'o', '"', '.' };
    /* Ascii art used here is derived from the FIGlet font "collosal" */

    int pin_val = (int) strtoul(pin_str, &ptr, 10);
    if (*ptr) {
        return NULL;
    }
    int len = strlen(pin_str);
    int *pin = (int *) calloc( len, sizeof(int));
    if(!pin) {
        return NULL;
    }

    for (int i = 0; i < len; i++) {
        pin[len - 1 - i] = pin_val % 10;
        pin_val = pin_val / 10;
    }
  
    int size = 4 + h*(margin + len*(w + gap + 1));
    char *pin_image = (char *) calloc(size, sizeof(char));
    if (!pin_image) {
        return NULL;
    }
    char *pos = pin_image;
    snprintf(pos, 2, "\n"); 
    pos++;

    for (int i = 0; i < h; i++) {
        for (int j = 0; j < margin; j++) {
            snprintf(pos, 2,  " ");
            pos++;
        }

        for (int j = 0; j < len; j++) {
            int l = pin[j];
            char *p = digits[l][i];
            for (int k = 0; k < w; k++) {
                char *ptr;
                strncpy(num, p++, 1);
                int r = (int) strtoul(num, &ptr, 10);
                snprintf(pos, 2, "%c", pixels[r]);
                pos++;
            }
            for (int n=0; n < gap ; n++) {
                snprintf(pos, 2, " ");
                pos++;
            }
        }
        snprintf(pos, 2, "\n");
        pos++;
    }
    snprintf(pos, 2, "\n");
    return pin_image;
}

static void dump_audio_to_file(unsigned char *data, int datalen, unsigned char type) {
    if (!audio_dumpfile && audio_type != previous_audio_type) {
        char suffix[20];
        std::string fn = audio_dumpfile_name;
        previous_audio_type = audio_type;
        audio_dumpfile_count++;
        audio_dump_count = 0;
        /* type 0x20 is lossless ALAC, type 0x80 is compressed AAC-ELD, type 0x10 is "other" */
        if (audio_type == 0x20) {
            snprintf(suffix, sizeof(suffix), ".%d.alac", audio_dumpfile_count);
        } else if (audio_type == 0x80) {
            snprintf(suffix, sizeof(suffix), ".%d.aac", audio_dumpfile_count);
        } else {
            snprintf(suffix, sizeof(suffix), ".%d.aud", audio_dumpfile_count);
        }
        fn.append(suffix);
        audio_dumpfile = fopen(fn.c_str(),"w");
        if (audio_dumpfile == NULL) {
            LOGE("could not open file %s for dumping audio frames",fn.c_str());
        }
    }

    if (audio_dumpfile) {
        fwrite(data, 1, datalen, audio_dumpfile);
        if (audio_dump_limit) {
            audio_dump_count++;
            if (audio_dump_count == audio_dump_limit) {
                fclose(audio_dumpfile);
                audio_dumpfile = NULL;
            }          
        }
    }
}

static void dump_video_to_file(unsigned char *data, int datalen) {
    /*  SPS NAL has (data[4] & 0x1f) = 0x07  */
    if ((data[4] & 0x1f) == 0x07  && video_dumpfile && video_dump_limit) {
        fwrite(mark, 1, sizeof(mark), video_dumpfile);
        fclose(video_dumpfile);
        video_dumpfile = NULL;
        video_dump_count = 0;                     
    }

    if (!video_dumpfile) {
        std::string fn = video_dumpfile_name;
        if (video_dump_limit) {
            char suffix[20];
            video_dumpfile_count++;
            snprintf(suffix, sizeof(suffix), ".%d", video_dumpfile_count);
            fn.append(suffix);
        }
        fn.append(".h264");
        video_dumpfile = fopen (fn.c_str(),"w");
        if (video_dumpfile == NULL) {
            LOGE("could not open file %s for dumping h264 frames",fn.c_str());
        }
    }

    if (video_dumpfile) {
        if (video_dump_limit == 0) {
            fwrite(data, 1, datalen, video_dumpfile);
        } else if (video_dump_count < video_dump_limit) {
            video_dump_count++;
            fwrite(data, 1, datalen, video_dumpfile);
        }
    }
}

static gboolean feedback_callback(gpointer loop) {
    if (open_connections) {
        if (missed_feedback_limit && missed_feedback > missed_feedback_limit) {
            LOGI("***ERROR lost connection with client (network problem?)");
            LOGI("   Interval since last client feedback request exceeds limit of %u seconds", missed_feedback_limit);
            LOGI("   Sometimes the network connection may recover after a longer delay:\n"
                 "   the default limit n = %d seconds, can be changed with the \"-reset n\" option", MISSED_FEEDBACK_LIMIT);
            if (!nofreeze) {
                close_window = false; /* leave "frozen" window open if reset_video is false */
            }
            reset_httpd = true;
            relaunch_video = true;
            full_video_reset = true;
            g_main_loop_quit((GMainLoop *) loop);
            return TRUE;
        } else if (missed_feedback > 2) {
            LOGE("%3u seconds since last client feedback request (expected every two seconds); client may be offline", missed_feedback);
        }
        missed_feedback++;
    } else {
        missed_feedback = 0;
    }
    return TRUE;
}

static gboolean reset_callback(gpointer loop) {
    if (reset_loop) {
        g_main_loop_quit((GMainLoop *) loop);
    }
    return TRUE;
}

static gboolean x11_window_callback(gpointer loop) {
    /* called while trying to find an x11 window used by playbin (HLS mode) */
    if (waiting_for_x11_window()) {
        return TRUE;
    }
    g_source_remove(gst_x11_window_id);
    gst_x11_window_id = 0;
    return FALSE;
}

/* signals handlers (ctrl-c, etc )*/

[[noreturn]] static void cleanup();

#ifdef _WIN32
static gboolean handle_signal(gpointer data) {
    relaunch_video = false;
    g_main_loop_quit(gmainloop);
    return G_SOURCE_REMOVE;
}

static BOOL WINAPI CtrlHandler(DWORD signal) {
    switch (signal) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (gmainloop) {
            g_idle_add(handle_signal, NULL);
            return TRUE;
        } else {  
            cleanup();
            exit(0);
	}
    default:
        return FALSE;
    }
}
#else
static void CtrlHandler(int signum) {
    cleanup();
    exit(0);
}

static gboolean sigint_callback(gpointer loop) {
    relaunch_video = false;
    g_main_loop_quit((GMainLoop *) loop);
    return TRUE;
}

static gboolean sigterm_callback(gpointer loop) {
    relaunch_video = false;
    g_main_loop_quit((GMainLoop *) loop);
    return TRUE;
}

static gboolean sighup_callback(gpointer loop) {
    relaunch_video = false;
    g_main_loop_quit((GMainLoop *) loop);
    return TRUE;
}
#endif

static void display_progress(uint32_t start, uint32_t curr, uint32_t end) {
    if (curr < start || curr > end) {
        return;
    }
    int duration = (int)  (end  - start)/44100;
    int position = (int)  (curr - start)/44100;
    int remain = duration - position;
    printf("audio progress (min:sec): %3d:%2.2d; remaining: %3d:%2.2d; track length %d:%2.2d\r",
           position/60, position%60, remain/60, remain%60, duration/60, duration%60);
    fflush(NULL);
}

static gboolean progress_callback (gpointer loop) {
    if (monitor_progress) {
        if ((rtptime_start || rtptime_end) && rtptime != rtptime_prev ) { //only display if rtptime has changed since last call
            display_progress(rtptime_start, rtptime, rtptime_end);
            rtptime_prev = rtptime;
        }
        if (render_coverart && coverart_artist == "_expired_" && rtptime - rtptime_coverart_expired > 44100 * 5) {
            /* remove any expired coverart still being rendered more than 5 secs after it expired */ 
            coverart_artist.erase();
            video_renderer_cycle();
        }
        return TRUE;
    } else {
        progress_id = 0;
        return FALSE;
    }
}

static bool receiver_has_network() {
#ifndef _WIN32
    struct ifaddrs *interfaces = NULL;
    if (getifaddrs(&interfaces) != 0) return false;
    bool available = false;
    for (struct ifaddrs *it = interfaces; it; it = it->ifa_next) {
        if (it->ifa_addr && (it->ifa_flags & IFF_UP) && !(it->ifa_flags & IFF_LOOPBACK) &&
            (it->ifa_addr->sa_family == AF_INET || it->ifa_addr->sa_family == AF_INET6)) {
            available = true; break;
        }
    }
    freeifaddrs(interfaces);
    return available;
#else
    return raop && raop_is_running(raop);
#endif
}

/* Only fixed, typed records use this path. The ordinary protocol logger is
 * deliberately excluded from the bounded on-disk playback history. */
static void trace_playback_record(const char *format, ...) {
    char text[6144];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    LOGI("%s", text);
#ifndef UXPLAY_RECEIVER_TEST
    if (screen_info == SCREEN_INFO_DEBUG) {
        static std::atomic<bool> unavailable_reported(false);
        if (!playback_trace_file_write(text)) {
            ++trace_history_failures;
            if (!unavailable_reported.exchange(true))
                LOGE("Playback diagnostic history has lost a record; check report history availability and write failures");
        }
    }
#endif
}

static uint64_t screen_generation() {
    screen_status_snapshot_t snapshot;
    screen_status_get_snapshot(&snapshot);
    return snapshot.generation;
}

static void screen_set_backend(bool direct_video) {
    screen_status_snapshot_t current;
    screen_status_get_snapshot(&current);
    screen_status_video_t video = {};
    const char *backend = direct_video && airplay_video_mpv ? "mpv" : "GStreamer";
    g_strlcpy(video.backend, backend, sizeof(video.backend));
    const char *policy = hls_pi4 ? "hls-pi4" : "GStreamer default";
#ifdef UXPLAY_HAVE_MPV
    if (direct_video && airplay_video_mpv) policy = mpv_policy == MPV_DECODE_SOFTWARE ? "software" :
        mpv_policy == MPV_DECODE_PI4_SAFE ? "pi4-safe" : "pi4-hevc-experimental";
#endif
    g_strlcpy(video.decode_policy, policy, sizeof(video.decode_policy));
    screen_status_set_video(current.generation, &video);
}

static bool hide_status_display() {
    if (!status_display || screen_status_renderer_hide(status_display)) return true;
    screen_status_event(screen_generation(), SCREEN_EVENT_RECOVERY_REQUIRED);
    LOGE("Display release was not confirmed; refusing a competing video output");
    return false;
}

static void show_status_display() {
    if (status_display && !screen_status_renderer_show(status_display)) {
        screen_status_fail(screen_generation(), SCREEN_ERROR_OUTPUT);
        LOGE("Could not display receiver status; see the receiver log");
    }
}

/* These protocol setup callbacks need a result before starting RTP. They
 * enqueue stop, then wait at most six seconds while the owning main loop
 * performs IPC, escalation, reaping and display release. This synchronous
 * setup wait can delay the shared HTTP server; ordinary play/control callbacks
 * remain queued. A fully asynchronous setup path needs bounded early-RTP
 * buffering and is deferred from this first build. */
static bool wait_for_mpv_release() {
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player && mpv_owns_output) {
        mirror_waiting = true;
        mpv_backend_stop(mpv_player, mpv_generation);
        const gint64 deadline = g_get_monotonic_time() + 6 * G_USEC_PER_SEC;
        while (mpv_owns_output && g_get_monotonic_time() < deadline) g_usleep(10000);
        mirror_waiting = false;
        if (mpv_owns_output) {
            screen_status_event(screen_generation(), SCREEN_EVENT_RECOVERY_REQUIRED);
            return false;
        }
    }
#endif
    return true;
}

#ifdef UXPLAY_HAVE_MPV
static const char *mpv_trace_codec(const char *codec) {
    const char *known[] = {"h264", "hevc", "av1", "vp8", "vp9", "mpeg2video", "mpeg4"};
    if (!codec[0]) return "unknown";
    for (const char *name : known) if (!strcmp(codec, name)) return name;
    return "other";
}

static const char *mpv_trace_state(mpv_backend_state_t state) {
    switch (state) {
    case MPV_BACKEND_IDLE: return "idle";
    case MPV_BACKEND_STARTING: return "starting";
    case MPV_BACKEND_LOADING: return "loading";
    case MPV_BACKEND_BUFFERING: return "buffering";
    case MPV_BACKEND_PLAYING: return "playing";
    case MPV_BACKEND_PAUSED: return "paused";
    case MPV_BACKEND_STOPPING: return "stopping";
    case MPV_BACKEND_ENDED: return "ended";
    case MPV_BACKEND_FAILED: return "failed";
    default: return "unknown";
    }
}

static const char *mpv_trace_label(const char *value, const char *allowed) {
    if (!value[0] || strlen(value) > 95) return "unknown";
    for (const char *p = value; *p; ++p)
        if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_') return "other";
    char token[100];
    g_snprintf(token, sizeof(token), "|%s|", value);
    return strstr(allowed, token) ? value : "other";
}

static const char *mpv_trace_reason(const char *value) {
    return mpv_trace_label(value, "|http-client-error|http-server-error|hls-init-failure|hls-segment-failure|hls-reload-failure|missing-reference|video-decode-error|invalid-data|timestamp-discontinuity|audio-output-underrun|audio-output-init-error|audio-decode-error|pes-size-mismatch|packet-corrupt|demux-read-error|hls-expired-segments|hls-sequence-change|virtual-terminal-unavailable|frame-present-failure|drm-display-failure|unknown|");
}

static const char *mpv_trace_control_error(const char *value) {
    if (!value[0]) return "none";
    if (!strcmp(value, "mpv rejected seek")) return "seek-rejected";
    if (!strcmp(value, "mpv rejected volume control")) return "volume-rejected";
    if (!strcmp(value, "mpv rejected the diagnostic overlay")) return "overlay-rejected";
    if (!strcmp(value, "mpv volume acknowledgement timed out")) return "volume-ack-timeout";
    if (!strcmp(value, "mpv overlay acknowledgement timed out")) return "overlay-ack-timeout";
    if (!strcmp(value, "mpv seek completion timed out")) return "seek-timeout";
    return "other";
}

static void trace_mpv_control(uint64_t generation, const char *action, bool accepted, double value) {
    if (!generation) return; /* No player attempt exists yet. */
    trace_playback_record("MPV control: session=%" G_GUINT64_FORMAT " action=%s accepted=%d value=%.3f monotonic_ms=%" G_GINT64_FORMAT,
         (guint64)generation, action, accepted, std::isfinite(value) ? value : 0.0,
         (gint64)(g_get_monotonic_time() / 1000));
}

/* A first timestamp, including zero, is a baseline. Only a later advancing
 * timestamp is progress; neither observation establishes physical output. */
struct mpv_progress_observer_t {
    uint64_t generation = 0;
    double position = 0;
    bool baseline = false;
    gint64 started_at = 0, advanced_at = 0;
};

static bool observe_mpv_progress(mpv_progress_observer_t &o,
                                 const mpv_backend_snapshot_t &p, gint64 now) {
    if (o.generation != p.generation) {
        o = mpv_progress_observer_t();
        o.generation = p.generation;
        o.started_at = now;
    }
    const bool can_advance = !p.seeking && !p.requested_paused &&
        !(p.actual_paused_known && p.actual_paused);
    const bool advanced = can_advance && p.position_known && o.baseline &&
        p.position > o.position + 0.001;
    if (advanced) o.advanced_at = now;
    o.baseline = p.position_known && can_advance;
    o.position = p.position;
    return advanced;
}

/* Every string in this trace comes from a local allowlist. Only typed
 * measurements cross from mpv; no source location or raw player text does. */
static void format_mpv_trace(const mpv_backend_snapshot_t &p, char *text, size_t capacity, bool terminal_report = false) {
    g_snprintf(text, capacity,
        "MPV playback: session=%" G_GUINT64_FORMAT " state=%s ready=%d child=%d paused=%d seeking=%d"
        " codec=%s size=%dx%d position_known=%d position=%.3f audio_position_known=%d audio_position=%.3f"
        " cache_known=%d cache_seconds=%.3f cache_bytes_known=%d cache_bytes=%" G_GINT64_FORMAT
        " cache_speed_known=%d cache_bytes_per_second=%" G_GINT64_FORMAT
        " decoder_drops=%" G_GINT64_FORMAT " output_drops=%" G_GINT64_FORMAT
        " audio_rate=%d audio_channels=%d avsync_known=%d avsync=%.3f"
        " error_code=%d file_error_known=%d file_error_code=%d exit_known=%d exit_code=%d signal=%d"
        " error_reports_known=%d video_decode_errors=%" G_GUINT64_FORMAT
        " audio_decode_errors=%" G_GUINT64_FORMAT " demux_errors=%" G_GUINT64_FORMAT
        " network_errors=%" G_GUINT64_FORMAT " video_output_errors=%" G_GUINT64_FORMAT
        " audio_output_errors=%" G_GUINT64_FORMAT " other_errors=%" G_GUINT64_FORMAT
        " metadata_errors=%" G_GUINT64_FORMAT,
        (guint64)p.generation, mpv_trace_state(p.state), p.ready, p.child_alive, p.requested_paused, p.seeking,
        mpv_trace_codec(p.video_codec), p.width, p.height, p.position_known, p.position,
        p.audio_position_known, p.audio_position, p.cache_duration_known, p.cache_duration,
        p.cache_bytes_known, (gint64)p.cache_bytes, p.cache_speed_known, (gint64)p.cache_speed,
        (gint64)p.dropped_frames, (gint64)p.output_dropped_frames, p.audio_samplerate, p.audio_channels,
        p.avsync_known, p.avsync, p.error_code, p.file_error_code_known, p.file_error_code,
        p.child_exit_known, p.child_exit_code, p.child_signal, p.log_messages_active,
        (guint64)p.video_decode_errors, (guint64)p.audio_decode_errors, (guint64)p.demux_errors,
        (guint64)p.network_errors, (guint64)p.video_output_errors, (guint64)p.audio_output_errors,
        (guint64)p.unclassified_errors, (guint64)p.metadata_observation_errors);
    const size_t used = strlen(text);
    if (used >= capacity) return;
    g_snprintf(text + used, capacity - used,
        " schema=2 monotonic_ms=%" G_GINT64_FORMAT
        " actual_paused_known=%d actual_paused=%d core_idle_known=%d core_idle=%d"
        " cache_eof_known=%d cache_eof=%d cache_underrun_known=%d cache_underrun=%d cache_idle_known=%d cache_idle=%d"
        " selected_video_known=%d selected_video_id=%" G_GINT64_FORMAT
        " selected_audio_known=%d selected_audio_id=%" G_GINT64_FORMAT
        " decoded_parameters_known=%d decoded_width=%d decoded_height=%d"
        " video_observation_age_ms=%" G_GINT64_FORMAT " audio_observation_age_ms=%" G_GINT64_FORMAT
        " cache_observation_age_ms=%" G_GINT64_FORMAT
        " video_decoder=%s audio_decoder=%s pixel_format=%s hwdec=%s vo=%s ao=%s"
        " end_reason=%s warning_reports=%" G_GUINT64_FORMAT " video_decode_warnings=%" G_GUINT64_FORMAT
        " missing_reference_warnings=%" G_GUINT64_FORMAT " invalid_data_warnings=%" G_GUINT64_FORMAT
        " timestamp_warnings=%" G_GUINT64_FORMAT " audio_output_warnings=%" G_GUINT64_FORMAT
        " unknown_warnings=%" G_GUINT64_FORMAT " diagnostic_reason=%s reason_at_ms=%" G_GUINT64_FORMAT
        " http_status_known=%d http_status=%d http_error_count=%" G_GUINT64_FORMAT
        " hls_init_failures=%" G_GUINT64_FORMAT " hls_segment_failures=%" G_GUINT64_FORMAT " hls_reload_failures=%" G_GUINT64_FORMAT,
        (gint64)(g_get_monotonic_time()/1000),
        p.actual_paused_known, p.actual_paused, p.core_idle_known, p.core_idle,
        p.cache_eof_known, p.cache_eof, p.cache_underrun_known, p.cache_underrun, p.cache_idle_known, p.cache_idle,
        p.selected_video_known, (gint64)p.selected_video_id, p.selected_audio_known, (gint64)p.selected_audio_id,
        p.decoded_parameters_known, p.decoded_width, p.decoded_height,
        (gint64)p.video_observation_age_ms, (gint64)p.audio_observation_age_ms, (gint64)p.cache_observation_age_ms,
        mpv_trace_label(p.video_decoder, "|h264|h264_v4l2m2m|hevc|hevc_v4l2m2m|vp8|vp9|av1|mpeg2video|mpeg4|"),
        mpv_trace_label(p.audio_decoder, "|aac|aac_fixed|mp3|mp3float|ac3|eac3|alac|flac|opus|vorbis|pcm_s16le|pcm_s24le|"),
        mpv_trace_label(p.pixel_format, "|yuv420p|yuv420p10|yuv420p10le|yuv422p|yuv422p10|yuv422p10le|yuv444p|yuv444p10|yuv444p10le|nv12|drm_prime|"),
        mpv_trace_label(p.hwdec_current, "|no|drm|drm-copy|v4l2m2m|v4l2m2m-copy|vaapi|vaapi-copy|vdpau|vdpau-copy|"),
        mpv_trace_label(p.video_output, "|gpu|gpu-next|drm|x11|xv|null|"),
        mpv_trace_label(p.audio_output, "|alsa|pulse|pipewire|jack|null|"),
        mpv_trace_label(p.end_reason, "|eof|stop|quit|error|redirect|unknown|"),
        (guint64)p.log_warnings, (guint64)p.video_decode_warnings, (guint64)p.missing_reference_warnings,
        (guint64)p.invalid_data_warnings, (guint64)p.timestamp_warnings, (guint64)p.audio_output_warnings,
        (guint64)p.unknown_warnings,
        mpv_trace_reason(p.diagnostic_reason),
        (guint64)p.diagnostic_reason_at_ms, p.last_http_status_known, p.last_http_status, (guint64)p.http_error_count,
        (guint64)p.hls_init_failures, (guint64)p.hls_segment_failures, (guint64)p.hls_reload_failures);
    const size_t health_used = strlen(text);
    if (health_used >= capacity) return;
    g_snprintf(text + health_used, capacity - health_used,
        " child_pid=%d child_generation=%" G_GUINT64_FORMAT " terminal_report=%d terminal_reports_dropped=%" G_GUINT64_FORMAT
        " history_write_failures=%" G_GUINT64_FORMAT
        " decode_policy=%s control_error=%s failure_stage=%s terminal_draining=%d log_overflows=%" G_GUINT64_FORMAT
        " event_overflows=%" G_GUINT64_FORMAT " log_text_rejected=%" G_GUINT64_FORMAT
        " ipc_read_budget_exhaustions=%" G_GUINT64_FORMAT
        " video_reader_pts_known=%d video_reader_pts=%.3f video_cache_end_known=%d video_cache_end=%.3f"
        " video_cache_duration_known=%d video_cache_duration=%.3f"
        " audio_reader_pts_known=%d audio_reader_pts=%.3f audio_cache_end_known=%d audio_cache_end=%.3f"
        " audio_cache_duration_known=%d audio_cache_duration=%.3f",
        p.child_pid, (guint64)p.child_generation, terminal_report, (guint64)p.terminal_reports_dropped,
        (guint64)trace_history_failures.load(),
        mpv_policy == MPV_DECODE_SOFTWARE ? "software" : mpv_policy == MPV_DECODE_PI4_SAFE ? "pi4-safe" : "pi4-hevc-experimental",
        mpv_trace_control_error(p.control_error), mpv_trace_label(p.failure_stage, "|startup|ipc|load|playback|control|stop|audio-output|video-output|"),
        p.terminal_diagnostics_draining, (guint64)p.log_overflows, (guint64)p.event_overflows,
        (guint64)p.log_text_rejected, (guint64)p.ipc_read_budget_exhaustions,
        p.cache_video.reader_pts_known, p.cache_video.reader_pts, p.cache_video.cache_end_known, p.cache_video.cache_end,
        p.cache_video.cache_duration_known, p.cache_video.cache_duration,
        p.cache_audio.reader_pts_known, p.cache_audio.reader_pts, p.cache_audio.cache_end_known, p.cache_audio.cache_end,
        p.cache_audio.cache_duration_known, p.cache_audio.cache_duration);
    const size_t packet_used = strlen(text);
    if (packet_used >= capacity) return;
    g_snprintf(text + packet_used, capacity - packet_used,
        " packet_capture_enabled=%d packet_capture_active=%d packet_capture_complete=%d"
        " packet_capture_started_ms=%" G_GUINT64_FORMAT " packet_capture_ended_ms=%" G_GUINT64_FORMAT
        " packet_log_rejected=%" G_GUINT64_FORMAT " packet_capture_errors=%" G_GUINT64_FORMAT
        " video_packets=%" G_GUINT64_FORMAT " video_packet_bytes=%" G_GUINT64_FORMAT
        " video_packets_with_pts=%" G_GUINT64_FORMAT " video_packet_first_pts=%.6f video_packet_last_pts=%.6f"
        " video_packet_first_at_ms=%" G_GUINT64_FORMAT " video_packet_last_at_ms=%" G_GUINT64_FORMAT
        " audio_packets=%" G_GUINT64_FORMAT " audio_packet_bytes=%" G_GUINT64_FORMAT
        " audio_packets_with_pts=%" G_GUINT64_FORMAT " audio_packet_first_pts=%.6f audio_packet_last_pts=%.6f"
        " audio_packet_first_at_ms=%" G_GUINT64_FORMAT " audio_packet_last_at_ms=%" G_GUINT64_FORMAT
        " last_warning_stage=%s last_warning_reason=%s last_warning_at_ms=%" G_GUINT64_FORMAT
        " packet_corrupt_warnings=%" G_GUINT64_FORMAT " pes_mismatch_warnings=%" G_GUINT64_FORMAT
        " demux_read_warnings=%" G_GUINT64_FORMAT,
        p.packet_diagnostics_enabled, p.packet_diagnostics_active, p.packet_diagnostics_complete,
        (guint64)p.packet_capture_started_at_ms, (guint64)p.packet_capture_ended_at_ms,
        (guint64)p.packet_log_rejected, (guint64)p.packet_capture_errors,
        (guint64)p.packet_video.packets, (guint64)p.packet_video.bytes, (guint64)p.packet_video.pts_packets,
        p.packet_video.first_pts, p.packet_video.last_pts,
        (guint64)p.packet_video.first_at_ms, (guint64)p.packet_video.last_at_ms,
        (guint64)p.packet_audio.packets, (guint64)p.packet_audio.bytes, (guint64)p.packet_audio.pts_packets,
        p.packet_audio.first_pts, p.packet_audio.last_pts,
        (guint64)p.packet_audio.first_at_ms, (guint64)p.packet_audio.last_at_ms,
        mpv_trace_label(p.last_warning_stage, "|video-decode|audio-decode|video-output|audio-output|demux|source|unclassified|unknown|"),
        mpv_trace_reason(p.last_warning_reason),
        (guint64)p.last_warning_at_ms, (guint64)p.packet_corrupt_warnings,
        (guint64)p.pes_mismatch_warnings, (guint64)p.demux_read_warnings);
}

static void trace_mpv_playback(const mpv_backend_snapshot_t &player, bool terminal_report = false) {
    static uint64_t generation = 0;
    static mpv_backend_state_t state = MPV_BACKEND_IDLE;
    static mpv_backend_error_t error = MPV_BACKEND_ERROR_NONE;
    static bool child_alive = false, exit_known = false;
    static gint64 last_log_at = 0;
    const gint64 now = g_get_monotonic_time();
    bool changed = generation != player.generation || state != player.state ||
        error != player.error_code || child_alive != player.child_alive || exit_known != player.child_exit_known;
    if (!terminal_report && !changed && (!player.child_alive || screen_info != SCREEN_INFO_DEBUG ||
        now - last_log_at < 2 * G_USEC_PER_SEC)) return;
    char text[6144];
    format_mpv_trace(player, text, sizeof(text), terminal_report);
    trace_playback_record("%s", text);
    if (terminal_report) return;
    generation = player.generation;
    state = player.state;
    error = player.error_code;
    child_alive = player.child_alive;
    exit_known = player.child_exit_known;
    last_log_at = now;
}

static void drain_mpv_terminal_reports() {
    mpv_backend_snapshot_t terminal;
    while (mpv_backend_take_terminal_snapshot(mpv_player, &terminal))
        trace_mpv_playback(terminal, true);
}

static screen_status_error_t mpv_screen_error(const mpv_backend_snapshot_t &player) {
    if (strstr(player.error, "timed out")) return SCREEN_ERROR_TIMEOUT;
    switch (player.error_code) {
    case MPV_BACKEND_ERROR_LOAD:
    case MPV_BACKEND_ERROR_NO_MEDIA:
    case MPV_BACKEND_ERROR_FORMAT: return SCREEN_ERROR_SOURCE;
    case MPV_BACKEND_ERROR_AUDIO_OUTPUT:
    case MPV_BACKEND_ERROR_VIDEO_OUTPUT: return SCREEN_ERROR_OUTPUT;
    default: return SCREEN_ERROR_BACKEND;
    }
}
#endif

static gboolean receiver_screen_tick(gpointer unused) {
    std::lock_guard<std::mutex> guard(display_mutex);
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player) {
        if (mpv_owns_output && !mpv_output_prepared && !mpv_rebuild_pending) {
            const gint64 release_started = g_get_monotonic_time();
            trace_playback_record("Playback session: session=%" G_GUINT64_FORMAT " event=output-release-begin monotonic_ms=%" G_GINT64_FORMAT,
                 (guint64)mpv_generation, (gint64)(release_started/1000));
            if (!hide_status_display() || !video_renderer_suspend_output()) {
                trace_playback_record("Playback session: session=%" G_GUINT64_FORMAT " event=output-release-failed monotonic_ms=%" G_GINT64_FORMAT " elapsed_ms=%" G_GINT64_FORMAT,
                     (guint64)mpv_generation, (gint64)(g_get_monotonic_time()/1000), (gint64)((g_get_monotonic_time()-release_started)/1000));
                screen_status_event(screen_generation(), SCREEN_EVENT_RECOVERY_REQUIRED);
                mpv_backend_stop(mpv_player, mpv_generation);
                /* Do not spawn the queued player if an output still owns DRM. */
                return TRUE;
            }
            if (use_audio) audio_renderer_stop();
            trace_playback_record("Playback session: session=%" G_GUINT64_FORMAT " event=output-release-complete monotonic_ms=%" G_GINT64_FORMAT " elapsed_ms=%" G_GINT64_FORMAT,
                 (guint64)mpv_generation, (gint64)(g_get_monotonic_time()/1000), (gint64)((g_get_monotonic_time()-release_started)/1000));
            mpv_output_prepared = true;
            playback_output_released = false;
        }
        mpv_backend_poll(mpv_player);
        drain_mpv_terminal_reports();
        mpv_backend_snapshot_t player;
        mpv_backend_snapshot(mpv_player, &player);
        /* Replacement can change the requested generation before the old
         * child exits. Persist that child's final evidence under its own ID. */
        if (player.generation) trace_mpv_playback(player);
        if (mpv_owns_output && player.generation == mpv_generation && !mpv_rebuild_pending) {
            const uint64_t generation = player.generation;
            screen_status_video_t video = {};
            g_strlcpy(video.backend, "mpv", sizeof(video.backend));
            g_strlcpy(video.codec, player.video_codec, sizeof(video.codec));
            g_strlcpy(video.decoder, player.video_decoder, sizeof(video.decoder));
            g_strlcpy(video.profile, player.video_profile, sizeof(video.profile));
            g_strlcpy(video.pixel_format, player.pixel_format, sizeof(video.pixel_format));
            /* Pixel format is not evidence of the decoder memory path. */
            const char *policy = mpv_policy == MPV_DECODE_SOFTWARE ? "software" :
                mpv_policy == MPV_DECODE_PI4_SAFE ? "pi4-safe" : "pi4-hevc-experimental";
            g_strlcpy(video.decode_policy, policy, sizeof(video.decode_policy));
            video.width = player.width > 0 ? player.width : 0;
            video.height = player.height > 0 ? player.height : 0;
            if (std::isfinite(player.fps) && player.fps > 0 && player.fps <= 1000) {
                video.fps_num = (unsigned)(player.fps * 1000); video.fps_den = 1000;
            }
            video.hardware_known = player.hwdec_current[0] != 0;
            video.hardware_active = video.hardware_known && strcmp(player.hwdec_current, "no") != 0;
            video.decoded_parameters_known = player.decoded_parameters_known;
            video.overlay_known = player.ready;
            video.overlay_supported = player.ready && strcmp(player.video_output, "null") != 0;
            video.dropped_known = player.dropped_frames >= 0;
            video.dropped_frames = video.dropped_known ? player.dropped_frames : 0;
            video.output_dropped_known = player.output_dropped_frames >= 0;
            video.output_dropped_frames = video.output_dropped_known ? player.output_dropped_frames : 0;
            screen_status_set_video(generation, &video);
            screen_status_snapshot_t previous;
            screen_status_get_snapshot(&previous);
            screen_status_audio_t audio = previous.audio;
            g_strlcpy(audio.codec, player.audio_codec, sizeof(audio.codec));
            g_strlcpy(audio.output, player.audio_output, sizeof(audio.output));
            audio.channels = player.audio_channels > 0 ? player.audio_channels : 0;
            audio.sample_rate = player.audio_samplerate > 0 ? player.audio_samplerate : 0;
            screen_status_set_audio(generation, &audio);
            screen_status_progress_t progress = {};
            progress.position_known = player.position_known;
            progress.position_seconds = player.position;
            progress.duration_known = player.duration_known;
            progress.duration_seconds = player.duration;
            progress.buffer_known = player.cache_buffering_known;
            progress.buffer_percent = player.cache_buffering_known ? (int)player.cache_buffering_percent : -1;
            progress.cache_seconds_known = player.cache_duration_known;
            progress.cache_seconds = player.cache_duration;
            progress.audio_position_known = player.audio_position_known;
            progress.audio_position = player.audio_position;
            progress.avsync_known = player.avsync_known;
            progress.avsync = player.avsync;
            progress.actual_paused_known = player.actual_paused_known;
            progress.actual_paused = player.actual_paused;
            progress.core_idle_known = player.core_idle_known;
            progress.core_idle = player.core_idle;
            progress.cache_eof_known = player.cache_eof_known;
            progress.cache_eof = player.cache_eof;
            progress.cache_underrun_known = player.cache_underrun_known;
            progress.cache_underrun = player.cache_underrun;
            progress.cache_idle_known = player.cache_idle_known;
            progress.cache_idle = player.cache_idle;
            progress.diagnostics_known = player.log_messages_active;
            g_strlcpy(progress.diagnostic_stage, player.diagnostic_stage, sizeof(progress.diagnostic_stage));
            progress.video_decode_errors = player.video_decode_errors;
            progress.audio_decode_errors = player.audio_decode_errors;
            progress.demux_errors = player.demux_errors;
            progress.network_errors = player.network_errors;
            progress.video_output_errors = player.video_output_errors;
            progress.audio_output_errors = player.audio_output_errors;
            progress.unclassified_errors = player.unclassified_errors;
            progress.log_warnings = player.log_warnings;
            progress.video_decode_warnings = player.video_decode_warnings;
            progress.missing_reference_warnings = player.missing_reference_warnings;
            progress.invalid_data_warnings = player.invalid_data_warnings;
            progress.timestamp_warnings = player.timestamp_warnings;
            progress.audio_output_warnings = player.audio_output_warnings;
            progress.unknown_warnings = player.unknown_warnings;
            progress.last_http_status_known = player.last_http_status_known;
            progress.last_http_status = player.last_http_status;
            g_strlcpy(progress.diagnostic_reason, player.diagnostic_reason, sizeof(progress.diagnostic_reason));
            g_strlcpy(progress.last_warning_stage, player.last_warning_stage, sizeof(progress.last_warning_stage));
            g_strlcpy(progress.last_warning_reason, player.last_warning_reason, sizeof(progress.last_warning_reason));
            progress.packet_capture_enabled = player.packet_diagnostics_enabled;
            progress.packet_capture_active = player.packet_diagnostics_active;
            progress.packet_capture_complete = player.packet_diagnostics_complete;
            progress.video_packets = player.packet_video.packets;
            progress.audio_packets = player.packet_audio.packets;
            screen_status_set_progress(generation, &progress);
            static mpv_progress_observer_t observed_progress;
            const gint64 now = g_get_monotonic_time();
            const bool progressed = observe_mpv_progress(observed_progress, player, now);
            if (previous.pause_requested && !player.requested_paused &&
                player.actual_paused_known && !player.actual_paused)
                screen_status_event(generation, SCREEN_EVENT_RESUMED);
            if (previous.state == SCREEN_STATE_SEEKING && !player.seeking)
                screen_status_event(generation, SCREEN_EVENT_SEEK_COMPLETE);
            switch (player.state) {
            case MPV_BACKEND_STARTING: case MPV_BACKEND_LOADING:
                screen_status_event(generation, SCREEN_EVENT_OPENING); break;
            case MPV_BACKEND_BUFFERING:
                screen_status_event(generation, SCREEN_EVENT_BUFFERING); break;
            case MPV_BACKEND_PAUSED:
                screen_status_event(generation, SCREEN_EVENT_PAUSED); break;
            case MPV_BACKEND_PLAYING: {
                if (player.actual_paused_known && player.actual_paused)
                    screen_status_event(generation, SCREEN_EVENT_PAUSED);
                else if (progressed)
                    screen_status_event(generation, SCREEN_EVENT_OUTPUT_PROGRESS);
                else if (!player.requested_paused &&
                         !(player.actual_paused_known && player.actual_paused) &&
                         now - (observed_progress.advanced_at ? observed_progress.advanced_at :
                                observed_progress.started_at) > 3 * G_USEC_PER_SEC)
                    screen_status_event(generation, SCREEN_EVENT_WAITING_DATA);
                break;
            }
            case MPV_BACKEND_STOPPING:
                screen_status_event(generation, SCREEN_EVENT_STOPPING); break;
            case MPV_BACKEND_FAILED:
                if (player.recovery_required) screen_status_event(generation, SCREEN_EVENT_RECOVERY_REQUIRED);
                else {
                    char detail[160];
                    if (player.file_error_code_known)
                        g_snprintf(detail, sizeof(detail), "%s - %s (mpv error %d)",
                                   player.error, player.file_error, player.file_error_code);
                    else g_strlcpy(detail, player.error, sizeof(detail));
                    screen_status_fail_detail(generation, mpv_screen_error(player), detail);
                }
                break;
            default: break;
            }
            /* Stop/error/EOF can only return ownership after waitpid confirms
             * exit. A fresh mirror shell is constructed by the existing loop. */
            if (!player.child_alive && (player.state == MPV_BACKEND_ENDED || player.state == MPV_BACKEND_FAILED || player.state == MPV_BACKEND_IDLE)) {
                mpv_rebuild_pending = true;
                close_window = true;
                preserve_connections = true;
                full_video_reset = true;
                relaunch_video = true;
                reset_loop = true;
            }
            static gint64 last_osd = 0;
            if (screen_info != SCREEN_INFO_OFF && player.child_alive &&
                g_get_monotonic_time() - last_osd >= 500000) {
                last_osd = g_get_monotonic_time();
                screen_status_snapshot_t snapshot;
                char text[2048];
                screen_status_get_snapshot(&snapshot);
                screen_status_format(&snapshot, text, sizeof(text), true);
                mpv_backend_set_osd(mpv_player, generation, text);
            }
        }
    }
#endif
    static gint64 last_refresh = 0;
    gint64 now = g_get_monotonic_time();
    if (now - last_refresh >= G_USEC_PER_SEC) {
        screen_status_set_readiness(receiver_has_network(), raop && raop_is_running(raop),
                                    receiver_registered, true, playback_output_released);
        screen_status_snapshot_t current;
        screen_status_get_snapshot(&current);
        if (use_audio && !mpv_owns_output && current.session_active &&
            (current.kind == SCREEN_SESSION_AUDIO_ONLY || current.kind == SCREEN_SESSION_MIRRORING)) {
            screen_status_audio_t observed = {};
            if (audio_renderer_get_screen_snapshot(&observed)) {
                bool progressed = observed.output_buffers > current.audio.output_buffers;
                screen_status_set_audio(current.generation, &observed);
                if (progressed && current.kind == SCREEN_SESSION_AUDIO_ONLY)
                    screen_status_event(current.generation, SCREEN_EVENT_OUTPUT_PROGRESS);
            }
        }
        if (!mpv_owns_output && use_video) video_renderer_screen_refresh();
        screen_status_get_snapshot(&current);
        if (!mpv_owns_output && use_video && current.state == SCREEN_STATE_FAILED && !playback_output_released) {
            close_window = true;
            preserve_connections = true;
            full_video_reset = true;
            relaunch_video = true;
            reset_loop = true;
            url.clear();
        }
        if (status_display) screen_status_renderer_tick(status_display);
        last_refresh = now;
    }
    return TRUE;
}

static gboolean video_eos_watch_callback (gpointer loop) {
    if (video_renderer_eos_watch()) {
        /* HLS video has sent EOS */
        LOGI("hls video has sent EOS");
        video_renderer_hls_ready();
        raop_handle_eos(raop);
    }
    return TRUE;
}

#define MAX_VIDEO_RENDERERS 3
#define MAX_AUDIO_RENDERERS 2
static void main_loop()  {
    guint gst_video_bus_watch_id[MAX_VIDEO_RENDERERS] = { 0 };
    guint gst_audio_bus_watch_id[MAX_AUDIO_RENDERERS] = { 0 };
    GMainLoop *loop = g_main_loop_new(NULL,FALSE);
    relaunch_video = false;
    monitor_progress = false;
    reset_loop = false;
    reset_httpd = false;
    preserve_connections = false;
    n_video_renderers = 0;
    n_audio_renderers = 0;
    if (use_video) {
        n_video_renderers = 1;
        relaunch_video = true;
        if (url.empty()) {
            if (h265_support) {
                n_video_renderers++;
            }
            if (render_coverart) {
                n_video_renderers++;
            }
            /* renderer[0] : h264 video; followed by  h265 video (optional) and jpeg (optional)  */
            gst_x11_window_id = 0;
	    video_eos_watch_id = 0;
        } else {
            /* hls video will be rendered: renderer[0] : hls  */
            url.erase();
            video_eos_watch_id = g_timeout_add(100, (GSourceFunc) video_eos_watch_callback, (gpointer) loop);
            gst_x11_window_id = g_timeout_add(100, (GSourceFunc) x11_window_callback, (gpointer) loop);
        }
        g_assert(n_video_renderers <= MAX_VIDEO_RENDERERS);
        for (int i = 0; i < n_video_renderers; i++) {
            gst_video_bus_watch_id[i] = (guint) video_renderer_listen((void *)loop, i);
        }
    }
    if (use_audio) {
        rtptime_start = 0;
        rtptime_end = 0;
        monitor_progress = true;
        artist.erase();
        coverart_artist.erase();
        progress_id  = g_timeout_add_seconds(1,(GSourceFunc) progress_callback, (gpointer) loop);
        n_audio_renderers = 2;
        g_assert(n_audio_renderers <= MAX_AUDIO_RENDERERS);
        for (int i = 0; i < n_audio_renderers; i++) {
            gst_audio_bus_watch_id[i] = (guint) audio_renderer_listen((void *)loop, i);      
        }
    }

    missed_feedback = 0;
    guint screen_watch_id = (screen_info != SCREEN_INFO_OFF || airplay_video_mpv) ?
        g_timeout_add(25, receiver_screen_tick, NULL) : 0;
    guint feedback_watch_id = g_timeout_add_seconds(1, (GSourceFunc) feedback_callback, (gpointer) loop);
    guint reset_watch_id = g_timeout_add(100, (GSourceFunc) reset_callback, (gpointer) loop);

#ifdef _WIN32
    gmainloop = loop;
#else 
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    guint sigterm_watch_id = g_unix_signal_add(SIGTERM, (GSourceFunc) sigterm_callback, (gpointer) loop);
    guint sigint_watch_id = g_unix_signal_add(SIGINT, (GSourceFunc) sigint_callback, (gpointer) loop);
    guint sighup_watch_id = g_unix_signal_add(SIGHUP, (GSourceFunc) sigint_callback, (gpointer) loop);
#endif
    g_main_loop_run(loop);

#ifdef _WIN32
    gmainloop = NULL;
#else
    signal(SIGINT, CtrlHandler);  //switch back to non-mainloop CtrlHandler
    signal(SIGTERM, CtrlHandler);
    signal(SIGHUP, CtrlHandler);
    if (sigint_watch_id > 0) g_source_remove(sigint_watch_id);
    if (sigterm_watch_id > 0) g_source_remove(sigterm_watch_id);
    if (sighup_watch_id > 0) g_source_remove(sighup_watch_id);
#endif

    for (int i = 0; i < n_video_renderers; i++) {
        if (gst_video_bus_watch_id[i] > 0) g_source_remove(gst_video_bus_watch_id[i]);
    }
    for (int i = 0; i < n_audio_renderers; i++) {
        if (gst_audio_bus_watch_id[i] > 0) g_source_remove(gst_audio_bus_watch_id[i]);
    }
    if (gst_x11_window_id > 0) g_source_remove(gst_x11_window_id);
    if (reset_watch_id > 0) g_source_remove(reset_watch_id);
    if (progress_id > 0) g_source_remove(progress_id);
    if (video_eos_watch_id > 0) g_source_remove(video_eos_watch_id);
    if (feedback_watch_id > 0) g_source_remove(feedback_watch_id);
    if (screen_watch_id) g_source_remove(screen_watch_id);
    g_main_loop_unref(loop);
}    

static int parse_hw_addr (std::string str, std::vector<char> &hw_addr) {
    for (int i = 0; i < (int) str.length(); i += 3) {
        hw_addr.push_back((char) stol(str.substr(i), NULL, 16));
    }
    return 0;
}

static const char *get_homedir() {
    const char *homedir = getenv("XDG_CONFIG_HOMEDIR");
    if (homedir == NULL) {
        homedir = getenv("HOME");
    }
#ifndef _WIN32
    if (homedir == NULL){
        homedir = getpwuid(getuid())->pw_dir;
    }
#endif
    return homedir;
}

static std::string find_uxplay_config_file() {
    std::string no_config_file = "";
    const char *homedir = NULL;
    const char *uxplayrc = NULL;
    std::string config0, config1, config2;
    struct stat sb;
    uxplayrc = getenv("UXPLAYRC");   /* first look for $UXPLAYRC */
    if (uxplayrc) {
        config0 = uxplayrc;
        if (stat(config0.c_str(), &sb) == 0) return config0;
    }
    homedir = get_homedir();
    if (homedir) {
      config1 = homedir;
      config1.append("/.uxplayrc");
      if (stat(config1.c_str(), &sb) == 0) return config1;  /* look for ~/.uxplayrc */
      config2 = homedir;
      config2.append("/.config/uxplayrc"); /* look for ~/.config/uxplayrc */
      if (stat(config2.c_str(), &sb) == 0) return config2;
    }
    return no_config_file;
}

static std::string find_mac () {
/*  finds the MAC address of a network interface *
 *  in a Windows, Linux, *BSD or macOS system.   */
    std::string mac = "";
    char str[3];
#ifdef _WIN32
    ULONG buflen = sizeof(IP_ADAPTER_ADDRESSES);
    PIP_ADAPTER_ADDRESSES addresses = (IP_ADAPTER_ADDRESSES*) malloc(buflen);
    if (addresses == NULL) { 					
        return mac;
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buflen) == ERROR_BUFFER_OVERFLOW) {
        free(addresses);
        addresses = (IP_ADAPTER_ADDRESSES*) malloc(buflen);
        if (addresses == NULL) {
            return mac;
        }
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buflen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES address = addresses; address != NULL; address = address->Next) {
            if (address->PhysicalAddressLength != 6                 /* MAC has 6 octets */
                || (address->IfType != 6 && address->IfType != 71)  /* Ethernet or Wireless interface */
                || address->OperStatus != 1) {                      /* interface is up */
                continue;
            }
            mac.erase();
            for (int i = 0; i < 6; i++) {
                snprintf(str, sizeof(str), "%02x", int(address->PhysicalAddress[i]));
                mac = mac + str;
                if (i < 5) mac = mac + ":";
            }
	    break;
        }
    }
    free(addresses);
    return mac;
#else
    struct ifaddrs *ifap, *ifaptr;
    int non_null_octets = 0;
    unsigned char octet[6];
    if (getifaddrs(&ifap) == 0) {
        for(ifaptr = ifap; ifaptr != NULL; ifaptr = ifaptr->ifa_next) {
            if(ifaptr->ifa_addr == NULL) continue;
#ifdef __linux__
            if (ifaptr->ifa_addr->sa_family != AF_PACKET) continue;
            struct sockaddr_ll *s = (struct sockaddr_ll*) ifaptr->ifa_addr;
            for (int i = 0; i < 6; i++) {
                if ((octet[i] = s->sll_addr[i]) != 0) non_null_octets++;
            }
#else    /* macOS and *BSD */
            if (ifaptr->ifa_addr->sa_family != AF_LINK) continue;
            unsigned char *ptr = (unsigned char *) LLADDR((struct sockaddr_dl *) ifaptr->ifa_addr);
            for (int i= 0; i < 6 ; i++) {
                if ((octet[i] = *ptr) != 0) non_null_octets++;
                ptr++;
            }
#endif
            if (non_null_octets) {
                mac.erase();
                for (int i = 0; i < 6 ; i++) {
                    snprintf(str, sizeof(str), "%02x", octet[i]);
                    mac = mac + str;
                    if (i < 5) mac = mac + ":";
                }
                break;
            }
        }
    }
    freeifaddrs(ifap);
#endif
    return mac;
}

static bool validate_mac(char * mac_address) {
    char c;
    if (strlen(mac_address) != 17)  return false;
    for (int i = 0; i < 17; i++) {
        c = *(mac_address + i);
        if (i % 3 == 2) {
            if (c != ':')  return false;
        } else {
            if (c < '0') return false;
            if (c > '9' && c < 'A') return false;
            if (c > 'F' && c < 'a') return false;
            if (c > 'f') return false;
        }
    }
    return true;
}

static std::string random_mac () {
    char str[4];
    unsigned char random[6];
    get_random_bytes(random, sizeof(random));
    /* mark MAC address as locally administered, i.e. random */
    random[0] = random[0] & ~0x01;
    random[0] = random[0] | 0x02;
    snprintf(str,3,"%2.2x", random[0]);
    std::string mac_address(str);
    for (int i = 1; i < 6; i++) {
        snprintf(str,4,":%2.2x", random[i]);
        mac_address = mac_address + str;
    }
    return mac_address;
}

static void print_info (char *name) {
    printf("UxPlay %s: An open-source AirPlay mirroring server.\n", VERSION);
    printf("=========== Website: https://github.com/FDH2/UxPlay ==========\n");
    printf("Usage: %s [-n name] [-s wxh] [-p [n]] [(other options)]\n", name);
    printf("Options:\n");
    printf("-n name   Specify network name of the AirPlay server (UTF-8/ascii)\n");
    printf("-nh       Do not add \"@hostname\" at the end of AirPlay server name\n");
    printf("-h265     Support h265 (4K) video (with h265 versions of h264 plugins)\n");
    printf("-mp4 [fn] Record (non-HLS)audio/video to mp4 file \"fn.[n].[format].mp4\"\n");
    printf("          n=1,2,.. format = H264/5, ALAC/AAC. Default fn=\"recording\"\n");
    printf("-airplay-video-backend gstreamer|mpv  Direct video player (default gstreamer)\n");
    printf("-screen-info off|status|debug  HDMI receiver status (default off)\n");
    printf("-mpv-decode software|pi4-safe|pi4-hevc-experimental  Default software\n");
    printf("-mpv-executable path  Optional mpv executable (build with UXPLAY_ENABLE_MPV)\n");
    printf("-mpv-vo name; -mpv-gpu-context name; -mpv-gpu-api name\n");
    printf("-mpv-drm-device path; -mpv-drm-connector name; -mpv-audio-device name\n");
    printf("-mpv-h264-hwdec name  Explicitly qualified Pi 4 H.264 hwdec for pi4-safe\n");
    printf("-mpv-render-profile default|fast  mpv rendering quality/cost (default default)\n");
    printf("-hls [v]  Support direct AirPlay video (HTTP/HLS): \n");
    printf("          v = 2 or 3 (default 3) optionally selects video player version\n");
    printf("-hls-pi4  Enable HLS; limit cached YouTube video to H.264/AAC-LC,\n");
    printf("          up to 1920x1080 at 60 fps; select vc4 for kmssink unless\n");
    printf("          a device is specified; use software HEVC to avoid Pi driver\n");
    printf("          shutdown hangs (H.264 hardware decoding remains available)\n");
    printf("-lang xx  HLS language preferences (\"fr:es:..\", overrides $LANGUAGE)\n");
    printf("-lang     (or -lang 0): play undubbed HLS version (overrides $LANGUAGE)\n");
    printf("-scrsv n  Screensaver override n: 0=off 1=on while displaying video 2=always on\n");
    printf("-pin[xxxx]Use a 4-digit pin code to control client access (default: no)\n");
    printf("          default pin is random: optionally use fixed pin xxxx\n");
    printf("-reg [fn] Keep a register in $HOME/.uxplay.register to verify returning\n");
    printf("          client pin-registration; (option: use file \"fn\" for this)\n");
    printf("-pw [pwd] Require use of password to control client access;\n");
    printf("          (with no pwd, pin entry is required at *each* connection.)\n");  
    printf("          (option \"-pw\" after \"-pin\" overrides it, and vice versa)\n");
    printf("-vsync [x]Mirror mode: sync audio to video using timestamps (default)\n");
    printf("          x is optional audio delay: millisecs, decimal, can be neg.\n");
    printf("-vsync no Switch off audio/(server)video timestamp synchronization \n");
    printf("-async [x]Audio-Only mode: sync audio to client video (default: no)\n");
    printf("-async no Switch off audio/(client)video timestamp synchronization\n");
    printf("-db l[:h] Set minimum volume attenuation to l dB (decibels, negative);\n");
    printf("          optional: set maximum to h dB (+ or -) default: -30.0:0.0 dB\n");
    printf("-taper    Use a \"tapered\" AirPlay volume-control profile\n");
    printf("-vol <v>  Set initial audio-streaming volume: range [mute=0.0:1.0=full]\n"); 
    printf("-s wxh[@r]Request to client for video display resolution [refresh_rate]\n"); 
    printf("          default 1920x1080[@60] (or 3840x2160[@60] with -h265 option)\n");
    printf("-o        Set display \"overscanned\" mode on (not usually needed)\n");
    printf("-fs       Full-screen (only with X11, Wayland, VAAPI, D3D11/12, kms)\n");
    printf("-p        Use legacy ports UDP 6000:6001:7011 TCP 7000:7001:7100\n");
    printf("-p n      Use TCP and UDP ports n,n+1,n+2. range %d-%d\n", LOWEST_ALLOWED_PORT, HIGHEST_PORT);
    printf("          use \"-p n1,n2,n3\" to set each port, \"n1,n2\" for n3 = n2+1\n");
    printf("          \"-p tcp n\" or \"-p udp n\" sets TCP or UDP ports separately\n");
    printf("-avdec    Force software h264 video decoding with libav decoder\n"); 
    printf("-vp ...   Choose the GSteamer h264 parser: default \"h264parse\"\n");
    printf("-vd ...   Choose the GStreamer h264 decoder; default \"decodebin\"\n");
    printf("          choices: (software) avdec_h264; (hardware) v4l2h264dec,\n");
    printf("          nvdec, nvh264dec, vaapih264dec, vtdec,etc.\n");
    printf("          choices: avdec_h264,vaapih264dec,nvdec,nvh264dec,v4l2h264dec\n");
    printf("-vc ...   Choose the GStreamer videoconverter; default \"videoconvert\"\n");
    printf("          another choice when using v4l2h264dec: v4l2convert\n");
    printf("-vs ...   Choose the GStreamer videosink; default \"autovideosink\"\n");
    printf("          some choices: ximagesink,xvimagesink,vaapisink,glimagesink,\n");
    printf("          gtksink,waylandsink,kmssink,fbdevsink,osxvideosink,\n");
    printf("          d3d11videosink,d3d12videosink, etc.\n");
    printf("-vs 0     Streamed audio only, with no video display window\n");
    printf("-vrtp pl  Use rtph26[4,5]pay to send decoded video elsewhere: \"pl\"\n");
    printf("          is the remaining pipeline, starting with rtph26*pay options:\n");
    printf("          e.g. \"config-interval=1 ! udpsink host=127.0.0.1 port=5000\"\n");
    printf("          Writes output to \"fn.N.mp4\"\n");
    printf("-v4l2     Use Video4Linux2 for GPU hardware h264 decoding\n");
    printf("-bt709    Sometimes needed for Raspberry Pi models using Video4Linux2 \n");
    printf("-srgb     Display \"Full range\" [0-255] color, not \"Limited Range\"[16-235]\n");
    printf("          This is a workaround for a GStreamer problem, until it is fixed\n");
    printf("-srgb no  Disable srgb option (use when enabled by default: Linux, *BSD)\n");
    printf("-as ...   Choose the GStreamer audiosink; default \"autoaudiosink\"\n");
    printf("          some choices:pulsesink,alsasink,pipewiresink,jackaudiosink,\n");
    printf("          osssink,oss4sink,osxaudiosink,wasapisink,directsoundsink.\n");
    printf("-as 0     (or -a)  Turn audio off, streamed video only\n");
    printf("-artp pl  Use rtpL16pay to send decoded audio elsewhere: \"pl\"\n");
    printf("          is the remaining pipeline, starting with rtpL16pay options:\n");
    printf("          e.g. \"pt=96 ! udpsink host=127.0.0.1 port=5002\"\n");
    printf("-al x     Audio latency in seconds (default 0.25) reported to client.\n");
    printf("-ca [<fn>]In Audio (ALAC) mode, render cover-art [or write to file <fn>]\n");
    printf("-md <fn>  In Airplay Audio (ALAC) mode, write metadata text to file <fn>\n");
    printf("-reset n  Reset after n seconds of client silence (default n=%d, 0=never)\n", MISSED_FEEDBACK_LIMIT);
    printf("-nofreeze Do NOT leave frozen screen in place after reset\n");
    printf("-nc       Do NOT  Close video window when client stops mirroring\n");
    printf("-nc no    Cancel the -nc option (DO close video window) \n");
    printf("-nohold   Drop current connection when new client connects.\n");
    printf("-restrict Restrict clients to those specified by \"-allow <deviceID>\"\n");
    printf("          UxPlay displays deviceID when a client attempts to connect\n");
    printf("          Use \"-restrict no\" for no client restrictions (default)\n");
    printf("-allow <i>Permit deviceID = <i> to connect if restrictions are imposed\n");
    printf("-block <i>Always block connections from deviceID = <i>\n");
    printf("-FPSdata  Show video-streaming performance reports sent by client.\n");
    printf("-fps n    Set maximum allowed streaming framerate, default 30\n");
    printf("-f {H|V|I}Horizontal|Vertical flip, or both=Inversion=rotate 180 deg\n");
    printf("-r {R|L}  Rotate 90 degrees Right (cw) or Left (ccw)\n");
    printf("-m [mac]  Set MAC address (also Device ID);use for concurrent UxPlays\n");
    printf("          if mac xx:xx:xx:xx:xx:xx is not given, a random MAC is used\n");
    printf("-key [fn] Store private key in $HOME/.uxplay.pem (or in file \"fn\")\n");
    printf("-dacp [fn]Export client DACP information to file $HOME/.uxplay.dacp\n");
    printf("          (option to use file \"fn\" instead); used for client remote\n");
    printf("-ble [fn] For BluetoothLE beacon: write data to file ~/.uxplay.ble\n");
    printf("          optional: write to file \"fn\" (\"fn\" = \"off\" to cancel)\n");
    printf("-d [n]    Enable debug logging; optional: n=1 to skip normal packet data\n");
    printf("-vdmp [n] Dump h264 video output to \"fn.h264\"; fn=\"videodump\",change\n");
    printf("          with \"-vdmp [n] filename\". If [n] is given, file fn.x.h264\n");
    printf("          x=1,2,.. opens whenever a new SPS/PPS NAL arrives, and <=n\n");
    printf("          NAL units are dumped.\n");
    printf("-admp [n] Dump audio output to \"fn.x.fmt\", fmt ={aac, alac, aud}, x\n");
    printf("          =1,2,..; fn=\"audiodump\"; change with \"-admp [n] filename\".\n");
    printf("          x increases when audio format changes. If n is given, <= n\n");
    printf("          audio packets are dumped. \"aud\"= unknown format.\n");
    printf("-v        Displays version information\n");
    printf("-h        Displays this help\n");
    printf("-rc fn    Read startup options from file \"fn\" instead of ~/.uxplayrc, etc\n");
    printf("Startup options in $UXPLAYRC, ~/.uxplayrc, or ~/.config/uxplayrc are\n");
    printf("applied first (command-line options may modify them): format is one \n");
    printf("option per line, no initial \"-\"; lines starting with \"#\" are ignored.\n");
}

static bool option_has_value(const int i, const int argc, std::string option, const char *next_arg) {
    if (i >= argc - 1 || next_arg[0] == '-') {
        LOGE("invalid: \"%s\" had no argument", option.c_str());
        return false;
     }
    return true;
}

static bool get_display_settings (std::string value, unsigned short *w, unsigned short *h, unsigned short *r) {
    // assume str  = wxh@r is valid if w and h are positive decimal integers
    // with no more than 4 digits, r < 256 (stored in one byte).
    char *end;
    std::size_t pos = value.find_first_of("x");
    if (pos == std::string::npos) return false;
    std::string str1 = value.substr(pos+1);
    value.erase(pos);
    if (value.length() == 0 || value.length() > 4 || value[0] == '-') return false;
    *w = (unsigned short) strtoul(value.c_str(), &end, 10);
    if (*end || *w == 0)  return false;
    pos = str1.find_first_of("@");
    if(pos != std::string::npos) {
        std::string str2 = str1.substr(pos+1);
        if (str2.length() == 0 || str2.length() > 3 || str2[0] == '-') return false;
        *r = (unsigned short) strtoul(str2.c_str(), &end, 10);
        if (*end || *r == 0 || *r > 255) return false;
        str1.erase(pos);
    }
    if (str1.length() == 0 || str1.length() > 4 || str1[0] == '-') return false;
    *h = (unsigned short) strtoul(str1.c_str(), &end, 10);
    if (*end || *h == 0) return false;
    return true;
}

static bool get_value (const char *str, unsigned int *n) {
    // if n > 0 str must be a positive decimal <= input value *n  
    // if n = 0, str must be a non-negative decimal
    if (strlen(str) == 0 || strlen(str) > 10 || str[0] == '-') return false;
    char *end;
    unsigned long l = strtoul(str, &end, 10);
    if (*end) return false;
    if (*n && (l == 0 || l > *n)) return false;
    *n = (unsigned int) l;
    return true;
}

static bool get_ports (int nports, std::string option, const char * value, unsigned short * const port) {
    /*valid entries are comma-separated values port_1,port_2,...,port_r, 0 < r <= nports */
    /*where ports are distinct, and are in the allowed range.                            */
    /*missed values are consecutive to last given value (at least one value needed).    */
    char *end;
    unsigned long l;
    std::size_t pos;
    std::string val(value), str;
    for (int i = 0; i <= nports ; i++)  {
        if(i == nports) break;
        pos = val.find_first_of(',');
        str = val.substr(0,pos);
        if(str.length() == 0 || str.length() > 5 || str[0] == '-') break;
        l = strtoul(str.c_str(), &end, 10);
        if (*end || l < LOWEST_ALLOWED_PORT || l > HIGHEST_PORT) break;
         *(port + i) = (unsigned short) l;
        for  (int j = 0; j < i ; j++) {
            if( *(port + j) == *(port + i)) break;
        }
        if(pos == std::string::npos) {
            if (nports + *(port + i) > i + 1 + HIGHEST_PORT) break;
            for (int j = i + 1; j < nports; j++) {
                *(port + j) = *(port + j - 1) + 1;
            }
            return true;
        }
        val.erase(0, pos+1);
    }
    LOGE("invalid \"%s %s\", all %d ports must be in range [%d,%d]",
         option.c_str(), value, nports, LOWEST_ALLOWED_PORT, HIGHEST_PORT);
    return false;
}

static bool get_videoflip (const char *str, videoflip_t *videoflip) {
    if (strlen(str) > 1) return false;
    switch (str[0]) {
    case 'I':
        *videoflip = INVERT;
        break;
    case 'H':
        *videoflip = HFLIP;
        break;
    case 'V':
        *videoflip = VFLIP;
        break;
    default:
        return false;
    }
    return true;
}

static bool get_videorotate (const char *str, videoflip_t *videoflip) {
    if (strlen(str) > 1) return false;
    switch (str[0]) {
    case 'L':
        *videoflip = LEFT;
        break;
    case 'R':
        *videoflip = RIGHT;
        break;
    default:
        return false;
    }
    return true;
}

static void append_hostname(std::string &server_name) {
#ifdef _WIN32   /*modification for compilation on Windows */
    char buffer[256] = "";
    unsigned long size = sizeof(buffer);
    if (GetComputerNameA(buffer, &size)) {
        std::string name = server_name;
        name.append("@");
        name.append(buffer);
        server_name = name;
    }
#else
    struct utsname buf;
    if (!uname(&buf)) {
        std::string name = server_name;
        name.append("@");
        name.append(buf.nodename);
        server_name = name;
    }
#endif
}

bool is_utf8(const char *string, bool *is_printable_ascii) {
    /* test if C-string is printable ascii or valid UTF-8 (max 4 bytes) */
    if (is_printable_ascii)  {
        *is_printable_ascii = true;
    }
    int len = (int) strlen(string);
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char) string[i];
        int n = 0;
        if (0x20 <= c && c <= 0x7e) {
            continue;   //printable ascii, no control characters.
        } else if (is_printable_ascii) {
            *is_printable_ascii = false;
        }
        if (0x00 <= c && c <= 0x7f) {
            continue; //one byte code, 0bbbbbbb
        } else if (c == 0xc0 || c == 0xc1) {
             return false;  //two byte code, invalid start byte
        } else if ((c & 0xe0) == 0xc0) {
            n = 1; //two byte code, 110bbbbb
        } else if (c == 0xe0 && i < len - 1 && (unsigned char) string[i + 1] < 0xa0) {
            return false;  //three byte code, overlong encoding
        } else if (c == 0xed && i < len - 1 && (unsigned char) string[i + 1] > 0x9f) {
            return false;  //three byte code, exclude U+dc00 to U+dfff
        } else if ((c & 0xf0) == 0xe0) {
            n = 2; //three byte code 1110bbbb
        } else if (c >= 0xf5) {
            return false;  //four byte code, invalid start byte
        } else if (c == 0xf0 && i < len - 1 && (unsigned char) string[i + 1] < 0x90) {
            return false;  //four byte code, overlong encoding
        } else if (c == 0xf4 && i < len - 1 && (unsigned char) string[i + 1] > 0x8f) {
            return false;  //four byte code, out of range character (> U+10ffff)
        } else if ((c & 0xf8) == 0xf0) {
            n = 3; //four byte code, 11110bbb
        } else {
            return false;  //more than 4 bytes
        }
        for (int j = 0; j < n && i < len ; j++) { // n bytes matching 10bbbbbb must follow ?
            if ((++i == len) || (((unsigned char) string[i] & 0xc0) != 0x80)) {
                return false;
            }
        }
	
    }
    return true;
}

static void parse_arguments (int argc, char *argv[]) {
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (!is_utf8(argv[i], NULL)) {
            fprintf(stderr,"Error: detected a non-ascii or non-UTF-8 string \"%s\""
                    "while parsing input arguments", argv[i]);
            exit(0);
        }
    }
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "-rc") {
            i++;  //specifies startup file: has already been processed
        } else if (arg == "-allow") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            i++;
            allowed_clients.push_back(argv[i]);
        } else if (arg == "-block") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            i++;
            blocked_clients.push_back(argv[i]);    
        } else if (arg == "-restrict") {
            if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    restrict_clients = false;
                    i++;
                    continue;
                }
	    } 
            restrict_clients = true;
        } else if (arg == "-n") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            bool ascii;
            server_name_is_utf8 = false;
            server_name.erase();
            bool utf8 = is_utf8(argv[++i], &ascii);
            if (!utf8) {
                fprintf(stderr, "invalid (non-UTF-8/ascii) server name in \"-n %s\"", argv[i]);
                exit(1);
            }
            server_name = std::string(argv[i]);
            if (!ascii) {
                server_name_is_utf8 = true;
                printf("WARNING: a non-ascii (UTF-8) server-name \"%s\" was specified:"
                       " ensure correct locale settings to display it\n",server_name.c_str()); 
            }
        } else if (arg == "-nh") {
            do_append_hostname = false;
        } else if (arg == "-async") {
            audio_sync = true;
	    if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    audio_sync = false;
                    i++;
                    continue;
		}
                char *end;
                int n = (int) (strtof(argv[i + 1], &end) * 1000);
                if (*end == '\0') {
                    i++;
                    if (n > -SECOND_IN_USECS && n < SECOND_IN_USECS) {
                        audio_delay_alac = n * 1000; /* units are nsecs */
                    } else {
                        fprintf(stderr, "invalid -async %s: requested delays must be smaller than +/- 1000 millisecs\n", argv[i] );
                        exit (1);
                    }
                }
            }
        } else if (arg == "-scrsv") {
            if (!option_has_value(i, argc, argv[i], argv[i+1])) exit(1);
            unsigned int n = 0;
            if (!get_value(argv[++i], &n) || n > 2) {
                fprintf(stderr, "invalid \"-scrsv %s\"; values 0, 1, 2 allowed\n", argv[i]);
                exit(1);
            }
#ifdef DBUS
            scrsv = n;
#else
            fprintf(stderr,"invalid: option \"-scrsv\" is currently only implemented for Linux/*BSD systems with D-Bus service\n");
            exit(1);
#endif
        } else if (arg == "-vsync") {
            video_sync = true;
	    if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    video_sync = false;
                    i++;
                    continue;
                }
                char *end;
                int n = (int) (strtof(argv[i + 1], &end) * 1000);
                if (*end == '\0') {
                    i++;
                    if (n > -SECOND_IN_USECS && n < SECOND_IN_USECS) {
                        audio_delay_aac = n * 1000;     /* units are nsecs */
                    } else {
                        fprintf(stderr, "invalid -vsync %s: requested delays must be smaller than +/- 1000 millisecs\n", argv[i]);
                        exit (1);
                    }
                }
            }
        } else if (arg == "-s") {
            if (!option_has_value(i, argc, argv[i], argv[i+1])) exit(1);
            std::string value(argv[++i]);
            if (!get_display_settings(value, &display[0], &display[1], &display[2])) {
                fprintf(stderr, "invalid \"-s %s\"; -s wxh : max w,h=9999; -s wxh@r : max r=255\n",
                        argv[i]);
                exit(1);
            }
        } else if (arg == "-fps") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            unsigned int n = 255;
            if (!get_value(argv[++i], &n)) {
                fprintf(stderr, "invalid \"-fps %s\"; -fps n : max n=255, default n=30\n", argv[i]);
                exit(1);
            }
            display[3] = (unsigned short) n;
        } else if (arg == "-o") {
            display[4] = 1;
        } else if (arg == "-f") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            if (!get_videoflip(argv[++i], &videoflip[0])) {
                fprintf(stderr,"invalid \"-f %s\" , unknown flip type, choices are H, V, I\n",argv[i]);
                exit(1);
            }
        } else if (arg == "-r") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            if (!get_videorotate(argv[++i], &videoflip[1])) {
                fprintf(stderr,"invalid \"-r %s\" , unknown rotation  type, choices are R, L\n",argv[i]);
                exit(1);
            }
        } else if (arg == "-p") {
            if (i == argc - 1 || argv[i + 1][0] == '-') {
                tcp[0] = 7100; tcp[1] = 7000; tcp[2] = 7001;
                udp[0] = 7011; udp[1] = 6001; udp[2] = 6000;
                continue;
            }
            std::string value(argv[++i]);
            if (value == "tcp") {
                arg.append(" tcp");
                if(!get_ports(3, arg, argv[++i], tcp)) exit(1);
            } else if (value == "udp") {
                arg.append( " udp");
                if(!get_ports(3, arg, argv[++i], udp)) exit(1);
            } else {
                if(!get_ports(3, arg, argv[i], tcp)) exit(1);
                for (int j = 0; j < 3; j++) {
                    udp[j] = tcp[j];
                }
            }
        } else if (arg == "-m") {
            if (i < argc - 1 && *argv[i+1] != '-') {
                if (validate_mac(argv[++i])) {
                    mac_address.erase();
                    mac_address = argv[i];
                    use_random_hw_addr = false;
                } else {
                    fprintf(stderr,"invalid mac address \"%s\": address must have form"
                            " \"xx:xx:xx:xx:xx:xx\", x = 0-9, A-F or a-f\n", argv[i]);
                    exit(1);
                }
            } else {
                use_random_hw_addr  = true;
            }
        } else if (arg == "-a") {
            use_audio = false;
        } else if (arg == "-d") {
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 1;
                if (!get_value(argv[++i], &n)) {
                    fprintf(stderr, "invalid \"-d %s\"; -d n : max n=1 (suppress packet data in debug output)\n", argv[i]);
                    exit(1);
                }
                debug_log = true;
                suppress_packet_debug_data = true;
            } else {
                debug_log = !debug_log;
                suppress_packet_debug_data = false;
            }
        } else if (arg == "-h"  || arg == "--help" || arg == "-?" || arg == "-help") {
            print_info(argv[0]);
            exit(0);
        } else if (arg == "-v") {
            printf("UxPlay version %s; for help, use option \"-h\"\n", VERSION);
            exit(0);
        } else if (arg == "-vp") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            video_parser.erase();
            video_parser.append(argv[++i]);
        } else if (arg == "-vd") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            video_decoder.erase();
            video_decoder.append(argv[++i]);
        } else if (arg == "-vc") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            video_converter.erase();
            video_converter.append(argv[++i]);
        } else if (arg == "-vs") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            videosink.erase();
            videosink.append(argv[++i]);
            std::size_t pos = videosink.find(" ");
            if (pos != std::string::npos) {
                videosink_options.erase();
                videosink_options = videosink.substr(pos);
                videosink.erase(pos);
            }
        } else if (arg == "-as") {
            if (!option_has_value(i, argc, arg, argv[i+1])) exit(1);
            audiosink.erase();
            audiosink.append(argv[++i]);
        } else if (arg == "-t") {
            fprintf(stderr,"The uxplay option \"-t\" has been removed: it was a workaround for an  Avahi issue.\n");
            fprintf(stderr,"The correct solution is to open network port UDP 5353 in the firewall for mDNS queries\n");
            exit(1);
        } else if (arg == "-nc") {
            new_window_closing_behavior = false;
            if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    new_window_closing_behavior = true;
                    i++;
                    continue;
                }
            }
        } else if (arg == "-avdec") {
            video_parser.erase();
            video_parser = "h264parse";
            video_decoder.erase();
            video_decoder = "avdec_h264";
            video_converter.erase();
            video_converter = "videoconvert";
        } else if (arg == "-v4l2") {
            video_decoder.erase();
            video_decoder = "v4l2h264dec";
            video_converter.erase();
            video_converter = "v4l2convert";
        } else if (arg == "-rpi" || arg == "-rpifb" || arg == "-rpigl" || arg == "-rpiwl") {
            fprintf(stderr,"*** -rpi* options do not apply to Raspberry Pi model 5, and have been removed\n");
            fprintf(stderr,"     For models 3 and 4, use their equivalents, if needed:\n");
            fprintf(stderr,"     -rpi   was equivalent to \"-v4l2\"\n");
            fprintf(stderr,"     -rpifb was equivalent to \"-v4l2 -vs kmssink\"\n");
            fprintf(stderr,"     -rpigl was equivalent to \"-v4l2 -vs glimagesink\"\n");
            fprintf(stderr,"     -rpiwl was equivalent to \"-v4l2 -vs waylandsink\"\n");
            fprintf(stderr,"     Option \"-bt709\" may also be needed for R Pi model 4B and earlier\n");
            exit(1);
        } else if (arg == "-fs" ) {
            fullscreen = true;
        } else if (arg == "-FPSdata") {
            show_client_FPS_data = true;
        } else if (arg == "-reset") {
            /* now using feedback  (every 1 sec ) instead of ntp timeouts (every 3 secs) to detect offline client and reset connections */
            fprintf(stderr,"*** NOTE CHANGE: -reset n now means reset n seconds (not 3n seconds) after client goes offline\n");	  
            missed_feedback_limit = 0;
            if (!get_value(argv[++i], &missed_feedback_limit)) {
                fprintf(stderr, "invalid \"-reset %s\"; -reset n must have n >= 0,  default n = %d seconds\n", argv[i], MISSED_FEEDBACK_LIMIT);
                exit(1);
            }
	} else if (arg == "-vrtp") {
	  if (!option_has_value(i, argc, arg, argv[i+1])) {
	    fprintf(stderr,"option \"-vrtp\" must be followed by a pipeline for sending the video stream:\n"
		    "e.g., \"<rtph26[4,5]pay options> ! udpsink host=127.0.0.1 port -= 5000\"\n");
	    exit(1);
          }
	  rtp_pipeline.erase();
	  rtp_pipeline.append(argv[++i]);
	} else if (arg == "-artp") {
	  if (!option_has_value(i, argc, arg, argv[i+1])) {
	    fprintf(stderr,"option \"-artp\" must be followed by a pipeline for sending the audio stream:\n"
		    "e.g., \"<rtpL16pay options> ! udpsink host=127.0.0.1 port=5002\"\n");
	    exit(1);
          }
	  audio_rtp_pipeline.erase();
	  audio_rtp_pipeline.append(argv[++i]);
	} else if (arg == "-vdmp") {
            dump_video = true;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 0;
                if (get_value (argv[++i], &n)) {
                    if (n == 0) {
                        fprintf(stderr, "invalid \"-vdmp 0 %s\"; -vdmp n  needs a non-zero value of n\n", argv[i]);
                        exit(1);
                    }
                    video_dump_limit = n;
                    if (option_has_value(i, argc, arg, argv[i+1])) {
                        video_dumpfile_name.erase();
                        video_dumpfile_name.append(argv[++i]);
                    }
                } else {
                    video_dumpfile_name.erase();
                    video_dumpfile_name.append(argv[i]);
                }
                const char *fn = video_dumpfile_name.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-vdmp <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   		
            }
        } else if (arg == "-mp4"){
            mux_to_file = true;
            if (i < argc - 1 && *argv[i+1] != '-') {
                mux_filename.erase();
                mux_filename.append(argv[++i]);
                const char *fn = mux_filename.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-mp4 <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }
            }
        } else if (arg == "-admp") {
            dump_audio = true;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 0;
                if (get_value (argv[++i], &n)) {
                    if (n == 0) {
                        fprintf(stderr, "invalid \"-admp 0 %s\"; -admp n  needs a non-zero value of n\n", argv[i]);
                        exit(1);
                    }
                    audio_dump_limit = n;
                    if (option_has_value(i, argc, arg, argv[i+1])) {
                        audio_dumpfile_name.erase();
                        audio_dumpfile_name.append(argv[++i]);
                    }
                } else {
                    audio_dumpfile_name.erase();
                    audio_dumpfile_name.append(argv[i]);
                }
                const char *fn = audio_dumpfile_name.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-admp <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }
            }
        } else if (arg  == "-ca" ) {
            if (i < argc - 1 && *argv[i+1] != '-') {
                coverart_filename.erase();
                coverart_filename.append(argv[++i]);
                const char *fn = coverart_filename.c_str();
                render_coverart = false;
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-ca <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   
            } else {
                render_coverart = true;
            }
        } else if (arg  == "-md" ) {
            if (option_has_value(i, argc, arg, argv[i+1])) {
                metadata_filename.erase();
                metadata_filename.append(argv[++i]);
                const char *fn = metadata_filename.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-md <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   
            } else {
                fprintf(stderr,"option -md must be followed by a filename for metadata text output\n");
                exit(1);
            }
        } else if (arg  == "-ble" ) {
            ble_filename.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                i++;
                if (strlen(argv[i]) != 3 || strncmp(argv[i], "off", 3)) { 
                    ble_filename.append(argv[i]);
                    if (!file_has_write_access(argv[i])) {
                        fprintf(stderr, "%s cannot be written to:\noption \"-ble<fn>\" must be to a file with write access\n", argv[i]);
                        exit(1);
                    }
                }
            } else {
                static const char* homedir = get_homedir();
                if (homedir) {
                    ble_filename = homedir;
                    ble_filename.append("/.uxplay.ble");
                    if (!file_has_write_access(ble_filename.c_str())) {
                        fprintf(stderr, "%s cannot be written to\n",ble_filename.c_str()) ;
                        exit(1);
                    }
                } else {
                    fprintf(stderr,"failed to obtain home directory\n");
                    exit(1);
                }
            }
        } else if (arg == "-bt709") {
            bt709_fix = true;
        } else if (arg == "-srgb") {
            srgb_fix = true;
	    if (i <  argc - 1) {
                if (strlen(argv[i+1]) == 2 && strncmp(argv[i+1], "no", 2) == 0) {
                    srgb_fix = false;
                    i++;
                    continue;
                }
            }
        } else if (arg == "-nohold") {
            nohold = 1;
        } else if (arg == "-al") {
	    int n;
            char *end;
            if (i < argc - 1 && *argv[i+1] != '-') {
                n = (int) (strtof(argv[++i], &end) * SECOND_IN_USECS);
                if (*end == '\0' && n >=0 && n <= 10 * SECOND_IN_USECS) {
                    audiodelay = n;
                    continue;
                }
            }
            fprintf(stderr, "invalid -al %s: value must be a decimal time offset in seconds, range [0,10]\n"
                    "(like 5 or 4.8, which will be converted to a whole number of microseconds)\n", argv[i]);
            exit(1);
        } else if (arg == "-pin") {
            setup_legacy_pairing = true;
            pin_pw = 1;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 9999;
                if (!get_value(argv[++i], &n)) {
                    fprintf(stderr, "invalid \"-pin %s\"; -pin nnnn : max nnnn=9999, (4 digits)\n", argv[i]);
                    exit(1);
                }
                pin = n + 10000;
            }
	} else if (arg == "-reg") {
            registration_list = true;
            pairing_register.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                pairing_register.append(argv[++i]);
                const char * fn = pairing_register.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-reg <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   
            }
        } else if (arg == "-key") {
            keyfile.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                keyfile.append(argv[++i]);
                const char * fn = keyfile.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-key <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   
            } else {
	        //                fprintf(stderr, "option \"-key <fn>\" requires a path <fn> to a file for persistent key storage\n");
	        // exit(1);
                keyfile.erase();
                keyfile.append("0");
            }
        } else if (arg == "-pw") {
            setup_legacy_pairing = false;
            if (i < argc - 1 && *argv[i+1] != '-') {
                password.erase();
                password.append(argv[++i]);
                pin_pw = 2;
                if (password.size() < min_password_length) {
                    fprintf(stderr, "invalid client-access password \"%s\": length must be at least %u characters\n", password.c_str(), min_password_length);
                    exit(1);
                }
            } else {
                pin_pw = 3;  //a random password (pin) will be displayed at each connection
            }
        } else if (arg == "-dacp") {
            dacpfile.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                dacpfile.append(argv[++i]);
                const char *fn = dacpfile.c_str();
                if (!file_has_write_access(fn)) {
                    fprintf(stderr, "%s cannot be written to:\noption \"-dacp <fn>\" must be to a file with write access\n", fn);
                    exit(1);
                }   
            } else {
                dacpfile.append(get_homedir());
                dacpfile.append("/.uxplay.dacp");
            }
        } else if (arg == "-taper") {
            taper_volume = true;
        } else if (arg == "-db") {
            bool db_bad = true;
            double db1, db2;
            if ( i < argc -1) {
                char *end1, *end2;
                db1 = strtod(argv[i+1], &end1);
                if (*end1 == ':') {
                    db2 = strtod(++end1, &end2);
                    if ( *end2 == '\0' && end2 > end1  && db1 < 0 && db1 < db2) {
                        db_bad = false;
                    }
                } else  if (*end1 =='\0' && db1 < 0 ) {
                    db_bad = false;
                    db2 = 0.0;
                }
            }
            if (db_bad) {
                fprintf(stderr, "invalid \"-db  %s\": db value must be \"low\" or \"low:high\", low < 0 and high > low are decibel gains\n", argv[i+1]); 
                exit(1);
            }
            i++;
            db_low = db1;
            db_high = db2;
            printf("db range %f:%f\n", db_low, db_high);
        } else if (arg ==  "-vol") {
            bool vol_bad = true;
            if (i < argc - 1) {
                char *end;
                double frac = strtod(argv[i+1], &end);
                if (*end == '\0' && frac >= 0.0 && frac <= 1.0) {
                    if (frac == 0.0) {
                        initial_volume = -144.0;
                    } else if (frac == 1.0) {
                        initial_volume = 0.0;
                    } else {
                        double db_flat = -30.0  + 30.0*frac;
                        //double db = 10.0 * (log10(frac) / log10(2.0));  //tapered 
                        //printf("db %f db_flat %f \n", db, db_flat);
                        //db = (db > db_flat) ? db : db_flat;
                        initial_volume = db_flat;
                    }
                }
                printf("initial_volume attenuation %f db\n", initial_volume);
                vol_bad = false;
            }
            if (vol_bad) {
                fprintf(stderr, "invalid \"-vol %s\", value must be between 0.0 (mute) and 1.0 (full volume)\n", argv[i+1]);
                exit(1);
            }
            i++;
        } else if (arg == "-airplay-video-backend" || arg == "-screen-info" || arg.compare(0, 5, "-mpv-") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                fprintf(stderr, "%s requires a value\n", arg.c_str()); exit(1);
            }
            std::string value = argv[++i];
            if (arg == "-screen-info") {
                if (!screen_status_parse_mode(value.c_str(), &screen_info)) {
                    fprintf(stderr, "screen-info must be off, status or debug\n"); exit(1);
                }
            } else if (arg == "-airplay-video-backend") {
                if (value != "mpv" && value != "gstreamer") {
                    fprintf(stderr, "airplay-video-backend must be gstreamer or mpv\n"); exit(1);
                }
                airplay_video_mpv = value == "mpv";
#ifndef UXPLAY_HAVE_MPV
                if (airplay_video_mpv) { fprintf(stderr, "mpv backend not built; configure with -DUXPLAY_ENABLE_MPV=ON\n"); exit(1); }
#endif
                hls_support = true;
            } else {
#ifdef UXPLAY_HAVE_MPV
                if (arg == "-mpv-decode") {
                    if (value == "software") mpv_policy = MPV_DECODE_SOFTWARE;
                    else if (value == "pi4-safe") mpv_policy = MPV_DECODE_PI4_SAFE;
                    else if (value == "pi4-hevc-experimental") mpv_policy = MPV_DECODE_PI4_HEVC_EXPERIMENTAL;
                    else { fprintf(stderr, "invalid mpv-decode policy\n"); exit(1); }
                } else if (arg == "-mpv-render-profile") {
                    if (value != "default" && value != "fast") {
                        fprintf(stderr, "mpv-render-profile must be default or fast\n"); exit(1);
                    }
                    mpv_fast_rendering = value == "fast";
                } else if (arg == "-mpv-executable") mpv_executable = value;
                else if (arg == "-mpv-vo") mpv_vo = value;
                else if (arg == "-mpv-gpu-context") mpv_context = value;
                else if (arg == "-mpv-gpu-api") mpv_api = value;
                else if (arg == "-mpv-drm-device") mpv_drm_device = value;
                else if (arg == "-mpv-drm-connector") mpv_connector = value;
                else if (arg == "-mpv-audio-device") mpv_audio_device = value;
                else if (arg == "-mpv-h264-hwdec") mpv_h264_hwdec = value;
                else { fprintf(stderr, "unknown mpv option %s\n", arg.c_str()); exit(1); }
#else
                fprintf(stderr, "mpv options require -DUXPLAY_ENABLE_MPV=ON\n"); exit(1);
#endif
            }
        } else if (arg == "-hls") {
            hls_support = true;
            if (i < argc - 1 && *argv[i+1] != '-') {
                unsigned int n = 3;
                if (!get_value(argv[++i], &n) || playbin_version < 2) {
                    fprintf(stderr, "invalid \"-hls %s\"; -hls n only allows \"playbin\" video player versions 2 or 3\n", argv[i]);
                    exit(1);
                }
                playbin_version = (guint) n;
            }
        } else if (arg == "-hls-pi4") {
            hls_support = true;
            hls_pi4 = true;
        } else if (arg == "-lang") {
            lang.erase();
            if (i < argc - 1 && *argv[i+1] != '-') {
                lang = argv[++i];
            }
        } else if (arg == "-h265") {
            h265_support = true;
        } else if (arg == "-nofreeze") {
            nofreeze = true;
        } else {
            fprintf(stderr, "unknown option %s, stopping (for help use option \"-h\")\n",argv[i]);
            exit(1);
        }
    }
}

static void process_metadata(int count, const char *dmap_tag, const unsigned char* metadata, int datalen, std::string *metadata_text) {
    int dmap_type = 0;
    /* DMAP metadata items can be strings (dmap_type = 9); other types are byte, short, int, long, date, and list.  *
     * The DMAP item begins with a 4-character (4-letter) "dmap_tag" string that identifies the type.               */

    if (debug_log) {
        printf("%d: dmap_tag [%s], %d\n", count, dmap_tag, datalen);
    }

    /* UTF-8 String-type DMAP tags seen in Apple Music Radio are processed here.   *
     * (DMAP tags "asal", "asar", "ascp", "asgn", "minm" ). TODO expand this */  
    
    if (datalen == 0) {
        return;
    }

    if (dmap_tag[0] == 'a' && dmap_tag[1] == 's') {
        dmap_type = 9;
        switch (dmap_tag[2]) {
        case 'a':
            switch (dmap_tag[3]) {
            case 'a':
                metadata_text->append("Album artist: ");  /*asaa*/
                break;
            case 'l':
                metadata_text->append("Album: ");  /*asal*/
                if (render_coverart) {
                    track_album.erase();
                    track_album.append(metadata, metadata + datalen);
                }
                break;
            case 'r':
                metadata_text->append("Artist: ");  /*asar*/
                if (render_coverart) {
                    artist.erase();
                    artist.append(metadata, metadata + datalen);
                    if (coverart_artist == "_pending_") { 
                        coverart_artist = artist;
                    }
                    if (coverart_artist != "_expired_" && coverart_artist != artist) {
                        coverart_artist = "_expired_";
                        rtptime_coverart_expired = rtptime;
                    }
                }  
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;    
        case 'c':
            switch (dmap_tag[3]) {
            case 'm':
                metadata_text->append("Comment: ");  /*ascm*/
                break;
            case 'n':
                metadata_text->append("Content description: ");  /*ascn*/
                break;
            case 'p':
                metadata_text->append("Composer: ");  /*ascp*/
                break;
            case 't':
                metadata_text->append("Category: ");  /*asct*/
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;
        case 's':
            switch (dmap_tag[3]) {
            case 'a':
                metadata_text->append("Sort Artist: "); /*assa*/
                break;
            case 'c':
                metadata_text->append("Sort Composer: ");  /*assc*/
                break;
            case 'l':
                metadata_text->append("Sort Album artist: ");  /*assl*/
                break;
            case 'n':
                metadata_text->append("Sort Name: ");  /*assn*/
                break;
            case 's':
                metadata_text->append("Sort Series: ");  /*asss*/
                break;
            case 'u':
                metadata_text->append("Sort Album: ");  /*assu*/
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;
        default:
	    if (strcmp(dmap_tag, "asdt") == 0) {
                metadata_text->append("Description: ");
            } else if (strcmp (dmap_tag, "asfm") == 0) {
                metadata_text->append("Format: ");
            } else if (strcmp (dmap_tag, "asgn") == 0) {
                metadata_text->append("Genre: ");
            } else if (strcmp (dmap_tag, "asky") == 0) {
                metadata_text->append("Keywords: ");
            } else if (strcmp (dmap_tag, "aslc") == 0) {
                metadata_text->append("Long Content Description: ");
            } else {
                dmap_type = 0;
            }
            break;
        }
    } else if (strcmp (dmap_tag, "minm") == 0) {
        dmap_type = 9;
        metadata_text->append("Title: ");
        if (render_coverart) {
            track_title.erase();
            track_title.append(metadata, metadata + datalen);
        }
    }

    if (dmap_type == 9) {
        char *str = (char *) calloc(datalen + 1, sizeof(char));
        if (!str) {
            printf("Memeory allocation failure (str)\n");
            exit(1);
        }
        memcpy(str, metadata, datalen);
        metadata_text->append(str);
        metadata_text->append("\n");
        free(str);
    } else if (debug_log) {
        std::string md = "";
        char hex[4];
        for (int i = 0; i < datalen; i++) {
            if (i > 0 && i % 16 == 0) {
                md.append("\n");
            }
            snprintf(hex, 4, "%2.2x ", (int) metadata[i]);
            md.append(hex);
        }
        LOGI("%s", md.c_str());
    }
}

static int parse_dmap_header(const unsigned char *metadata, char *tag, int *len) {
    const unsigned char *header = metadata;

    bool istag = true;
    for (int i = 0; i < 4; i++) {
        tag[i] =  (char) *header;
        if (!isalpha(tag[i])) {
            istag = false;
        }
        header++;
    }

    *len = 0;
    for (int i = 0; i < 4; i++) {
        *len <<= 8;
        *len += (int) *header;
        header++;
    }
    if (!istag || *len < 0) {
        return 1;
    }
    return 0;
}

static int register_dnssd() {
    int dnssd_error;
    uint64_t features;
    
    dnssd_error = dnssd_register_raop(dnssd, raop_port);
    if (dnssd_error) {
        if (ble_filename.empty()) {
            if (dnssd_error == -65537) {
                LOGE("No DNS-SD Server found (DNSServiceRegister call returned kDNSServiceErr_Unknown)");
            } else if (dnssd_error == -65548) {
                LOGE("DNSServiceRegister call returned kDNSServiceErr_NameConflict");
                LOGI("Is another instance of %s running with the same DeviceID (MAC address) or using same network ports?",
                     DEFAULT_NAME);
                LOGI("Use options -m ... and -p ... to allow multiple instances of %s to run concurrently", DEFAULT_NAME); 
            } else {
                LOGE("dnssd_register_raop failed with error code %d\n"
                     "mDNS Error codes are in range FFFE FF00 (-65792) to FFFE FFFF (-65537) "
                     "(see Apple's dns_sd.h)", dnssd_error);
            }
            return -3;
        } else {
            LOGI("dnssd_register_raop failed: ignoring because Bluetooth LE service discovery may be available");
        }
    }

    dnssd_error = dnssd_register_airplay(dnssd, airplay_port);
    if (dnssd_error) {
        if (ble_filename.empty()) {
            LOGE("dnssd_register_airplay failed with error code %d\n"
                 "mDNS Error codes are in range FFFE FF00 (-65792) to FFFE FFFF (-65537) "
                 "(see Apple's dns_sd.h)", dnssd_error);
            return -4;
        } else {
            LOGI("dnssd_register_airplay failed: ignoring because Bluetooth LE service discovery may be available");   
        }
    }

    LOGD("register_dnssd: advertised AirPlay service with \"Features\" code = 0x%llX",
         dnssd_get_airplay_features(dnssd));
    return 0;
}

static void unregister_dnssd() {
    if (dnssd) {
        dnssd_unregister_raop(dnssd);
        dnssd_unregister_airplay(dnssd);
    }
    return;
}

static void stop_dnssd() {
    if (dnssd) {
        unregister_dnssd();
        dnssd_destroy(dnssd);
        dnssd = NULL;
	return;
    }	
}

static int start_dnssd(std::vector<char> hw_addr, std::string name) {
    int dnssd_error;
    if (dnssd) {
        LOGE("start_dnssd error: dnssd != NULL");
        return 2;
    }
    /* pin_pw controls client access
      pin_pw  = 1: client must enter pin displayed onscreen (first access only)
              = 2: client must enter password (same password for all clients)
              = 3: client must enter randoe 4-digit password displayed like an  onscreen pin (every access)
              = 0:  no access control
    */
    dnssd = dnssd_init(name.c_str(), strlen(name.c_str()), hw_addr.data(), hw_addr.size(), &dnssd_error, pin_pw);
    if (dnssd_error) {
        LOGE("Could not initialize dnssd library!: error %d", dnssd_error);
        return 1;
    }

    /* after dnssd starts, reset the default feature set here 
     * (overwrites features set in dnssdint.h)
     * default: FEATURES_1 = 0x5A7FFEE6, FEATURES_2 = 0 */

    dnssd_set_airplay_features(dnssd,  0, 0); // AirPlay video supported 
    dnssd_set_airplay_features(dnssd,  1, 1); // photo supported 
    dnssd_set_airplay_features(dnssd,  2, 1); // video protected with FairPlay DRM 
    dnssd_set_airplay_features(dnssd,  3, 0); // volume control supported for videos

    dnssd_set_airplay_features(dnssd,  4, 0); // http live streaming (HLS) supported
    dnssd_set_airplay_features(dnssd,  5, 1); // slideshow supported 
    dnssd_set_airplay_features(dnssd,  6, 1); // 
    dnssd_set_airplay_features(dnssd,  7, 1); // mirroring supported

    dnssd_set_airplay_features(dnssd,  8, 0); // screen rotation  supported 
    dnssd_set_airplay_features(dnssd,  9, 1); // audio supported 
    dnssd_set_airplay_features(dnssd, 10, 1); //  
    dnssd_set_airplay_features(dnssd, 11, 1); // audio packet redundancy supported

    dnssd_set_airplay_features(dnssd, 12, 1); // FaiPlay secure auth supported 
    dnssd_set_airplay_features(dnssd, 13, 1); // photo preloading  supported 
    dnssd_set_airplay_features(dnssd, 14, 1); // Authentication bit 4:  FairPlay authentication
    dnssd_set_airplay_features(dnssd, 15, 1); // Metadata bit 1 support:   Artwork 

    dnssd_set_airplay_features(dnssd, 16, 1); // Metadata bit 2 support:  Soundtrack  Progress 
    dnssd_set_airplay_features(dnssd, 17, 1); // Metadata bit 0 support:  Text (DAACP) "Now Playing" info.
    dnssd_set_airplay_features(dnssd, 18, 1); // Audio format 1 support:   
    dnssd_set_airplay_features(dnssd, 19, 1); // Audio format 2 support: must be set for AirPlay 2 multiroom audio 

    dnssd_set_airplay_features(dnssd, 20, 1); // Audio format 3 support: must be set for AirPlay 2 multiroom audio 
    dnssd_set_airplay_features(dnssd, 21, 1); // Audio format 4 support:
    dnssd_set_airplay_features(dnssd, 22, 1); // Authentication type 4: FairPlay authentication
    dnssd_set_airplay_features(dnssd, 23, 0); // Authentication type 1: RSA Authentication

    dnssd_set_airplay_features(dnssd, 24, 0); // 
    dnssd_set_airplay_features(dnssd, 25, 1); // 
    dnssd_set_airplay_features(dnssd, 26, 0); // Has Unified Advertiser info
    dnssd_set_airplay_features(dnssd, 27, 1); // Supports Legacy Pairing

    dnssd_set_airplay_features(dnssd, 28, 1); //  
    dnssd_set_airplay_features(dnssd, 29, 0); // 
    dnssd_set_airplay_features(dnssd, 30, 1); // RAOP support: with this bit set, the AirTunes service is not required. 
    dnssd_set_airplay_features(dnssd, 31, 0); // 


    /*  bits 32-63: see  https://emanualcozzi.net/docs/airplay2/features 
    dnssd_set_airplay_features(dnssd, 32, 0); // isCarPlay when ON,; Supports InitialVolume when OFF
    dnssd_set_airplay_features(dnssd, 33, 0); // Supports Air Play Video Play Queue
    dnssd_set_airplay_features(dnssd, 34, 0); // Supports Air Play from cloud (requires that bit 6 is ON)
    dnssd_set_airplay_features(dnssd, 35, 0); // Supports TLS_PSK

    dnssd_set_airplay_features(dnssd, 36, 0); //
    dnssd_set_airplay_features(dnssd, 37, 0); //
    dnssd_set_airplay_features(dnssd, 38, 0); //  Supports Unified Media Control (CoreUtils Pairing and Encryption)
    dnssd_set_airplay_features(dnssd, 39, 0); //

    dnssd_set_airplay_features(dnssd, 40, 0); // Supports Buffered Audio
    dnssd_set_airplay_features(dnssd, 41, 0); // Supports PTP
    dnssd_set_airplay_features(dnssd, 42, 0); // Supports Screen Multi Codec (allows h265 video)
    dnssd_set_airplay_features(dnssd, 43, 0); // Supports System Pairing

    dnssd_set_airplay_features(dnssd, 44, 0); // is AP Valeria Screen Sender
    dnssd_set_airplay_features(dnssd, 45, 0); //
    dnssd_set_airplay_features(dnssd, 46, 0); // Supports HomeKit Pairing and Access Control
    dnssd_set_airplay_features(dnssd, 47, 0); //

    dnssd_set_airplay_features(dnssd, 48, 0); // Supports CoreUtils Pairing and Encryption
    dnssd_set_airplay_features(dnssd, 49, 0); //
    dnssd_set_airplay_features(dnssd, 50, 0); // Metadata bit 3: "Now Playing" info sent by bplist not DAACP test
    dnssd_set_airplay_features(dnssd, 51, 0); // Supports Unified Pair Setup and MFi Authentication

    dnssd_set_airplay_features(dnssd, 52, 0); // Supports Set Peers Extended Message
    dnssd_set_airplay_features(dnssd, 53, 0); //
    dnssd_set_airplay_features(dnssd, 54, 0); // Supports AP Sync
    dnssd_set_airplay_features(dnssd, 55, 0); // Supports WoL

    dnssd_set_airplay_features(dnssd, 56, 0); // Supports Wol
    dnssd_set_airplay_features(dnssd, 57, 0); //
    dnssd_set_airplay_features(dnssd, 58, 0); // Supports Hangdog Remote Control
    dnssd_set_airplay_features(dnssd, 59, 0); // Supports AudioStreamConnection setup

    dnssd_set_airplay_features(dnssd, 60, 0); // Supports Audo Media Data Control         
    dnssd_set_airplay_features(dnssd, 61, 0); // Supports RFC2198 redundancy
    */

    /* needed for HLS video support */
    dnssd_set_airplay_features(dnssd, 0, (int) hls_support);
    dnssd_set_airplay_features(dnssd, 4, (int) hls_support);
    // not sure about this one (bit 8, screen rotation supported):
    //dnssd_set_airplay_features(dnssd, 8, (int) hls_support);
    
    /* needed for h265 video support */
    dnssd_set_airplay_features(dnssd, 42, (int) h265_support);

    /* bit 27 of Features determines whether the AirPlay2 client-pairing protocol will be used (1) or not (0) */
    dnssd_set_airplay_features(dnssd, 27, (int) setup_legacy_pairing);
    return 0;
}

static bool check_client(char *deviceid) {
    bool ret = false;
    int list =  allowed_clients.size();
    for (int i = 0; i < list ; i++) {
        if (!strcmp(deviceid,allowed_clients[i].c_str())) {
	    ret = true;
	    break;
        }
    }
    return ret;
}

static bool check_blocked_client(char *deviceid) {
    bool ret = false;
    int list =  blocked_clients.size();
    for (int i = 0; i < list ; i++) {
        if (!strcmp(deviceid,blocked_clients[i].c_str())) {
	    ret = true;
	    break;
        }
    }
    return ret;
}

// Server callbacks


//to be simplified

extern "C" void video_reset(void *cls, reset_type_t type) {
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player && mpv_owns_output) {
        if (type == RESET_TYPE_RTP_TO_HLS_TEARDOWN || type == RESET_TYPE_RTP_SHUTDOWN) return;
        /* Callback only requests stop. The loop releases/reaps before it
         * rebuilds GStreamer or presents the idle screen. */
        screen_status_event(screen_generation(), SCREEN_EVENT_STOPPING);
        mpv_backend_stop(mpv_player, mpv_generation);
        return;
    }
#endif
    if (type != RESET_TYPE_ON_VIDEO_PLAY) {
        screen_status_event(screen_generation(), SCREEN_EVENT_STOPPING);
    }
    std::lock_guard<std::mutex> guard(display_mutex);
    if (mpv_owns_output) return; /* A replacement won the lock after the check above. */
    if (!hide_status_display()) return;
    switch (type) {
    case RESET_TYPE_NOHOLD:
        LOGD("video_reset: type = NoHold");
        if (hls_support) {
	    url.erase();
            raop_destroy_airplay_video(raop, -1);
        }
    case RESET_TYPE_HLS_EOS:
        LOGD("video_reset: type= HLS_eos");
        if (use_video) {
            video_renderer_stop();
           /* reset the video renderer immediately to avoid a timing issue if we wait for main_loop to reset */ 
            video_renderer_destroy();
            video_renderer_init(render_logger, server_name.c_str(), videoflip, video_parser.c_str(), rtp_pipeline.c_str(),
                                video_decoder.c_str(), video_converter.c_str(), videosink.c_str(),
                                videosink_options.c_str(), fullscreen, video_sync, h265_support,
                                render_coverart, playbin_version, NULL);
            video_renderer_start();
            close_window = false;  // we already closed the window
        }
        preserve_connections = false; //we already closed all other connections
        remote_clock_offset = 0;
        relaunch_video = true;
        break;
    case RESET_TYPE_RTP_TO_HLS_TEARDOWN:
        LOGD("video_reset: type = RTP_to_HLS_Shutdown");
        preserve_connections = true;
    case RESET_TYPE_RTP_SHUTDOWN:
        LOGD("video_reset: type = RTP_Shutdown");      
        if (use_video) {
            video_renderer_stop();
        }
        remote_clock_offset = 0;
        relaunch_video = true;
        break;
    case RESET_TYPE_HLS_SHUTDOWN:
        LOGD("video_reset: type = HLS_Shutdown");
        if (use_video) {
            video_renderer_stop();
        }
        if (hls_support) {
            url.erase();
            raop_destroy_airplay_video(raop, -1);
        }
        raop_remove_hls_connections(raop);
        preserve_connections = true;
        remote_clock_offset = 0;
        relaunch_video = true;
        break;
    case RESET_TYPE_ON_VIDEO_PLAY:
        LOGD("video_reset: type = on_video_play");      
        break;
    default:
        g_assert(FALSE);
        break;
    }
    if ((screen_info != SCREEN_INFO_OFF || airplay_video_mpv) && type != RESET_TYPE_ON_VIDEO_PLAY)
        full_video_reset = true; /* The idle presenter also needs the post-loop release path. */
    reset_loop = true;
}

extern "C" int video_set_codec(void *cls, video_codec_t codec) {
    bool video_is_h265 = (codec == VIDEO_CODEC_H265);
    if (mux_to_file) {
        mux_renderer_choose_video_codec(video_is_h265);
    }
    if (!use_video) {
        return 0;
    }
    if (!wait_for_mpv_release()) return -1;
    std::lock_guard<std::mutex> guard(display_mutex);
    if (mpv_owns_output) return -1; /* A newer direct request won the handover. */
    if (!hide_status_display()) return -1;
    screen_status_snapshot_t current;
    screen_status_get_snapshot(&current);
    if (current.kind != SCREEN_SESSION_MIRRORING || !current.session_active) {
        screen_status_begin_session(SCREEN_SESSION_MIRRORING, SCREEN_ROUTE_RTP, "AirPlay");
        screen_set_backend(false);
    }
    screen_status_event(screen_generation(), SCREEN_EVENT_OPENING);
    playback_output_released = false;
    playback_generation = screen_generation();
    int result = video_renderer_choose_codec(false, video_is_h265);
    if (result) screen_status_fail(screen_generation(), SCREEN_ERROR_DECODER);
    return result;
}

extern "C" void display_pin(void *cls, char *pin) {
    int margin = 10;
    int spacing = 3;
    char *image = create_pin_display(pin, margin, spacing);
    if (!image) {
        LOGE("create_pin_display could not create pin image, pin = %s", pin);
    } else {
        LOGI("%s\n",image);     
        free (image);
    }
}

extern "C" const char *passwd(void *cls, int *len){
    if (pin_pw == 2) {
        *len = password.size();
        return password.c_str();
    } else if  (pin_pw == 3) {
        *len = -1;
    } else {
        *len = 0;   /* no password used */
    }
    return NULL;
}

extern "C" void export_dacp(void *cls, const char *active_remote, const char *dacp_id) {
      if (dacpfile.length()) {
        FILE *fp = fopen(dacpfile.c_str(), "w");
        if (fp) {
            fprintf(fp,"%s\n%s\n", dacp_id, active_remote);
            fclose(fp);
        } else {
            LOGE("failed to open DACP export file \"%s\"", dacpfile.c_str());
        }
    }
}

extern "C" void conn_init (void *cls) {
    open_connections++;
    LOGD("Open connections: %i", open_connections);
    //video_renderer_update_background(1);
}

extern "C" void conn_destroy (void *cls) {
    //video_renderer_update_background(-1);
    open_connections--;
    LOGD("Open connections: %i", open_connections);
    if (open_connections == 0) {
        screen_status_snapshot_t screen;
        screen_status_get_snapshot(&screen);
        if (screen.kind == SCREEN_SESSION_AUDIO_ONLY)
            screen_status_event(screen.generation, SCREEN_EVENT_STOPPED);
        remote_clock_offset = 0;
        if (use_audio) {
            audio_renderer_stop();
        }
        if (dacpfile.length()) {
            remove (dacpfile.c_str());
        }
        if (mux_to_file) {
            mux_renderer_stop();
        }
    }
}

extern "C" void conn_feedback (void *cls) {
    /* received client heartbeat signal: connection still exists */
    missed_feedback = 0;
}

extern "C" void conn_reset (void *cls, int reason) {
    switch (reason) {
    case 1:
        LOGI("*** ERROR lost connection with client (network problem?)");
	break;
    case 2:
        LOGI("*** ERROR Unsupported HLS streaming source: (exit attempt to stream)");
	break;      
    default:
      break;
    }
    
    if (!nofreeze) {
        close_window = false;    /* leave "frozen" window open */
    }
    reset_httpd = true;
    relaunch_video = true;
    reset_loop = true;
}

extern "C" void report_client_request(void *cls, char *deviceid, char * model, char *name, bool * admit) {
    LOGI("connection request from %s (%s) with deviceID = %s\n", name, model, deviceid);
    if (restrict_clients) {
        *admit = check_client(deviceid);
        if (*admit == false) {
            LOGI("client connections have been restricted to those with listed deviceID,\nuse \"-allow %s\" to allow this client to connect.\n",
                 deviceid);
        }
    } else {
        *admit = true;
    }
    if (check_blocked_client(deviceid)) {
        *admit = false;
        LOGI("*** attempt to connect by blocked client (clientID %s): DENIED\n", deviceid);
    }
    // Pass device model to renderer for device frame display
    if (*admit && use_video) {
        video_renderer_set_device_model(model, name);
    }
}

extern "C" void audio_process (void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    if (mpv_owns_output) return;
    if (dump_audio) {
        dump_audio_to_file(data->data, data->data_len, (data->data)[0] & 0xf0);
    }
    if (mux_to_file) {
        mux_renderer_push_audio(data->data, data->data_len, data->ntp_time_remote);
    }
    if (use_audio) {
        if (!remote_clock_offset) {
            uint64_t local_time = (data->ntp_time_local ? data->ntp_time_local : get_local_time());
            remote_clock_offset = local_time - data->ntp_time_remote;
        }
        data->ntp_time_remote = data->ntp_time_remote + remote_clock_offset;
        switch (data->ct) {
        case 2:
            /* for progress monitor (ALAC audio only) */
            rtptime = data->rtp_time;
            if (audio_delay_alac) {
                data->ntp_time_remote = (uint64_t) ((int64_t) data->ntp_time_remote + audio_delay_alac);
            }
            break;
        case 4:
        case 8:
            monitor_progress =  false;
            if (audio_delay_aac) {
                data->ntp_time_remote = (uint64_t) ((int64_t) data->ntp_time_remote + audio_delay_aac);
            }
            break;
        default:
            break;
        }
        audio_renderer_render_buffer(data->data, &(data->data_len), &(data->seqnum), &(data->ntp_time_remote));
    }
}

extern "C" void video_process (void *cls, raop_ntp_t *ntp, video_decode_struct *data) {
    if (mpv_owns_output) return;
    if (dump_video) {
        dump_video_to_file(data->data, data->data_len);
    }
    if (mux_to_file) {
        mux_renderer_push_video(data->data, data->data_len, data->ntp_time_remote);
    }
    if (use_video) {
        std::lock_guard<std::mutex> guard(display_mutex);
        if (mpv_owns_output) return;
        if (!remote_clock_offset) {
            uint64_t local_time = (data->ntp_time_local ? data->ntp_time_local : get_local_time());
            remote_clock_offset = local_time - data->ntp_time_remote;
        }
        int count = 0;
        uint64_t pts_mismatch = 0;
        do {
            data->ntp_time_remote = data->ntp_time_remote + remote_clock_offset;
            pts_mismatch = video_renderer_render_buffer(data->data, &(data->data_len), &(data->nal_count), &(data->ntp_time_remote));
            if (pts_mismatch) {
                LOGI("adjust timestamps by %8.6f secs", (double) pts_mismatch / SECOND_IN_NSECS);
                remote_clock_offset += pts_mismatch;
            }
            count++;
        } while (pts_mismatch && count < 10);
    }
}

#ifdef DBUS
extern "C" void mirror_video_running  (void *cls, bool is_running) {
    if (scrsv != 1) {
        return;
    }
    dbus_screensaver_inhibiter(is_running);
}
#endif

extern "C" void video_pause (void *cls) {
    if (use_video) {
        video_renderer_pause();
    }
}

extern "C" void video_resume (void *cls) {
    if (use_video) {
        video_renderer_resume();
    }
}


extern "C" void audio_flush (void *cls) {
    if (use_audio) {
        audio_renderer_flush();
    }
}

extern "C" void video_flush (void *cls) {
    if (use_video) {
        video_renderer_flush();
    }
}

extern "C" double audio_set_client_volume(void *cls) {
    return initial_volume;
}

extern "C" void audio_set_volume (void *cls, float volume) {
    double db, db_flat, frac, gst_volume;
    if (!use_audio) {
      return;
    }
    /* convert from AirPlay dB  volume in range {-30dB : 0dB}, to GStreamer volume */
    if (volume == -144.0f) {   /* AirPlay "mute" signal */
        frac = 0.0;
    } else if (volume < -30.0f) {
        LOGE(" invalid AirPlay volume %f", volume);
        frac = 0.0;
    } else if (volume > 0.0f) {
        LOGE(" invalid AirPlay volume %f", volume);
        frac = 1.0;
    } else if (volume == -30.0f) {
        frac = 0.0;
    } else if (volume == 0.0f) {
        frac = 1.0;
    } else {
        frac = (double) ( (30.0f + volume) / 30.0f);
        frac = (frac > 1.0) ? 1.0 : frac;
    }

    /* frac is length of volume slider as fraction of max length */
    /* also (steps/16) where steps is number of discrete steps above mute (16 = full volume) */
    if (frac == 0.0) {
        gst_volume = 0.0;
    } else {
      /* flat rescaling of decibel range from {-30dB : 0dB} to {db_low : db_high} */  
        db_flat = db_low + (db_high-db_low) * frac;
        if (taper_volume) {
            /* taper the volume reduction by the (rescaled) Airplay {-30:0} range so each reduction of
             * the remaining slider length by 50% reduces the perceived volume by 50% (-10dB gain)
             * (This is the "dasl-tapering" scheme offered by shairport-sync) */
            db = db_high + 10.0 * (log10(frac) / log10(2.0));
            db = (db  > db_flat) ? db : db_flat;
        } else {
            db = db_flat;
        }
        /* conversion from (gain) decibels to GStreamer's linear volume scale */
        gst_volume = pow(10.0, 0.05*db);
    }
    audio_renderer_set_volume(gst_volume);
    video_renderer_hls_set_volume(gst_volume);
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player) {
        mpv_backend_snapshot_t player;
        mpv_backend_snapshot(mpv_player, &player);
        mpv_backend_set_volume(mpv_player, player.generation, std::min(100.0, gst_volume * 100), volume == -144.0f);
    }
#endif
    screen_status_snapshot_t snapshot;
    screen_status_get_snapshot(&snapshot);
    snapshot.audio.volume_known = true;
    snapshot.audio.volume = gst_volume;
    snapshot.audio.muted = volume == -144.0f;
    screen_status_set_audio(snapshot.generation, &snapshot.audio);
}

extern "C" void audio_get_format (void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen, bool *isMedia, uint64_t *audioFormat) {
    if (!wait_for_mpv_release()) return;
    std::lock_guard<std::mutex> guard(display_mutex);
    if (mpv_owns_output) return;
    screen_status_snapshot_t screen;
    screen_status_get_snapshot(&screen);
    if (!*usingScreen && (screen.kind != SCREEN_SESSION_AUDIO_ONLY || !screen.session_active)) {
        screen_status_begin_session(SCREEN_SESSION_AUDIO_ONLY, SCREEN_ROUTE_RTP, "AirPlay");
        screen_set_backend(false);
        screen_status_event(screen_generation(), SCREEN_EVENT_WAITING_DATA);
    }
    unsigned char type;
    LOGI("RAOP audio: stage=setup ct=%d spf=%d usingScreen=%d isMedia=%d audioFormat=0x%lx",
         *ct, *spf, *usingScreen, *isMedia, (unsigned long) *audioFormat);
    switch (*ct) {
    case 2:
        type = 0x20;
        break;
    case 8:
        type = 0x80;
        break;
    default:
        type = 0x10;
        break;
    }
    if (audio_dumpfile && type != audio_type) {
        fclose(audio_dumpfile);
        audio_dumpfile = NULL;
    }
    audio_type = type;
    
    if (use_audio) {
      audio_renderer_start(ct);
    }

    if (mux_to_file) {
        mux_renderer_choose_audio_codec(*ct);
    }

    if (coverart_filename.length()) {
        write_coverart(coverart_filename.c_str(), (const void *) empty_image, sizeof(empty_image));
    }
    if (metadata_filename.length()) {
        write_metadata(metadata_filename.c_str(), "no data\n");
    }
}

extern "C" void video_report_size(void *cls, float *width_source, float *height_source, float *width, float *height) {
    if (use_video) {
        video_renderer_size(width_source, height_source, width, height);
    }
}

extern "C" void audio_set_coverart(void *cls, const void *buffer, int buflen) {
    if (buffer && coverart_filename.length()) {
        write_coverart(coverart_filename.c_str(), buffer, buflen);
        LOGI("coverart size %d written to %s", buflen,  coverart_filename.c_str());
    } else if (buffer && render_coverart && !mpv_owns_output) {
        std::lock_guard<std::mutex> guard(display_mutex);
        if (mpv_owns_output) return;
        if (!hide_status_display()) return;
        playback_output_released = false;
        video_renderer_choose_codec(true, false);  /* video_is_jpeg = true */
        video_renderer_display_jpeg(buffer, &buflen);
        coverart_artist = "_pending_";
    }
}

extern "C" void audio_stop_coverart_rendering(void *cls) {
    if (render_coverart) {
        video_reset(cls, RESET_TYPE_RTP_SHUTDOWN);
    }
}

extern "C" void audio_set_progress(void *cls, uint32_t *start, uint32_t *curr, uint32_t *end) {
    rtptime_start = *start;
    rtptime = *curr;
    rtptime_end = *end;
    display_progress(rtptime_start, rtptime, rtptime_end);
}

extern "C" void audio_set_metadata(void *cls, const void *buffer, int buflen) {
    char dmap_tag[5] = {0x0};
    const unsigned char *metadata = (const  unsigned char *) buffer;
    int datalen;
    int count = 0;

    printf("====================Audio Metadata==================\n");

    if (buflen < 8) {
        LOGE("received invalid metadata, length %d < 8", buflen);
        return;
    } else if (parse_dmap_header(metadata, dmap_tag, &datalen)) {
        LOGE("received invalid metadata, tag [%s]  datalen %d", dmap_tag, datalen);
        return;
    }
    metadata += 8;
    buflen -= 8;

    if (strcmp(dmap_tag, "mlit") != 0 || datalen != buflen) {
        LOGE("received metadata with tag %s, but is not a DMAP listingitem, or datalen = %d !=  buflen %d",
             dmap_tag, datalen, buflen);
        return;
    }
    std::string metadata_text = "";
    while (buflen >= 8) {
        count++;
        if (parse_dmap_header(metadata, dmap_tag, &datalen)) {
            LOGE("received metadata with invalid DMAP header:  tag = [%s],  datalen = %d", dmap_tag, datalen);
            return;
        }
        metadata += 8;
        buflen -= 8;
        process_metadata(count, (const char *) dmap_tag, metadata, datalen, &metadata_text);
        metadata += datalen;
        buflen -= datalen;
    }
    LOGI("%s", metadata_text.c_str());
    if (metadata_filename.length()) {
        write_metadata(metadata_filename.c_str(), metadata_text.c_str());
    }
    if (buflen != 0) {
        LOGE("%d bytes of metadata were not processed", buflen);
    }
    // Update video renderer with track metadata for cover art display
    if (render_coverart) {
        video_renderer_set_track_metadata(
            track_title.length() ? track_title.c_str() : NULL,
            artist.length() ? artist.c_str() : NULL,
            track_album.length() ? track_album.c_str() : NULL);
    }
}

extern "C" void register_client(void *cls, const char *device_id, const char *client_pk, const char *client_name) {
    if (!registration_list) {
      /* we are not maintaining a list of registered clients */
        return;
    }
    LOGI("registered new client: %s DeviceID = %s PK = \n%s", client_name, device_id, client_pk);
    registered_keys.push_back(client_pk);
    if (strlen(pairing_register.c_str())) {
        FILE *fp = fopen(pairing_register.c_str(), "a");
        if (fp) {
            fprintf(fp, "%s,%s,%s\n", client_pk, device_id, client_name);
            fclose(fp);
        }
    }
}

extern "C" bool check_register(void *cls, const char *client_pk) {
    if (!registration_list) {
        /* we are not maintaining a list of registered clients */
        return true;
    }
    LOGD("check returning client's pairing registration");
    std::string pk = client_pk;
    if (std::find(registered_keys.rbegin(), registered_keys.rend(), pk) != registered_keys.rend()) {
        LOGD("registration found: PK=%s", client_pk);
        return true;
    } else {
        LOGE("returning client's pairing registration not found: PK=%s", client_pk);
        return false;
    }
}
/* control  callbacks for video player (unimplemented) */

extern "C" void on_video_request(void *cls, bool direct_http) {
    const uint64_t generation = screen_status_begin_session(SCREEN_SESSION_DIRECT_VIDEO,
        direct_http ? SCREEN_ROUTE_DIRECT_HTTP : SCREEN_ROUTE_PLAYLIST_CACHE, "AirPlay");
    screen_set_backend(true);
    screen_status_event(generation, SCREEN_EVENT_PREPARING);
    trace_playback_record("Playback session: session=%" G_GUINT64_FORMAT " event=request route=%s monotonic_ms=%" G_GINT64_FORMAT,
         (guint64)generation, direct_http ? "direct-http" : "playlist-cache", (gint64)(g_get_monotonic_time()/1000));
}

extern "C" void on_video_request_error(void *cls) {
    const uint64_t generation = screen_generation();
    trace_playback_record("Playback session: session=%" G_GUINT64_FORMAT " event=source-failed monotonic_ms=%" G_GINT64_FORMAT,
         (guint64)generation, (gint64)(g_get_monotonic_time()/1000));
    screen_status_fail(generation, SCREEN_ERROR_SOURCE);
#ifdef UXPLAY_HAVE_MPV
    const uint64_t player_generation = mpv_generation;
    if (mpv_player && mpv_owns_output) mpv_backend_stop(mpv_player, player_generation);
#endif
}

extern "C" void on_video_play(void *cls, const char* location, const float start_position, bool direct_http) {
    screen_status_snapshot_t current;
    screen_status_get_snapshot(&current);
    if (!current.session_active || current.kind != SCREEN_SESSION_DIRECT_VIDEO ||
        (current.state != SCREEN_STATE_PREPARING && current.state != SCREEN_STATE_INCOMING)) {
        on_video_request(cls, direct_http);
    }
    screen_status_event(screen_generation(), SCREEN_EVENT_OPENING);
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player) {
        std::lock_guard<std::mutex> guard(display_mutex);
        const uint64_t generation = screen_generation();
        if (!mpv_backend_open(mpv_player, generation, location, start_position)) {
            trace_playback_record("MPV playback: session=%" G_GUINT64_FORMAT " request=rejected", (guint64)generation);
            screen_status_fail(generation, SCREEN_ERROR_SOURCE);
            if (mpv_owns_output) mpv_backend_stop(mpv_player, mpv_generation);
            return;
        }
        mpv_rebuild_pending = false;
        mpv_generation = generation;
        mpv_owns_output = true;
        trace_playback_record("MPV playback: session=%" G_GUINT64_FORMAT " request=queued route=%s start=%.3f",
             (guint64)generation, direct_http ? "direct-http" : "playlist-cache", start_position);
        return;
    }
#endif
    /* Register this request before rebuilding the renderer. */
    video_renderer_set_start_with_source(start_position, direct_http);
    url.erase();
    url.append(location);
    relaunch_video = true;
    preserve_connections = true;
    LOGI("Direct playback: play request received; start position %.3f seconds", start_position);
    video_reset(cls, RESET_TYPE_ON_VIDEO_PLAY);
}

extern "C" void on_video_scrub(void *cls, const float position) {
    LOGI("on_video_scrub: position = %7.5f\n", position);
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player) {
        const uint64_t generation = mpv_generation;
        const bool accepted = mpv_backend_seek(mpv_player, generation, position);
        trace_mpv_control(generation, "seek", accepted, position);
        if (accepted)
            screen_status_event(generation, SCREEN_EVENT_SEEKING);
        return;
    }
#endif
    video_renderer_seek(position);
}

extern "C" void on_video_rate(void *cls, const float rate) {
    LOGI("on_video_rate = %7.5f\n", rate);
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player) {
        const uint64_t generation = mpv_generation;
        const bool accepted = rate == 1.0f ? mpv_backend_resume(mpv_player, generation) :
            rate == 0.0f ? mpv_backend_pause(mpv_player, generation) : false;
        trace_mpv_control(generation, rate == 1.0f ? "resume" : rate == 0.0f ? "pause" : "rate-ignored", accepted, rate);
        if (rate == 1.0f && accepted)
            screen_status_event(generation, SCREEN_EVENT_RESUMED);
        if (rate == 0.0f && accepted)
            screen_status_event(generation, SCREEN_EVENT_PAUSED);
        return;
    }
#endif
    if (rate == 1.0f) {
        screen_status_event(screen_generation(), SCREEN_EVENT_RESUMED);
        video_renderer_resume();
    } else if (rate ==  0.0f) {
        screen_status_event(screen_generation(), SCREEN_EVENT_PAUSED);
        video_renderer_pause();
    } else  {
        LOGI("on_video_rate: ignoring unexpected value rate = %f\n", rate);
    }
}



extern "C" float on_video_playlist_remove (void *cls) {
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player) {
        mpv_backend_snapshot_t snapshot;
        const uint64_t generation = mpv_generation;
        const bool accepted = mpv_backend_pause(mpv_player, generation);
        trace_mpv_control(generation, "playlist-pause", accepted, 0);
        mpv_backend_snapshot(mpv_player, &snapshot);
        return snapshot.generation == generation && snapshot.position_known ? (float)snapshot.position : 0.0f;
    }
#endif
    double duration, position, seek_start, seek_end;
    float rate;
    bool buffer_empty, buffer_full;
    LOGI("************************* on_video_playlist_remove\n");
    video_renderer_pause();
    video_get_playback_info(&duration, &position, &seek_start, &seek_end, &rate, &buffer_empty, &buffer_full);
    return (float) position;
}

 extern "C" void on_video_stop(void *cls) {
    LOGI("Direct playback: stop requested");
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player) {
        std::lock_guard<std::mutex> guard(display_mutex);
        const uint64_t generation = mpv_generation;
        const uint64_t request_generation = screen_generation();
        screen_status_event(request_generation, SCREEN_EVENT_STOPPING);
        const bool accepted = mpv_backend_stop(mpv_player, generation);
        trace_mpv_control(generation, "stop", accepted, 0);
        /* A /stop can cancel a request before its URL reaches the adapter.
         * There is then no child/rebuild event to finish that screen request. */
        if (!mpv_owns_output) {
            screen_status_event(request_generation, SCREEN_EVENT_STOPPED);
            trace_playback_record("Playback session: session=%" G_GUINT64_FORMAT " event=stopped monotonic_ms=%" G_GINT64_FORMAT,
                 (guint64)request_generation, (gint64)(g_get_monotonic_time()/1000));
        }
        return;
    }
#endif
    screen_status_event(screen_generation(), SCREEN_EVENT_STOPPING);
    video_reset(cls, RESET_TYPE_HLS_SHUTDOWN);
 }

extern "C" void on_video_acquire_playback_info (void *cls, playback_info_t *playback_info) {
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player) {
        mpv_backend_snapshot_t snapshot;
        mpv_backend_snapshot(mpv_player, &snapshot);
        memset(playback_info, 0, sizeof(*playback_info));
        if (snapshot.generation != mpv_generation || snapshot.generation != screen_generation()) {
            playback_info->rate = 1.0f;
            playback_info->playback_buffer_empty = true;
            return;
        }
        bool terminal = snapshot.state == MPV_BACKEND_ENDED || snapshot.state == MPV_BACKEND_FAILED ||
            (snapshot.state == MPV_BACKEND_IDLE && !mpv_owns_output);
        playback_info->duration = terminal ? -1.0 : snapshot.duration_known ? snapshot.duration : 0.0;
        playback_info->position = terminal ? -1.0 : snapshot.position_known ? snapshot.position : 0.0;
        playback_info->rate = snapshot.requested_paused ? 0.0f : 1.0f;
        playback_info->ready_to_play = snapshot.ready && !terminal;
        playback_info->playback_buffer_empty = snapshot.buffering;
        playback_info->playback_buffer_full = snapshot.ready && !snapshot.buffering && !terminal;
        playback_info->playback_likely_to_keep_up = playback_info->playback_buffer_full;
        if (snapshot.seekable_known && snapshot.seekable) {
            if (snapshot.cache_range_known) {
                playback_info->seek_start = snapshot.cache_start;
                playback_info->seek_duration = std::max(0.0, snapshot.cache_end - snapshot.cache_start);
            } else if (snapshot.duration_known) playback_info->seek_duration = snapshot.duration;
        }
        return;
    }
#endif
    int buffering_level;
    bool still_playing = video_get_playback_info_with_readiness(&playback_info->duration, &playback_info->position,
                                                 &playback_info->seek_start, &playback_info->seek_duration,
                                                 &playback_info->rate,
                                                 &playback_info->playback_buffer_empty,
                                                 &playback_info->playback_buffer_full,
                                                 &playback_info->ready_to_play,
                                                 &playback_info->playback_likely_to_keep_up);
    
#ifdef DBUS
    /*  this seems to be  called every second for first 900 secs (15 mins?) of HLS video, and subsequently
	at 30 second intervals (use it to signal HLS video activity to  the  DBus screensaver inhibitor) */
    if (scrsv == 1) {
        if (playback_info->position > previous_hls_position && !dbus_last_message) {
            dbus_screensaver_inhibiter(true);
        } else if (playback_info->position == previous_hls_position && dbus_last_message) {
            dbus_screensaver_inhibiter(false);
        }
        previous_hls_position = playback_info->position;
    }
#endif
    
    if (!still_playing) {
        LOGI(" video has finished, %f", playback_info->position);
        playback_info->position = -1.0;
        playback_info->duration = -1.0;
        video_renderer_stop();
    }
}

extern "C" void log_callback (void *cls, int level, const char *msg) {
    switch (level) {
    case LOGGER_DEBUG:
        LOGD("%s", msg);
        break;
    case LOGGER_WARNING:
        LOGW("%s", msg);
        break;
    case LOGGER_INFO:
        LOGI("%s", msg);
        break;
    case LOGGER_ERR:
        LOGE("%s", msg);
        break;
    default:
        break;
    }
}

static int start_raop_server (unsigned short display[5], unsigned short tcp[3], unsigned short udp[3], bool debug_log) {
    raop_callbacks_t raop_cbs;
    memset(&raop_cbs, 0, sizeof(raop_cbs));
    raop_cbs.conn_init = conn_init;
    raop_cbs.conn_destroy = conn_destroy;
    raop_cbs.conn_reset = conn_reset;
    raop_cbs.conn_feedback = conn_feedback;
    raop_cbs.audio_process = audio_process;
    raop_cbs.video_process = video_process;
    raop_cbs.audio_flush = audio_flush;
    raop_cbs.video_flush = video_flush;
    raop_cbs.video_pause = video_pause;
    raop_cbs.video_resume = video_resume;
    raop_cbs.audio_set_client_volume = audio_set_client_volume;
    raop_cbs.audio_set_volume = audio_set_volume;
    raop_cbs.audio_get_format = audio_get_format;
    raop_cbs.video_report_size = video_report_size;
    raop_cbs.audio_set_metadata = audio_set_metadata;
    raop_cbs.audio_set_coverart = audio_set_coverart;
    raop_cbs.audio_stop_coverart_rendering = audio_stop_coverart_rendering;
    raop_cbs.audio_set_progress = audio_set_progress;
    raop_cbs.report_client_request = report_client_request;
    raop_cbs.display_pin = display_pin;
    raop_cbs.register_client = register_client;
    raop_cbs.check_register = check_register;
    raop_cbs.passwd = passwd;
    raop_cbs.export_dacp = export_dacp;
    raop_cbs.video_reset = video_reset;
    raop_cbs.video_set_codec = video_set_codec;
#ifdef DBUS
    raop_cbs.mirror_video_running = mirror_video_running;
#endif
    raop_cbs.on_video_request = on_video_request;
    raop_cbs.on_video_request_error = on_video_request_error;
    raop_cbs.on_video_play = on_video_play;
    raop_cbs.on_video_scrub = on_video_scrub;
    raop_cbs.on_video_rate = on_video_rate;
    raop_cbs.on_video_stop = on_video_stop;
    raop_cbs.on_video_playlist_remove = on_video_playlist_remove;
    raop_cbs.on_video_acquire_playback_info = on_video_acquire_playback_info;

    raop = raop_init(&raop_cbs);
    if (raop == NULL) {
        LOGE("Error initializing raop!");
        return -1;
    }
    raop_set_log_callback(raop, log_callback, NULL);
    raop_set_log_level(raop, log_level);
    /* set nohold = 1 to allow  capture by new client */
    if (raop_init2(raop, nohold, mac_address.c_str(), keyfile.c_str())){
        LOGE("Error initializing raop (2)!");
        free (raop);
        return -1;
    }

    /* write desired display pixel width, pixel height, refresh_rate, max_fps, overscanned.  */
    /* use 0 for default values 1920,1080,60,30,0; these are sent to the Airplay client      */

    if (display[0]) raop_set_plist(raop, "width", (int) display[0]);
    if (display[1]) raop_set_plist(raop, "height", (int) display[1]);
    if (display[2]) raop_set_plist(raop, "refreshRate", (int) display[2]);
    if (display[3]) raop_set_plist(raop, "maxFPS", (int) display[3]);
    if (display[4]) raop_set_plist(raop, "overscanned", (int) display[4]);

    if (show_client_FPS_data) raop_set_plist(raop, "clientFPSdata", 1);
    if (audiodelay >= 0) raop_set_plist(raop, "audio_delay_micros", audiodelay);
    if (pin_pw == 1) raop_set_plist(raop, "pin", (int) pin);
    if (hls_support) raop_set_plist(raop, "hls", 1);
    if (airplay_video_mpv) raop_set_plist(raop, "hls_scoped_cache", 1);
    if (hls_pi4) {
        raop_set_plist(raop, "hls_pi4", 1);
        LOGI("Cached YouTube HLS profile: Raspberry Pi 4, H.264/AAC-LC up to 1080p60");
    }

    /* network port selection (ports listed as "0" will be dynamically assigned) */
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);

    raop_port = raop_get_port(raop);
    if (raop_start_httpd(raop, &raop_port) < 0 || !raop_is_running(raop)) {
        LOGE("AirPlay listener failed to start");
        return -3;
    }
    raop_set_port(raop, raop_port);

    /* use raop_port for airplay_port (instead of tcp[2]) */
    airplay_port = raop_port;

    if (dnssd) {
        raop_set_dnssd(raop, dnssd);
    } else {
        LOGE("raop_set failed to set dnssd");
        return -2;
    }
    return 0;
}

static void stop_raop_server () {
    if (raop) {
        raop_destroy(raop);
        raop = NULL;
    }
    return;
}

static void read_config_file(const char * filename, const char * uxplay_name) {
    std::string config_file = filename;
    std::string option_char = "-";
    std::vector<std::string> options;
    options.push_back(uxplay_name);
    std::ifstream file(config_file);
    if (file.is_open()) {
        fprintf(stdout,"UxPlay: reading configuration from  %s\n", config_file.c_str());
        std::string line;
        while (std::getline(file, line)) {
            if (line[0] == '#') continue;
            //  first process line into separate option items with '\0' as delimiter
            bool is_part_of_item, in_quotes;
            char endchar;
            is_part_of_item = false;
            for (int i = 0; i < (int) line.size(); i++) {
                if (is_part_of_item == false) {
                    if (line[i] == ' ') {
                        line[i] = '\0';
                    } else {
                        // start of new item
                        is_part_of_item = true;
                        switch (line[i]) {
                        case '\'':
                        case '\"':
                            endchar = line[i];
                            line[i] = '\0';
                            in_quotes = true;
                            break;
                        default:
                            in_quotes = false;
		            endchar = ' ';
		            break;
                        }
                    }
                } else {
                    /* previous character was inside this item */
                    if (line[i] == endchar) {
                        if (in_quotes) {
                            /* cases where endchar is inside quoted item */
                            if (i > 0 && line[i - 1] == '\\') continue;
                            if (i + 1 < (int) line.size() && line[i + 1] != ' ') continue;
		        }
                        line[i] =  '\0';
                        is_part_of_item = false;
                    }
                }
            }

            // now tokenize the processed line   
            std::istringstream iss(line);
            std::string token;
            bool first = true;
            while (std::getline(iss, token, '\0')) {
                if (token.size() > 0) {
                    if (first) {
                        options.push_back(option_char + token.c_str());
                        first = false;
                    } else {
                        options.push_back(token.c_str());
                    }
                }
	    }
	}
        file.close();
    } else {
        fprintf(stderr,"UxPlay: failed to open configuration file at %s\n", config_file.c_str());
    }
    if (options.size() > 1) {

        int argc = options.size();
        char **argv = (char **) malloc(sizeof(char*) * argc);
        if (argv == NULL) {
            printf("Memory allocation failure (argV)\n");
            exit(1);
        }
        for (int i = 0; i < argc; i++) {
            argv[i] = (char *) options[i].c_str();
        }
        parse_arguments (argc, argv);
        free (argv);
    }
}

/* The only post-session rebuild path. Test harnesses exercise this same
 * function, including the child-exit and display-release boundaries. */
static bool rebuild_video_outputs() {
    std::lock_guard<std::mutex> guard(display_mutex);
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player && mpv_owns_output) {
        mpv_backend_snapshot_t player;
        mpv_backend_snapshot(mpv_player, &player);
        /* A replacement request may arrive after the loop decided to reset. */
        if (!mpv_rebuild_pending) return true;
        if (player.child_alive) return false;
    }
#endif
    if (!hide_status_display()) return false;
    uint64_t finished_generation = mpv_rebuild_pending ? (uint64_t)mpv_generation : (uint64_t)playback_generation;
    screen_status_snapshot_t pending_screen;
    screen_status_get_snapshot(&pending_screen);
    if (pending_screen.state == SCREEN_STATE_STOPPING &&
        pending_screen.generation >= finished_generation)
        finished_generation = pending_screen.generation;
    video_renderer_destroy();
    if (!preserve_connections) {
        url.erase();
        if (raop) raop_remove_known_connections(raop);
    }
    const char *uri = url.empty() ? NULL : url.c_str();
    playback_output_released = false;
    playback_generation = uri ? screen_generation() : 0;
    video_renderer_init(render_logger, server_name.c_str(), videoflip, video_parser.c_str(), rtp_pipeline.c_str(),
                        video_decoder.c_str(), video_converter.c_str(), videosink.c_str(),
                        videosink_options.c_str(), fullscreen, video_sync, h265_support,
                        render_coverart, playbin_version, uri);
    full_video_reset = false;
    video_renderer_start();
    bool suspended = false;
    if (!uri && (status_display || airplay_video_mpv)) {
        if (!video_renderer_suspend_output()) {
            screen_status_event(screen_generation(), SCREEN_EVENT_RECOVERY_REQUIRED);
            return false;
        }
        suspended = true;
    }
    if (mpv_rebuild_pending) {
        mpv_rebuild_pending = false;
        mpv_output_prepared = false;
        mpv_owns_output = false;
    }
    if (!uri) {
        playback_output_released = suspended;
        screen_status_set_readiness(receiver_has_network(), raop && raop_is_running(raop), receiver_registered, true, suspended);
        screen_status_event(finished_generation, SCREEN_EVENT_STOPPED);
        if (!mirror_waiting) show_status_display();
        trace_playback_record("Playback session: session=%" G_GUINT64_FORMAT " event=receiver-rebuilt monotonic_ms=%" G_GINT64_FORMAT,
             (guint64)finished_generation, (gint64)(g_get_monotonic_time()/1000));
    }
    return true;
}

static void configure_stdout_buffering() {
    /* Send complete log lines promptly when stdout is captured by systemd or a
     * pipe. Configure this before any output, including the macOS wrapper. */
#ifdef _WIN32
    /* The Windows CRT treats _IOLBF as full buffering. */
    setvbuf(stdout, NULL, _IONBF, 0);
#else
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
#endif
}

#ifdef GST_MACOS
/* workaround for GStreamer >= 1.22 "Official Builds" on macOS */
#include <TargetConditionals.h>
#include <gst/gstmacos.h>
void real_main (int argc, char *argv[]);

int main (int argc, char *argv[]) {
    configure_stdout_buffering();
    LOGI("*=== Using gst_macos_main wrapper for GStreamer >= 1.22 on macOS ===*");
    return  gst_macos_main ((GstMainFunc) real_main, argc, argv , NULL);
}

void real_main (int argc, char *argv[]) {
#else
int main (int argc, char *argv[]) {
    configure_stdout_buffering();
#endif
    std::vector<char> server_hw_addr;
    std::string config_file = "";

#ifdef _WIN32
    if (!SetConsoleCtrlHandler(CtrlHandler, TRUE)) {
        LOGE("Could not set control handler");
        exit(1);
    }
#else
    signal(SIGINT, CtrlHandler);
    signal(SIGTERM, CtrlHandler);
    signal(SIGHUP, CtrlHandler);
#endif

#ifdef __OpenBSD__
    if (unveil("/", "rwc") == -1 || unveil(NULL, NULL) == -1) {
        err(1, "unveil");
    }
#endif

#ifdef SUPPRESS_AVAHI_COMPAT_WARNING
    // suppress avahi_compat nag message.  avahi emits a "nag" warning (once)
    // if  getenv("AVAHI_COMPAT_NOWARN") returns null.
    static char avahi_compat_nowarn[] = "AVAHI_COMPAT_NOWARN=1";
    if (!getenv("AVAHI_COMPAT_NOWARN")) putenv(avahi_compat_nowarn);
#endif

    /* for HLS video language preferences */
    char *lang_env = getenv("LANGUAGE");
    if (lang_env && strlen(lang_env)) {
        lang.erase();
        lang = lang_env;
    }
    
    char *rcfile = NULL;
    /* see if option -rc was given */
    for (int i = 1; i < argc ; i++) {
        std::string arg(argv[i]);
        if (arg == "-rc") {
            struct stat sb;
            if (i+1 == argc) {
                LOGE ("option -rc requires a filename  (-rc <filename>)");
                exit(1);
            }
            rcfile = argv[i+1];
            if (stat(rcfile, &sb) == -1) {
                LOGE("startup file %s specified by option -rc was not found", rcfile);
                exit(0);
            }
            break;
        }
    }
    if (rcfile) {
        config_file = rcfile;
    } else {	
        config_file = find_uxplay_config_file();
    }
    if (config_file.length()) {
        read_config_file(config_file.c_str(), argv[0]);
    }
    parse_arguments (argc, argv);

    log_level = (debug_log ? LOGGER_DEBUG_DATA : LOGGER_INFO);
    if (debug_log && suppress_packet_debug_data) {
        log_level = LOGGER_DEBUG;
    }

    
#ifdef _WIN32    /*  use utf-8 terminal output; don't buffer stdout in WIN32 when debug_log = false */
    SetConsoleOutputCP(CP_UTF8);
    if (!debug_log) {
        setbuf(stdout, NULL);
    }
#endif

    LOGI("UxPlay %s: An Open-Source AirPlay mirroring and audio-streaming server.", VERSION);

#ifdef DBUS
    if (scrsv && !use_video) {
        LOGI ("-scrsv = %d will be ignored, as no video will be rendered", scrsv);
        scrsv = 0;
    }
    if (scrsv) {
        DBusError dbus_error;
        dbus_error_init(&dbus_error);
        dbus_connection = dbus_bus_get(DBUS_BUS_SESSION, &dbus_error);
        if (dbus_error_is_set(&dbus_error)) {
            dbus_error_free(&dbus_error);
            scrsv = 0;
            LOGI ("D-Bus session not found: screensaver inhibition option (\"-scrsv\") will not be active");
        }
    }
    if (scrsv) {
        LOGD ("D-Bus session support is available, connection %p", dbus_connection);
        std::string desktop = getenv("XDG_CURRENT_DESKTOP"); 
        LOGD("Desktop Environment:  %s", desktop.c_str());

        /* if dbus_service, dbus_path, dbus_interface, dbus_inhibit, dbus_uninhibit *
         * in the detected  Desktop Environments are still non-conforming to the    *
         * org.freedesktop.ScreenSaver interface, they can be modifed here          */

        /* some desktop environments (e.g. Xfce 4, Mate) modify the D-Bus service name */
        std::string name;
        if (strstr(desktop.c_str(), "XFCE")) {
            name = "xfce";
        } else if (strstr(desktop.c_str(), "MATE")) {
            name = "mate";
        }
  
        if (!name.empty()) {
            size_t pos;
            std::string replace_word = "freedesktop";
            pos = dbus_service.find(replace_word);
            dbus_service.replace(pos, replace_word.size(), name);
            pos = dbus_path.find(replace_word);
            dbus_path.replace(pos, replace_word.size(), name);
            pos = dbus_interface.find(replace_word);
            dbus_interface.replace(pos, replace_word.size(), name);
        }

        LOGI("Will attempt to use %s (D-Bus screensaver inhibition) %s", dbus_service.c_str(),
             (scrsv == 1 ? "while displaying mirrored or streamed video" : "always"));
        if (scrsv == 2) {
            dbus_screensaver_inhibiter(true);
        }
    }
#endif
    if (audiosink == "0") {
        use_audio = false;
        dump_audio = false;
    }
    if (dump_video) {
        if (video_dump_limit > 0) {
             LOGI("dump video using \"-vdmp %d %s\"", video_dump_limit, video_dumpfile_name.c_str());
        } else {
             LOGI("dump video using \"-vdmp %s\"", video_dumpfile_name.c_str());
        }
    }
    if (dump_audio) {
        if (audio_dump_limit > 0) {
            LOGI("dump audio using \"-admp %d %s\"", audio_dump_limit, audio_dumpfile_name.c_str());
        } else {
            LOGI("dump audio using \"-admp %s\"",  audio_dumpfile_name.c_str());
        }
    }

#if __APPLE__
    /* warn about default  use of -nc option on macOS */
    if (!new_window_closing_behavior) {
        LOGI("UxPlay on macOS is using -nc option as workaround for GStreamer problem: use \"-nc no\" to omit workaround");
    }
#endif

    if (videosink == "0") {
        use_video = false;
	videosink.erase();
        videosink.append("fakesink");
	videosink_options.erase();
	LOGI("video_disabled");
        display[3] = 1; /* set fps to 1 frame per sec when no video will be shown */
    }

    if (hls_pi4 && videosink == "kmssink") {
        /* Pi's display driver is known. Generic KMS discovery tries unrelated
         * drivers first and costs several seconds on every new video sink.
         * Keep explicit device selection, and put this property before any
         * optional pipeline extension consumed by the mirroring renderer. */
        size_t options_end = videosink_options.find('!');
        std::string sink_options = videosink_options.substr(0, options_end);
        if (sink_options.find("driver-name=") == std::string::npos &&
            sink_options.find("bus-id=") == std::string::npos &&
            sink_options.find("fd=") == std::string::npos) {
            videosink_options.insert(options_end == std::string::npos ? videosink_options.size() : options_end,
                                     " driver-name=vc4 ");
            LOGI("Pi 4 profile: using vc4 display driver to avoid repeated KMS discovery");
        }
    }

    if (fullscreen && use_video) {
        if (videosink == "waylandsink" || videosink == "vaapisink") {
            videosink_options.append(" fullscreen=true");
        } else if (videosink == "kmssink") {
            videosink_options.append(" force-modesetting=TRUE ");	  
        }
    }

    if (videosink == "d3d11videosink"  && videosink_options.empty() && use_video) {
        if (fullscreen) {
            videosink_options.append(" fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_PROPERTY fullscreen=TRUE ");
        } else {
            videosink_options.append(" fullscreen-toggle-mode=GST_D3D11_WINDOW_FULLSCREEN_TOGGLE_MODE_ALT_ENTER ");
            LOGI("Use Alt-Enter key combination to toggle into/out of full-screen mode");
        }
    }

    if (videosink == "d3d12videosink"  && videosink_options.empty() && use_video) {
        if (fullscreen) {
            videosink_options.append(" fullscreen=TRUE ");
        } else {
            videosink_options.append(" fullscreen-on-alt-enter=TRUE ");
            LOGI("Use Alt-Enter key combination to toggle into/out of full-screen mode");
        }
    } 

    if (bt709_fix && use_video) {
        video_parser.append(" ! ");
        video_parser.append(BT709_FIX);
    }

    if (srgb_fix && use_video) {
        std::string option = video_converter;
        video_converter.append(SRGB_FIX);
        video_converter.append(option);
    }
    
    if (pin_pw == 1 && registration_list) {
        if (pairing_register == "") {
            const char * homedir = get_homedir();
            if (homedir) {
                pairing_register = homedir;
                pairing_register.append("/.uxplay.register");
             }
        }
    }

    /* read in public keys that were previously registered with pair-setup-pin */
    if (pin_pw == 1 && registration_list && strlen(pairing_register.c_str())) {
        size_t len = 0;
        std::string  key;
        int clients = 0;
        std::ifstream file(pairing_register);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                /*32 bytes pk -> base64 -> strlen(pk64) = 44 chars = line[0:43]; add '\0' at line[44] */ 
                line[44] = '\0';
                std::string pk = line.c_str();
                registered_keys.push_back(key.assign(pk));
                clients ++;
            }
            if (clients) {
                LOGI("Register %s lists %d pin-registered clients", pairing_register.c_str(), clients);
            }
            file.close();
        }
    }

    if (pin_pw == 1 && keyfile == "0") {
        const char * homedir = get_homedir();
        if (homedir) {
            keyfile.erase();
            keyfile = homedir;
            keyfile.append("/.uxplay.pem");
        } else {
	    LOGE("could not determine $HOME: public key wiil not be saved, and so will not be persistent");
        }
    }

    if (keyfile != "") {
        LOGI("public key storage (for persistence) is in %s", keyfile.c_str());
    }
    
    if (do_append_hostname) {
        append_hostname(server_name);
    }

    if (!gstreamer_init()) {
        LOGE ("stopping");
        exit (1);
    }

    render_logger = logger_init();
    logger_set_callback(render_logger, log_callback, NULL);
    logger_set_level(render_logger, log_level);

    screen_status_init(screen_info, server_name.c_str());
    screen_set_backend(true);
    video_renderer_configure_screen(screen_info);
    if (screen_info != SCREEN_INFO_OFF && use_video)
        status_display = screen_status_renderer_new(render_logger, videosink.c_str(), videosink_options.c_str());
#ifdef UXPLAY_HAVE_MPV
    if (airplay_video_mpv) {
        if (!use_video) { LOGE("mpv AirPlay video requires video output enabled"); exit(1); }
        mpv_config.executable = mpv_executable.empty() ? NULL : mpv_executable.c_str();
        mpv_config.video_output = mpv_vo.empty() ? NULL : mpv_vo.c_str();
        mpv_config.gpu_context = mpv_context.empty() ? NULL : mpv_context.c_str();
        mpv_config.gpu_api = mpv_api.empty() ? NULL : mpv_api.c_str();
        mpv_config.drm_device = mpv_drm_device.empty() ? NULL : mpv_drm_device.c_str();
        mpv_config.drm_connector = mpv_connector.empty() ? NULL : mpv_connector.c_str();
        mpv_config.audio_device = mpv_audio_device.empty() ? NULL : mpv_audio_device.c_str();
        mpv_config.qualified_h264_hwdec = mpv_h264_hwdec.empty() ? NULL : mpv_h264_hwdec.c_str();
        mpv_config.fast_rendering = mpv_fast_rendering;
        mpv_config.decode_policy = mpv_policy;
        mpv_config.disable_audio = !use_audio;
        mpv_config.packet_diagnostics = screen_info == SCREEN_INFO_DEBUG;
        char error[192];
        if (!mpv_backend_preflight(&mpv_config, error, sizeof(error))) {
            LOGE("mpv startup check failed: %s", error); exit(1);
        }
        mpv_player = mpv_backend_create(&mpv_config, error, sizeof(error));
        if (!mpv_player) { LOGE("mpv setup failed: %s", error); exit(1); }
        LOGI("AirPlay video backend: mpv (mirroring and RAOP audio: GStreamer)");
    }
#endif

    if (hls_pi4) {
        video_renderer_configure_pi4(render_logger);
    }

    if (use_audio) {
        audio_renderer_init(render_logger, audiosink.c_str(), &audio_sync, &video_sync, audio_rtp_pipeline.c_str());
    } else {
        LOGI("audio_disabled");
    }
    if (use_video) {
        video_renderer_init(render_logger, server_name.c_str(), videoflip, video_parser.c_str(), rtp_pipeline.c_str(),
                            video_decoder.c_str(), video_converter.c_str(), videosink.c_str(),
                            videosink_options.c_str(), fullscreen, video_sync, h265_support,
                            render_coverart, playbin_version, NULL);
        video_renderer_start();
        if (status_display || airplay_video_mpv) {
            if (!video_renderer_suspend_output()) {
                screen_status_event(screen_generation(), SCREEN_EVENT_RECOVERY_REQUIRED);
                LOGE("Initial GStreamer output release failed"); exit(1);
            }
            show_status_display();
        }
#ifdef __OpenBSD__
    } else {
        if (pledge("stdio rpath wpath cpath inet unix prot_exec", NULL) == -1) {
            err(1, "pledge");
        }
#endif
    }

    if (mux_to_file) {
        mux_renderer_init(render_logger, mux_filename.c_str(), use_audio, use_video);
    }

    if (udp[0]) {
        LOGI("using network ports UDP %d %d %d TCP %d %d %d", udp[0], udp[1], udp[2], tcp[0], tcp[1], tcp[2]);
    }

    if (!use_random_hw_addr) {
        if (strlen(mac_address.c_str()) == 0) {
            mac_address = find_mac();
            LOGI("using system MAC address %s",mac_address.c_str());	    
        } else {
            LOGI("using user-set MAC address %s",mac_address.c_str());
        }
    }
    if (mac_address.empty()) {
        mac_address = random_mac();
        LOGI("using randomly-generated MAC address %s",mac_address.c_str());
    }
    parse_hw_addr(mac_address, server_hw_addr);

    if (coverart_filename.length()) {
        LOGI("any AirPlay audio cover-art will be written to file  %s",coverart_filename.c_str());
        write_coverart(coverart_filename.c_str(), (const void *) empty_image, sizeof(empty_image));
    }

    if (metadata_filename.length()) {
        LOGI("any AirPlay audio metadata text will be written to file  %s",metadata_filename.c_str());
        write_metadata(metadata_filename.c_str(), "no data\n");
    }

    /* set default resolutions for h264 or h265*/
    if (!display[0] && !display[1]) {
        if (h265_support) {
            display[0] = 3840;
            display[1] = 2160;
        } else {
            display[0] = 1920;
            display[1] = 1080;
        }	  
    }

    if (start_dnssd(server_hw_addr, server_name)) {
        cleanup();
    }
    if (start_raop_server(display, tcp, udp, debug_log)) {
        stop_dnssd();
        cleanup();
    }

    if (lang.length() > 1) {
        raop_set_lang(raop, lang.c_str());
    }
    
#define PID_MAX 4194304 // 2^22
    if (ble_filename.length()) {
#ifdef _WIN32
        DWORD winpid = GetCurrentProcessId();
	uint32_t pid = (uint32_t) winpid;
        g_assert(pid <= PID_MAX);
#else
        pid_t pid = getpid();
        g_assert (pid <= PID_MAX && pid >= 0);
#endif
        write_bledata((uint32_t *) &pid, argv[0], ble_filename.c_str());
        LOGI("Bluetooth LE beacon-based service discovery is possible: PID data written to %s", ble_filename.c_str());
    }
    
    if (register_dnssd()) {
        stop_raop_server();
        stop_dnssd();
        cleanup();
    }
    receiver_registered = true;
    playback_output_released = status_display || airplay_video_mpv || !use_video;
    screen_status_set_readiness(receiver_has_network(), true, true, true, playback_output_released);
    if (status_display) screen_status_renderer_tick(status_display);
    reconnect:
    compression_type = 0;
    close_window = new_window_closing_behavior;
    main_loop();
    if (relaunch_video) {
        if (reset_httpd) {
            raop_stop_httpd(raop);
        }
        if (use_audio) {
            LOGI("RAOP audio: stage=stop-request reason=video-relaunch direct_video=%d", !url.empty());
            audio_renderer_stop();
        }
        if (use_video && (close_window || preserve_connections || full_video_reset) && !rebuild_video_outputs()) {
            stop_raop_server();
            stop_dnssd();
            cleanup();
        }
        if (reset_httpd) {
            unsigned short port = raop_get_port(raop);
            raop_start_httpd(raop, &port);
            raop_set_port(raop, port);
        }
        if (mux_to_file) {
            mux_renderer_stop();
        }
        goto reconnect;
    } else {
        LOGI("Stopping RAOP Server...");
        stop_raop_server();
        stop_dnssd();
    }
    cleanup();
}
 
[[noreturn]] static void cleanup() {
#ifdef UXPLAY_HAVE_MPV
    if (mpv_player) {
        mpv_backend_shutdown(mpv_player);
        const gint64 deadline = g_get_monotonic_time() + 7 * G_USEC_PER_SEC;
        drain_mpv_terminal_reports();
        while (!mpv_backend_destroy(mpv_player)) {
            mpv_backend_poll(mpv_player);
            drain_mpv_terminal_reports();
            mpv_backend_snapshot_t player;
            mpv_backend_snapshot(mpv_player, &player);
            trace_mpv_playback(player);
            if (g_get_monotonic_time() >= deadline) {
                LOGE("mpv did not exit; output recovery requires the service supervisor");
                /* Preserve the output boundary: never start another player. */
                exit(1);
            }
            g_usleep(10000);
        }
        mpv_player = NULL;
    }
#endif
    if (status_display) {
        if (!screen_status_renderer_free(status_display)) {
            LOGE("Status display did not release output"); exit(1);
        }
        status_display = NULL;
    }
    if (use_audio) {
        audio_renderer_destroy();
    }
    if (use_video)  {
        video_renderer_destroy();
    }
    logger_destroy(render_logger);
    render_logger = NULL;
    if(audio_dumpfile) {
        fclose(audio_dumpfile);
    }
    if (video_dumpfile) {
        fwrite(mark, 1, sizeof(mark), video_dumpfile);
        fclose(video_dumpfile);
    }
    if (coverart_filename.length()) {
        remove (coverart_filename.c_str());
    }
    if (metadata_filename.length()) {
        remove (metadata_filename.c_str());
    }
    if (ble_filename.length()) {
        remove (ble_filename.c_str());
    }
#ifdef DBUS
    if (dbus_connection) {
        LOGD("Ending D-Bus connection %p", dbus_connection);
        if (dbus_last_message) {
            dbus_screensaver_inhibiter(false);
        }
        if (dbus_pending) {
            dbus_pending_call_cancel(dbus_pending);
            dbus_pending_call_unref(dbus_pending);
        }
        dbus_connection_unref(dbus_connection);
    }
#endif
    exit(0);
}
