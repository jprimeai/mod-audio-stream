/*
 * audio_streamer_glue.cpp
 *
 * Glue layer between the FreeSWITCH media-bug and the WebSocket client.
 *
 * Key capabilities
 * ─────────────────
 * • Full-duplex: audio is streamed to the WS server while responses are played
 *   back independently on the same channel.
 * • Base64-encoded JSON payloads ({"type":"streamAudio", "data":{…}}) AND raw
 *   binary WebSocket frames are both supported as inbound audio sources.
 * • Supported return formats: raw (r8/r16/r24/r32/r48/r64), wav, mp3, ogg,
 *   opus, pcmu, pcma.
 * • Playback can be paused, resumed and drained (break) independently of the
 *   upload stream.
 * • Buffer pool reduces heap pressure on heavy-load deployments.
 * • EVENT_PLAYBACK_START is fired (with segment index and queue depth) every
 *   time a new audio segment is ready for playback.
 * • EVENT_PLAYBACK_STOP is fired when the break/drain command is called.
 */

#include <string>
#include <cstring>
#include <fstream>
#include <deque>
#include <vector>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <atomic>

#include "mod_audio_stream.h"
#include "WebSocketClient.h"
#include <switch_json.h>
#include <switch_buffer.h>
#include "base64.h"

#define FRAME_SIZE_8000  320   /* 20 ms @ 8 kHz, 16-bit mono = 320 bytes */

/* ═══════════════════════════════════════════════════════════════════════════
 * BufferPool
 *
 * A simple free-list of pre-cleared byte vectors.  Acquired vectors are
 * returned to the pool after each media-bug callback, avoiding repeated heap
 * allocation under heavy concurrent load.
 * ═══════════════════════════════════════════════════════════════════════════ */
class BufferPool {
public:
    /* Acquire a vector, preferring a recycled one from the pool. */
    std::vector<uint8_t> acquire() {
        std::lock_guard<std::mutex> lk(m_mu);
        if (!m_pool.empty()) {
            auto v = std::move(m_pool.back());
            m_pool.pop_back();
            v.clear();
            return v;
        }
        return {};
    }

    /* Return a used vector back to the pool (cleared internally). */
    void release(std::vector<uint8_t> &&v) {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_pool.size() < MAX_POOL) {
            v.clear();
            m_pool.push_back(std::move(v));
        }
    }

private:
    static constexpr size_t MAX_POOL = 32;
    std::vector<std::vector<uint8_t>> m_pool;
    std::mutex m_mu;
};


/* ═══════════════════════════════════════════════════════════════════════════
 * AudioStreamer
 * ═══════════════════════════════════════════════════════════════════════════ */
class AudioStreamer {
public:
    /* ── Factory ─────────────────────────────────────────────────────────── */
    static std::shared_ptr<AudioStreamer> create(
        const char *uuid,
        const char *wsUri,
        responseHandler_t callback,
        int deflate,
        int heart_beat,
        bool suppressLog,
        int sampling,
        const char *extra_headers,
        const char *tls_cafile,
        const char *tls_keyfile,
        const char *tls_certfile,
        bool tls_disable_hostname_validation)
    {
        std::shared_ptr<AudioStreamer> sp(new AudioStreamer(
            uuid, wsUri, callback, deflate, heart_beat,
            suppressLog, sampling, extra_headers,
            tls_cafile, tls_keyfile, tls_certfile,
            tls_disable_hostname_validation));

        sp->bindCallbacks(std::weak_ptr<AudioStreamer>(sp));
        sp->client.connect();
        return sp;
    }

    ~AudioStreamer() = default;

    /* ── Transport ───────────────────────────────────────────────────────── */
    void disconnect() {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "AudioStreamer: disconnecting\n");
        client.disconnect();
    }

    bool isConnected() {
        return client.isConnected();
    }

    void writeBinary(const uint8_t *buffer, size_t len) {
        if (!isConnected()) return;
        client.sendBinary(buffer, len);
    }

    void writeText(const char *text) {
        if (!isConnected()) return;
        client.sendMessage(text, strlen(text));
    }

    /* ── File / resource cleanup ─────────────────────────────────────────── */
    void deleteFiles() {
        std::vector<std::string> files;
        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            if (m_Files.empty()) return;
            files.assign(m_Files.begin(), m_Files.end());
            m_Files.clear();
            m_playbackQueue.clear();
            m_playFile = 0;
        }
        for (const auto &fn : files) {
            ::remove(fn.c_str());
        }
    }

    /* Detach all WS callbacks so no events fire after this point. */
    void markCleanedUp() {
        m_cleanedUp.store(true, std::memory_order_release);
        client.setMessageCallback({});
        client.setBinaryCallback({});
        client.setOpenCallback({});
        client.setErrorCallback({});
        client.setCloseCallback({});
    }

    bool isCleanedUp() const {
        return m_cleanedUp.load(std::memory_order_acquire);
    }

    /* ── Playback queue control ──────────────────────────────────────────── */

    /* Pause or resume queuing of new play events (upload stream is unaffected). */
    void pausePlayback(bool pause) {
        m_playbackPaused.store(pause, std::memory_order_release);
    }

    bool isPlaybackPaused() const {
        return m_playbackPaused.load(std::memory_order_acquire);
    }

    /* Drain: clear the pending queue and arm the drain flag.
     * New audio arriving while drain is armed will be discarded.
     * Call resetDrain() (via playback_resume) to re-enable playback. */
    int drainPlayback() {
        m_drainRequested.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lk(m_stateMutex);
        int cleared = static_cast<int>(m_playbackQueue.size());
        m_playbackQueue.clear();
        return cleared;
    }

    /* Reset the drain flag – called by playback_resume. */
    void resetDrain() {
        m_drainRequested.store(false, std::memory_order_release);
    }

    bool isDrainRequested() const {
        return m_drainRequested.load(std::memory_order_acquire);
    }

    int getPlaybackQueueDepth() {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        return static_cast<int>(m_playbackQueue.size());
    }

    /* ── Buffer pool accessor (used by stream_frame) ─────────────────────── */
    BufferPool &getBufferPool() { return m_bufferPool; }

    /* ─────────────────────────────────────────────────────────────────────── */
