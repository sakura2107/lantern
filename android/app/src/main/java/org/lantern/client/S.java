package org.lantern.client;

/**
 * User-facing strings for the presentation layer, collected in one place so a later
 * localization pass can swap this class (or back it with resources) without touching layouts.
 * Messages produced by the protocol/transfer layers are reported verbatim and live with them.
 */
final class S {
    private S() {}

    // Shell
    static final String APP_NAME = "Lantern";
    static final String TAB_DEVICE = "设备", TAB_DESKTOP = "桌面", TAB_FILES = "文件", TAB_TRANSFERS = "传输";
    static final String TITLE_DESKTOP = "远程桌面", TITLE_FILES = "电脑文件", TITLE_TRANSFERS = "传输";
    static final String STATE_UNPAIRED = "未配对", STATE_SAVED = "未连接", STATE_CONNECTING = "连接中",
                        STATE_CONNECTED = "已连接", STATE_FAILED = "连接失败", STATE_INVALID = "地址有误";
    static final String ACTIVE_COUNT = "%d 项进行中或排队";

    // Common
    static final String CANCEL = "取消", DONE = "完成", SAVE = "保存", GO = "前往", CREATE = "创建", RETRY = "重试";
    static final String MORE = "更多", MORE_ACTIONS = "更多操作", UNSUPPORTED = "不支持";
    static final String EXPANDED = "已展开", COLLAPSED = "已收起";
    static final String ALLOWED = "已允许", NOT_ALLOWED = "未允许";
    static final String NEED_DEVICE = "请先在设备页连接电脑";
    static final String FILE = "文件", FOLDER = "文件夹";
    static final String TODAY_AT = "今天 %02d:%02d", MONTH_DAY = "%d月%d日";
    static final String CHOOSER_TITLE = "用应用打开（只读）";

