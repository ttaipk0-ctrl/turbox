#ifndef PAWNS_CPP
#define PAWNS_CPP

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <dirent.h>
#include <csignal>
#include <sys/types.h>
#include <sys/stat.h>

class PawnsEngine {
private:
    static inline int safe_exec(const std::string& cmd) {
        int r = system(cmd.c_str());
        (void)r;
        return r;
    }

    static bool check_pawns_socket(pid_t pid) {
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
                bool found = (fgets(buf, sizeof(buf), p)) != nullptr;
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
            std::getline(f, line);
            while (std::getline(f, line)) {
                std::istringstream ss(line);
                std::string sl, local, rem, st, tx_rx, tr, retr, uid, timeout, inode;
                if (ss >> sl >> local >> rem >> st >> tx_rx >> tr >> retr >> uid >> timeout >> inode) {
                    if (st == "01" && inodes.find(inode) != inodes.end()) {
                        return true;
                    }
                }
            }
        }
        return false;
#endif
    }

    static std::string parse_pawns_log() {
        std::ifstream f("/tmp/.tb_pawns.log");
        if (!f.is_open()) return "";
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) {
                lines.push_back(line);
                if (lines.size() > 30) lines.erase(lines.begin());
            }
        }
        if (lines.empty()) return "";
        std::string latest = lines.back();
        if (latest.rfind("[Pawns] ", 0) == 0) latest = latest.substr(8);
        if (latest.length() > 100) latest = latest.substr(0, 97) + "...";
        return latest;
    }

public:
    static pid_t get_pid() {
        pid_t pid = -1;
        std::ifstream pf("/tmp/.tb_pawns.pid");
        if (pf >> pid && pid > 0 && kill(pid, 0) == 0) {
            return pid;
        }
        FILE* p = popen("pgrep -x 'pawns-cli' 2>/dev/null | head -n 1", "r");
        if (p) {
            char buf[32];
            if (fgets(buf, sizeof(buf), p)) pid = (pid_t)std::atoi(buf);
            pclose(p);
            if (pid > 0 && kill(pid, 0) == 0) return pid;
        }
        return -1;
    }

    static bool is_alive() {
        return (get_pid() > 0);
    }

    static void stop() {
        pid_t pid = get_pid();
        if (pid > 0) {
            kill(pid, SIGTERM);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            kill(pid, SIGKILL);
        }
        safe_exec("pkill -9 -x 'pawns-cli' 2>/dev/null || true; "
                  "rm -rf /tmp/.tb_pawns* 2>/dev/null || true; "
                  "rm -f /tmp/.tb_pawns.pid 2>/dev/null || true");
    }

    static std::string get_status_report() {
        pid_t pid = get_pid();
        if (pid > 0) {
            bool connected = check_pawns_socket(pid);
            std::string sock_str = connected ? "ESTABLISHED" : "CONNECTING";
            std::string log_msg = parse_pawns_log();
            if (log_msg.empty()) log_msg = "Pawns service is active";
            return "[Pawns: PID " + std::to_string(pid) + "] Socket: " + sock_str + " | Log: " + log_msg;
        } else {
            std::string log_msg = parse_pawns_log();
            if (log_msg.find("authenticate user failed") != std::string::npos) {
                return "[Pawns] Auth Failed (Sai email/mat khau tren pawns.app)";
            } else if (log_msg.find("Missing credentials") != std::string::npos || log_msg.find("Skipped") != std::string::npos) {
                return "[Pawns] Not started (Missing credentials)";
            } else if (!log_msg.empty()) {
                return "[Pawns] Not started (" + log_msg + ")";
            } else {
                return "[Pawns] Not started";
            }
        }
    }

    static void start(const std::string& email, const std::string& pass, const std::string& node_id) {
        if (is_alive()) return;

        if (email.empty() || email.find("YOUR_") != std::string::npos ||
            pass.empty() || pass.find("YOUR_") != std::string::npos) {
            std::ofstream pf("/tmp/.tb_pawns.log");
            if (pf.is_open()) {
                pf << "[Pawns] Skipped: Missing credentials" << std::endl;
            }
            return;
        }

#if defined(__APPLE__) || defined(__MACH__)
        std::ofstream pf("/tmp/.tb_pawns.log");
        if (pf.is_open()) {
            pf << "[Pawns] Pawns CLI chi ho tro Linux native. Tren macOS vui long cai Pawns.app." << std::endl;
        }
        return;
#else
        std::string pawns_bin = "/tmp/.tb_pawns/pawns-cli";
        if (access(pawns_bin.c_str(), X_OK) != 0) {
            safe_exec("mkdir -p /tmp/.tb_pawns 2>/dev/null");
            FILE* py_f = fopen("/tmp/.tb_pawns_get.py", "w");
            if (py_f) {
                fputs("import urllib.request, json, tarfile, io, os, platform\n"
                      "try:\n"
                      "    m = platform.machine().lower()\n"
                      "    is_arm = 'aarch64' in m or 'arm' in m\n"
                      "    tok = json.loads(urllib.request.urlopen('https://auth.docker.io/token?service=registry.docker.io&scope=repository:iproyal/pawns-cli:pull', timeout=15).read().decode())['token']\n"
                      "    layer = 'sha256:15b93f40521a2c6beeb61aa9753e4def6761cd03a0e68eb460e8a909a6dc7194' if is_arm else 'sha256:c97a3c810504f8ac054c17d4b559c20274985ed72bbcad3622f75136c8ffadec'\n"
                      "    req = urllib.request.Request('https://registry-1.docker.io/v2/iproyal/pawns-cli/blobs/' + layer, headers={'Authorization': 'Bearer ' + tok})\n"
                      "    data = urllib.request.urlopen(req, timeout=30).read()\n"
                      "    tf = tarfile.open(fileobj=io.BytesIO(data))\n"
                      "    tf.extract('pawns-cli', '/tmp/.tb_pawns')\n"
                      "    os.chmod('/tmp/.tb_pawns/pawns-cli', 0o755)\n"
                      "except Exception:\n"
                      "    pass\n", py_f);
                fclose(py_f);
                safe_exec("python3 /tmp/.tb_pawns_get.py 2>/dev/null && rm -f /tmp/.tb_pawns_get.py");
            }
        }

        if (access(pawns_bin.c_str(), X_OK) == 0) {
            std::string run_cmd = "nohup " + pawns_bin +
                                  " -email='" + email + "' -password='" + pass +
                                  "' -device-id='" + node_id + "' -device-name='" + node_id +
                                  "' -accept-tos > /tmp/.tb_pawns.log 2>&1 & echo $! > /tmp/.tb_pawns.pid";
            safe_exec(run_cmd);
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
#endif
    }
};

#endif // PAWNS_CPP