private:
    /* ── Constructor ─────────────────────────────────────────────────────── */
    AudioStreamer(
        const char *uuid,
        const char *wsUri,
        responseHandler_t callback,
        int deflate,
        int heart_beat,
        bool suppressLog,
        int sampling,
        const char *extra_headers,
        const char *tls_cafile,
        const char *tls_keyfile,
        const char *tls_certfile,
        bool tls_disable_hostname_validation)
        : m_sessionId(uuid),
          m_notify(callback),
          m_suppress_log(suppressLog),
          m_extra_headers(extra_headers),
          m_playFile(0),
          m_sampling(sampling)
    {
        WebSocketHeaders hdrs;
        WebSocketTLSOptions tls;

        if (m_extra_headers) {
            cJSON *headers_json = cJSON_Parse(m_extra_headers);
            if (headers_json) {
                cJSON *it = headers_json->child;
                while (it) {
                    if (it->type == cJSON_String && it->valuestring)
                        hdrs.set(it->string, it->valuestring);
                    it = it->next;
                }
                cJSON_Delete(headers_json);
            }
        }

        client.setUrl(wsUri);

        if (tls_cafile)  tls.caFile   = tls_cafile;
        if (tls_keyfile) tls.keyFile  = tls_keyfile;
        if (tls_certfile) tls.certFile = tls_certfile;
        tls.disableHostnameValidation = tls_disable_hostname_validation;
        client.setTLSOptions(tls);

        if (heart_beat)  client.setPingInterval(heart_beat);
        if (deflate)     client.enableCompression(false);
        if (!hdrs.empty()) client.setHeaders(hdrs);
    }

    /* ── Internal result type for processMessage ─────────────────────────── */
    struct ProcessResult {
        switch_bool_t ok = SWITCH_FALSE;
        std::string   rewrittenJsonData;
        std::vector<std::string> errors;
        int segmentIdx  = 0;
        int queueDepth  = 0;
    };

    static void push_err(ProcessResult &out,
                         const std::string &sid,
                         const std::string &s)
    {
        out.errors.push_back("(" + sid + ") " + s);
    }

    /* ── WS callback binding ─────────────────────────────────────────────── */
    void bindCallbacks(std::weak_ptr<AudioStreamer> wp) {

        /* Text frames – JSON envelopes carrying base64 audio or control msgs */
        client.setMessageCallback([wp](const std::string &message) {
            auto self = wp.lock();
            if (!self || self->isCleanedUp()) return;
            self->eventCallback(MESSAGE, message.c_str());
        });

        /* Binary frames – raw audio (no base64/JSON overhead).
         * No EVENT_JSON is fired for binary frames. */
        client.setBinaryCallback([wp](const void *data, size_t len) {
            auto self = wp.lock();
            if (!self || self->isCleanedUp()) return;
            if (self->isDrainRequested() || self->isPlaybackPaused()) return;
            self->handleBinaryAudio(data, len);
        });

        client.setOpenCallback([wp]() {
            auto self = wp.lock();
            if (!self || self->isCleanedUp()) return;

            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "connected");
            char *json_str = cJSON_PrintUnformatted(root);
            self->eventCallback(CONNECT_SUCCESS, json_str);
            cJSON_Delete(root);
            switch_safe_free(json_str);
        });

        client.setErrorCallback([wp](int code, const std::string &msg) {
            auto self = wp.lock();
            if (!self || self->isCleanedUp()) return;

            cJSON *root    = cJSON_CreateObject();
            cJSON *message = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "error");
            cJSON_AddNumberToObject(message, "code", code);
            cJSON_AddStringToObject(message, "error", msg.c_str());
            cJSON_AddItemToObject(root, "message", message);
            char *json_str = cJSON_PrintUnformatted(root);
            self->eventCallback(CONNECT_ERROR, json_str);
            cJSON_Delete(root);
            switch_safe_free(json_str);
        });

        client.setCloseCallback([wp](int code, const std::string &reason) {
            auto self = wp.lock();
            if (!self || self->isCleanedUp()) return;

            cJSON *root    = cJSON_CreateObject();
            cJSON *message = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "status", "disconnected");
            cJSON_AddNumberToObject(message, "code", code);
            cJSON_AddStringToObject(message, "reason", reason.c_str());
            cJSON_AddItemToObject(root, "message", message);
            char *json_str = cJSON_PrintUnformatted(root);
            self->eventCallback(CONNECTION_DROPPED, json_str);
            cJSON_Delete(root);
            switch_safe_free(json_str);
        });
    }

    /* ── Raw binary audio from WebSocket ─────────────────────────────────── */
    /*
     * Binary frames contain raw PCM audio at the session's negotiated sample
     * rate.  The extension is chosen to match so FreeSWITCH knows how to
     * render the file.
     */
    void handleBinaryAudio(const void *data, size_t len) {
        if (!data || len == 0) return;

        const char *ext = rawExtForRate(m_sampling);
        if (!ext) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                "(%s) handleBinaryAudio: unknown sampling rate %d – defaulting to .r8\n",
                m_sessionId.c_str(), m_sampling);
            ext = ".r8";
        }

        int idx = 0;
        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            idx = m_playFile++;
        }

        char filePath[SWITCH_MAX_FILENAME];
        switch_snprintf(filePath, sizeof(filePath), "%s%s%s_%d.tmp%s",
            SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR,
            m_sessionId.c_str(), idx, ext);

        {
            std::ofstream f(filePath, std::ios::binary);
            if (!f.is_open()) {
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
                    "(%s) handleBinaryAudio: cannot open %s for write\n",
                    m_sessionId.c_str(), filePath);
                return;
            }
            f.write(static_cast<const char *>(data),
                    static_cast<std::streamsize>(len));
        }

        int queueDepth = 0;
        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            m_Files.insert(filePath);
            m_playbackQueue.push_back(filePath);
            queueDepth = static_cast<int>(m_playbackQueue.size());
        }

        /* Build a play-event JSON payload similar to processMessage output */
        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "file", filePath);
        cJSON_AddStringToObject(root, "audioDataType", "raw");
        cJSON_AddNumberToObject(root, "sampleRate", m_sampling);
        cJSON_AddNumberToObject(root, "segment", idx);
        cJSON_AddNumberToObject(root, "queueDepth", queueDepth);
        char *json_str = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);

        if (json_str) {
            eventCallback(BINARY_AUDIO, json_str);
            switch_safe_free(json_str);
        }
    }

    /* ── Helpers ─────────────────────────────────────────────────────────── */
    static const char *rawExtForRate(int hz) {
        switch (hz) {
            case  8000: return ".r8";
            case 16000: return ".r16";
            case 24000: return ".r24";
            case 32000: return ".r32";
            case 48000: return ".r48";
            case 64000: return ".r64";
            default:    return nullptr;
        }
    }

    switch_media_bug_t *get_media_bug(switch_core_session_t *session) {
        switch_channel_t *channel = switch_core_session_get_channel(session);
        if (!channel) return nullptr;
        return static_cast<switch_media_bug_t *>(
            switch_channel_get_private(channel, MY_BUG_NAME));
    }

    void media_bug_close(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if (bug) {
            auto *tech_pvt =
                static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
            tech_pvt->close_requested = 1;
            switch_core_media_bug_close(&bug, SWITCH_FALSE);
        }
    }

    void send_initial_metadata(switch_core_session_t *session) {
        auto *bug = get_media_bug(session);
        if (!bug) return;
        auto *tech_pvt =
            static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
        if (tech_pvt && strlen(tech_pvt->initialMetadata) > 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG, "sending initial metadata %s\n",
                tech_pvt->initialMetadata);
            writeText(tech_pvt->initialMetadata);
        }
    }

    /* ── Central event dispatcher ────────────────────────────────────────── */
    void eventCallback(notifyEvent_t event, const char *message) {
        std::string msg = message ? message : "";

        ProcessResult pr;
        if (event == MESSAGE) {
            pr = processMessage(msg);
            if (pr.ok == SWITCH_TRUE) {
                msg = pr.rewrittenJsonData;
                /* Suppress the play event if playback is paused or being drained */
                if (m_drainRequested.load(std::memory_order_acquire) ||
                    m_playbackPaused.load(std::memory_order_acquire)) {
                    pr.ok = SWITCH_FALSE;
                }
            }
        }

        switch_core_session_t *psession =
            switch_core_session_locate(m_sessionId.c_str());
        if (!psession) return;

        for (const auto &e : pr.errors) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession),
                SWITCH_LOG_ERROR, "%s\n", e.c_str());
        }

        switch (event) {

            case CONNECT_SUCCESS:
                send_initial_metadata(psession);
                m_notify(psession, EVENT_CONNECT, msg.c_str());
                break;

            case CONNECTION_DROPPED:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession),
                    SWITCH_LOG_INFO, "connection closed\n");
                m_notify(psession, EVENT_DISCONNECT, msg.c_str());
                break;

            case CONNECT_ERROR:
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession),
                    SWITCH_LOG_INFO, "connection error\n");
                m_notify(psession, EVENT_ERROR, msg.c_str());
                media_bug_close(psession);
                break;

            case MESSAGE:
                if (pr.ok == SWITCH_TRUE) {
                    /* Legacy play event (kept for backward compatibility) */
                    m_notify(psession, EVENT_PLAY, msg.c_str());
                    /* New per-segment tracking event */
                    m_notify(psession, EVENT_PLAYBACK_START, msg.c_str());
                } else {
                    m_notify(psession, EVENT_JSON, msg.c_str());
                }
                if (!m_suppress_log) {
                    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession),
                        SWITCH_LOG_DEBUG, "response: %s\n", msg.c_str());
                }
                break;

            case BINARY_AUDIO:
                /* Raw binary audio: fire play events only – no JSON event */
                m_notify(psession, EVENT_PLAY, msg.c_str());
                m_notify(psession, EVENT_PLAYBACK_START, msg.c_str());
                break;
        }

        switch_core_session_rwunlock(psession);
    }

    /* ── JSON streamAudio processor ─────────────────────────────────────── */
    /*
     * Parses a {"type":"streamAudio","data":{…}} message, decodes the base64
     * audio payload, writes it to a temp file, tracks it in the playback queue,
     * and rewrites the JSON to include "file", "segment" and "queueDepth".
     *
     * Supported audioDataType values:
     *   raw  (requires sampleRate), wav, mp3, ogg, opus, pcmu, pcma
     */
    ProcessResult processMessage(const std::string &message) {
        ProcessResult out;

        using jsonPtr = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
        jsonPtr root(cJSON_Parse(message.c_str()), &cJSON_Delete);
        if (!root) return out;

        const char *jsonType = cJSON_GetObjectCstr(root.get(), "type");
        if (!jsonType || std::strcmp(jsonType, "streamAudio") != 0)
            return out;

        cJSON *jsonData = cJSON_GetObjectItem(root.get(), "data");
        if (!jsonData) {
            push_err(out, m_sessionId, "processMessage: no 'data' in streamAudio");
            return out;
        }

        const char *jsAudioDataType =
            cJSON_GetObjectCstr(jsonData, "audioDataType");
        if (!jsAudioDataType) jsAudioDataType = "";

        jsonPtr jsonAudio(
            cJSON_DetachItemFromObject(jsonData, "audioData"), &cJSON_Delete);
        if (!jsonAudio) {
            push_err(out, m_sessionId,
                "processMessage: streamAudio missing 'audioData' field");
            return out;
        }
        if (!cJSON_IsString(jsonAudio.get()) || !jsonAudio->valuestring) {
            push_err(out, m_sessionId,
                "processMessage: 'audioData' is not a string (expected base64)");
            return out;
        }

        /* sampleRate is required for raw PCM types */
        int sampleRate = 0;
        if (cJSON *jr = cJSON_GetObjectItem(jsonData, "sampleRate"))
            sampleRate = jr->valueint;

        /* Map audioDataType → file extension */
        std::string fileType;
        if (std::strcmp(jsAudioDataType, "raw") == 0) {
            const char *ext = rawExtForRate(sampleRate);
            if (!ext) {
                push_err(out, m_sessionId,
                    "processMessage: unsupported raw sampleRate: " +
                    std::to_string(sampleRate));
                return out;
            }
            fileType = ext;
        } else if (std::strcmp(jsAudioDataType, "wav")  == 0) fileType = ".wav";
        else if (std::strcmp(jsAudioDataType, "mp3")    == 0) fileType = ".mp3";
        else if (std::strcmp(jsAudioDataType, "ogg")    == 0) fileType = ".ogg";
        else if (std::strcmp(jsAudioDataType, "opus")   == 0) fileType = ".opus";
        else if (std::strcmp(jsAudioDataType, "pcmu")   == 0) fileType = ".pcmu";
        else if (std::strcmp(jsAudioDataType, "pcma")   == 0) fileType = ".pcma";
        else {
            push_err(out, m_sessionId,
                "processMessage: unsupported audioDataType: " +
                std::string(jsAudioDataType));
            return out;
        }

        /* Decode base64 payload */
        std::string decoded;
        try {
            decoded = base64_decode(jsonAudio->valuestring);
        } catch (const std::exception &e) {
            push_err(out, m_sessionId,
                "processMessage: base64 decode error: " + std::string(e.what()));
            return out;
        }

        int idx = 0;
        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            idx = m_playFile++;
        }

        char filePath[SWITCH_MAX_FILENAME];
        switch_snprintf(filePath, sizeof(filePath), "%s%s%s_%d.tmp%s",
            SWITCH_GLOBAL_dirs.temp_dir, SWITCH_PATH_SEPARATOR,
            m_sessionId.c_str(), idx, fileType.c_str());

        /* Write decoded audio to disk */
        {
            std::ofstream f(filePath, std::ios::binary);
            if (!f.is_open()) {
                push_err(out, m_sessionId,
                    std::string("processMessage: cannot open for write: ") + filePath);
                return out;
            }
            f.write(decoded.data(), static_cast<std::streamsize>(decoded.size()));
            if (!f.good()) {
                push_err(out, m_sessionId,
                    std::string("processMessage: write failed: ") + filePath);
                return out;
            }
        }

        /* Track for cleanup and queue depth reporting */
        int queueDepth = 0;
        {
            std::lock_guard<std::mutex> lk(m_stateMutex);
            m_Files.insert(filePath);
            m_playbackQueue.push_back(filePath);
            queueDepth = static_cast<int>(m_playbackQueue.size());
        }

        out.segmentIdx = idx;
        out.queueDepth = queueDepth;

        /* Augment the data object with derived fields */
        cJSON_AddStringToObject(jsonData, "file",       filePath);
        cJSON_AddNumberToObject(jsonData, "segment",    idx);
        cJSON_AddNumberToObject(jsonData, "queueDepth", queueDepth);

        char *jsonString = cJSON_PrintUnformatted(jsonData);
        if (!jsonString) {
            push_err(out, m_sessionId,
                "processMessage: cJSON_PrintUnformatted failed");
            return out;
        }
        out.rewrittenJsonData.assign(jsonString);
        std::free(jsonString);
        out.ok = SWITCH_TRUE;
        return out;
    }

    /* ── Member variables ────────────────────────────────────────────────── */
    std::string           m_sessionId;
    responseHandler_t     m_notify;
    WebSocketClient       client;
    bool                  m_suppress_log;
    const char           *m_extra_headers;
    int                   m_playFile;    /* monotonic segment counter          */
    int                   m_sampling;   /* session's target sample rate (Hz)  */

    std::unordered_set<std::string> m_Files;         /* all temp files (cleanup)   */
    std::deque<std::string>         m_playbackQueue; /* pending play segments      */

    std::atomic<bool> m_cleanedUp{false};
    std::atomic<bool> m_playbackPaused{false};
    std::atomic<bool> m_drainRequested{false};

    std::mutex   m_stateMutex;
    BufferPool   m_bufferPool;
};