    // Device page
    static final String CONNECT = "连接电脑", CONNECTING = "连接中…", RECONNECT = "重新连接", BROWSE_FILES = "浏览文件";
    static final String DISCONNECT = "断开连接",
        DISCONNECTED_NOTICE = "已断开连接；配对资料已保留",
        DISCONNECTED_DETAIL = "已断开。画面、声音、控制和文件均已停止，传输任务已暂停；点“连接电脑”即可恢复。",
        ENDED_BY_HOST_NOTICE = "电脑已结束此设备的连接，已断开",
        ENDED_BY_HOST_DETAIL = "电脑在主机管理中结束了此设备的连接。配对仍然有效，需要时点“连接电脑”重新连接。";
    static final String HERO_UNPAIRED = "还没有连接电脑";
    static final String HERO_UNPAIRED_DETAIL = "在电脑的 Lantern 主机管理里点“扫码添加设备”，再点这里的“扫码配对”扫一下就能连上。也可以在下方手动填写。";
    static final String HERO_SAVED = "已保存一台电脑", HERO_SAVED_AT = "%s";
    static final String HERO_SAVED_DETAIL = "连接以验证电脑身份。后台传输任务会按原授权继续。";
    static final String HERO_CONNECTING = "正在连接", HERO_CONNECTING_TO = "正在连接 %s";
    static final String HERO_CONNECTING_DETAIL = "正在核对主机指纹和设备密钥…";
    static final String HERO_CONNECTED = "已连接", HERO_CONNECTED_TO = "%s";
    static final String HERO_CONNECTED_DETAIL = "电脑身份已验证。文件、画面、控制和声音分别授权，可以单独开启。";
    static final String HERO_FAILED = "无法连接电脑";
    static final String HERO_FAILED_DETAIL = "确认电脑上的 Lantern 主机已启动、手机能连到电脑所在的网络（在外面需要电脑开启公网访问），并核对指纹和密钥。";
    static final String HERO_FAILED_REASON = "%s\n确认电脑上的 Lantern 主机已启动、手机能连到电脑所在的网络（在外面需要电脑开启公网访问），并核对指纹和密钥。";
    static final String HERO_INVALID = "电脑地址格式不对";
    static final String HERO_INVALID_DETAIL = "填写 IPv4、IPv6 或主机名，端口可选，例如 192.168.1.10 或 192.168.1.10:47443。";
    static final String FINGERPRINT_SHORT = "指纹 %s";
    static final String PAIRING = "配对信息", PAIRING_SAVED = "已保存 %s 的指纹和密钥", PAIRING_INCOMPLETE = "需要地址、指纹和密钥";
    static final String FIELD_HOST = "电脑地址", HINT_HOST = "192.168.1.10";
    static final String DISCOVER = "发现";
    static final String FIELD_FINGERPRINT = "主机指纹", HINT_FINGERPRINT = "粘贴电脑上显示的 64 位指纹";
    static final String HELP_FINGERPRINT = "在电脑的 Lantern 主机管理里查看，用于确认连的是你的电脑。";
    static final String FINGERPRINT_LENGTH = "指纹应为 64 位十六进制字符，当前是 %d 位";
    static final String PASTE_FINGERPRINT = "粘贴指纹";
    static final String CLIPBOARD_EMPTY = "剪贴板里没有文本";
    static final String CLIPBOARD_NOT_FINGERPRINT = "剪贴板内容不是 64 位指纹，未粘贴";
    static final String FIELD_SECRET = "设备密钥", HINT_SECRET = "首次连接时填写";
    static final String HELP_SECRET = "电脑为这台手机单独生成的密钥，保存后由系统加密。";
    static final String SHOW_SECRET = "显示密钥", HIDE_SECRET = "隐藏密钥";
    static final String TOOLS = "工具";
    static final String DIAGNOSTICS = "网络诊断", DIAGNOSTICS_DETAIL = "测量到电脑的加密往返延迟";
    static final String NOTIFICATIONS = "传输通知", NOTIFICATIONS_DETAIL = "后台上传下载时显示进度和暂停按钮";
    static final String LICENSES = "开源许可", LICENSES_DETAIL = "本应用使用的第三方组件", LICENSES_SUBTITLE = "随应用附带的许可文本";
    static final String LICENSES_MISSING = "此安装包未附带许可文本";
    static final String PRIVACY_NOTE = "配对密钥由 Android 系统密钥库加密保存。指纹、密钥、剪贴板和文件内容不会写入日志。";
    static final String DISCOVERING = "正在发现同一网络中的电脑…";
    static final String DISCOVER_TITLE = "发现的电脑", DISCOVER_SUBTITLE = "这些电脑尚未验证身份。选择后只填写地址。";
    static final String DISCOVER_NONE = "没有发现电脑";
    static final String DISCOVER_NONE_DETAIL = "确认电脑上的主机绑定了局域网地址（或 0.0.0.0）、已启动，并且 UDP 47442 端口未被防火墙拦截。也可以直接填写地址。";
    static final String VERIFY_TITLE = "核对电脑指纹";
    static final String VERIFY_DETAIL = "已填写地址。下面是这台电脑声明的指纹，请与电脑上主机管理显示的指纹逐段对照。一致时才使用它。";
    static final String VERIFY_DIFFERS = "已填写地址。注意：这台电脑声明的指纹与已保存的不同。除非你确认电脑重新生成过身份，否则不要替换。";
    static final String ADDRESS_ONLY = "仅填写地址", USE_FINGERPRINT = "一致，使用此指纹";
    static final String FINGERPRINT_FILLED = "已填入指纹，填写设备密钥后连接";
    static final String DIAG_RUNNING = "正在测量 5 次加密往返，请稍候…";
    static final String DIAG_TITLE = "网络诊断", DIAG_SUBTITLE = "手机用%s，经 %s";
    static final String DIAG_HANDSHAKE = "TLS 与身份验证", DIAG_RTT_MEDIAN = "往返延迟（中位）", DIAG_RTT_RANGE = "往返延迟范围",
                        DIAG_SPEED = "文件传输速度", DIAG_IDLE = "当前没有传输";
    static final String DIAG_NOTE = "往返延迟包含主机处理时间，不代表可用带宽。使用移动网络时，局域网地址通常无法访问。";

