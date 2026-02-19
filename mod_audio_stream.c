/*
 * mod_audio_stream
 *
 * FreeSWITCH module that streams audio to a WebSocket server and plays back
 * the server's audio responses on the same channel – true full-duplex.
 *
 * API syntax
 * ──────────
 *   uuid_audio_stream <uuid> start  <wss-url> <mix-type> <sample-rate> [metadata]
 *   uuid_audio_stream <uuid> stop   [final-text]
 *   uuid_audio_stream <uuid> pause
 *   uuid_audio_stream <uuid> resume
 *   uuid_audio_stream <uuid> send_text <text>
 *   uuid_audio_stream <uuid> playback_pause
 *   uuid_audio_stream <uuid> playback_resume
 *   uuid_audio_stream <uuid> break
 *
 * mix-type   : mono | mixed | stereo
 * sample-rate: 8k | 16k | <numeric Hz, must be a multiple of 8000>
 */
#include "mod_audio_stream.h"
#include "audio_streamer_glue.h"

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_stream_shutdown);
SWITCH_MODULE_LOAD_FUNCTION(mod_audio_stream_load);

SWITCH_MODULE_DEFINITION(mod_audio_stream, mod_audio_stream_load,
                          mod_audio_stream_shutdown,
                          NULL /* no runtime thread */);

/* ── FreeSWITCH event bridge ─────────────────────────────────────────────── */
static void responseHandler(switch_core_session_t *session,
                             const char *eventName,
                             const char *json)
{
    switch_event_t  *event;
    switch_channel_t *channel = switch_core_session_get_channel(session);

    switch_event_create_subclass(&event, SWITCH_EVENT_CUSTOM, eventName);
    switch_channel_event_set_data(channel, event);
    if (json) switch_event_add_body(event, "%s", json);
    switch_event_fire(&event);
}

/* ── Media-bug callback ──────────────────────────────────────────────────── */
static switch_bool_t capture_callback(switch_media_bug_t *bug,
                                       void *user_data,
                                       switch_abc_type_t type)
{
    switch_core_session_t *session =
        switch_core_media_bug_get_session(bug);
    private_t *tech_pvt = (private_t *)user_data;
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE: %d\n", type);
    switch (type) {

        case SWITCH_ABC_TYPE_INIT:
            break;

        case SWITCH_ABC_TYPE_CLOSE:
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_INFO, "Got SWITCH_ABC_TYPE_CLOSE\n");
            {
                /* Distinguish normal channel hangup from a module-initiated
                 * close (close_requested) so we avoid double-remove. */
                int channel_closing = tech_pvt->close_requested ? 0 : 1;
                stream_session_cleanup(session, NULL, channel_closing);
            }
            break;

        case SWITCH_ABC_TYPE_READ:
        case SWITCH_ABC_TYPE_WRITE:
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG, "Got SWITCH_ABC_TYPE_WRITE\n");
            if (tech_pvt->close_requested)
                return SWITCH_FALSE;
            return stream_frame(bug);        
        default:
            break;
    }

    return SWITCH_TRUE;
}

/* ── start_capture ───────────────────────────────────────────────────────── */
static switch_status_t start_capture(switch_core_session_t *session,
                                      switch_media_bug_flag_t flags,
                                      char *wsUri,
                                      int sampling,
                                      char *metadata)
{
    switch_channel_t    *channel = switch_core_session_get_channel(session);
    switch_media_bug_t  *bug;
    switch_status_t      status;
    switch_codec_t      *read_codec;
    void                *pUserData = NULL;
    int                  channels  = (flags & SMBF_STEREO) ? 2 : 1;

    if (switch_channel_get_private(channel, MY_BUG_NAME)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR, "mod_audio_stream: bug already attached\n");
        return SWITCH_STATUS_FALSE;
    }

    if (switch_channel_pre_answer(channel) != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR,
            "mod_audio_stream: channel must reach pre-answer before start\n");
        return SWITCH_STATUS_FALSE;
    }

    read_codec = switch_core_session_get_read_codec(session);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_DEBUG, "calling stream_session_init\n");

    if (stream_session_init(session, responseHandler,
                            read_codec->implementation->actual_samples_per_second,
                            wsUri, sampling, channels, metadata,
                            &pUserData) == SWITCH_STATUS_FALSE) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR,
            "mod_audio_stream: stream_session_init failed\n");
        return SWITCH_STATUS_FALSE;
    }

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_DEBUG, "adding media bug ==============================================\n");

    if ((status = switch_core_media_bug_add(
             session, MY_BUG_NAME, NULL,
             capture_callback, pUserData, 0, flags, &bug)) !=
        SWITCH_STATUS_SUCCESS) {
        return status;
    }

    switch_channel_set_private(channel, MY_BUG_NAME, bug);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_DEBUG, "start_capture: done\n");
    return SWITCH_STATUS_SUCCESS;
}

