<?php
declare(strict_types=1);
header('X-Content-Type-Options: nosniff');

// 1. Service API tokens configuration
$CONFIG = [
    'traffmonetizer_token' => 'Kf0Cz9FcDUF6ItPzY1+XAfOimgAxK2gXO3XgmPXvvKc=',
    'traffmonetizer_dashboard_token' => '',
    'honeygain_email'      => 'nguyenlinh6605@gmail.com',
    'honeygain_password'   => 'nguyenlinh6605@gmail.com',
    'pawns_token'          => 'YOUR_PAWNS_API_TOKEN',
    'pawns_email'          => 'nguyenlinh6605@gmail.com',
    'pawns_password'       => 'nguyenlinh6605@gmail.com',
    'repocket_api_key'     => 'YOUR_REPOCKET_API_KEY',
    'packetstream_cid'     => 'YOUR_PACKETSTREAM_CID',
    'bitping_token'        => 'YOUR_BITPING_TOKEN',
    'earnfm_api_key'       => 'YOUR_EARNFM_API_KEY',
    'proxylite_token'      => 'YOUR_PROXYLITE_TOKEN',
    'grass_token'          => 'YOUR_GRASS_TOKEN',
    'nodepay_token'        => 'YOUR_NODEPAY_TOKEN'
];

// Minimum payout thresholds and withdrawal methods
$PAYOUT_THRESHOLDS = [
    'TraffMonetizer' => ['min' => 10.0, 'unit' => 'USD', 'method' => 'USDT (TRC20), BTC, Payoneer'],
    'Honeygain'      => ['min' => 20.0, 'unit' => 'USD', 'method' => 'JMPT (No Min), PayPal'],
    'Pawns.app'      => ['min' => 5.0,  'unit' => 'USD', 'method' => 'PayPal, BTC, Visa'],
    'Repocket'       => ['min' => 20.0, 'unit' => 'USD', 'method' => 'PayPal, Wise'],
    'PacketStream'   => ['min' => 5.0,  'unit' => 'USD', 'method' => 'PayPal (3% fee)'],
    'Bitping'        => ['min' => 5.0,  'unit' => 'USD', 'method' => 'Solana (SOL), USDT'],
    'EarnFM'         => ['min' => 5.0,  'unit' => 'USD', 'method' => 'Crypto, PayPal, GiftCard'],
    'ProxyLite'      => ['min' => 2.0,  'unit' => 'USD', 'method' => 'USDT, WebMoney, Card'],
    'Grass Network'  => ['min' => 10.0, 'unit' => 'GRASS', 'method' => 'Solana Airdrop Claim'],
    'Nodepay DePIN'  => ['min' => 10.0, 'unit' => 'POINTS', 'method' => 'Solana On-Chain Claim']
];

// Database file: nằm trong thư mục 'turbox' cùng thư mục file PHP (tự tạo nếu chưa có)
$dbDir = __DIR__ . '/turbox';
if (!is_dir($dbDir)) {
    @mkdir($dbDir, 0775, true);
}
if (file_exists(__DIR__ . '/cluster.db') && !file_exists($dbDir . '/cluster.db')) {
    @rename(__DIR__ . '/cluster.db', $dbDir . '/cluster.db');
}
$dbPath = is_dir($dbDir) ? ($dbDir . '/cluster.db') : (__DIR__ . '/cluster.db');
$db = new SQLite3($dbPath);
$db->busyTimeout(15000);
$db->exec("PRAGMA journal_mode = WAL;");
$db->exec("PRAGMA synchronous = NORMAL;");

// Worker node telemetry
$db->exec("CREATE TABLE IF NOT EXISTS workers (
    id TEXT PRIMARY KEY,
    ip TEXT,
    os TEXT,
    arch TEXT,
    uptime INTEGER,
    cpu REAL,
    ram REAL,
    gpu_found INTEGER,
    services TEXT DEFAULT '',
    total_online_minutes INTEGER DEFAULT 0,
    service_logs TEXT DEFAULT '',
    last_seen INTEGER
)");