    // Desktop page
    static final String PREVIEW_OFF = "画面未开启";
    static final String PREVIEW_OFF_DETAIL = "开启后在这里预览电脑桌面，可全屏、缩放和控制。文件和声音不需要开启画面。";
    static final String PREVIEW_NEED_DEVICE = "先在设备页连接电脑，再开启画面。";
    static final String PREVIEW_HIDDEN = "本机画面已隐藏";
    static final String PREVIEW_HIDDEN_DETAIL = "电脑仍在采集并发送画面，只是手机不显示。要让电脑停止采集，请关闭画面。";
    static final String START_PICTURE = "开启画面", SHOW_PICTURE = "显示画面";
    static final String TILE_PICTURE = "画面", TILE_CONTROL = "控制", TILE_SOUND = "声音", TILE_HIDE = "隐藏本机画面";
    static final String PICTURE_OFF = "未开启", PICTURE_ON = "接收中", PICTURE_ON_HIDDEN = "接收中，本机隐藏";
    static final String CONTROL_OFF = "仅观看", CONTROL_PENDING = "授权中…", CONTROL_ON = "可点按和输入";
    static final String SOUND_OFF = "关闭", SOUND_CONNECTING = "连接中…", SOUND_RECONNECTING = "重连中…", SOUND_ON = "播放电脑声音";
    static final String SOUND_ON_SHORT = "声音开";
    static final String HIDE_OFF = "画面显示中", HIDE_ON = "电脑仍在发送";
    static final String TOOL_FULLSCREEN = "全屏", TOOL_KEYBOARD = "键盘", TOOL_QUALITY = "画质", TOOL_MONITOR = "显示器",
                        TOOL_CLIPBOARD = "剪贴板", TOOL_SHORTCUTS = "快捷键", TOOL_TIMING = "时序", TOOL_PRIVACY = "隐私屏";
    static final String NEED_PICTURE = "先开启画面，再进入全屏";
    static final String MONITOR_N = "显示器 %d";
    static final String MONITOR_TITLE = "选择显示器", MONITOR_SUBTITLE = "切换后控制会关闭，确认画面后再开启。";
    static final String PRIVACY_TITLE = "主机隐私屏";
    static final String PRIVACY_SHORT = "当前版本不支持，电脑屏幕会照常显示";
    static final String PRIVACY_EXPLAIN = "当前版本不支持主机隐私屏，远程操作时电脑本地屏幕会照常显示。\n\n“隐藏本机画面”只让手机不显示，电脑仍在采集和发送；关闭“画面”会让电脑停止采集。";
    static final String TOOLS_TITLE = "桌面工具", TOOLS_SUBTITLE = "剪贴板和快捷键使用独立的控制授权。";
    static final String CLIP_SUBTITLE = "仅支持文本，最多 64 KiB。不会自动发送粘贴快捷键。";
    static final String CLIP_PUSH = "发送到电脑剪贴板", CLIP_PUSH_DETAIL = "把手机剪贴板里的文本复制到电脑";
    static final String CLIP_PULL = "取回电脑剪贴板", CLIP_PULL_DETAIL = "把电脑剪贴板里的文本复制到手机";
    static final String SHORTCUTS_TITLE = "发送快捷键", SHORTCUTS_SUBTITLE = "需要控制授权。", SHORTCUTS_DETAIL = "复制、粘贴、切换应用等";
    static final String KEY_COPY = "复制", KEY_PASTE = "粘贴", KEY_ALL = "全选", KEY_SWITCH = "切换应用", KEY_DESKTOP = "显示桌面", KEY_CLOSE = "关闭窗口";
    static final String TIMING_SUBTITLE = "决定画面是否等待声音。";
    static final String TIMING_FAST = "响应优先", TIMING_FAST_DETAIL = "画面不等声音，控制延迟最低。适合操作电脑。";
    static final String TIMING_SYNC = "声音同步", TIMING_SYNC_DETAIL = "画面按声音时钟最多等待 350 ms。适合看视频。";
    static final String TIMING_FAST_SHORT = "响应优先", TIMING_SYNC_SHORT = "声音同步";
    static final String QUALITY_TITLE = "画质", QUALITY_SUBTITLE = "帧率是目标值，实际更新率显示在画面左上角。";
    static final String Q_SECTION_H264 = "硬件编码（H.264，需要电脑支持）", Q_SECTION_JPEG = "兼容模式（JPEG）";
    static final String Q_SMOOTH = "流畅", Q_SMOOTH_DETAIL = "1080p，60 fps，12 Mbps";
    static final String Q_SAVER = "省带宽", Q_SAVER_DETAIL = "720p，60 fps，8 Mbps";
    static final String Q_ECO = "节能", Q_ECO_DETAIL = "720p，30 fps，6 Mbps";
    static final String Q_FAST = "快速", Q_FAST_DETAIL = "宽 960 像素，质量 55";
    static final String Q_BALANCED = "均衡", Q_BALANCED_DETAIL = "宽 1280 像素，质量 65";
    static final String Q_SHARP = "清晰", Q_SHARP_DETAIL = "宽 1920 像素，质量 75";
    static final String FULL_EXIT = "退出", FULL_ROTATE = "横竖屏", FULL_COLLAPSE = "收起", FULL_TOOLS = "显示工具栏",
                        FULL_CONTROL_ON = "控制中", FULL_RIGHT = "右键", FULL_RIGHT_ARMED = "点按处右键",
                        RIGHT_ARMED = "下一次点按将发送右键；也可以双指轻点", RIGHT_NEED_CONTROL = "请先开启控制，再使用右键";