/* ═══════════════════════════════════════════════════════════════════════════
 * Internal (anonymous-namespace) helpers
 * ═══════════════════════════════════════════════════════════════════════════ */
namespace {

    switch_status_t stream_data_init(
        private_t              *tech_pvt,
        switch_core_session_t  *session,
        char                   *wsUri,
        uint32_t                sampling,          /* codec native rate        */
        int                     desiredSampling,   /* target rate for WS       */
        int                     channels,
        char                   *metadata,
        responseHandler_t       responseHandler,
        int                     deflate,
        int                     heart_beat,
        bool                    suppressLog,
        int                     rtp_packets,
        const char             *extra_headers,
        const char             *tls_cafile,
        const char             *tls_keyfile,
        const char             *tls_certfile,
        bool                    tls_disable_hostname_validation)
    {
        int err;
        switch_memory_pool_t *pool = switch_core_session_get_pool(session);

        memset(tech_pvt, 0, sizeof(private_t));

        strncpy(tech_pvt->sessionId, switch_core_session_get_uuid(session),
                MAX_SESSION_ID - 1);
        strncpy(tech_pvt->ws_uri, wsUri, MAX_WS_URI - 1);
        tech_pvt->sampling       = desiredSampling;
        tech_pvt->responseHandler = responseHandler;
        tech_pvt->rtp_packets    = rtp_packets;
        tech_pvt->channels       = channels;
        tech_pvt->audio_paused   = 0;
        tech_pvt->playback_paused = 0;
        tech_pvt->drain_requested = 0;

        if (metadata)
            strncpy(tech_pvt->initialMetadata, metadata, MAX_METADATA_LEN - 1);

        const size_t buflen =
            (size_t)FRAME_SIZE_8000 * desiredSampling / 8000 * channels * rtp_packets;

        auto sp = AudioStreamer::create(
            tech_pvt->sessionId, wsUri, responseHandler,
            deflate, heart_beat, suppressLog, desiredSampling,
            extra_headers, tls_cafile, tls_keyfile,
            tls_certfile, tls_disable_hostname_validation);

        tech_pvt->pAudioStreamer = new std::shared_ptr<AudioStreamer>(sp);

        switch_mutex_init(&tech_pvt->mutex, SWITCH_MUTEX_NESTED, pool);

        if (switch_buffer_create(pool, &tech_pvt->sbuffer, buflen) !=
            SWITCH_STATUS_SUCCESS) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_ERROR, "%s: error creating switch buffer\n",
                tech_pvt->sessionId);
            return SWITCH_STATUS_FALSE;
        }

        if (desiredSampling != (int)sampling) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG, "(%s) resampling %u → %u\n",
                tech_pvt->sessionId, sampling, desiredSampling);
            tech_pvt->resampler = speex_resampler_init(
                channels, sampling, desiredSampling,
                SWITCH_RESAMPLE_QUALITY, &err);
            if (err != 0) {
                switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                    SWITCH_LOG_ERROR, "resampler init error: %s\n",
                    speex_resampler_strerror(err));
                return SWITCH_STATUS_FALSE;
            }
        } else {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_DEBUG, "(%s) no resampling needed\n",
                tech_pvt->sessionId);
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_DEBUG, "(%s) stream_data_init OK\n",
            tech_pvt->sessionId);
        return SWITCH_STATUS_SUCCESS;
    }

    void destroy_tech_pvt(private_t *tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
            "%s destroy_tech_pvt\n", tech_pvt->sessionId);
        if (tech_pvt->resampler) {
            speex_resampler_destroy(tech_pvt->resampler);
            tech_pvt->resampler = nullptr;
        }
        if (tech_pvt->mutex) {
            switch_mutex_destroy(tech_pvt->mutex);
            tech_pvt->mutex = nullptr;
        }
    }

} /* anonymous namespace */