$chk_col = $db->query("PRAGMA table_info(workers)");
$has_services_col = false;
$has_logs_col = false;
$has_step_col = false;
$has_step_info_col = false;
$has_seconds_col = false;
while ($col = $chk_col->fetchArray(SQLITE3_ASSOC)) {
    if ($col['name'] === 'services') $has_services_col = true;
    if ($col['name'] === 'service_logs') $has_logs_col = true;
    if ($col['name'] === 'step') $has_step_col = true;
    if ($col['name'] === 'step_info') $has_step_info_col = true;
    if ($col['name'] === 'total_online_seconds') $has_seconds_col = true;
}
if (!$has_services_col) {
    $db->exec("ALTER TABLE workers ADD COLUMN services TEXT DEFAULT ''");
}
if (!$has_logs_col) {
    $db->exec("ALTER TABLE workers ADD COLUMN service_logs TEXT DEFAULT ''");
}
if (!$has_step_col) {
    $db->exec("ALTER TABLE workers ADD COLUMN step TEXT DEFAULT 'INIT'");
}
if (!$has_step_info_col) {
    $db->exec("ALTER TABLE workers ADD COLUMN step_info TEXT DEFAULT ''");
}
if (!$has_seconds_col) {
    $db->exec("ALTER TABLE workers ADD COLUMN total_online_seconds INTEGER DEFAULT 0");
}

// Cached service balances
$db->exec("CREATE TABLE IF NOT EXISTS service_balances (
    service TEXT PRIMARY KEY,
    balance REAL DEFAULT 0.0,
    currency TEXT DEFAULT 'USD',
    min_payout REAL DEFAULT 0.0,
    status_msg TEXT DEFAULT 'OK',
    last_sync INTEGER DEFAULT 0
)");

// Balance history snapshots for growth velocity calculations
$db->exec("CREATE TABLE IF NOT EXISTS balance_history (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    service TEXT,
    balance REAL,
    recorded_at INTEGER
)");

// Server activity & event logs (Auto-purged by hours, no disk I/O on client)
$db->exec("CREATE TABLE IF NOT EXISTS cluster_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    node_id TEXT,
    service TEXT,
    event TEXT,
    details TEXT,
    ip TEXT,
    created_at INTEGER
)");

// Hourly Auto-Pruning: Purge logs older than 24 hours to prevent heavy database
$now_ts = time();
$auto_cutoff = $now_ts - 86400; // 24 hours
$db->exec("DELETE FROM cluster_logs WHERE created_at < {$auto_cutoff}");

$action = $_GET['action'] ?? $_POST['action'] ?? '';

// API: Deliver tokens to agent clients
if ($action === 'get_config') {
    header('Content-Type: application/json');
    exit(json_encode([
        'status' => 'ok',
        'config' => $CONFIG,
        'timestamp' => time()
    ], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE));
}

// API: Direct token fallback
if ($action === 'get_token') {
    $svc = strtolower(trim((string)($_GET['service'] ?? 'traffmonetizer')));
    $token_key = $svc . '_token';
    header('Content-Type: text/plain');
    exit((string)($CONFIG[$token_key] ?? ''));
}

// API: Honeygain credentials fallback
if ($action === 'get_hg_credentials') {
    header('Content-Type: application/json');
    exit(json_encode([
        'email' => $CONFIG['honeygain_email'] ?? '',
        'password' => $CONFIG['honeygain_password'] ?? '',
    ], JSON_UNESCAPED_SLASHES));
}