    // Files page
    static final String ROOT_FOLDER = "根目录", CURRENT_FOLDER = "当前位置：%s";
    static final String SEARCH = "搜索", SEARCH_HINT = "搜索文件名", SEARCH_SCOPE = "在“%s”及其子文件夹中查找。";
    static final String SEARCH_RESULTS_FOR = "“%s”的结果", CLEAR_SEARCH = "退出搜索";
    static final String SEARCHING = "正在搜索：找到 %d 项，已检查 %d 项";
    static final String SEARCH_DONE = "搜索完成：找到 %d 项，共检查 %d 项";
    static final String SORT = "排序";
    static final String SORT_NAME_ASC = "名称（A 到 Z）", SORT_NAME_DESC = "名称（Z 到 A）", SORT_SIZE_ASC = "大小（从小到大）",
                        SORT_SIZE_DESC = "大小（从大到小）", SORT_TIME_ASC = "修改时间（从旧到新）", SORT_TIME_DESC = "修改时间（从新到旧）";
    static final String SELECT = "多选", EXIT_SELECTION = "退出多选", SELECT_ALL = "全选", SELECT_ITEMS = "选择项目",
                        SELECTED_N = "已选 %d 项", NOTHING_TO_SELECT = "当前文件夹没有可选择的项目";
    static final String DOWNLOAD = "下载", COPY_TO = "复制到", MOVE_TO = "移动到", DELETE = "删除";
    static final String QUEUED_DOWNLOADS = "已加入 %d 个下载任务";
    static final String MOVE_TITLE = "移动到", COPY_TITLE = "复制到";
    static final String DEST_HELP = "在电脑上处理 %d 项，不经过手机。填写授权目录内的文件夹路径，填 / 表示根目录。";
    static final String DEST_HINT = "例如 Anime/2024";
    static final String MOVE_CONFIRM = "移动", COPY_CONFIRM = "复制";
    static final String PREV_PAGE = "上一页", NEXT_PAGE = "下一页";
    static final String PAGE_RANGE = "第 %d–%d 项，共 %d 项", PAGE_EMPTY = "共 %d 项";
    static final String CANCEL_REQUESTED = "已请求取消，已完成的部分会保留";
    static final String UPLOAD = "上传", UPLOAD_TITLE = "上传到电脑", UPLOAD_TO = "保存到“%s”。";
    static final String UPLOAD_FILES = "选择文件", UPLOAD_FILES_DETAIL = "可多选，一次最多 16 个";
    static final String UPLOAD_FOLDER = "选择整个文件夹", UPLOAD_FOLDER_DETAIL = "保留目录结构，逐个文件提交";
    static final String MODIFIED_AT = "修改于 %s";
    static final String OPEN_WITH_APP = "用手机应用打开", OPEN_WITH_APP_DETAIL = "直接读取电脑上的原文件，不占用手机空间";
    static final String DOWNLOAD_OPEN = "下载后打开", DOWNLOAD_OPEN_DETAIL = "保存到应用缓存，校验完整后打开";
    static final String QUEUE_DOWNLOAD = "加入下载任务", QUEUE_DOWNLOAD_DETAIL = "在后台下载，可暂停和断点续传";
    static final String OPEN_FOLDER = "打开", DOWNLOAD_FOLDER = "下载整个文件夹", DOWNLOAD_FOLDER_DETAIL = "在后台逐个下载，可暂停";
    static final String MANAGE = "在电脑上管理";
    static final String PREVIEW_THUMB = "预览缩略图", LOADING_PREVIEW = "正在生成缩略图…", THUMB_NOTE = "电脑生成的小尺寸预览";
    static final String RENAME_MOVE = "重命名或移动", RENAME_MOVE_DETAIL = "修改名称或所在文件夹";
    static final String RENAME_HELP = "填写新的相对路径。只改名称时保留前面的文件夹部分。";
    static final String HOST_COPY = "在电脑上复制", HOST_COPY_DETAIL = "副本直接在电脑上生成";
    static final String COPY_HELP = "填写副本的相对路径。", COPY_SUFFIX = "-副本";
    static final String DETAILS = "详细信息";
    static final String INFO_TYPE = "类型：", INFO_PATH = "位置：", INFO_SIZE = "大小：", INFO_BYTES = "（%d 字节）", INFO_MODIFIED = "修改时间：";
    static final String DELETE_FOLDER_DETAIL = "包括其中所有文件";
    static final String DELETE_ONE = "删除“%s”？", DELETE_MANY = "删除 %d 项？";
    static final String DELETE_DETAIL = "将从电脑上永久删除，包括文件夹里的内容，无法撤销。中途取消会保留尚未删除的部分。";
    static final String FOLDER_ACTIONS = "此文件夹", NEW_FOLDER = "新建文件夹", NEW_FOLDER_HINT = "文件夹名称", REFRESH = "刷新";
    static final String GO_TO_PATH = "前往路径", GO_TO_PATH_DETAIL = "直接输入授权目录内的路径", HINT_PATH = "相对路径，例如 Anime/2024";
    static final String CREATED = "已创建“%s”";
    static final String WORKING_CREATE = "正在创建文件夹…", WORKING_RENAME = "正在重命名…", WORKING_COPY = "正在复制",
                        WORKING_MOVE = "正在移动", WORKING_DELETE = "正在删除";
    static final String HOST_JOB_PROGRESS = "电脑正在处理：%d / %d", HOST_JOB_RUNNING = "电脑正在处理…";
    static final String BATCH_PROGRESS = "%s：%d / %d", BATCH_DONE = "已完成 %d 项";
    static final String OPENING = "正在准备只读访问…";
    static final String DOWNLOADING = "正在下载 %s：%s / %s";
    static final String EMPTY_NO_DEVICE = "还没有连接电脑", EMPTY_NO_DEVICE_DETAIL = "连接后可以浏览电脑授权的文件夹，不需要开启远程画面。";
    static final String GO_CONNECT = "去连接";
    static final String EMPTY_LOADING = "正在读取文件夹…";
    static final String EMPTY_NO_PERMISSION = "没有文件读取权限", EMPTY_NO_PERMISSION_DETAIL = "电脑没有给这台手机授予文件读取权限。可以在电脑的主机管理里为此设备开启；画面和声音不受影响。";
    static final String EMPTY_NO_RESULTS = "没有找到匹配的文件", EMPTY_NO_RESULTS_DETAIL = "换个关键词，或回到上一级文件夹再搜索。";
    static final String EMPTY_NOT_LOADED = "文件列表未加载", EMPTY_NOT_LOADED_DETAIL = "已保存的电脑尚未在本次连接。读取列表时会验证身份。";
    static final String LOAD_FILES = "读取文件列表";
    static final String EMPTY_FOLDER = "这个文件夹是空的", EMPTY_FOLDER_DETAIL = "可以上传手机里的文件到这里。";
    static final String EMPTY_LOAD_FAILED = "无法读取这个文件夹";

