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
while ($col = $chk_col->fetchArray(SQLITE3_ASSOC)) {
    if ($col['name'] === 'services') $has_services_col = true;
    if ($col['name'] === 'service_logs') $has_logs_col = true;
}
if (!$has_services_col) {
    $db->exec("ALTER TABLE workers ADD COLUMN services TEXT DEFAULT ''");
}
if (!$has_logs_col) {
    $db->exec("ALTER TABLE workers ADD COLUMN service_logs TEXT DEFAULT ''");
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

// API: Manual log purge action (Xóa sạch toàn bộ log)
if ($action === 'purge_logs') {
    $db->exec("DELETE FROM cluster_logs");
    header('Location: cluster.php?msg=purged');
    exit;
}

// API: Get filtered server logs (Plaintext for CLI / deploy.sh)
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
    $ip = $_SERVER['REMOTE_ADDR'] ?? '0.0.0.0';
    $os = substr(trim((string)($_POST['os'] ?? 'linux')), 0, 16);
    $arch = substr(trim((string)($_POST['arch'] ?? 'x86_64')), 0, 16);
    $uptime = (int)($_POST['uptime'] ?? 0);
    $cpu = round((float)($_POST['cpu'] ?? 0.0), 1);
    $ram = round((float)($_POST['ram'] ?? 0.0), 1);
    $gpu = (int)($_POST['gpu'] ?? 0);
    $now = time();

    $existing = $db->querySingle("SELECT last_seen, total_online_minutes, service_logs FROM workers WHERE id = '" . SQLite3::escapeString($id) . "'", true);
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

    $stmt = $db->prepare("INSERT INTO workers (id, ip, os, arch, uptime, cpu, ram, gpu_found, services, total_online_minutes, service_logs, last_seen)
        VALUES (:id, :ip, :os, :arch, :uptime, :cpu, :ram, :gpu, :services, :acc_mins, :service_logs, :now)
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
    $stmt->bindValue(':now', $now, SQLITE3_INTEGER);
    $stmt->execute();

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

    // Log ONLY significant events, first boot, or explicit debug requests (prevent flooding DB with 60s routine heartbeats)
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

// 2. HÀM KẾT NỐI API THỰC TẾ 10 DỊCH VỤ & TÍNH TOÁN DỰ PHÓNG
function sync_real_service_balances(array $config, array $thresholds, SQLite3 $db, bool $force = false): array {
    $now = time();
    $cache_timeout = 600; // Cache 10 phút chống rate-limit từ nhà cung cấp

    $services = [
        'TraffMonetizer' => [
            'token' => $config['traffmonetizer_token'] ?? '',
            'url' => '',
            'type' => 'node_token',
            'extractor' => function($data) {
                return 0.0;
            }
        ],
        'Honeygain' => [
            'token' => $config['honeygain_token'] ?? '',
            'url' => 'https://dashboard.honeygain.com/api/v1/users/balances',
            'type' => 'bearer',
            'extractor' => function($data) {
                $credits = (float)($data['data']['payout']['credits'] ?? $data['payout']['credits'] ?? $data['credits'] ?? 0.0);
                return round($credits / 1000.0, 3);
            }
        ],
        'Pawns.app' => [
            'token' => $config['pawns_token'] ?? '',
            'url' => 'https://api.pawns.app/v1/dashboard',
            'type' => 'bearer',
            'extractor' => function($data) {
                return (float)($data['balance'] ?? $data['data']['balance'] ?? 0.0);
            }
        ],
        'Repocket' => [
            'token' => $config['repocket_api_key'] ?? '',
            'url' => 'https://api.repocket.co/v1/user/balance',
            'type' => 'apikey',
            'extractor' => function($data) {
                return (float)($data['balance'] ?? $data['data']['balance'] ?? 0.0);
            }
        ],
        'PacketStream' => [
            'token' => $config['packetstream_cid'] ?? '',
            'url' => 'https://packetstream.io/dashboard/api',
            'type' => 'cid',
            'extractor' => function($data) {
                return (float)($data['balance'] ?? $data['amount'] ?? 0.0);
            }
        ],
        'Bitping' => [
            'token' => $config['bitping_token'] ?? '',
            'url' => 'https://api.bitping.com/v1/user/earnings',
            'type' => 'bearer',
            'extractor' => function($data) {
                return (float)($data['balance'] ?? $data['total_earnings'] ?? 0.0);
            }
        ],
        'EarnFM' => [
            'token' => $config['earnfm_api_key'] ?? '',
            'url' => 'https://earn.fm/api/v1/user/balance',
            'type' => 'apikey',
            'extractor' => function($data) {
                return (float)($data['balance'] ?? $data['data']['balance'] ?? 0.0);
            }
        ],
        'ProxyLite' => [
            'token' => $config['proxylite_token'] ?? '',
            'url' => 'https://proxylite.ru/api/balance',
            'type' => 'apikey',
            'extractor' => function($data) {
                return (float)($data['balance'] ?? $data['rub'] ?? 0.0) / 95.0; // Quy đổi RUB sang USD
            }
        ],
        'Grass Network' => [
            'token' => $config['grass_token'] ?? '',
            'url' => 'https://api.getgrass.io/users/stats',
            'type' => 'bearer',
            'extractor' => function($data) {
                return (float)($data['totalPoints'] ?? $data['data']['totalPoints'] ?? 0.0);
            }
        ],
        'Nodepay DePIN' => [
            'token' => $config['nodepay_token'] ?? '',
            'url' => 'https://api.nodepay.ai/user/earnings',
            'type' => 'bearer',
            'extractor' => function($data) {
                return (float)($data['total_points'] ?? $data['data']['total_points'] ?? 0.0);
            }
        ]
    ];

    $results = [];

    foreach ($services as $name => $s) {
        $token = trim($s['token']);
        $has_token = (!empty($token) && strpos($token, 'YOUR_') === false);
        $min_req = $thresholds[$name]['min'] ?? 10.0;
        $unit = $thresholds[$name]['unit'] ?? 'USD';

        $row = $db->querySingle("SELECT balance, status_msg, last_sync FROM service_balances WHERE service = '" . SQLite3::escapeString($name) . "'", true);
        $balance = (float)($row['balance'] ?? 0.0);
        $status_msg = (string)($row['status_msg'] ?? ($has_token ? 'Chưa đồng bộ' : 'Chưa cấu hình'));
        $last_sync = (int)($row['last_sync'] ?? 0);

        if ($s['type'] === 'node_token') {
            $status_msg = $has_token ? 'Đang chạy trên Node (Xem số dư tại TraffMonetizer.com)' : 'Chưa cấu hình Token';
            $stmt = $db->prepare("INSERT INTO service_balances (service, balance, currency, min_payout, status_msg, last_sync)
                VALUES (:svc, :bal, :cur, :min, :msg, :sync)
                ON CONFLICT(service) DO UPDATE SET
                    status_msg=excluded.status_msg,
                    last_sync=excluded.last_sync");
            $stmt->bindValue(':svc', $name, SQLITE3_TEXT);
            $stmt->bindValue(':bal', $balance, SQLITE3_FLOAT);
            $stmt->bindValue(':cur', $unit, SQLITE3_TEXT);
            $stmt->bindValue(':min', $min_req, SQLITE3_FLOAT);
            $stmt->bindValue(':msg', $status_msg, SQLITE3_TEXT);
            $stmt->bindValue(':sync', $now, SQLITE3_INTEGER);
            $stmt->execute();
            continue;
        }

        if ($has_token && ($force || ($now - $last_sync > $cache_timeout))) {
            $ch = curl_init();
            curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
            curl_setopt($ch, CURLOPT_TIMEOUT, 8);
            curl_setopt($ch, CURLOPT_SSL_VERIFYPEER, false);
            curl_setopt($ch, CURLOPT_SSL_VERIFYHOST, 0);
            curl_setopt($ch, CURLOPT_USERAGENT, 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)');

            if ($s['type'] === 'bearer') {
                curl_setopt($ch, CURLOPT_URL, $s['url']);
                curl_setopt($ch, CURLOPT_HTTPHEADER, ["Authorization: Bearer $token", "Accept: application/json"]);
            } elseif ($s['type'] === 'apikey') {
                curl_setopt($ch, CURLOPT_URL, $s['url']);
                curl_setopt($ch, CURLOPT_HTTPHEADER, ["x-api-key: $token", "Authorization: $token", "Accept: application/json"]);
            } elseif ($s['type'] === 'cid') {
                curl_setopt($ch, CURLOPT_URL, $s['url'] . '?cid=' . urlencode($token));
            }

            $body = curl_exec($ch);
            $code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
            $err = curl_error($ch);
            curl_close($ch);

            if ($code >= 200 && $code < 300 && !empty($body)) {
                $json = json_decode($body, true);
                if (is_array($json)) {
                    $balance = $s['extractor']($json);
                    $status_msg = 'Đã kết nối API';
                } else {
                    $status_msg = 'Lỗi JSON';
                }
            } else {
                $status_msg = !empty($err) ? "Lỗi: $err" : "HTTP $code";
            }

            $stmt = $db->prepare("INSERT INTO service_balances (service, balance, currency, min_payout, status_msg, last_sync)
                VALUES (:svc, :bal, :cur, :min, :msg, :sync)
                ON CONFLICT(service) DO UPDATE SET
                    balance=excluded.balance,
                    currency=excluded.currency,
                    min_payout=excluded.min_payout,
                    status_msg=excluded.status_msg,
                    last_sync=excluded.last_sync");
            $stmt->bindValue(':svc', $name, SQLITE3_TEXT);
            $stmt->bindValue(':bal', $balance, SQLITE3_FLOAT);
            $stmt->bindValue(':cur', $unit, SQLITE3_TEXT);
            $stmt->bindValue(':min', $min_req, SQLITE3_FLOAT);
            $stmt->bindValue(':msg', $status_msg, SQLITE3_TEXT);
            $stmt->bindValue(':sync', $now, SQLITE3_INTEGER);
            $stmt->execute();

            $last_rec = (int)$db->querySingle("SELECT recorded_at FROM balance_history WHERE service = '" . SQLite3::escapeString($name) . "' ORDER BY recorded_at DESC LIMIT 1");
            if (($now - $last_rec) >= 1800) {
                $h_stmt = $db->prepare("INSERT INTO balance_history (service, balance, recorded_at) VALUES (:svc, :bal, :ts)");
                $h_stmt->bindValue(':svc', $name, SQLITE3_TEXT);
                $h_stmt->bindValue(':bal', $balance, SQLITE3_FLOAT);
                $h_stmt->bindValue(':ts', $now, SQLITE3_INTEGER);
                $h_stmt->execute();
            }

            // Log significant financial events only
            $old_bal = (float)($row['balance'] ?? 0.0);
            if ($balance > $old_bal && $old_bal > 0.0) {
                $gain = round($balance - $old_bal, 4);
                $db->exec("INSERT INTO cluster_logs (node_id, service, event, details, ip, created_at)
                    VALUES ('server', '" . SQLite3::escapeString($name) . "', 'BALANCE_UP', 'Số dư tăng +{$gain} {$unit} (Hiện tại: {$balance})', '127.0.0.1', {$now})");
            } elseif ($code >= 400 || !empty($err)) {
                $last_err = (int)$db->querySingle("SELECT created_at FROM cluster_logs WHERE service = '" . SQLite3::escapeString($name) . "' AND event = 'SYNC_ERROR' ORDER BY created_at DESC LIMIT 1");
                if (($now - $last_err) > 21600) {
                    $db->exec("INSERT INTO cluster_logs (node_id, service, event, details, ip, created_at)
                        VALUES ('server', '" . SQLite3::escapeString($name) . "', 'SYNC_ERROR', '" . SQLite3::escapeString($status_msg) . "', '127.0.0.1', {$now})");
                }
            }

            if ($balance >= $min_req && $has_token) {
                $recent_met = $db->querySingle("SELECT id FROM cluster_logs WHERE service = '" . SQLite3::escapeString($name) . "' AND event = 'THRESHOLD_MET' AND created_at > " . ($now - 43200));
                if (!$recent_met) {
                    $db->exec("INSERT INTO cluster_logs (node_id, service, event, details, ip, created_at)
                        VALUES ('server', '" . SQLite3::escapeString($name) . "', 'THRESHOLD_MET', 'Đủ điều kiện rút thưởng: {$balance} {$unit} (Yêu cầu: {$min_req})', '127.0.0.1', {$now})");
                }
            }

            $last_sync = $now;
        }

        // TÍNH TỐC ĐỘ THỰC TẾ & NGÀY ĐỦ MIN TỪ LỊCH SỬ
        $daily_rate = 0.0;
        $monthly_rate = 0.0;
        $eta_text = 'Chờ mốc đo kế tiếp';

        if ($has_token) {
            $base_rec = $db->querySingle("SELECT balance, recorded_at FROM balance_history WHERE service = '" . SQLite3::escapeString($name) . "' ORDER BY recorded_at ASC LIMIT 1", true);
            if (!empty($base_rec['recorded_at'])) {
                $sec_diff = $now - (int)$base_rec['recorded_at'];
                $bal_diff = $balance - (float)$base_rec['balance'];

                if ($sec_diff >= 1800 && $bal_diff >= 0) {
                    $days_span = max(0.01, $sec_diff / 86400.0);
                    $daily_rate = round($bal_diff / $days_span, 4);
                    $monthly_rate = round($daily_rate * 30.0, 2);

                    if ($balance >= $min_req) {
                        $eta_text = '✓ ĐÃ ĐỦ MIN RÚT!';
                    } elseif ($daily_rate > 0.0001) {
                        $days_to_go = (int)ceil(($min_req - $balance) / $daily_rate);
                        $target_date = date('d/m/Y', $now + ($days_to_go * 86400));
                        $eta_text = "$target_date (~{$days_to_go}d)";
                    } else {
                        $eta_text = 'Đang đo tốc độ...';
                    }
                } elseif ($balance >= $min_req) {
                    $eta_text = '✓ ĐÃ ĐỦ MIN RÚT!';
                }
            } elseif ($balance >= $min_req) {
                $eta_text = '✓ ĐÃ ĐỦ MIN RÚT!';
            }
        }

        $can_withdraw = ($balance >= $min_req);
        $progress = ($min_req > 0) ? min(100.0, round(($balance / $min_req) * 100, 1)) : 0.0;
        $need_more = max(0.0, round($min_req - $balance, 3));

        $results[$name] = [
            'name'         => $name,
            'balance'      => $balance,
            'unit'         => $unit,
            'min_payout'   => $min_req,
            'method'       => $thresholds[$name]['method'] ?? 'USDT/PayPal',
            'progress'     => $progress,
            'can_withdraw' => $can_withdraw,
            'need_more'    => $need_more,
            'daily_rate'   => $daily_rate,
            'monthly_rate' => $monthly_rate,
            'eta_text'     => $eta_text,
            'status_msg'   => $status_msg,
            'last_sync'    => $last_sync,
            'has_token'    => $has_token
        ];
    }

    return $results;
}

$is_sync = ($action === 'sync');
$balances = sync_real_service_balances($CONFIG, $PAYOUT_THRESHOLDS, $db, $is_sync);

$total_real_usd = 0.0;
$total_ready_withdraw = 0.0;
$total_projected_monthly = 0.0;
$services_ready = 0;

foreach ($balances as $b) {
    // Chi cong don tien USD vao tong (cac diem token giu nguyen tai dong rieng)
    if ($b['unit'] === 'USD') {
        $total_real_usd += $b['balance'];
        $total_projected_monthly += $b['monthly_rate'];
        if ($b['can_withdraw']) {
            $total_ready_withdraw += $b['balance'];
            $services_ready++;
        }
    }
}

// Danh sach worker
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
  
  /* Header */
  .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; }
  .title-wrap { display: flex; align-items: center; gap: 10px; }
  .pulse-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--green); box-shadow: 0 0 10px var(--green); }
  .title { font-size: 16px; font-weight: 700; letter-spacing: -0.02em; margin: 0; }
  .badge-live { font-size: 11px; background: rgba(16, 185, 129, 0.1); color: var(--green); border: 1px solid rgba(16, 185, 129, 0.2); padding: 2px 8px; border-radius: 9999px; font-weight: 600; }
  .btn-sync {
    background: #0284c7;
    color: #fff;
    text-decoration: none;
    padding: 7px 14px;
    border-radius: 6px;
    font-size: 12px;
    font-weight: 600;
    transition: background 0.15s ease;
    display: inline-flex;
    align-items: center;
    gap: 6px;
  }
  .btn-sync:hover { background: #0369a1; }
  
  /* KPI Grid */
  .grid-kpi { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 14px; margin-bottom: 28px; }
  .kpi-card {
    background: var(--card);
    border: 1px solid var(--border);
    border-radius: 8px;
    padding: 16px 20px;
  }
  .kpi-label { font-size: 11px; font-weight: 600; color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; }
  .kpi-value { font-size: 24px; font-weight: 700; color: #fff; margin: 6px 0 2px 0; font-family: ui-monospace, monospace; }
  .kpi-sub { font-size: 12px; color: var(--dim); }
  
  /* Section Header */
  .section-hdr { display: flex; justify-content: space-between; align-items: baseline; margin-bottom: 12px; }
  .section-title { font-size: 13px; font-weight: 700; color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; }
  
  /* Clean Minimalist Table */
  .table-box { background: var(--card); border: 1px solid var(--border); border-radius: 8px; overflow: hidden; margin-bottom: 28px; }
  table { width: 100%; border-collapse: collapse; text-align: left; }
  th { background: #0b1120; color: var(--muted); font-size: 11px; font-weight: 600; text-transform: uppercase; letter-spacing: 0.04em; padding: 12px 16px; border-bottom: 1px solid var(--border); }
  td { padding: 12px 16px; font-size: 13px; border-bottom: 1px solid var(--border); color: #cbd5e1; }
  tr:last-child td { border-bottom: none; }
  tr:hover td { background: var(--card-hover); }
  
  /* Progress & Badges */
  .progress-track { width: 80px; height: 5px; background: #1e293b; border-radius: 9999px; overflow: hidden; display: inline-block; vertical-align: middle; margin-right: 8px; }
  .progress-fill { height: 100%; background: var(--green); border-radius: 9999px; }
  .progress-fill.pending { background: var(--amber); }
  
  .badge { display: inline-flex; align-items: center; padding: 3px 8px; border-radius: 4px; font-size: 11px; font-weight: 600; }
  .badge-success { background: rgba(16, 185, 129, 0.15); color: #34d399; }
  .badge-warning { background: rgba(245, 158, 11, 0.15); color: #fbbf24; }
  .badge-danger { background: rgba(239, 68, 68, 0.15); color: #f87171; }
  .badge-muted { background: #1e293b; color: #94a3b8; }
  
  .filter-bar {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    align-items: center;
    background: #0b1120;
    padding: 10px 14px;
    border-bottom: 1px solid var(--border);
  }
  .filter-input, .filter-select {
    background: #1e293b;
    border: 1px solid #334155;
    color: #e2e8f0;
    padding: 6px 10px;
    border-radius: 6px;
    font-size: 12px;
    font-family: inherit;
    outline: none;
  }
  .filter-input:focus, .filter-select:focus {
    border-color: var(--accent);
  }
  .btn-filter {
    background: #0284c7;
    color: #fff;
    border: none;
    padding: 6px 12px;
    border-radius: 6px;
    font-size: 12px;
    font-weight: 600;
    cursor: pointer;
  }
  .btn-filter:hover { background: #0369a1; }
  .btn-purge {
    background: #334155;
    color: #cbd5e1;
    border: none;
    padding: 6px 12px;
    border-radius: 6px;
    font-size: 12px;
    font-weight: 500;
    cursor: pointer;
    margin-left: auto;
    transition: all 0.15s ease;
  }
  .btn-purge:hover { background: #dc2626; color: #fff; }
  
  .rate-val { font-weight: 600; color: var(--accent); font-family: ui-monospace, monospace; }
  .rate-sub { font-size: 11px; color: var(--dim); }
  .eta-val { font-weight: 600; color: #e2e8f0; font-family: ui-monospace, monospace; }
</style>
</head>
<body>

<div class="container">
  <!-- Top Navigation & Status -->
  <div class="header">
    <div class="title-wrap">
      <div class="pulse-dot"></div>
      <h1 class="title">CLUSTER FINANCIAL & NODE TELEMETRY</h1>
      <span class="badge-live"><?= $online_workers ?> NODES ONLINE</span>
    </div>
    <a href="?action=sync" class="btn-sync">
      <span>🔄</span> Cập nhật số dư từ API
    </a>
  </div>

  <!-- 4 Clean Key Metrics -->
  <div class="grid-kpi">
    <div class="kpi-card">
      <div class="kpi-label">Tổng Số Dư Khả Dụng</div>
      <div class="kpi-value" style="color: #fbbf24;">$<?= number_format($total_real_usd, 2) ?></div>
      <div class="kpi-sub">≈ <?= number_format($total_real_usd * 25400, 0, ',', '.') ?> VNĐ trên 10 ví</div>
    </div>

    <div class="kpi-card">
      <div class="kpi-label">Dự Phóng Doanh Thu Tháng</div>
      <div class="kpi-value" style="color: var(--accent);">+$<?= number_format($total_projected_monthly, 2) ?></div>
      <div class="kpi-sub">Tính từ tốc độ tăng trưởng thực tế</div>
    </div>

    <div class="kpi-card">
      <div class="kpi-label">Đủ Điều Kiện Rút Ngay</div>
      <div class="kpi-value" style="color: var(--green);">$<?= number_format($total_ready_withdraw, 2) ?></div>
      <div class="kpi-sub"><?= $services_ready ?> tài khoản đã đạt mức Min Payout</div>
    </div>

    <div class="kpi-card">
      <div class="kpi-label">Quy Mô Cluster</div>
      <div class="kpi-value" style="color: #c084fc;"><?= $online_workers ?> <span style="font-size:16px; color:var(--dim); font-weight:normal;">/ <?= $total_workers ?> Nodes</span></div>
      <div class="kpi-sub">Chia sẻ chạy chung 10 nền tảng</div>
    </div>
  </div>

  <!-- 10 Services Balance & Payout Projection Table -->
  <div class="section-hdr">
    <span class="section-title">Theo Dõi 10 Dịch Vụ & Dự Báo Ngày Đủ Min Rút</span>
    <span style="font-size:12px; color:var(--dim);">Tự động tính từ biến động số dư thực tế</span>
  </div>

  <div class="table-box">
    <table>
      <thead>
        <tr>
          <th>Nền Tảng</th>
          <th>Số Dư Thực Tế</th>
          <th>Min Rút</th>
          <th>Tiến Độ</th>
          <th>Tốc Độ Thực Tế</th>
          <th>Dự Kiến Đủ Min (ETA)</th>
          <th>Trạng Thái</th>
          <th>Cổng Rút</th>
        </tr>
      </thead>
      <tbody>
        <?php foreach ($balances as $b): ?>
        <tr>
          <td>
            <strong style="color: #fff;"><?= htmlspecialchars($b['name']) ?></strong>
          </td>
          <td>
            <?php if ($b['has_token']): ?>
              <span style="font-weight:700; color:var(--green); font-family:ui-monospace, monospace;">
                <?= ($b['unit'] === 'USD') ? '$' : '' ?><?= number_format($b['balance'], 3) ?> <small style="font-weight:normal; color:var(--dim);"><?= htmlspecialchars($b['unit']) ?></small>
              </span>
            <?php else: ?>
              <span style="color:var(--dim);">0.000</span>
            <?php endif; ?>
          </td>
          <td style="font-family:ui-monospace, monospace; color:var(--muted);">
            <?= ($b['unit'] === 'USD') ? '$' : '' ?><?= number_format($b['min_payout'], 1) ?>
          </td>
          <td>
            <div class="progress-track">
              <div class="progress-fill <?= $b['can_withdraw'] ? '' : 'pending' ?>" style="width: <?= $b['progress'] ?>%;"></div>
            </div>
            <span style="font-size:12px; font-family:ui-monospace, monospace;"><?= $b['progress'] ?>%</span>
          </td>
          <td>
            <?php if ($b['daily_rate'] > 0): ?>
              <span class="rate-val">+$<?= number_format($b['daily_rate'], 4) ?>/d</span>
              <div class="rate-sub">~$<?= number_format($b['monthly_rate'], 2) ?>/mo</div>
            <?php else: ?>
              <span style="color:var(--dim); font-size:12px;">Đang tích lũy...</span>
            <?php endif; ?>
          </td>
          <td>
            <?php if ($b['can_withdraw']): ?>
              <span style="color:var(--green); font-weight:700;">Đủ điều kiện!</span>
            <?php elseif ($b['has_token']): ?>
              <span class="eta-val"><?= htmlspecialchars($b['eta_text']) ?></span>
            <?php else: ?>
              <span style="color:var(--dim);">--</span>
            <?php endif; ?>
          </td>
          <td>
            <?php if (!$b['has_token']): ?>
              <span class="badge badge-muted">Chưa có Token</span>
            <?php elseif ($b['can_withdraw']): ?>
              <span class="badge badge-success">✓ Rút Được Ngay</span>
            <?php else: ?>
              <span class="badge badge-warning">Thiếu <?= ($b['unit'] === 'USD') ? '$' : '' ?><?= number_format($b['need_more'], 2) ?></span>
            <?php endif; ?>
          </td>
          <td style="font-size:12px; color:var(--dim);">
            <?= htmlspecialchars($b['method']) ?>
          </td>
        </tr>
        <?php endforeach; ?>
      </tbody>
    </table>
  </div>

  <!-- Worker Nodes Telemetry Table -->
  <div class="section-hdr">
    <span class="section-title">Danh Sách Máy Con Đang Hoạt Động (Workers)</span>
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
        <?php if (empty($workers)): ?>
        <tr>
          <td colspan="9" style="text-align:center; color:var(--dim); padding:24px;">Chưa có máy con nào kết nối.</td>
        </tr>
        <?php endif; ?>
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
            <?php else: ?>
              <span class="badge" style="background:#451a03; color:#f59e0b; font-weight:600; font-size:11px;">⏳ Đang khởi động Engine...</span>
            <?php endif; ?>
          </td>
          <td style="max-width:280px;">
            <?php if (!empty($w['service_logs'])): ?>
              <div style="font-family:ui-monospace, monospace; font-size:11px; color:#38bdf8; background:#0f172a; padding:4px 8px; border-radius:4px; border:1px solid #1e293b; white-space:nowrap; overflow:hidden; text-overflow:ellipsis;" title="<?= htmlspecialchars($w['service_logs']) ?>">
                🟢 <?= htmlspecialchars($w['service_logs']) ?>
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

  <!-- Server Activity Logs -->
  <?php
    $f_ip = trim((string)($_GET['filter_ip'] ?? ''));
    $f_svc = trim((string)($_GET['filter_service'] ?? ''));
    $f_hours = max(1, (int)($_GET['filter_hours'] ?? 24));
    $cutoff_v = time() - ($f_hours * 3600);

    $log_sql = "SELECT * FROM cluster_logs WHERE created_at >= {$cutoff_v}";
    if (!empty($f_ip)) {
        $log_sql .= " AND ip LIKE '%" . SQLite3::escapeString($f_ip) . "%'";
    }
    if (!empty($f_svc) && $f_svc !== 'ALL') {
        $log_sql .= " AND (service = '" . SQLite3::escapeString($f_svc) . "' OR event = '" . SQLite3::escapeString($f_svc) . "')";
    }
    $log_sql .= " ORDER BY id DESC LIMIT 50";
    $log_res = $db->query($log_sql);

    $ips_query = $db->query("SELECT DISTINCT ip FROM workers WHERE ip IS NOT NULL AND ip != '' UNION SELECT DISTINCT ip FROM cluster_logs WHERE ip IS NOT NULL AND ip != ''");
    $distinct_ips = [];
    while ($ip_r = $ips_query->fetchArray(SQLITE3_ASSOC)) {
        if (!empty($ip_r['ip'])) $distinct_ips[] = $ip_r['ip'];
    }
  ?>
  <div class="section-hdr" style="margin-top:28px;">
    <span class="section-title">Nhật Ký Sự Kiện & Hoạt Động Server (Tự dọn dẹp theo giờ)</span>
    <span style="font-size:12px; color:var(--dim);">Tự động xoá log cũ hơn 24 giờ • Không cần mở file SQLite</span>
  </div>

  <div class="table-box">
    <!-- Log Filter Toolbar -->
    <form method="GET" class="filter-bar">
      <div style="display:flex; align-items:center; gap:6px; flex-wrap:wrap;">
        <span style="color:var(--muted); font-size:12px; font-weight:600;">🔍 Lọc:</span>
        <select name="filter_service" class="filter-select" onchange="this.form.submit()">
          <option value="ALL">-- Tất cả Dịch vụ & Event --</option>
          <option value="ENGINE_DATA" <?= $f_svc === 'ENGINE_DATA' ? 'selected' : '' ?>>ENGINE_DATA (Dữ liệu log từ máy con)</option>
          <option value="NodeAgent" <?= $f_svc === 'NodeAgent' ? 'selected' : '' ?>>NodeAgent (Node Workers)</option>
          <option value="NODE_JOIN" <?= $f_svc === 'NODE_JOIN' ? 'selected' : '' ?>>NODE_JOIN (Node mới kết nối)</option>
          <option value="RECONNECT" <?= $f_svc === 'RECONNECT' ? 'selected' : '' ?>>RECONNECT (Node kết nối lại)</option>
          <option value="HIGH_LOAD" <?= $f_svc === 'HIGH_LOAD' ? 'selected' : '' ?>>HIGH_LOAD (Cảnh báo quá tải)</option>
          <option value="BALANCE_UP" <?= $f_svc === 'BALANCE_UP' ? 'selected' : '' ?>>BALANCE_UP (Số dư tăng)</option>
          <option value="THRESHOLD_MET" <?= $f_svc === 'THRESHOLD_MET' ? 'selected' : '' ?>>THRESHOLD_MET (Đạt min rút)</option>
          <option value="SYNC_ERROR" <?= $f_svc === 'SYNC_ERROR' ? 'selected' : '' ?>>SYNC_ERROR (Lỗi kết nối)</option>
          <option value="TraffMonetizer" <?= $f_svc === 'TraffMonetizer' ? 'selected' : '' ?>>TraffMonetizer</option>
          <option value="Honeygain" <?= $f_svc === 'Honeygain' ? 'selected' : '' ?>>Honeygain</option>
          <option value="Pawns.app" <?= $f_svc === 'Pawns.app' ? 'selected' : '' ?>>Pawns.app</option>
          <option value="Repocket" <?= $f_svc === 'Repocket' ? 'selected' : '' ?>>Repocket</option>
          <option value="PacketStream" <?= $f_svc === 'PacketStream' ? 'selected' : '' ?>>PacketStream</option>
          <option value="Bitping" <?= $f_svc === 'Bitping' ? 'selected' : '' ?>>Bitping</option>
          <option value="EarnFM" <?= $f_svc === 'EarnFM' ? 'selected' : '' ?>>EarnFM</option>
          <option value="ProxyLite" <?= $f_svc === 'ProxyLite' ? 'selected' : '' ?>>ProxyLite</option>
          <option value="Grass Network" <?= $f_svc === 'Grass Network' ? 'selected' : '' ?>>Grass Network</option>
          <option value="Nodepay DePIN" <?= $f_svc === 'Nodepay DePIN' ? 'selected' : '' ?>>Nodepay DePIN</option>
        </select>

        <select name="filter_ip" class="filter-select" onchange="this.form.submit()">
          <option value="">-- Tất cả IP --</option>
          <?php foreach ($distinct_ips as $tip): ?>
          <option value="<?= htmlspecialchars($tip) ?>" <?= $f_ip === $tip ? 'selected' : '' ?>><?= htmlspecialchars($tip) ?></option>
          <?php endforeach; ?>
        </select>

        <select name="filter_hours" class="filter-select" onchange="this.form.submit()">
          <option value="1" <?= $f_hours === 1 ? 'selected' : '' ?>>1 Giờ gần nhất</option>
          <option value="6" <?= $f_hours === 6 ? 'selected' : '' ?>>6 Giờ gần nhất</option>
          <option value="12" <?= $f_hours === 12 ? 'selected' : '' ?>>12 Giờ gần nhất</option>
          <option value="24" <?= $f_hours === 24 ? 'selected' : '' ?>>24 Giờ (Toàn bộ)</option>
        </select>
      </div>

      <input type="text" id="liveSearchInput" placeholder="Tìm nhanh IP, tên service, node..." class="filter-input" style="width:210px;" onkeyup="filterLogRows()">

      <div style="margin-left:auto; display:flex; gap:8px;">
        <button type="button" id="btnCopyErrors" onclick="copyErrorLogs()" class="btn-filter" style="background:#dc2626; color:#fff; cursor:pointer; font-weight:600; display:inline-flex; align-items:center; gap:5px; border:none; padding:7px 12px; border-radius:6px;">⚠️ Copy Log Lỗi</button>
        <button type="button" id="btnCopyAll" onclick="copyAllLogs()" class="btn-filter" style="background:#0284c7; color:#fff; cursor:pointer; font-weight:600; display:inline-flex; align-items:center; gap:5px; border:none; padding:7px 12px; border-radius:6px;">📋 Copy Toàn Bộ Log</button>
        <?php if (!empty($f_ip) || (!empty($f_svc) && $f_svc !== 'ALL') || $f_hours !== 24): ?>
        <a href="cluster.php" class="btn-filter" style="background:#475569; text-decoration:none; display:inline-flex; align-items:center;">Đặt lại</a>
        <?php endif; ?>
        <a href="?action=purge_logs" class="btn-purge" title="Xóa sạch toàn bộ nhật ký sự kiện" onclick="return confirm('Xác nhận xóa sạch toàn bộ log trong database?');" style="text-decoration:none; display:inline-flex; align-items:center;">🗑️ Xóa sạch log</a>
      </div>
    </form>

    <table>
      <thead>
        <tr>
          <th>Thời Gian</th>
          <th>Dịch Vụ / Nguồn</th>
          <th>Node ID</th>
          <th>IP Address</th>
          <th>Sự Kiện</th>
          <th>Chi Tiết</th>
        </tr>
      </thead>
      <tbody>
        <?php
          $has_logs = false;
          while ($log = $log_res->fetchArray(SQLITE3_ASSOC)):
            $has_logs = true;
            $evt = (string)($log['event'] ?? '');
            $badge_cls = 'badge-muted';
            if ($evt === 'NODE_JOIN' || $evt === 'NODE_START' || $evt === 'BALANCE_UP' || $evt === 'THRESHOLD_MET') $badge_cls = 'badge-success';
            elseif ($evt === 'RECONNECT') $badge_cls = 'badge-warning';
            elseif ($evt === 'HIGH_LOAD' || strpos($evt, 'ERROR') !== false) $badge_cls = 'badge-danger';
        ?>
        <tr class="log-item-row" data-event="<?= htmlspecialchars($evt) ?>">
          <td style="font-family:ui-monospace, monospace; color:var(--dim); font-size:12px; white-space:nowrap;"><?= date('Y-m-d H:i:s', (int)$log['created_at']) ?></td>
          <td><span style="font-weight:600; color:#38bdf8;"><?= htmlspecialchars($log['service'] ?: 'NodeAgent') ?></span></td>
          <td><strong style="color:#fff; font-family:ui-monospace, monospace;"><?= htmlspecialchars($log['node_id']) ?></strong></td>
          <td style="font-family:ui-monospace, monospace; color:var(--muted);"><?= htmlspecialchars($log['ip']) ?></td>
          <td><span class="badge <?= $badge_cls ?>"><?= htmlspecialchars($log['event']) ?></span></td>
          <td style="color:var(--muted); font-size:12px;"><?= htmlspecialchars($log['details']) ?></td>
        </tr>
        <?php endwhile; ?>
        <?php if (!$has_logs): ?>
        <tr>
          <td colspan="6" style="text-align:center; color:var(--dim); padding:24px;">Không có log sự kiện nào phù hợp bộ lọc.</td>
        </tr>
        <?php endif; ?>
      </tbody>
    </table>
  </div>
</div>

<script>
function filterLogRows() {
  var q = (document.getElementById('liveSearchInput').value || '').toLowerCase();
  var rows = document.querySelectorAll('.log-item-row');
  for (var i = 0; i < rows.length; i++) {
    rows[i].style.display = rows[i].textContent.toLowerCase().indexOf(q) > -1 ? '' : 'none';
  }
}

function copyToClipboardFailsafe(text, btnId, successLabel, defaultLabel, activeBg, defaultBg) {
  var btn = document.getElementById(btnId);
  function onSuccess() {
    if (!btn) return;
    btn.innerHTML = successLabel;
    btn.style.background = activeBg;
    setTimeout(function() {
      btn.innerHTML = defaultLabel;
      btn.style.background = defaultBg;
    }, 2500);
  }

  // Strategy 1: Modern navigator.clipboard API (requires secure context HTTPS or localhost)
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).then(onSuccess).catch(function() {
      if (execFallback(text)) onSuccess();
      else promptFallback(text);
    });
    return;
  }

  // Strategy 2: ExecCommand('copy') with hidden textarea (works seamlessly on HTTP and custom domains)
  if (execFallback(text)) {
    onSuccess();
    return;
  }

  // Strategy 3: Prompt user directly if all programmatic copy attempts are denied
  promptFallback(text);
}

function execFallback(text) {
  var ta = document.createElement('textarea');
  ta.value = text;
  ta.setAttribute('readonly', '');
  ta.style.position = 'fixed';
  ta.style.left = '-9999px';
  ta.style.top = '0';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.focus();
  ta.select();
  ta.setSelectionRange(0, 999999);
  var ok = false;
  try {
    ok = document.execCommand('copy');
  } catch (err) {
    ok = false;
  }
  document.body.removeChild(ta);
  return ok;
}

function promptFallback(text) {
  window.prompt('Sao chép nhật ký bên dưới (Ctrl+C / Cmd+C):', text);
}

function copyErrorLogs() {
  var rows = document.querySelectorAll('.log-item-row');
  var lines = ['=== NHAT KY LOI CLUSTER (ERROR LOGS) ===', 'Thoi Gian | Dich Vu | Node ID | IP | Su Kien | Chi Tiet'];
  var count = 0;
  for (var i = 0; i < rows.length; i++) {
    var ev = rows[i].getAttribute('data-event') || '';
    var badge = rows[i].querySelector('.badge');
    var badgeText = badge ? badge.textContent : '';
    if (ev.indexOf('ERROR') > -1 || ev.indexOf('HIGH_LOAD') > -1 || badgeText.indexOf('ERROR') > -1 || badgeText.indexOf('HIGH_LOAD') > -1) {
      var cells = rows[i].querySelectorAll('td');
      var r = [];
      for (var j = 0; j < cells.length; j++) {
        r.push(cells[j].textContent.trim());
      }
      lines.push(r.join(' | '));
      count++;
    }
  }
  if (count === 0) lines.push('(Khong co su kien loi nao trong he thong)');
  copyToClipboardFailsafe(
    lines.join(String.fromCharCode(10)),
    'btnCopyErrors',
    'Da Copy (' + count + ' loi)!',
    '⚠️ Copy Log Lỗi',
    '#16a34a',
    '#dc2626'
  );
}

function copyAllLogs() {
  var rows = document.querySelectorAll('.log-item-row');
  var lines = ['=== TOAN BO NHAT KY CLUSTER (ALL LOGS) ===', 'Thoi Gian | Dich Vu | Node ID | IP | Su Kien | Chi Tiet'];
  for (var i = 0; i < rows.length; i++) {
    var cells = rows[i].querySelectorAll('td');
    var r = [];
    for (var j = 0; j < cells.length; j++) {
      r.push(cells[j].textContent.trim());
    }
    lines.push(r.join(' | '));
  }
  if (rows.length === 0) lines.push('(Khong co dong log nao)');
  copyToClipboardFailsafe(
    lines.join(String.fromCharCode(10)),
    'btnCopyAll',
    'Da Copy ' + rows.length + ' dong!',
    '📋 Copy Toàn Bộ Log',
    '#16a34a',
    '#0284c7'
  );
}
</script>

</body>
</html>