/* ═══════════════════════════════════════════════════════════════════════════
 * Public C API
 * ═══════════════════════════════════════════════════════════════════════════ */
extern "C" {

/* ── URI validation ──────────────────────────────────────────────────────── */
int validate_ws_uri(const char *url, char *wsUri) {
    const char *hostStart = nullptr;
    const char *hostEnd   = nullptr;
    const char *portStart = nullptr;

    if (strncmp(url, "ws://", 5) == 0) {
        hostStart = url + 5;
    } else if (strncmp(url, "wss://", 6) == 0) {
        hostStart = url + 6;
    } else {
        return 0;
    }

    hostEnd = hostStart;
    while (*hostEnd && *hostEnd != ':' && *hostEnd != '/') {
        if (!std::isalnum((unsigned char)*hostEnd) &&
            *hostEnd != '-' && *hostEnd != '.') {
            return 0;
        }
        ++hostEnd;
    }
    if (hostStart == hostEnd) return 0;

    if (*hostEnd == ':') {
        portStart = hostEnd + 1;
        while (*portStart && *portStart != '/') {
            if (!std::isdigit((unsigned char)*portStart)) return 0;
            ++portStart;
        }
    }

    std::strncpy(wsUri, url, MAX_WS_URI - 1);
    wsUri[MAX_WS_URI - 1] = '\0';
    return 1;
}

/* ── UTF-8 validation ────────────────────────────────────────────────────── */
switch_status_t is_valid_utf8(const char *str) {
    while (*str) {
        if ((*str & 0x80) == 0x00) {
            str++;
        } else if ((*str & 0xE0) == 0xC0) {
            if ((str[1] & 0xC0) != 0x80) return SWITCH_STATUS_FALSE;
            str += 2;
        } else if ((*str & 0xF0) == 0xE0) {
            if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80)
                return SWITCH_STATUS_FALSE;
            str += 3;
        } else if ((*str & 0xF8) == 0xF0) {
            if ((str[1] & 0xC0) != 0x80 || (str[2] & 0xC0) != 0x80 ||
                (str[3] & 0xC0) != 0x80)
                return SWITCH_STATUS_FALSE;
            str += 4;
        } else {
            return SWITCH_STATUS_FALSE;
        }
    }
    return SWITCH_STATUS_SUCCESS;
}