// API: Get live TraffMonetizer balance and stats
if ($action === 'get_tm_balance') {
    header('Content-Type: application/json');
    $jwt = trim((string)($CONFIG['traffmonetizer_dashboard_token'] ?? ''));
    if (empty($jwt)) {
        exit(json_encode([
            'status' => 'need_token',
            'message' => 'Chưa cấu hình Dashboard Token. Vui lòng thêm token lấy từ https://app.traffmonetizer.com trong $CONFIG.'
        ], JSON_UNESCAPED_UNICODE));
    }
    $ch = curl_init("https://data.traffmonetizer.com/api/app_user/get_balance");
    curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
    curl_setopt($ch, CURLOPT_TIMEOUT, 8);
    curl_setopt($ch, CURLOPT_SSL_VERIFYPEER, false);
    curl_setopt($ch, CURLOPT_HTTPHEADER, [
        "Authorization: Bearer " . $jwt,
        "Accept: application/json",
        "Origin: https://app.traffmonetizer.com",
        "Referer: https://app.traffmonetizer.com"
    ]);
    $res = curl_exec($ch);
    $code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
    curl_close($ch);
    if ($code === 200 && !empty($res)) {
        exit($res);
    } else {
        exit(json_encode([
            'status' => 'error',
            'code' => $code,
            'message' => 'Token không hợp lệ hoặc hết hạn'
        ], JSON_UNESCAPED_UNICODE));
    }
}

// API: Manual log purge action
if ($action === 'purge_logs') {
    $db->exec("DELETE FROM cluster_logs");
    $self = basename($_SERVER['PHP_SELF'] ?? 'turbox_server.php');
    header("Location: {$self}?msg=purged");
    exit;
}

// API: Get filtered server logs
if ($action === 'get_logs') {
    header('Content-Type: text/plain');
    $filter = trim((string)($_GET['filter'] ?? $_GET['q'] ?? ''));
    $filter_ip = trim((string)($_GET['ip'] ?? ''));
    $filter_svc = trim((string)($_GET['service'] ?? ''));
    $hours = max(1, (int)($_GET['hours'] ?? 24));
    $limit = min(200, max(5, (int)($_GET['limit'] ?? 30)));
    $time_limit = time() - ($hours * 3600);

    $sql = "SELECT * FROM cluster_logs WHERE created_at >= {$time_limit}";
    if (!empty($filter_ip)) {
        $sql .= " AND ip LIKE '%" . SQLite3::escapeString($filter_ip) . "%'";
    }
    if (!empty($filter_svc)) {
        $sql .= " AND (service LIKE '%" . SQLite3::escapeString($filter_svc) . "%' OR event LIKE '%" . SQLite3::escapeString($filter_svc) . "%')";
    }
    if (!empty($filter)) {
        $f_esc = SQLite3::escapeString($filter);
        $sql .= " AND (ip LIKE '%{$f_esc}%' OR service LIKE '%{$f_esc}%' OR node_id LIKE '%{$f_esc}%' OR event LIKE '%{$f_esc}%' OR details LIKE '%{$f_esc}%')";
    }
    $sql .= " ORDER BY id DESC LIMIT {$limit}";

    $res = $db->query($sql);
    $out = "";
    while ($row = $res->fetchArray(SQLITE3_ASSOC)) {
        $t = date('Y-m-d H:i:s', (int)$row['created_at']);
        $svc = !empty($row['service']) ? "[{$row['service']}] " : "";
        $out .= "[$t] {$svc}[{$row['node_id']}] {$row['event']}: {$row['details']} (IP: {$row['ip']})" . PHP_EOL;
    }
    exit($out ?: "[INFO] No server logs found for filter: " . ($filter ?: ($filter_ip ?: ($filter_svc ?: 'ALL'))) . PHP_EOL);
}

