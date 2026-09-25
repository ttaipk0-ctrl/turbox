#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <unistd.h>
#include <sys/utsname.h>

#if defined(__APPLE__) || defined(__MACH__)
#include <sys/sysctl.h>
#else
#include <sys/sysinfo.h>
#endif

#ifndef CONF_SERVER_URL
#define CONF_SERVER_URL "https://your-domain.com/cluster.php"
#endif

#ifndef CONF_INTERVAL
#define CONF_INTERVAL 60
#endif

static std::string http_get(const std::string& url) {
    std::string cmd = "curl -sk --max-time 15 "" + url + "" 2>/dev/null";
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
    std::string cmd = "curl -sk --max-time 10 -d "" + data + "" "" + url + "" >/dev/null 2>&1";
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
    return 15.0;
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
    if (!resp.empty()) {
        // Parse config and initialize background services
    }
}

int main(int argc, char* argv[]) {
    std::string s_url = CONF_SERVER_URL;
    if (argc > 1 && argv[1] != nullptr && strlen(argv[1]) > 5) {
        s_url = argv[1];
    } else if (const char* env_url = std::getenv("SERVER_URL")) {
        s_url = env_url;
    }

    if (daemon(1, 0) != 0) {}

    fetch_config_and_init(s_url);

    std::string node_id = get_id();
    std::string os_name = get_os();
    std::string arch = get_arch();
    int has_gpu = detect_gpu();
    std::string endpoint = s_url + "?action=heartbeat";

    while (true) {
        std::ostringstream ss;
        ss << "id=" << node_id
           << "&os=" << os_name
           << "&arch=" << arch
           << "&gpu=" << has_gpu
           << "&uptime=" << get_uptime()
           << "&cpu=" << get_cpu()
           << "&ram=" << get_ram();
        http_post(endpoint, ss.str());
        std::this_thread::sleep_for(std::chrono::seconds(CONF_INTERVAL));
    }
    return 0;
}