/* ── Send text over WebSocket ────────────────────────────────────────────── */
switch_status_t stream_session_send_text(switch_core_session_t *session,
                                         char *text)
{
    switch_channel_t *channel =
        switch_core_session_get_channel(session);
    auto *bug = static_cast<switch_media_bug_t *>(
        switch_channel_get_private(channel, MY_BUG_NAME));
    if (!bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR, "stream_session_send_text: no bug\n");
        return SWITCH_STATUS_FALSE;
    }

    auto *tech_pvt =
        static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    std::shared_ptr<AudioStreamer> streamer;
    switch_mutex_lock(tech_pvt->mutex);
    if (tech_pvt->pAudioStreamer) {
        auto *sp_wrap =
            static_cast<std::shared_ptr<AudioStreamer> *>(tech_pvt->pAudioStreamer);
        if (sp_wrap && *sp_wrap) streamer = *sp_wrap;
    }
    switch_mutex_unlock(tech_pvt->mutex);

    if (streamer) {
        streamer->writeText(text);
        return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
}

/* ── Pause / resume the outbound (upload) stream ─────────────────────────── */
switch_status_t stream_session_pauseresume(switch_core_session_t *session,
                                            int pause)
{
    switch_channel_t *channel =
        switch_core_session_get_channel(session);
    auto *bug = static_cast<switch_media_bug_t *>(
        switch_channel_get_private(channel, MY_BUG_NAME));
    if (!bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR, "stream_session_pauseresume: no bug\n");
        return SWITCH_STATUS_FALSE;
    }

    auto *tech_pvt =
        static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    switch_core_media_bug_flush(bug);
    tech_pvt->audio_paused = pause;
    return SWITCH_STATUS_SUCCESS;
}

