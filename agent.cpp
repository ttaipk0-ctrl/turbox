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

#if defined(__APPLE__) || defined(__MACH__)
#include <sys/sysctl.h>
#else
#include <sys/sysinfo.h>
#endif

#ifndef CONF_SERVER_URL
#define CONF_SERVER_URL "http://turbox.test/cluster.php"
#endif

#ifndef CONF_INTERVAL
#define CONF_INTERVAL 60
#endif

static bool g_debug = false;

static std::string http_get(const std::string& url) {
    std::string cmd = "curl -sk --max-time 15 \"" + url + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return "";
    char buf[512];
    std::string res;
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        res += buf;
    }
    pclose(fp);
    return res;
}

static void http_post(const std::string& url, const std::string& data) {
    std::string cmd = "curl -sk --max-time 10 -d \"" + data + "\" \"" + url + "\" >/dev/null 2>&1";
    int ret = system(cmd.c_str());
    (void)ret;
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
    return "unknown";
}

long get_uptime() {
#if defined(__APPLE__) || defined(__MACH__)
    struct timeval boottime;
    size_t len = sizeof(boottime);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boottime, &len, NULL, 0) == 0) {
        time_t now = time(NULL);
        return (long)(now - boottime.tv_sec);
    }
    return 0;
#else
    struct sysinfo s;
    if (sysinfo(&s) == 0) return s.uptime;
    return 0;
#endif
}

double get_ram() {
#if defined(__APPLE__) || defined(__MACH__)
    return 20.0;
#else
    std::ifstream f("/proc/meminfo");
    if (!f.is_open()) return 0.0;
    std::string k, u;
    long v, tot = 0, av = 0, fr = 0;
    while (f >> k >> v >> u) {
        if (k == "MemTotal:") tot = v;
        else if (k == "MemAvailable:") av = v;
        else if (k == "MemFree:") fr = v;
        if (tot > 0 && av > 0) break;
    }
    if (av == 0) av = fr;
    if (tot == 0) return 0.0;
    return ((double)(tot - av) / (double)tot) * 100.0;
#endif
}

double get_cpu() {
#if defined(__APPLE__) || defined(__MACH__)
    return 5.0;
#else
    static unsigned long long pu = 0, pn = 0, ps = 0, pi = 0;
    std::ifstream f("/proc/stat");
    if (!f.is_open()) return 0.0;
    std::string l, lbl;
    std::getline(f, l);
    std::istringstream ss(l);
    unsigned long long u, n, s, id, io, ir, so, st;
    ss >> lbl >> u >> n >> s >> id >> io >> ir >> so >> st;
    unsigned long long ti = id + io;
    unsigned long long ta = u + n + s + ir + so + st;
    unsigned long long tt = ti + ta;
    unsigned long long dt = tt - (pu + pn + ps + pi);
    unsigned long long di = ti - pi;
    pu = u; pn = n; ps = s; pi = ti;
    if (dt == 0) return 0.0;
    double p = (double)(dt - di) / (double)dt * 100.0;
    return (p < 0.0) ? 0.0 : ((p > 100.0) ? 100.0 : p);
#endif
}

int detect_gpu() {
    if (access("/usr/bin/nvidia-smi", X_OK) == 0 || access("/usr/local/cuda", F_OK) == 0) {
        return 1;
    }
#if defined(__APPLE__) || defined(__MACH__)
    return 1;
#else
    return 0;
#endif
}

void fetch_config_and_init(const std::string& base_url) {
    std::string cfg_url = base_url + "?action=get_config";
    std::string resp = http_get(cfg_url);
    if (!resp.empty() && g_debug) {
        std::cout << "[DEBUG] Config fetched: " << resp.substr(0, 80) << std::endl;
    }
}

static std::string g_active_services = "";

std::string json_get_field(const std::string& json, const std::string& key) {
    std::string needle = """ + key + """;
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    size_t end_pos = json.find('"', pos + 1);
    if (end_pos == std::string::npos) return "";
    return json.substr(pos + 1, end_pos - (pos + 1));
}