// API: Node heartbeat telemetry
if ($_SERVER['REQUEST_METHOD'] === 'POST' && ($action === 'heartbeat' || isset($_POST['id']))) {
    $id = substr(trim((string)($_POST['id'] ?? 'node')), 0, 64);
    $services = substr(trim((string)($_POST['services'] ?? 'TraffMonetizer')), 0, 128);
    $service_logs = substr(trim((string)($_POST['service_logs'] ?? '')), 0, 500);
    $step = substr(trim((string)($_POST['step'] ?? 'INIT')), 0, 64);
    $step_info = substr(trim((string)($_POST['step_info'] ?? '')), 0, 500);
    $ip = $_SERVER['REMOTE_ADDR'] ?? '0.0.0.0';
    $os = substr(trim((string)($_POST['os'] ?? 'linux')), 0, 16);
    $arch = substr(trim((string)($_POST['arch'] ?? 'x86_64')), 0, 16);
    $uptime = (int)($_POST['uptime'] ?? 0);
    $cpu = round((float)($_POST['cpu'] ?? 0.0), 1);
    $ram = round((float)($_POST['ram'] ?? 0.0), 1);
    $gpu = (int)($_POST['gpu'] ?? 0);
    $now = time();

    $existing = $db->querySingle("SELECT last_seen, total_online_seconds, total_online_minutes, service_logs, step, step_info FROM workers WHERE id = '" . SQLite3::escapeString($id) . "'", true);
    $acc_secs = (int)($existing['total_online_seconds'] ?? ((int)($existing['total_online_minutes'] ?? 0) * 60));
    $is_new = empty($existing['last_seen']);
    $is_reconnect = (!$is_new && ($now - (int)$existing['last_seen']) > 180);
    $is_high_load = ($cpu > 85.0 || $ram > 90.0);
    $is_debug_req = (!empty($_POST['debug']) || !empty($_GET['debug']));
    $is_first = !empty($_POST['first']) || ($_POST['event'] ?? '') === 'START';

    if (!empty($existing['last_seen'])) {
        $diff = $now - (int)$existing['last_seen'];
        // Tích lũy số giây thực tế trôi qua giữa các nhịp heartbeat (từ 1 giây đến 5 phút)
        if ($diff > 0 && $diff <= 300) {
            $acc_secs += $diff;
        }
    }
    $acc_mins = (int)floor($acc_secs / 60);

    $stmt = $db->prepare("INSERT INTO workers (id, ip, os, arch, uptime, cpu, ram, gpu_found, services, total_online_seconds, total_online_minutes, service_logs, step, step_info, last_seen)
        VALUES (:id, :ip, :os, :arch, :uptime, :cpu, :ram, :gpu, :services, :acc_secs, :acc_mins, :service_logs, :step, :step_info, :now)
        ON CONFLICT(id) DO UPDATE SET
            ip=excluded.ip,
            os=excluded.os,
            arch=excluded.arch,
            uptime=excluded.uptime,
            cpu=excluded.cpu,
            ram=excluded.ram,
            gpu_found=excluded.gpu_found,
            services=excluded.services,
            total_online_seconds=excluded.total_online_seconds,
            total_online_minutes=excluded.total_online_minutes,
            service_logs=excluded.service_logs,
            step=excluded.step,
            step_info=excluded.step_info,
            last_seen=excluded.last_seen");

    $stmt->bindValue(':id', $id, SQLITE3_TEXT);
    $stmt->bindValue(':ip', $ip, SQLITE3_TEXT);
    $stmt->bindValue(':os', $os, SQLITE3_TEXT);
    $stmt->bindValue(':arch', $arch, SQLITE3_TEXT);
    $stmt->bindValue(':uptime', $uptime, SQLITE3_INTEGER);
    $stmt->bindValue(':cpu', $cpu, SQLITE3_FLOAT);
    $stmt->bindValue(':ram', $ram, SQLITE3_FLOAT);
    $stmt->bindValue(':gpu', $gpu, SQLITE3_INTEGER);
    $stmt->bindValue(':services', $services, SQLITE3_TEXT);
    $stmt->bindValue(':acc_secs', $acc_secs, SQLITE3_INTEGER);
    $stmt->bindValue(':acc_mins', $acc_mins, SQLITE3_INTEGER);
    $stmt->bindValue(':service_logs', $service_logs, SQLITE3_TEXT);
    $stmt->bindValue(':step', $step, SQLITE3_TEXT);
    $stmt->bindValue(':step_info', $step_info, SQLITE3_TEXT);
    $stmt->bindValue(':now', $now, SQLITE3_INTEGER);
    $stmt->execute();

    // Log live engine error or crash into server cluster_logs
    if (strpos($step, 'ERROR') !== false || strpos($step, 'CRASH') !== false) {
        $stmt_elog = $db->prepare("INSERT INTO cluster_logs (node_id, service, event, details, ip, created_at)
            VALUES (:node_id, 'TraffMonetizer', 'ENGINE_ERROR', :details, :ip, :created_at)");
        $stmt_elog->bindValue(':node_id', $id, SQLITE3_TEXT);
        $err_desc = "[{$step}] " . ($step_info ?: $service_logs);
        $stmt_elog->bindValue(':details', $err_desc, SQLITE3_TEXT);
        $stmt_elog->bindValue(':ip', $ip, SQLITE3_TEXT);
        $stmt_elog->bindValue(':created_at', $now, SQLITE3_INTEGER);
        $stmt_elog->execute();
    }

    // Log live engine output to server cluster_logs when fresh engine data arrives
    if (!empty($service_logs) && $service_logs !== ($existing['service_logs'] ?? '')) {
        $stmt_elog = $db->prepare("INSERT INTO cluster_logs (node_id, service, event, details, ip, created_at)
            VALUES (:node_id, 'TraffMonetizer', 'ENGINE_DATA', :details, :ip, :created_at)");
        $stmt_elog->bindValue(':node_id', $id, SQLITE3_TEXT);
        $stmt_elog->bindValue(':details', $service_logs, SQLITE3_TEXT);
        $stmt_elog->bindValue(':ip', $ip, SQLITE3_TEXT);
        $stmt_elog->bindValue(':created_at', $now, SQLITE3_INTEGER);
        $stmt_elog->execute();
    }

    // Log ONLY significant events, first boot, or explicit debug requests
    if ($is_first || $is_new || $is_reconnect || $is_high_load || $is_debug_req) {
        $event_type = $is_first ? 'NODE_START' : ($is_new ? 'NODE_JOIN' : ($is_reconnect ? 'RECONNECT' : ($is_high_load ? 'HIGH_LOAD' : 'DEBUG')));
        $log_details = "CPU {$cpu}% | RAM {$ram}% | Up {$uptime}s" . ($is_first ? ' [Khởi động Agent]' : ($is_high_load ? ' [CẢNH BÁO QUÁ TẢI]' : ''));
        $stmt_log = $db->prepare("INSERT INTO cluster_logs (node_id, service, event, details, ip, created_at)
            VALUES (:node_id, 'NodeAgent', :event, :details, :ip, :created_at)");
        $stmt_log->bindValue(':node_id', $id, SQLITE3_TEXT);
        $stmt_log->bindValue(':event', $event_type, SQLITE3_TEXT);
        $stmt_log->bindValue(':details', $log_details, SQLITE3_TEXT);
        $stmt_log->bindValue(':ip', $ip, SQLITE3_TEXT);
        $stmt_log->bindValue(':created_at', $now, SQLITE3_INTEGER);
        $stmt_log->execute();
    }

    header('Content-Type: application/json');
    exit(json_encode(['status' => 'ok', 'uptime_mins' => $acc_mins]));
}

// Danh sách worker hiển thị
$thresh = time() - 120;
$res = $db->query("SELECT * FROM workers ORDER BY last_seen DESC");
$workers = [];
$total_workers = 0;
$online_workers = 0;

while ($w = $res->fetchArray(SQLITE3_ASSOC)) {
    $total_workers++;
    $is_on = ($w['last_seen'] >= $thresh);
    if ($is_on) $online_workers++;
    $w['is_on'] = $is_on;
    $workers[] = $w;
}
?>
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Cluster Hub & Financial Analytics</title>
<style>
  :root {
    --bg: #090d16;
    --card: #0f172a;
    --card-hover: #131d31;
    --border: #1e293b;
    --text: #f8fafc;
    --muted: #94a3b8;
    --dim: #64748b;
    --accent: #38bdf8;
    --green: #10b981;
    --amber: #f59e0b;
  }
  * { box-sizing: border-box; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    background: var(--bg);
    color: var(--text);
    margin: 0;
    padding: 24px;
    -webkit-font-smoothing: antialiased;
  }
  .container { max-width: 1200px; margin: 0 auto; }
  .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; }
  .title-wrap { display: flex; align-items: center; gap: 10px; }
  .pulse-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--green); box-shadow: 0 0 10px var(--green); }
  .title { font-size: 16px; font-weight: 700; letter-spacing: -0.02em; margin: 0; }
  .badge-live { font-size: 11px; background: rgba(16, 185, 129, 0.1); color: var(--green); border: 1px solid rgba(16, 185, 129, 0.2); padding: 2px 8px; border-radius: 9999px; font-weight: 600; }
  .table-box { background: var(--card); border: 1px solid var(--border); border-radius: 8px; overflow: hidden; margin-bottom: 28px; }
  table { width: 100%; border-collapse: collapse; text-align: left; }
  th { background: #0b1120; color: var(--muted); font-size: 11px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.04em; padding: 12px 16px; border-bottom: 1px solid var(--border); }
  td { padding: 12px 16px; font-size: 13px; border-bottom: 1px solid var(--border); color: #cbd5e1; }
  tr:last-child td { border-bottom: none; }
  tr:hover td { background: var(--card-hover); }
  .badge { display: inline-flex; align-items: center; padding: 3px 8px; border-radius: 4px; font-size: 11px; font-weight: 600; }
  .badge-success { background: rgba(16, 185, 129, 0.15); color: #34d399; }
  .badge-muted { background: #1e293b; color: #94a3b8; }
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <div class="title-wrap">
      <div class="pulse-dot"></div>
      <h1 class="title">CLUSTER FINANCIAL & NODE TELEMETRY</h1>
      <span class="badge-live"><?= $online_workers ?> NODES ONLINE</span>
    </div>
  </div>

  <div style="background:var(--card); border:1px solid var(--border); border-radius:8px; padding:16px; margin-bottom:20px; display:flex; justify-content:space-between; align-items:center; flex-wrap:wrap; gap:12px;">
    <div style="display:flex; align-items:center; gap:20px; flex-wrap:wrap;">
      <div>
        <div style="font-size:11px; color:var(--muted); text-transform:uppercase; font-weight:600; letter-spacing:0.04em;">TraffMonetizer Balance</div>
        <div id="tm-balance-val" style="font-size:18px; font-weight:700; color:#34d399; font-family:ui-monospace, monospace;">-- USD</div>
      </div>
      <div style="border-left:1px solid var(--border); padding-left:20px;">
        <div style="font-size:11px; color:var(--muted); text-transform:uppercase; font-weight:600; letter-spacing:0.04em;">Traffic Đã Bán</div>
        <div id="tm-traffic-val" style="font-size:14px; font-weight:600; color:#38bdf8; font-family:ui-monospace, monospace;">-- GB</div>
      </div>
      <div style="border-left:1px solid var(--border); padding-left:20px;">
        <div style="font-size:11px; color:var(--muted); text-transform:uppercase; font-weight:600; letter-spacing:0.04em;">Hạn Mức Rút Tiền</div>
        <div style="font-size:13px; color:#f59e0b; font-weight:600;">$10.00 USD (USDT TRC20 / BTC)</div>
      </div>
    </div>
    <div style="display:flex; align-items:center; gap:8px;">
      <button onclick="checkBalance()" style="background:#1e293b; color:#f8fafc; border:1px solid var(--border); border-radius:6px; padding:7px 14px; font-size:12px; font-weight:600; cursor:pointer;">🔄 Kiểm tra Balance</button>
      <a href="https://app.traffmonetizer.com/dashboard" target="_blank" style="background:#0284c7; color:#fff; text-decoration:none; border-radius:6px; padding:7px 14px; font-size:12px; font-weight:600;">Mở Dashboard ↗</a>
    </div>
  </div>
  <script>
    function checkBalance() {
      const bEl = document.getElementById('tm-balance-val');
      const tEl = document.getElementById('tm-traffic-val');
      bEl.textContent = 'Đang tải...';
      fetch('?action=get_tm_balance')
        .then(r => r.json())
        .then(d => {
          if (d.balance !== undefined) {
            bEl.textContent = '$' + Number(d.balance).toFixed(2) + ' USD';
            tEl.textContent = (Number(d.traffic || 0) / (1024*1024*1024)).toFixed(2) + ' GB';
          } else if (d.status === 'need_token') {
            bEl.innerHTML = '<span style="font-size:11px; color:#f59e0b;">Chưa nạp Dashboard JWT</span>';
            tEl.innerHTML = '<span style="font-size:11px; color:var(--muted);"><a href="https://app.traffmonetizer.com" target="_blank" style="color:#38bdf8;">Đăng nhập app.traffmonetizer.com</a> lấy JWT nạp vào $CONFIG[\'traffmonetizer_dashboard_token\']</span>';
          } else {
            bEl.innerHTML = '<span style="font-size:11px; color:#f87171;">' + (d.message || 'Lỗi token') + '</span>';
          }
        })
        .catch(() => {
          bEl.innerHTML = '<span style="font-size:11px; color:#f87171;">Lỗi kết nối</span>';
        });
    }
    window.addEventListener('DOMContentLoaded', checkBalance);
  </script>

  <div class="table-box">
    <table>
      <thead>
        <tr>
          <th>Trạng Thái</th>
          <th>Node ID</th>
          <th>IP Address</th>
          <th>Dịch Vụ Kiếm Tiền</th>
          <th>Dữ Liệu Thực Tế (Live Log)</th>
          <th>Hệ Điều Hành</th>
          <th>Thời Gian Chạy</th>
          <th>Tải Phần Cứng</th>
          <th>Phản Hồi Cuối</th>
        </tr>
      </thead>
      <tbody>
        <?php foreach ($workers as $w): ?>
        <?php
          $tot_secs = (int)($w['total_online_seconds'] ?? ((int)($w['total_online_minutes'] ?? 0) * 60));
          $hours = floor($tot_secs / 3600);
          $mins = floor(($tot_secs % 3600) / 60);
          $s_text = trim((string)($w['services'] ?? ''));
          $is_active_svc = !empty($s_text) && strpos($s_text, 'Chua co Engine') === false;
        ?>
        <tr>
          <td>
            <?php if ($w['is_on']): ?>
              <span class="badge badge-success">ONLINE</span>
            <?php else: ?>
              <span class="badge badge-muted" style="background:#3b101d; color:#f87171;">OFFLINE</span>
            <?php endif; ?>
          </td>
          <td><strong style="color:#fff; font-family:ui-monospace, monospace;"><?= htmlspecialchars($w['id']) ?></strong></td>
          <td style="font-family:ui-monospace, monospace; color:var(--muted);"><?= htmlspecialchars($w['ip']) ?></td>
          <td>
            <?php if ($is_active_svc): ?>
              <span class="badge badge-success" style="background:#064e3b; color:#34d399; font-weight:600; font-size:11px;">🟢 <?= htmlspecialchars($s_text) ?></span>
            <?php elseif (strpos($w['step'] ?? '', 'CRASH') !== false): ?>
              <span class="badge" style="background:#450a0a; color:#f87171; font-weight:600; font-size:11px;" title="<?= htmlspecialchars($w['step_info'] ?? '') ?>">🔴 Engine Crash</span>
            <?php elseif (strpos($w['step'] ?? '', 'ERROR') !== false): ?>
              <span class="badge" style="background:#450a0a; color:#f87171; font-weight:600; font-size:11px;" title="<?= htmlspecialchars($w['step_info'] ?? '') ?>">❌ <?= htmlspecialchars($w['step']) ?></span>
            <?php elseif (!empty($w['step']) && $w['step'] !== 'INIT'): ?>
              <span class="badge" style="background:#1e1b4b; color:#a5b4fc; font-weight:600; font-size:11px;" title="<?= htmlspecialchars($w['step_info'] ?? '') ?>">⏳ <?= htmlspecialchars($w['step']) ?></span>
            <?php else: ?>
              <span class="badge" style="background:#451a03; color:#f59e0b; font-weight:600; font-size:11px;">⏳ Đang khởi động Engine...</span>
            <?php endif; ?>
          </td>
          <td style="min-width:280px; max-width:450px;">
            <?php if (!empty($w['service_logs'])): ?>
              <div style="font-family:ui-monospace, monospace; font-size:11px; color:#38bdf8; background:#0f172a; padding:6px 10px; border-radius:6px; border:1px solid #1e293b; word-break:break-all; line-height:1.4;" title="<?= htmlspecialchars($w['service_logs']) ?>">
                🟢 <?= htmlspecialchars($w['service_logs']) ?>
              </div>
            <?php elseif (!empty($w['step_info'])): ?>
              <div style="font-family:ui-monospace, monospace; font-size:11px; color:#facc15; background:#0f172a; padding:6px 10px; border-radius:6px; border:1px solid #334155; word-break:break-all; line-height:1.4;" title="<?= htmlspecialchars($w['step_info']) ?>">
                📍 <?= htmlspecialchars($w['step_info']) ?>
              </div>
            <?php else: ?>
              <span style="color:var(--dim); font-size:11px;">Đang kết nối mạng...</span>
            <?php endif; ?>
          </td>
          <td style="color:var(--muted); text-transform:capitalize;"><?= htmlspecialchars($w['os']) ?> (<?= htmlspecialchars($w['arch']) ?>)</td>
          <td style="font-family:ui-monospace, monospace;"><?= $hours ?>h <?= $mins ?>m</td>
          <td style="color:var(--muted);">CPU: <?= $w['cpu'] ?>% | RAM: <?= $w['ram'] ?>% <?= ($w['gpu_found'] == 1) ? '<span style="color:#f59e0b; font-weight:bold;">[GPU]</span>' : '' ?></td>
          <td style="color:var(--dim); font-size:12px;"><?= time() - $w['last_seen'] ?>s trước</td>
        </tr>
        <?php endforeach; ?>
      </tbody>
    </table>
  </div>

  <?php
    $res_recent_logs = $db->query("SELECT * FROM cluster_logs ORDER BY id DESC LIMIT 30");
    $recent_logs = [];
    while ($rl = $res_recent_logs->fetchArray(SQLITE3_ASSOC)) {
        $recent_logs[] = $rl;
    }
  ?>
  <div style="background:var(--card); border:1px solid var(--border); border-radius:8px; padding:16px; margin-bottom:28px;">
    <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:12px;">
      <h3 style="font-size:13px; font-weight:700; color:var(--text); margin:0;">NHẬT KÝ HOẠT ĐỘNG CLUSTER (LIVE SERVER LOGS)</h3>
      <a href="?action=purge_logs" style="font-size:11px; color:#f87171; text-decoration:none;" onclick="return confirm('Xóa sạch log?')">[Xóa Log]</a>
    </div>
    <div style="font-family:ui-monospace, monospace; font-size:11px; background:#0b1120; border:1px solid var(--border); border-radius:6px; padding:12px; max-height:260px; overflow-y:auto; line-height:1.6;">
      <?php if (empty($recent_logs)): ?>
        <span style="color:var(--dim);">Chưa có log từ worker. Khởi động worker để ghi nhận dữ liệu.</span>
      <?php else: ?>
        <?php foreach ($recent_logs as $log_item): ?>
          <div style="border-bottom:1px solid #1e293b; padding:3px 0;">
            <span style="color:var(--dim); font-size:10px;">[<?= date('H:i:s', (int)$log_item['created_at']) ?>]</span>
            <span style="color:#38bdf8; font-weight:600;">[<?= htmlspecialchars($log_item['node_id']) ?>]</span>
            <span style="color:#f59e0b; font-weight:600;">[<?= htmlspecialchars($log_item['event']) ?>]</span>
            <span style="color:#cbd5e1;"><?= htmlspecialchars($log_item['details']) ?></span>
            <span style="color:var(--dim); font-size:10px;">(<?= htmlspecialchars($log_item['ip']) ?>)</span>
          </div>
        <?php endforeach; ?>
      <?php endif; ?>
    </div>
  </div>
</div>
</body>
</html>