/* ── Pause / resume inbound (playback) stream independently ──────────────── */
/*
 * Playback pause prevents new play events from being fired while the outbound
 * upload stream continues uninterrupted.
 *
 * Resuming (pause=0) also resets any pending drain state so that segments
 * received after a prior break start playing immediately.
 */
switch_status_t stream_session_playback_pauseresume(
    switch_core_session_t *session, int pause)
{
    switch_channel_t *channel =
        switch_core_session_get_channel(session);
    auto *bug = static_cast<switch_media_bug_t *>(
        switch_channel_get_private(channel, MY_BUG_NAME));
    if (!bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR,
            "stream_session_playback_pauseresume: no bug\n");
        return SWITCH_STATUS_FALSE;
    }

    auto *tech_pvt =
        static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    std::shared_ptr<AudioStreamer> streamer;
    switch_mutex_lock(tech_pvt->mutex);
    if (tech_pvt->pAudioStreamer) {
        auto *sp_wrap =
            static_cast<std::shared_ptr<AudioStreamer> *>(tech_pvt->pAudioStreamer);
        if (sp_wrap && *sp_wrap) streamer = *sp_wrap;
    }
    switch_mutex_unlock(tech_pvt->mutex);

    tech_pvt->playback_paused = pause;

    if (streamer) {
        if (!pause) {
            streamer->resetDrain();        /* resume also clears any drain state */
            streamer->pausePlayback(false);
        } else {
            streamer->pausePlayback(true);
        }
        return SWITCH_STATUS_SUCCESS;
    }
    return SWITCH_STATUS_FALSE;
}

/* ── Break: drain the playback queue and fire EVENT_PLAYBACK_STOP ─────────── */
/*
 * The upload (outbound) stream is NOT interrupted.  After a break, playback
 * remains paused until stream_session_playback_pauseresume(…, 0) is called.
 */
switch_status_t stream_session_break(switch_core_session_t *session) {
    switch_channel_t *channel =
        switch_core_session_get_channel(session);
    auto *bug = static_cast<switch_media_bug_t *>(
        switch_channel_get_private(channel, MY_BUG_NAME));
    if (!bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR, "stream_session_break: no bug\n");
        return SWITCH_STATUS_FALSE;
    }

    auto *tech_pvt =
        static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt) return SWITCH_STATUS_FALSE;

    std::shared_ptr<AudioStreamer> streamer;
    switch_mutex_lock(tech_pvt->mutex);
    if (tech_pvt->pAudioStreamer) {
        auto *sp_wrap =
            static_cast<std::shared_ptr<AudioStreamer> *>(tech_pvt->pAudioStreamer);
        if (sp_wrap && *sp_wrap) streamer = *sp_wrap;
    }
    switch_mutex_unlock(tech_pvt->mutex);

    if (!streamer) return SWITCH_STATUS_FALSE;

    int cleared = streamer->drainPlayback();
    tech_pvt->drain_requested  = 1;
    tech_pvt->playback_paused  = 1;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_INFO,
        "stream_session_break: drained %d pending segment(s)\n", cleared);

    /* Notify ESL so it can stop whatever is currently playing */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "stopped");
    cJSON_AddNumberToObject(root, "cleared", cleared);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json_str) {
        tech_pvt->responseHandler(session, EVENT_PLAYBACK_STOP, json_str);
        switch_safe_free(json_str);
    }

    return SWITCH_STATUS_SUCCESS;
}

