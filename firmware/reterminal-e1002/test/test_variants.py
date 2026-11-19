"""Host regression checks: build isolation and real radio init against a fake IDF."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parents[1]
ROOT = HERE.parents[1]
spec = importlib.util.spec_from_file_location("e1002_build", HERE / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


def patched_source(project, relative):
    """Exercise committed patches without changing the submodule worktree."""
    pristine = subprocess.check_output(
        ["git", "-C", str(ROOT / "third_party" / project), "show", f"HEAD:{relative}"]
    )
    with tempfile.TemporaryDirectory() as tmp:
        target = Path(tmp) / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(pristine)
        patch_dir = ROOT / "patches" / project
        for name in (patch_dir / "series").read_text().splitlines():
            if name and not name.startswith("#"):
                subprocess.run(["git", "apply", f"--include={relative}",
                                str(patch_dir / name)], cwd=tmp, check=True)
        return target.read_text()


class Variants(unittest.TestCase):
    def test_isolated_config_and_common_defaults(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(build, "run") as run:
            # build.py resolves build_dir; on macOS /var/folders → /private/var/folders.
            out_root = Path(tmp).resolve()
            for name in build.IDF_VARIANTS:
                build.build_variant(name, tmp)
            commands = [call.args[0] for call in run.call_args_list]
            for name, cmd in zip(build.IDF_VARIANTS, commands):
                self.assertIn(f"SDKCONFIG={out_root}/e1002-{name}/sdkconfig", cmd)
                defaults = next(v for v in cmd if str(v).startswith("SDKCONFIG_DEFAULTS="))
                self.assertIn("sdkconfig.airdrop;", defaults)
                self.assertTrue(defaults.endswith(build.IDF_VARIANTS[name]))

    def test_open_cargo_command_targets_foa_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            cmd = build.open_cargo_cmd(tmp)
            self.assertEqual(cmd[0], "cargo")
            self.assertIn("--release", cmd)
            self.assertTrue(any("esp32s3" in str(c) for c in cmd))
            self.assertTrue(any("firmware" in str(c) for c in cmd))
            self.assertEqual(build.OPEN_TARGET, "xtensa-esp32s3-none-elf")
            self.assertIn(build.OPEN_TARGET, cmd)
            out = Path(tmp).resolve() / "e1002-open" / "cargo-target"
            self.assertIn(str(out), cmd)
            self.assertTrue((build.OPEN_DIR / "Cargo.toml").is_file())
            self.assertTrue((build.OPEN_DIR / "src" / "main.rs").is_file())
            main = (build.OPEN_DIR / "src" / "main.rs").read_text()
            self.assertIn("foa_sta", main)
            self.assertIn("foa_awdl", main)
            self.assertIn("AWDL_CHANNEL", main)
            # Not a heartbeat-only stub: real association/scan + companion UART TX.
            self.assertIn("connect_by_ssid", main)
            self.assertIn("scan_continuously", main)
            self.assertIn("companion_uart_task", main)
            self.assertIn("AwdlEvent", main)

    def test_radio_ownership_and_channel_guard(self):
        source = patched_source("ESP32Drop", "ESP32Drop/src/awdl/port/awdl_port_esp32.cpp")
        start = source.index("static esp_err_t wifi_probe_init(void) {")
        end = source.index("\n}\n", start) + 2
        function = source[start:end]
        stub = r'''
#include <cassert>
#include <cstring>
#include <vector>
#include <string>
using esp_err_t = int;
using wifi_mode_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=1, WIFI_MODE_STA=1, WIFI_MODE_AP=2,
 WIFI_MODE_APSTA=3, WIFI_IF_AP=1, WIFI_AUTH_OPEN=0, WIFI_STORAGE_RAM=0,
 WIFI_PS_NONE=0, WIFI_PROMIS_FILTER_MASK_MGMT=1, WIFI_PROMIS_FILTER_MASK_DATA=2,
 WIFI_SECOND_CHAN_NONE=0, PROBE_CHANNEL=6;
struct wifi_ap_record_t { int primary=6; };
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
struct wifi_config_t { struct { unsigned char ssid[33]; int channel, authmode,
 ssid_hidden, max_connection, beacon_interval; } ap; };
struct wifi_promiscuous_filter_t { int filter_mask; };
unsigned char g_awdl_mac[6];
int channel=6, mode=WIFI_MODE_STA, fail=0;
std::vector<std::string> calls;
int record(const char *s) { calls.push_back(s); return fail == (int)calls.size() ? 42 : 0; }
void ignore_state(int) {}
#define SIMPLE(name) template<class... T> int name(T...){return record(#name);}
SIMPLE(nvs_flash_init) SIMPLE(esp_netif_init) SIMPLE(esp_event_loop_create_default)
SIMPLE(esp_wifi_init) SIMPLE(esp_wifi_set_storage) SIMPLE(esp_wifi_set_mac)
SIMPLE(esp_wifi_set_config) SIMPLE(esp_wifi_start) SIMPLE(esp_wifi_register_80211_tx_cb)
SIMPLE(esp_wifi_set_ps) SIMPLE(esp_wifi_set_promiscuous_filter)
SIMPLE(esp_wifi_set_promiscuous_rx_cb) SIMPLE(esp_wifi_set_promiscuous)
SIMPLE(esp_wifi_set_channel)
int esp_wifi_get_mode(int *out) {*out=mode; return record("get_mode");}
int esp_wifi_sta_get_ap_info(wifi_ap_record_t *out) {out->primary=channel; return record("get_ap");}
int esp_wifi_set_mode(int m) {mode=m; return record("set_mode");}
int esp_wifi_get_mac(int, unsigned char *out) {memset(out, 0x10, 6); return record("get_mac");}
void raw_tx_done_cb() {} void sniffer_cb() {}
bool called(const char *s) {for(auto &c:calls) if(c==s)return true; return false;}
'''
        assertions = r'''
int main() {
 assert(wifi_probe_init()==ESP_OK);
 const int successful_calls = calls.size();
#if CONFIG_AIRDROP_COEX
 assert(mode==WIFI_MODE_APSTA);
 assert(!called("esp_wifi_init") && !called("esp_wifi_start"));
 assert(!called("esp_wifi_set_channel") && !called("esp_wifi_set_mac"));
 mode=WIFI_MODE_STA; channel=11; calls.clear();
 assert(wifi_probe_init()==ESP_ERR_INVALID_STATE); assert(mode==WIFI_MODE_STA);
 mode=WIFI_MODE_AP; channel=6; calls.clear();
 assert(wifi_probe_init()==ESP_ERR_INVALID_STATE); assert(mode==WIFI_MODE_AP);
#else
 assert(mode==WIFI_MODE_AP); assert(called("esp_wifi_init"));
 assert(called("esp_wifi_start") && called("esp_wifi_set_channel"));
 assert((g_awdl_mac[0] & 3)==2);
#endif
 // Every fallible driver API after the three shared init calls propagates errors.
 for (int n=4; n<=successful_calls; ++n) {
   calls.clear(); mode=WIFI_MODE_STA; channel=6; fail=n;
   assert(wifi_probe_init()==42);
 }
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "probe.cpp"
            cpp.write_text(stub + function + assertions)
            for coex in (0, 1):
                binary = Path(tmp) / f"probe-{coex}"
                subprocess.run(["g++", "-std=c++17", f"-DCONFIG_AIRDROP_COEX={coex}", str(cpp), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


    def test_listener_shutdown_keeps_interface_while_busy(self):
        source = patched_source("ESP32Drop", "ESP32Drop/src/airdrop/port/ad_port_esp32.cpp")
        start = source.index("int ad_end(void) {")
        end = source.index("\n}\n", start) + 2
        stub = r'''
#include <cassert>
#include <cstddef>
#define pdMS_TO_TICKS(x) (x)
bool g_listener_stop=false, g_listener_running=false, detached=false;
int remaining=0;
void vTaskDelay(int) { if(remaining>0 && --remaining==0) g_listener_running=false; }
void awdl_rx_tap_set(std::nullptr_t, std::nullptr_t) {detached=true;}
'''
        assertions = r'''
int main() {
 g_listener_running=true;
 assert(ad_end()==-1); assert(g_listener_stop); assert(!detached);
 remaining=3;
 assert(ad_end()==0); assert(detached); assert(!g_listener_running);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "stop.cpp"
            cpp.write_text(stub + source[start:end] + assertions)
            binary = Path(tmp) / "stop"
            subprocess.run(["g++", "-std=c++17", str(cpp), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


    def test_http_mutations_are_gated_only_during_coex(self):
        source = patched_source("esp32-photoframe", "main/http_server.c")
        start = source.index("static esp_err_t auth_gate(httpd_req_t *req)\n{")
        end = source.index("\n}\n", start) + 2
        stub = r'''
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
using esp_err_t=int;
constexpr int ESP_OK=0, HTTP_GET=0, HTTP_POST=1;
struct httpd_req_t {int method; const char *uri; void *user_ctx;};
using http_handler_fn=int(*)(httpd_req_t*);
enum auth_result_t {AUTH_OK, AUTH_DENIED, AUTH_LOCKED_OUT};
auth_result_t result=AUTH_OK;
bool active=false, handled=false;
const char *status=nullptr;
auth_result_t http_auth_check(httpd_req_t*, int64_t*) {return result;}
bool airdrop_bridge_is_active() {return active;}
void httpd_resp_set_status(httpd_req_t*, const char *s) {status=s;}
void httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) {}
void httpd_resp_set_type(httpd_req_t*, const char*) {}
int httpd_resp_sendstr(httpd_req_t*, const char*) {return 0;}
int handler(httpd_req_t*) {handled=true; return 0;}
'''
        assertions = r'''
int main() {
 httpd_req_t req{HTTP_POST, "/api/sleep", (void*)handler};
 auth_gate(&req); assert(handled);
 handled=false; active=true; auth_gate(&req);
#if CONFIG_AIRDROP_COEX
 assert(!handled && !strcmp(status,"503 Service Unavailable"));
#else
 assert(handled);
#endif
 handled=false; req.method=HTTP_GET; auth_gate(&req); assert(handled);
 handled=false; req.method=HTTP_POST; req.uri="/api/keep_alive";
 auth_gate(&req); assert(handled);
 handled=false; result=AUTH_DENIED;
 auth_gate(&req); assert(!handled && !strcmp(status,"401 Unauthorized"));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            cpp = Path(tmp) / "http.cpp"
            cpp.write_text(stub + source[start:end] + assertions)
            for coex in (0, 1):
                binary = Path(tmp) / f"http-{coex}"
                subprocess.run(["g++", "-std=c++17", f"-DCONFIG_AIRDROP_COEX={coex}",
                                str(cpp), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
