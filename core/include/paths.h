#ifndef AW_PATHS_H
#define AW_PATHS_H

/* Paths are SD-root relative. The platform Fs adapter maps them (for example by prefixing "sdmc:").
 * The platform must create AW_DATA_DIR (and its parents) before watcher_init: the Fs contract has no mkdir. */
#define AW_DATA_DIR       "/config/aw-watcher-switch"

#define AW_PATH_SESSIONS  AW_DATA_DIR "/sessions.jsonl"
#define AW_PATH_STATE_A   AW_DATA_DIR "/state.a"
#define AW_PATH_STATE_B   AW_DATA_DIR "/state.b"
#define AW_PATH_DIAG_FLAG AW_DATA_DIR "/diag"
#define AW_PATH_DIAG_LOG  AW_DATA_DIR "/diag.jsonl"

#endif