/* ── Session initialisation ──────────────────────────────────────────────── */
switch_status_t stream_session_init(
    switch_core_session_t *session,
    responseHandler_t      responseHandler,
    uint32_t               samples_per_second,
    char                  *wsUri,
    int                    sampling,
    int                    channels,
    char                  *metadata,
    void                 **ppUserData)
{
    int  deflate    = 0;
    int  heart_beat = 0;
    bool suppressLog = false;
    int  rtp_packets = 1;
    const char *buffer_size  = nullptr;
    const char *extra_headers = nullptr;
    const char *tls_cafile   = nullptr;
    const char *tls_keyfile  = nullptr;
    const char *tls_certfile = nullptr;
    bool tls_disable_hostname_validation = false;

    switch_channel_t *channel = switch_core_session_get_channel(session);

    if (switch_channel_var_true(channel, "STREAM_MESSAGE_DEFLATE"))
        deflate = 1;

    if (switch_channel_var_true(channel, "STREAM_SUPPRESS_LOG"))
        suppressLog = true;

    tls_cafile   = switch_channel_get_variable(channel, "STREAM_TLS_CA_FILE");
    tls_keyfile  = switch_channel_get_variable(channel, "STREAM_TLS_KEY_FILE");
    tls_certfile = switch_channel_get_variable(channel, "STREAM_TLS_CERT_FILE");

    if (switch_channel_var_true(channel, "STREAM_TLS_DISABLE_HOSTNAME_VALIDATION"))
        tls_disable_hostname_validation = true;

    const char *heartBeat =
        switch_channel_get_variable(channel, "STREAM_HEART_BEAT");
    if (heartBeat) {
        char *endptr;
        long value = strtol(heartBeat, &endptr, 10);
        if (*endptr == '\0' && value > 0 && value <= INT_MAX)
            heart_beat = static_cast<int>(value);
    }

    if ((buffer_size =
         switch_channel_get_variable(channel, "STREAM_BUFFER_SIZE"))) {
        int bSize = atoi(buffer_size);
        if (bSize % 20 != 0) {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
                SWITCH_LOG_WARNING,
                "%s: STREAM_BUFFER_SIZE %s is not a multiple of 20 ms – "
                "using default 20 ms\n",
                switch_channel_get_name(channel), buffer_size);
        } else if (bSize >= 20) {
            rtp_packets = bSize / 20;
        }
    }

    extra_headers =
        switch_channel_get_variable(channel, "STREAM_EXTRA_HEADERS");

    auto *tech_pvt = static_cast<private_t *>(
        switch_core_session_alloc(session, sizeof(private_t)));
    if (!tech_pvt) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_ERROR, "error allocating tech_pvt\n");
        return SWITCH_STATUS_FALSE;
    }

    if (stream_data_init(tech_pvt, session, wsUri,
                         samples_per_second, sampling, channels,
                         metadata, responseHandler,
                         deflate, heart_beat, suppressLog, rtp_packets,
                         extra_headers, tls_cafile, tls_keyfile, tls_certfile,
                         tls_disable_hostname_validation) !=
        SWITCH_STATUS_SUCCESS) {
        destroy_tech_pvt(tech_pvt);
        return SWITCH_STATUS_FALSE;
    }

    *ppUserData = tech_pvt;
    return SWITCH_STATUS_SUCCESS;
}

/* ── Per-frame media-bug callback ────────────────────────────────────────── */
/*
 * Called every 20 ms (or every rtp_packets*20 ms when buffering is enabled).
 * Audio is read from the media bug, optionally resampled, then sent to the
 * WebSocket.  The buffer pool is used to avoid per-call heap allocation.
 */