    // Transfers page
    static final String QUEUE_IDLE = "没有传输任务", QUEUE_QUIET = "没有进行中的传输";
    static final String QUEUE_MOVING = "%d 项正在传输", QUEUE_WAITING = "%d 项等待中", QUEUE_ATTENTION = "%d 项需要处理";
    static final String QUEUE_SPEED = "下载 %s · 上传 %s";
    static final String QUEUE_LIMIT = "，已限速 %s";
    static final String QUEUE_RTT = "网络往返 %.1f ms";
    static final String LIMIT_BUTTON = "限速 %s", LIMIT_SHORT = "限速";
    static final String PAUSE_ALL = "全部暂停", RESUME_ALL = "全部继续", CLEAR_DONE = "清理已完成";
    static final String PAUSED_BY_USER = "已手动暂停", PAUSED_ALL = "已暂停全部任务，进度已保留", RESUMED_N = "已继续 %d 个任务";
    static final String FILTER_ALL = "全部", FILTER_ACTIVE = "未完成", FILTER_DONE = "已结束";
    static final String NO_TASKS = "还没有传输任务", NO_TASKS_DETAIL = "在文件页下载，或把手机里的文件上传到电脑。";
    static final String GO_FILES = "去文件页";
    static final String T_RUNNING = "传输中", T_QUEUED = "排队中", T_NETWORK = "等待网络", T_CONFLICT = "需要处理",
                        T_PAUSED = "已暂停", T_DONE = "已完成", T_CANCELLED = "已取消";
    static final String DIRECTION_DOWN = "从电脑下载", DIRECTION_UP = "上传到电脑";
    static final String ITEMS_PROGRESS = "%d / %d 项";
    static final String ETA = "约 %s 后完成";
    static final String RETRYING = "连接中断，保留进度自动重试（%d/%d）";
    static final String OTHER_DEVICE_TASK = "属于另一台电脑，连接那台电脑后继续";
    static final String PAUSE = "暂停", PAUSE_DETAIL = "保留已传部分，稍后继续";
    static final String RESUME = "继续", RESUME_DETAIL = "从已传部分接着传输";
    static final String RESOLVE_CONFLICT = "处理同名冲突", RESOLVE_CONFLICT_DETAIL = "电脑上已有同名文件，原文件会保留";
    static final String OPEN_DONE = "打开文件", OPEN_DONE_DETAIL = "用手机应用打开已下载的文件";
    static final String MOVE_TOP = "移到队首", MOVE_TOP_DETAIL = "下一个开始传输";
    static final String CANCEL_TASK = "取消任务", CANCEL_TASK_DETAIL = "停止传输，已传部分保留，之后仍可继续";
    static final String REMOVE_TASK = "移除记录", REMOVE_TASK_DETAIL = "从列表中移除，电脑上的文件不受影响";
    static final String REMOVE_TASK_DETAIL_CACHE = "同时删除手机里的下载缓存，电脑上的文件不受影响";
    static final String CONFLICT_TITLE = "电脑上已有同名项目", CONFLICT_DETAIL = "电脑上的原文件会保留，不会被覆盖。";
    static final String CONFLICT_RENAME = "另存为新名称", CONFLICT_RENAME_DETAIL = "用新名称上传到同一文件夹";
    static final String CONFLICT_SKIP = "跳过此任务", CONFLICT_SKIP_DETAIL = "保留已传部分，不再上传";
    static final String CONFLICT_NEW_NAME = "新名称", SAVE_AS = "另存";
    static final String CLEAR_TITLE = "清理已完成的任务？", CLEAR_DETAIL = "会删除手机里已完成的下载缓存，电脑上的原文件保留。";
    static final String CLEAR_CONFIRM = "清理";
    static final String LIMIT_TITLE = "传输限速", LIMIT_DETAIL = "默认不限速，速度由网络决定。只有想给其他应用留出带宽时才需要限速。当前：%s。";
    static final String LIMIT_NONE = "不限速", LIMIT_NONE_DETAIL = "推荐";

