-- WIMI 客户端 SQLite 表结构初始化脚本。
-- 本文件需要与 SqliteClientRepository::kCurrentSchemaVersion 保持一致。

PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA busy_timeout = 5000;

BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS account_identity (
  -- 单行表；网络登录成功后首次写入，后续登录必须使用相同 uid。
  singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
  uid INTEGER NOT NULL CHECK (uid > 0),
  username TEXT NOT NULL DEFAULT ''
);

CREATE TABLE IF NOT EXISTS conversations (
  -- 本地稳定会话键，例如 direct:<uid> 或 group:<groupId>。
  conversation_id TEXT PRIMARY KEY,
  -- 会话列表和会话标题栏展示的名称。
  title TEXT NOT NULL,
  -- 会话列表展示的最后一条消息摘要。
  preview TEXT NOT NULL DEFAULT '',
  -- 会话列表摘要旁展示的时间文本。
  timestamp TEXT NOT NULL DEFAULT '',
  -- 头像背景色；在正式头像资源可用前用于生成占位头像。
  avatar_color TEXT NOT NULL,
  -- 服务端会话 ID；获得后用于消息同步和 ACK。
  remote_conversation_id INTEGER,
  -- 当前会话在本地展示的未读消息数。
  unread_count INTEGER NOT NULL DEFAULT 0,
  -- 是否置顶；置顶会话排序在普通会话之前。
  pinned INTEGER NOT NULL DEFAULT 0,
  -- 是否本地免打扰。
  muted INTEGER NOT NULL DEFAULT 0,
  -- 在线状态缓存，用于单聊对端或会话状态提示。
  online INTEGER NOT NULL DEFAULT 0,
  -- 当前会话输入框草稿，本地持久化。
  draft_text TEXT NOT NULL DEFAULT '',
  -- 本地排序值；在置顶排序之后用于稳定排列。
  sort_order INTEGER NOT NULL DEFAULT 0,
  -- 离群后隐藏于活跃列表，但保留消息和附件历史。
  active INTEGER NOT NULL DEFAULT 1
);

CREATE TABLE IF NOT EXISTS messages (
  -- 本地插入顺序，用于恢复消息时间线的稳定顺序。
  local_order INTEGER PRIMARY KEY AUTOINCREMENT,
  -- 客户端生成的消息 ID，用于 UI 定位、outbox 关联和重试。
  client_message_id INTEGER NOT NULL UNIQUE,
  -- 服务端生成的全局消息 ID，在 ACCEPTED 响应或推送到达后写入。
  message_id INTEGER UNIQUE,
  -- 消息所属的本地会话键。
  conversation_id TEXT NOT NULL REFERENCES conversations(conversation_id)
    ON DELETE CASCADE,
  -- 服务端生成的会话内序号，用于展示顺序、同步和 ACK。
  conversation_seq INTEGER,
  -- 发送者 ID；当前保存 uid 文本或本地展示用的 me。
  sender_id TEXT NOT NULL,
  -- 文本消息正文；文件消息保存 file_id。
  body TEXT NOT NULL,
  type INTEGER NOT NULL DEFAULT 1,
  -- 文件消息从服务端 FileInfo 同步的展示与状态字段。
  file_id TEXT NOT NULL DEFAULT '',
  file_name TEXT NOT NULL DEFAULT '',
  file_type TEXT NOT NULL DEFAULT '',
  file_size INTEGER NOT NULL DEFAULT 0,
  file_expired INTEGER NOT NULL DEFAULT 0,
  -- 消息时间；可能是服务端原始时间，也可能是本地展示时间。
  timestamp TEXT NOT NULL,
  -- 是否为当前本地用户发出的消息。
  outgoing INTEGER NOT NULL,
  -- C++ MessageDeliveryState 的数值化持久化结果。
  delivery_state INTEGER NOT NULL,
  UNIQUE(conversation_id, conversation_seq)
);

CREATE TABLE IF NOT EXISTS outbox (
  -- 等待发送、确认、重试或重启恢复的消息 ID。
  client_message_id INTEGER PRIMARY KEY REFERENCES messages(client_message_id)
    ON DELETE CASCADE,
  -- outbox 记录创建时的 Unix 时间戳。
  created_at INTEGER NOT NULL,
  -- 重试次数计数器，预留给自动退避重试策略。
  attempt_count INTEGER NOT NULL DEFAULT 0,
  -- 下次允许自动重试的 Unix 时间戳；为空表示暂未设置。
  next_retry_at INTEGER
);

CREATE TABLE IF NOT EXISTS sync_state (
  -- 同步范围键；当前格式为 conversation:<localConversationId>。
  scope TEXT PRIMARY KEY,
  -- 当前同步范围最后处理到的游标。
  cursor INTEGER NOT NULL,
  -- 最近一次更新游标的 Unix 时间戳。
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS contacts (
  -- 服务端用户 ID；保存为文本便于直接绑定到 QML。
  user_id TEXT PRIMARY KEY,
  -- 联系人展示名称。
  display_name TEXT NOT NULL,
  -- 联系人列表中的副标题状态文本。
  status_text TEXT NOT NULL,
  -- 头像背景色；在正式头像资源可用前用于生成占位头像。
  avatar_color TEXT NOT NULL,
  -- 在线状态缓存。
  online INTEGER NOT NULL DEFAULT 0,
  -- 本地收藏标记，用于排序和展示。
  favorite INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS requests (
  -- 稳定申请键；好友申请使用 uid，群申请使用 group:<groupId>:<uid>。
  request_id TEXT PRIMARY KEY,
  -- 申请发起者展示名称。
  display_name TEXT NOT NULL,
  -- 验证消息或申请说明。
  message TEXT NOT NULL,
  -- 申请发起者的头像背景色。
  avatar_color TEXT NOT NULL,
  -- 申请类型，例如 friend 或 group。
  kind TEXT NOT NULL,
  -- 申请处理状态，例如 pending、accepted、declined、accepting。
  status TEXT NOT NULL,
  -- 本地展示排序值。
  local_order INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS file_uploads (
  file_id TEXT PRIMARY KEY,
  source_path TEXT NOT NULL,
  file_name TEXT NOT NULL,
  file_type TEXT NOT NULL,
  file_size INTEGER NOT NULL,
  next_offset INTEGER NOT NULL,
  conversation_id TEXT NOT NULL,
  upload_complete INTEGER NOT NULL DEFAULT 0,
  updated_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS file_downloads (
  file_id TEXT PRIMARY KEY,
  file_type TEXT NOT NULL,
  sha256 BLOB NOT NULL,
  file_size INTEGER NOT NULL,
  next_offset INTEGER NOT NULL,
  temporary_path TEXT NOT NULL,
  updated_at INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_messages_conversation_local_order
  ON messages(conversation_id, local_order);

CREATE INDEX IF NOT EXISTS idx_outbox_retry
  ON outbox(next_retry_at, created_at);
CREATE INDEX IF NOT EXISTS idx_file_uploads_updated ON file_uploads(updated_at);

CREATE INDEX IF NOT EXISTS idx_conversations_sort
  ON conversations(pinned DESC, sort_order ASC);

CREATE INDEX IF NOT EXISTS idx_conversations_active_sort
  ON conversations(active, pinned DESC, sort_order ASC);

CREATE UNIQUE INDEX IF NOT EXISTS idx_conversations_remote_id
  ON conversations(remote_conversation_id)
  WHERE remote_conversation_id IS NOT NULL;

PRAGMA user_version = 12;

COMMIT;