switch_bool_t stream_frame(switch_media_bug_t *bug) {
    auto *tech_pvt =
        static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));
    if (!tech_pvt) return SWITCH_TRUE;
    if (tech_pvt->audio_paused || tech_pvt->cleanup_started) return SWITCH_TRUE;

    std::shared_ptr<AudioStreamer> streamer;
    std::vector<std::vector<uint8_t>> pending_send;

    if (switch_mutex_trylock(tech_pvt->mutex) != SWITCH_STATUS_SUCCESS)
        return SWITCH_TRUE;

    if (!tech_pvt->pAudioStreamer) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
    }

    auto *sp_ptr =
        static_cast<std::shared_ptr<AudioStreamer> *>(tech_pvt->pAudioStreamer);
    if (!sp_ptr || !(*sp_ptr)) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_TRUE;
    }
    streamer = *sp_ptr;

    auto       *resampler  = tech_pvt->resampler;
    const int   channels   = tech_pvt->channels;
    const int   rtp_packets = tech_pvt->rtp_packets;
    auto       &pool        = streamer->getBufferPool();

    /* ── No resampling path ── */
    if (resampler == nullptr) {

        uint8_t data_buf[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame = {};
        frame.data   = data_buf;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) ==
               SWITCH_STATUS_SUCCESS) {
            if (!frame.datalen) continue;

            if (rtp_packets == 1) {
                auto buf = pool.acquire();
                buf.assign(static_cast<uint8_t *>(frame.data),
                           static_cast<uint8_t *>(frame.data) + frame.datalen);
                pending_send.push_back(std::move(buf));
                continue;
            }

            if (switch_buffer_freespace(tech_pvt->sbuffer) >= frame.datalen)
                switch_buffer_write(tech_pvt->sbuffer, frame.data, frame.datalen);

            if (switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
                switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
                if (inuse > 0) {
                    auto buf = pool.acquire();
                    buf.resize(inuse);
                    switch_buffer_read(tech_pvt->sbuffer, buf.data(), inuse);
                    switch_buffer_zero(tech_pvt->sbuffer);
                    pending_send.push_back(std::move(buf));
                }
            }
        }

    /* ── Resampling path ── */
    } else {

        uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
        switch_frame_t frame = {};
        frame.data   = data;
        frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

        while (switch_core_media_bug_read(bug, &frame, SWITCH_TRUE) ==
               SWITCH_STATUS_SUCCESS) {
            if (!frame.datalen) continue;

            const size_t freespace = switch_buffer_freespace(tech_pvt->sbuffer);
            spx_uint32_t in_len  = frame.samples;
            spx_uint32_t out_len = static_cast<spx_uint32_t>(
                freespace / (channels * sizeof(spx_int16_t)));

            if (out_len == 0) {
                if (freespace == 0) {
                    switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
                    if (inuse > 0) {
                        auto buf = pool.acquire();
                        buf.resize(inuse);
                        switch_buffer_read(tech_pvt->sbuffer, buf.data(), inuse);
                        switch_buffer_zero(tech_pvt->sbuffer);
                        pending_send.push_back(std::move(buf));
                    }
                }
                continue;
            }

            std::vector<spx_int16_t> out(
                static_cast<size_t>(out_len) * static_cast<size_t>(channels));

            if (channels == 1) {
                speex_resampler_process_int(
                    resampler, 0,
                    reinterpret_cast<const spx_int16_t *>(frame.data), &in_len,
                    out.data(), &out_len);
            } else {
                speex_resampler_process_interleaved_int(
                    resampler,
                    reinterpret_cast<const spx_int16_t *>(frame.data), &in_len,
                    out.data(), &out_len);
            }

            if (out_len == 0) continue;

            const size_t bytes_written =
                static_cast<size_t>(out_len) * static_cast<size_t>(channels) *
                sizeof(spx_int16_t);

            if (rtp_packets == 1) {
                auto buf = pool.acquire();
                const auto *p = reinterpret_cast<const uint8_t *>(out.data());
                buf.assign(p, p + bytes_written);
                pending_send.push_back(std::move(buf));
                continue;
            }

            if (bytes_written <= switch_buffer_freespace(tech_pvt->sbuffer))
                switch_buffer_write(tech_pvt->sbuffer,
                                    reinterpret_cast<const uint8_t *>(out.data()),
                                    bytes_written);

            if (switch_buffer_freespace(tech_pvt->sbuffer) == 0) {
                switch_size_t inuse = switch_buffer_inuse(tech_pvt->sbuffer);
                if (inuse > 0) {
                    auto buf = pool.acquire();
                    buf.resize(inuse);
                    switch_buffer_read(tech_pvt->sbuffer, buf.data(), inuse);
                    switch_buffer_zero(tech_pvt->sbuffer);
                    pending_send.push_back(std::move(buf));
                }
            }
        }
    }

    switch_mutex_unlock(tech_pvt->mutex);

    if (!streamer || !streamer->isConnected()) return SWITCH_TRUE;

    for (auto &chunk : pending_send) {
        if (!chunk.empty())
            streamer->writeBinary(chunk.data(), chunk.size());
        pool.release(std::move(chunk));   /* return to pool for next call */
    }

    return SWITCH_TRUE;
}

/* ── Session teardown ────────────────────────────────────────────────────── */
switch_status_t stream_session_cleanup(switch_core_session_t *session,
                                        char *text,
                                        int channelIsClosing)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);
    auto *bug = static_cast<switch_media_bug_t *>(
        switch_channel_get_private(channel, MY_BUG_NAME));

    if (!bug) {
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
            SWITCH_LOG_DEBUG,
            "stream_session_cleanup: no bug – already closed\n");
        return SWITCH_STATUS_FALSE;
    }

    auto *tech_pvt =
        static_cast<private_t *>(switch_core_media_bug_get_user_data(bug));

    char sessionId[MAX_SESSION_ID];
    strncpy(sessionId, tech_pvt->sessionId, MAX_SESSION_ID - 1);

    std::shared_ptr<AudioStreamer> *sp_wrap = nullptr;
    std::shared_ptr<AudioStreamer>  streamer;

    switch_mutex_lock(tech_pvt->mutex);

    if (tech_pvt->cleanup_started) {
        switch_mutex_unlock(tech_pvt->mutex);
        return SWITCH_STATUS_SUCCESS;
    }
    tech_pvt->cleanup_started = 1;

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_DEBUG, "(%s) stream_session_cleanup\n", sessionId);

    switch_channel_set_private(channel, MY_BUG_NAME, nullptr);

    sp_wrap = static_cast<std::shared_ptr<AudioStreamer> *>(
        tech_pvt->pAudioStreamer);
    tech_pvt->pAudioStreamer = nullptr;

    if (sp_wrap && *sp_wrap)
        streamer = *sp_wrap;

    switch_mutex_unlock(tech_pvt->mutex);

    if (!channelIsClosing)
        switch_core_media_bug_remove(session, &bug);

    delete sp_wrap;

    if (streamer) {
        streamer->deleteFiles();
        if (text) streamer->writeText(text);
        streamer->markCleanedUp();
        streamer->disconnect();
    }

    destroy_tech_pvt(tech_pvt);

    switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session),
        SWITCH_LOG_INFO, "(%s) stream_session_cleanup: done\n", sessionId);
    return SWITCH_STATUS_SUCCESS;
}

} /* extern "C" */