    // Scan-to-pair
    static final String SCAN_PAIR = "扫码配对", PASTE_PAIR_LINK = "粘贴链接", CANCEL_SCAN = "取消";
    static final String SCAN_TITLE = "对准电脑上的二维码", SCAN_SUBTITLE = "在电脑 Lantern 主机管理里点“扫码添加设备”";
    static final String SCAN_NOT_LANTERN = "这不是 Lantern 的配对二维码";
    static final String CAMERA_DENIED = "没有相机权限。可以改用“粘贴链接”，或在系统设置里允许相机。";
    static final String CAMERA_FAILED = "无法打开相机，可以改用“粘贴链接”", CAMERA_NONE = "这台手机没有可用的相机";
    static final String CLIPBOARD_NOT_LINK = "剪贴板里没有 Lantern 配对链接";
    static final String LINK_INVALID_TITLE = "二维码无法使用";
    static final String LINK_INVALID_DETAIL = "二维码无效或已过期。请在电脑上点“刷新二维码”后重新扫描。如果手机时间不准，也会出现这个提示。";
    static final String PAIR_CONFIRM_TITLE = "与这台电脑配对？";
    static final String PAIR_CONFIRM = "名称：%s\n地址：%s\n指纹：%s …\n\n请确认和电脑上显示的一致。配对后，这台手机会出现在电脑的设备列表里。";
    static final String PAIR_CONFIRM_ACTION = "配对并连接", PAIRING_NOW = "正在与 %s 配对…", PAIRED_OK = "配对成功，正在连接…";
    static final String PAIR_FAILED_TITLE = "配对没有完成";
    static final String[] PAIR_ERRORS = {
        "二维码无效或已过期，请在电脑上刷新二维码；如果手机时间不准，也请校正。",
        "电脑端 Lantern 版本过旧，请更新后重新生成二维码。",
        "配对码无效、已被使用或已过期，请在电脑上刷新二维码。",
        "连不上电脑：请确认主机已启动并已允许通过防火墙。在家时连同一个 Wi-Fi；在外面扫码，需要电脑开启公网访问并允许外网扫码。",
        "电脑身份和二维码不一致，已停止配对。请重新核对电脑上的二维码。",
        "尝试太频繁了，请一分钟后再试。",
        "电脑的回应无效，没有保存任何凭据。请检查两端的版本。",
        "连接中断，无法确认是否已配对。请在电脑的设备列表里检查，删掉多余的条目后重新生成二维码。"};
    // Public network: entries learnt from the computer (LAN, public IPv6, hostname, public IPv4).
    static final String[] PATH_LABELS = {"", "局域网", "局域网", "公网 IPv6", "域名", "公网 IPv4"}; // by entry kind
    static final String PATH_PUBLIC_NOTICE = "正在经公网连接电脑；画面卡顿时可以在画质里选低一档。";
    static final String[] ENDPOINT_FAILURES = { // NetworkDialer.Failure order
        "域名解析失败", "没有响应（超时）", "被拒绝（没有在监听，或被路由器拦下）", "证书指纹不匹配", "连接中断", "已取消"};
    static final String ENDPOINT_OK = "可用", ENDPOINT_RTT = "可用 · 往返 %d ms", ENDPOINT_RTT_FAST = "可用 · 往返不到 1 ms";
    static final String CONNECT_ALL_FAILED = "所有地址都连不上：%s";
    static final String MORE_ADDRESSES = "%s 等 %d 个地址";
    static final String DIAG_ENTRIES = "各个地址";
    static final String DIAG_ENTRIES_NOTE = "用移动数据时，局域网地址连不上是正常的。公网地址连不上时，请在电脑主机管理的“公网访问”页检查防火墙和路由器。";
}