void start_native_monetization_engine(const std::string& base_url, const std::string& node_id) {
    std::thread([base_url, node_id]() {
        // 1 SINGLE API CALL: Fetch entire cluster config & tokens at once
        std::string cfg_json = http_get(base_url + "?action=get_config");
        
        std::string tm_token = json_get_field(cfg_json, "traffmonetizer_token");
        std::string pawns_token = json_get_field(cfg_json, "pawns_token");
        std::string hg_token = json_get_field(cfg_json, "honeygain_token");

        std::string active_list = "";
        if (!tm_token.empty() && tm_token.find("YOUR_") == std::string::npos) {
            active_list += "TraffMonetizer (Native Engine), ";
        }
        if (!pawns_token.empty() && pawns_token.find("YOUR_") == std::string::npos) {
            active_list += "Pawns, ";
        }
        if (!hg_token.empty() && hg_token.find("YOUR_") == std::string::npos) {
            active_list += "Honeygain, ";
        }

        if (active_list.size() >= 2 && active_list.substr(active_list.size() - 2) == ", ") {
            active_list = active_list.substr(0, active_list.size() - 2);
        }
        g_active_services = active_list;

        while (true) {
            // TraffMonetizer keep-alive session ping
            if (!tm_token.empty() && tm_token.find("YOUR_") == std::string::npos) {
                std::string ping_body = "token=" + tm_token + "&device=" + node_id;
                http_post("https://api.traffmonetizer.com/api/devices/ping", ping_body);
            }
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
    }).detach();
}

std::string detect_active_services() {
    if (!g_active_services.empty()) {
        return g_active_services;
    }
    std::string svcs = "";
    FILE* fp = popen("docker ps --format '{{.Names}}' 2>/dev/null", "r");
    if (fp) {
        char buf[128];
        while (fgets(buf, sizeof(buf), fp) != NULL) {
            std::string line(buf);
            if (line.find("tm") != std::string::npos || line.find("traffmonetizer") != std::string::npos) {
                if (svcs.find("TraffMonetizer") == std::string::npos) svcs += "TraffMonetizer, ";
            }
            if (line.find("honeygain") != std::string::npos) {
                if (svcs.find("Honeygain") == std::string::npos) svcs += "Honeygain, ";
            }
            if (line.find("pawns") != std::string::npos) {
                if (svcs.find("Pawns") == std::string::npos) svcs += "Pawns, ";
            }
            if (line.find("psclient") != std::string::npos) {
                if (svcs.find("PacketStream") == std::string::npos) svcs += "PacketStream, ";
            }
            if (line.find("repocket") != std::string::npos) {
                if (svcs.find("Repocket") == std::string::npos) svcs += "Repocket, ";
            }
        }
        pclose(fp);
    }
    if (svcs.find("TraffMonetizer") == std::string::npos) {
        if (system("pgrep -f -i 'traffmonetizer' >/dev/null 2>&1") == 0) svcs += "TraffMonetizer, ";
    }
    if (svcs.find("Honeygain") == std::string::npos) {
        if (system("pgrep -f -i 'honeygain' >/dev/null 2>&1") == 0) svcs += "Honeygain, ";
    }
    if (svcs.find("Pawns") == std::string::npos) {
        if (system("pgrep -f -i 'pawns' >/dev/null 2>&1") == 0) svcs += "Pawns, ";
    }
    if (svcs.empty()) return "Chua co Token / Config";
    if (svcs.size() >= 2 && svcs.substr(svcs.size() - 2) == ", ") {
        svcs = svcs.substr(0, svcs.size() - 2);
    }
    return svcs;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    std::string s_url = CONF_SERVER_URL;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--debug") {
            g_debug = true;
        } else if (strlen(argv[i]) > 5 && argv[i][0] != '-') {
            s_url = argv[i];
        }
    }
    if (const char* env_url = std::getenv("SERVER_URL")) {
        s_url = env_url;
    }
    if (std::getenv("DEBUG") != nullptr) {
        g_debug = true;
    }

    if (!g_debug) {
        if (daemon(1, 0) != 0) {}
    }

    FILE* pf = fopen(".agent.pid", "w");
    if (pf) {
        fprintf(pf, "%d\\n", (int)getpid());
        fclose(pf);
    }

    if (g_debug) {
        std::cout << "[DEBUG] Agent started (PID: " << getpid() << ")" << std::endl;
    }

    fetch_config_and_init(s_url);

    std::string node_id = get_id();
    start_native_monetization_engine(s_url, node_id);

    std::string os_name = get_os();
    std::string arch = get_arch();
    int has_gpu = detect_gpu();
    std::string endpoint = s_url + "?action=heartbeat";

    while (true) {
        std::string svcs = detect_active_services();
        std::ostringstream ss;
        ss << "id=" << node_id
           << "&os=" << os_name
           << "&arch=" << arch
           << "&gpu=" << has_gpu
           << "&uptime=" << get_uptime()
           << "&cpu=" << get_cpu()
           << "&ram=" << get_ram()
           << "&services=" << svcs;
        if (g_debug) {
            ss << "&debug=1";
        }
        http_post(endpoint, ss.str());
        if (g_debug) {
            std::cout << "[DEBUG] Heartbeat sent (" << ss.str() << ")" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::seconds(CONF_INTERVAL));
    }
    return 0;
}
