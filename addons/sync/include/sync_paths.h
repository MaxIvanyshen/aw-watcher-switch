#ifndef AWSYNC_PATHS_H
#define AWSYNC_PATHS_H
#define SYNC_DATA_DIR      "/config/aw-watcher-switch-sync"
#define SYNC_PATH_CONFIG   SYNC_DATA_DIR "/config.txt"
#define SYNC_PATH_JOURNAL  "/config/aw-watcher-switch/sessions.jsonl"   /* same string as AW_PATH_SESSIONS, read only */
#define SYNC_NAME_CKPT_A   "sync.a"
#define SYNC_NAME_CKPT_B   "sync.b"
#define SYNC_NAME_STATUS   "status.json"
#define SYNC_NAME_SKIPPED  "skipped.log"   /* reserved, unused: skips are counted in status.json */
#endif