/* ── Command helpers ─────────────────────────────────────────────────────── */
static switch_status_t do_stop(switch_core_session_t *session, char *text) {
    if (text)
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_INFO, "mod_audio_stream: stop with text: %s\n", text);
    else
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_INFO, "mod_audio_stream: stop\n");

    return stream_session_cleanup(session, text, 0);
}

static switch_status_t do_pauseresume(switch_core_session_t *session,
                                       int pause)
{
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_INFO, "mod_audio_stream: stream %s\n",
        pause ? "pause" : "resume");
    return stream_session_pauseresume(session, pause);
}

static switch_status_t do_playback_pauseresume(switch_core_session_t *session,
                                                int pause)
{
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_INFO, "mod_audio_stream: playback %s\n",
        pause ? "pause" : "resume");
    return stream_session_playback_pauseresume(session, pause);
}

static switch_status_t do_break(switch_core_session_t *session) {
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_INFO, "mod_audio_stream: break (drain playback queue)\n");
    return stream_session_break(session);
}

static switch_status_t do_send_text(switch_core_session_t *session,
                                     char *text)
{
    switch_channel_t   *channel = switch_core_session_get_channel(session);
    switch_media_bug_t *bug =
        switch_channel_get_private(channel, MY_BUG_NAME);

    if (bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_INFO, "mod_audio_stream: sending text: %s\n", text);
        return stream_session_send_text(session, text);
    }
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_ERROR,
        "mod_audio_stream: no bug, cannot send text: %s\n", text);
    return SWITCH_STATUS_FALSE;
}

/* ── API handler ─────────────────────────────────────────────────────────── */
#define STREAM_API_SYNTAX \
    "<uuid> start <wss-url> <mono|mixed|stereo> <8k|16k|Hz> [metadata]\n" \
    "<uuid> stop  [final-text]\n" \
    "<uuid> pause\n" \
    "<uuid> resume\n" \
    "<uuid> send_text <text>\n" \
    "<uuid> playback_pause\n" \
    "<uuid> playback_resume\n" \
    "<uuid> break"

SWITCH_STANDARD_API(stream_function)
{
    char *mycmd = NULL;
    char *argv[6] = {0};
    int   argc    = 0;

    switch_status_t status = SWITCH_STATUS_FALSE;

    if (!zstr(cmd) && (mycmd = strdup(cmd)))
        argc = switch_separate_string(mycmd, ' ', argv,
                                      sizeof(argv) / sizeof(argv[0]));

    assert(cmd);
    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_DEBUG, "mod_audio_stream cmd: %s\n", cmd ? cmd : "");

    if (zstr(cmd) || argc < 2 ||
        (strcasecmp(argv[1], "start") == 0 && argc < 4)) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR,
            "Bad command: %s %s %s\n", cmd, argv[0], argv[1]);
        stream->write_function(stream, "-USAGE: %s\n", STREAM_API_SYNTAX);
        goto done;
    }

    {
        switch_core_session_t *lsession = switch_core_session_locate(argv[0]);
        if (!lsession) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_ERROR, "Cannot locate session %s\n", argv[0]);
            goto done;
        }

        if (!strcasecmp(argv[1], "stop")) {
            if (argc > 2 &&
                is_valid_utf8(argv[2]) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR,
                    "stop text contains invalid UTF-8: %s\n", argv[2]);
                switch_core_session_rwunlock(lsession);
                goto done;
            }
            status = do_stop(lsession, argc > 2 ? argv[2] : NULL);

        } else if (!strcasecmp(argv[1], "pause")) {
            status = do_pauseresume(lsession, 1);

        } else if (!strcasecmp(argv[1], "resume")) {
            status = do_pauseresume(lsession, 0);

        } else if (!strcasecmp(argv[1], "playback_pause")) {
            status = do_playback_pauseresume(lsession, 1);

        } else if (!strcasecmp(argv[1], "playback_resume")) {
            status = do_playback_pauseresume(lsession, 0);

        } else if (!strcasecmp(argv[1], "break")) {
            status = do_break(lsession);

        } else if (!strcasecmp(argv[1], "send_text")) {
            if (argc < 3) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR,
                    "send_text requires a text argument\n");
                switch_core_session_rwunlock(lsession);
                goto done;
            }
            if (is_valid_utf8(argv[2]) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR,
                    "send_text argument contains invalid UTF-8: %s\n",
                    argv[2]);
                switch_core_session_rwunlock(lsession);
                goto done;
            }
            status = do_send_text(lsession, argv[2]);

        } else if (!strcasecmp(argv[1], "start")) {
            char  wsUri[MAX_WS_URI];
            int   sampling = 8000;
            switch_media_bug_flag_t flags = SMBF_READ_STREAM | SMBF_NO_PAUSE;
            char *metadata = argc > 5 ? argv[5] : NULL;

            if (metadata &&
                is_valid_utf8(metadata) != SWITCH_STATUS_SUCCESS) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR,
                    "metadata contains invalid UTF-8: %s\n", metadata);
                switch_core_session_rwunlock(lsession);
                goto done;
            }

            if (!strcasecmp(argv[3], "mixed")) {
                flags |= SMBF_WRITE_STREAM;
            } else if (!strcasecmp(argv[3], "stereo")) {
                flags |= SMBF_WRITE_STREAM;
                flags |= SMBF_STEREO;
            } else if (strcasecmp(argv[3], "mono") != 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR,
                    "invalid mix type '%s' – must be mono, mixed or stereo\n",
                    argv[3]);
                switch_core_session_rwunlock(lsession);
                goto done;
            }

            if (argc > 4) {
                if (!strcasecmp(argv[4], "8k"))
                    sampling = 8000;
                else if (!strcasecmp(argv[4], "16k"))
                    sampling = 16000;
                else
                    sampling = atoi(argv[4]);
            }

            if (!validate_ws_uri(argv[2], wsUri)) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR,
                    "invalid WebSocket URI: %s\n", argv[2]);
            } else if (sampling % 8000 != 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR,
                    "invalid sample rate %d – must be a multiple of 8000\n",
                    sampling);
            } else {
                status = start_capture(lsession, flags, wsUri,
                                       sampling, metadata);
            }

        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_ERROR,
                "unsupported command: %s\n", argv[1]);
        }

        switch_core_session_rwunlock(lsession);
    }

    if (status == SWITCH_STATUS_SUCCESS)
        stream->write_function(stream, "+OK Success\n");
    else
        stream->write_function(stream, "-ERR Operation Failed\n");

