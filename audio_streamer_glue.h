#ifndef AUDIO_STREAMER_GLUE_H
#define AUDIO_STREAMER_GLUE_H

#include "mod_audio_stream.h"

int             validate_ws_uri(const char *url, char *wsUri);
switch_status_t is_valid_utf8(const char *str);

/* Send a text message through the open WebSocket */
switch_status_t stream_session_send_text(switch_core_session_t *session, char *text);

/* Pause / resume the outbound (upload) audio stream */
switch_status_t stream_session_pauseresume(switch_core_session_t *session, int pause);

/* Pause / resume inbound (playback) audio independently from the upload stream.
 * Resuming also clears any pending drain-requested state so that new segments
 * received after a prior break start playing again immediately. */
switch_status_t stream_session_playback_pauseresume(switch_core_session_t *session, int pause);

/* Drain the pending playback queue and fire EVENT_PLAYBACK_STOP.
 * The upload stream is NOT interrupted.  Playback remains paused until an
 * explicit playback_resume call is made. */
switch_status_t stream_session_break(switch_core_session_t *session);

/* Lifecycle */
switch_status_t stream_session_init(switch_core_session_t *session,
                                    responseHandler_t responseHandler,
                                    uint32_t samples_per_second,
                                    char *wsUri,
                                    int sampling,
                                    int channels,
                                    char *metadata,
                                    void **ppUserData);

switch_bool_t   stream_frame(switch_media_bug_t *bug);

switch_status_t stream_session_cleanup(switch_core_session_t *session,
                                       char *text,
                                       int channelIsClosing);

#endif /* AUDIO_STREAMER_GLUE_H */
