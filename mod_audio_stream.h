#ifndef MOD_AUDIO_STREAM_H
#define MOD_AUDIO_STREAM_H

#include <switch.h>
#include <speex/speex_resampler.h>

#define MY_BUG_NAME "audio_stream"
#define MAX_SESSION_ID (256)
#define MAX_WS_URI (4096)
#define MAX_METADATA_LEN (8192)

/* ── FreeSWITCH custom event names ──────────────────────────────────────── */
#define EVENT_CONNECT           "mod_audio_stream::connect"
#define EVENT_DISCONNECT        "mod_audio_stream::disconnect"
#define EVENT_ERROR             "mod_audio_stream::error"
#define EVENT_JSON              "mod_audio_stream::json"
/* Fired each time an audio segment is ready for playback (file path in body) */
#define EVENT_PLAY              "mod_audio_stream::play"
/* Fired alongside EVENT_PLAY; includes segment index and current queue depth */
#define EVENT_PLAYBACK_START    "mod_audio_stream::playback_start"
/* Fired when the break/drain command clears the pending playback queue */
#define EVENT_PLAYBACK_STOP     "mod_audio_stream::playback_stop"

typedef void (*responseHandler_t)(switch_core_session_t *session,
                                  const char *eventName,
                                  const char *json);

struct private_data {
    switch_mutex_t          *mutex;
    char                     sessionId[MAX_SESSION_ID];
    SpeexResamplerState     *resampler;
    responseHandler_t        responseHandler;
    void                    *pAudioStreamer;
    char                     ws_uri[MAX_WS_URI];
    int                      sampling;
    int                      channels;
    /* outbound (upload) stream control */
    int                      audio_paused:1;
    /* lifecycle guards */
    int                      close_requested:1;
    int                      cleanup_started:1;
    /* independent playback (download) stream control */
    int                      playback_paused:1;   /* pause queueing new play events     */
    int                      drain_requested:1;   /* break: clear queue + stop playback */
    char                     initialMetadata[MAX_METADATA_LEN];
    switch_buffer_t         *sbuffer;
    int                      rtp_packets;

    /* ── Audio-tap diagnostic counters (approximate, no lock) ── */
    uint32_t                 dbg_read_cbs;    /* READ  bug callbacks fired      */
    uint32_t                 dbg_write_cbs;   /* WRITE bug callbacks fired      */
    uint32_t                 dbg_data_frames; /* stream_frame calls with data   */
    uint32_t                 dbg_frame_total; /* total stream_frame calls       */
};

typedef struct private_data private_t;

/* Internal WebSocket event types used between glue layer and AudioStreamer */
enum notifyEvent_t {
    CONNECT_SUCCESS,
    CONNECT_ERROR,
    CONNECTION_DROPPED,
    MESSAGE,       /* text frame – may contain streamAudio JSON */
    BINARY_AUDIO   /* raw binary audio frame (no base64/JSON envelope) */
};

#endif /* MOD_AUDIO_STREAM_H */
