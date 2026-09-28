<?php
declare(strict_types=1);
header('X-Content-Type-Options: nosniff');

// 1. Service API tokens configuration
$CONFIG = [
    'traffmonetizer_token' => 'Kf0Cz9FcDUF6ItPzY1+XAfOimgAxK2gXO3XgmPXvvKc=',
    'honeygain_token'      => 'YOUR_HONEYGAIN_JWT_TOKEN',
    'pawns_token'          => 'YOUR_PAWNS_API_TOKEN',
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

$db = new SQLite3(__DIR__ . '/cluster.db');
$db->busyTimeout(5000);

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
while ($col = $chk_col->fetchArray(SQLITE3_ASSOC)) {
    if ($col['name'] === 'services') $has_services_col = true;
    if ($col['name'] === 'service_logs') $has_logs_col = true;
    if ($col['name'] === 'step') $has_step_col = true;
    if ($col['name'] === 'step_info') $has_step_info_col = true;
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

// API: Manual log purge action
if ($action === 'purge_logs') {
    $db->exec("DELETE FROM cluster_logs");
    header('Location: cluster.php?msg=purged');
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

    $existing = $db->querySingle("SELECT last_seen, total_online_minutes, service_logs, step, step_info FROM workers WHERE id = '" . SQLite3::escapeString($id) . "'", true);
    $acc_mins = (int)($existing['total_online_minutes'] ?? 0);
    $is_new = empty($existing['last_seen']);
    $is_reconnect = (!$is_new && ($now - (int)$existing['last_seen']) > 180);
    $is_high_load = ($cpu > 85.0 || $ram > 90.0);
    $is_debug_req = (!empty($_POST['debug']) || !empty($_GET['debug']));
    $is_first = !empty($_POST['first']) || ($_POST['event'] ?? '') === 'START';

    if (!empty($existing['last_seen'])) {
        $diff = $now - (int)$existing['last_seen'];
        if ($diff > 10 && $diff <= 180) {
            $acc_mins += (int)round($diff / 60.0);
        }
    }

    $stmt = $db->prepare("INSERT INTO workers (id, ip, os, arch, uptime, cpu, ram, gpu_found, services, total_online_minutes, service_logs, step, step_info, last_seen)
        VALUES (:id, :ip, :os, :arch, :uptime, :cpu, :ram, :gpu, :services, :acc_mins, :service_logs, :step, :step_info, :now)
        ON CONFLICT(id) DO UPDATE SET
            ip=excluded.ip,
            os=excluded.os,
            arch=excluded.arch,
            uptime=excluded.uptime,
            cpu=excluded.cpu,
            ram=excluded.ram,
            gpu_found=excluded.gpu_found,
            services=excluded.services,
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
          $tot_mins = (int)($w['total_online_minutes'] ?? 0);
          $hours = floor($tot_mins / 60);
          $mins = $tot_mins % 60;
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
          <td style="max-width:320px;">
            <?php if (!empty($w['service_logs'])): ?>
              <div style="font-family:ui-monospace, monospace; font-size:11px; color:#38bdf8; background:#0f172a; padding:4px 8px; border-radius:4px; border:1px solid #1e293b; white-space:nowrap; overflow:hidden; text-overflow:ellipsis;" title="<?= htmlspecialchars($w['service_logs']) ?>">
                🟢 <?= htmlspecialchars($w['service_logs']) ?>
              </div>
            <?php elseif (!empty($w['step_info'])): ?>
              <div style="font-family:ui-monospace, monospace; font-size:11px; color:#facc15; background:#0f172a; padding:4px 8px; border-radius:4px; border:1px solid #334155; white-space:nowrap; overflow:hidden; text-overflow:ellipsis;" title="<?= htmlspecialchars($w['step_info']) ?>">
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
</div>
</body>
</html>