done:
    switch_safe_free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

/* ── Module load ─────────────────────────────────────────────────────────── */
SWITCH_MODULE_LOAD_FUNCTION(mod_audio_stream_load)
{
    switch_api_interface_t *api_interface;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "mod_audio_stream loading\n");

    *module_interface =
        switch_loadable_module_create_module_interface(pool, modname);

    /* Register all custom event subclasses */
    if (switch_event_reserve_subclass(EVENT_JSON)            != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_CONNECT)         != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_ERROR)           != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_DISCONNECT)      != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_PLAY)            != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_PLAYBACK_START)  != SWITCH_STATUS_SUCCESS ||
        switch_event_reserve_subclass(EVENT_PLAYBACK_STOP)   != SWITCH_STATUS_SUCCESS) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
            "mod_audio_stream: failed to reserve event subclasses\n");
        return SWITCH_STATUS_TERM;
    }

    SWITCH_ADD_API(api_interface, "uuid_audio_stream",
                   "Audio stream API", stream_function, STREAM_API_SYNTAX);

    /* Console tab-completion hints */
    switch_console_set_complete(
        "add uuid_audio_stream ::console::list_uuid start wss-url mono 8k");
    switch_console_set_complete(
        "add uuid_audio_stream ::console::list_uuid start wss-url mixed 16k");
    switch_console_set_complete(
        "add uuid_audio_stream ::console::list_uuid start wss-url stereo 16k");
    switch_console_set_complete(
        "add uuid_audio_stream ::console::list_uuid stop");
    switch_console_set_complete(
        "add uuid_audio_stream ::console::list_uuid pause");
    switch_console_set_complete(
        "add uuid_audio_stream ::console::list_uuid resume");
    switch_console_set_complete(
        "add uuid_audio_stream ::console::list_uuid send_text");
    switch_console_set_complete(
        "add uuid_audio_stream ::console::list_uuid playback_pause");
    switch_console_set_complete(
        "add uuid_audio_stream ::console::list_uuid playback_resume");
    switch_console_set_complete(
        "add uuid_audio_stream ::console::list_uuid break");

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
        "mod_audio_stream loaded successfully\n");
    return SWITCH_STATUS_SUCCESS;
}

/* ── Module shutdown ─────────────────────────────────────────────────────── */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_audio_stream_shutdown)
{
    switch_event_free_subclass(EVENT_JSON);
    switch_event_free_subclass(EVENT_CONNECT);
    switch_event_free_subclass(EVENT_DISCONNECT);
    switch_event_free_subclass(EVENT_ERROR);
    switch_event_free_subclass(EVENT_PLAY);
    switch_event_free_subclass(EVENT_PLAYBACK_START);
    switch_event_free_subclass(EVENT_PLAYBACK_STOP);
    return SWITCH_STATUS_SUCCESS;
}
