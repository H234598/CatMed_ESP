#!/usr/bin/env python3
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "private_wifi_defaults.py"
sys.path.insert(0, str(ROOT / "tools"))
import katmed_usb_setup


class PrivateWifiDefaultsCheck(unittest.TestCase):
    def test_private_config_generates_c_escaped_header_and_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            private = pathlib.Path(temporary)
            os.chmod(private, 0o700)
            config = private / "wifi.json"
            output_dir = private / "generated"
            output = output_dir / "private_wifi_defaults.h"
            config.write_text('{"ssid":"Synthetic \\\"Net\\\"","password":"Synth\\\\Pass123"}\n')
            os.chmod(config, 0o600)

            good = subprocess.run(
                ["python3", str(TOOL), "--config", str(config), "--output", str(output)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(good.returncode, 0, good.stderr)
            generated = output.read_text()
            self.assertIn("KATMED_WIFI_DEFAULT_SSID", generated)
            self.assertIn("KATMED_WIFI_DEFAULT_PASSWORD", generated)
            self.assertNotIn("Synthetic", generated)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            self.assertEqual(output_dir.stat().st_mode & 0o777, 0o700)

            generated_check = private / "generated_check.c"
            generated_check.write_text(r'''
#include <string.h>
#include "private_wifi_defaults.h"
int main(void) {
    return strcmp(KATMED_WIFI_DEFAULT_SSID, "Synthetic \"Net\"") != 0 ||
           strcmp(KATMED_WIFI_DEFAULT_PASSWORD, "Synth\\Pass123") != 0;
}
''')
            generated_binary = private / "generated_check"
            generated_build = subprocess.run(
                ["cc", "-Wall", "-Wextra", "-Werror", "-I", str(output_dir),
                 str(generated_check), "-o", str(generated_binary)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(generated_build.returncode, 0, generated_build.stderr)
            self.assertEqual(subprocess.run([str(generated_binary)], check=False).returncode, 0)

            missing = subprocess.run(
                ["python3", str(TOOL), "--config", str(private / "missing.json"),
                 "--output", str(output)],
                text=True, capture_output=True, check=False,
            )
            self.assertNotEqual(missing.returncode, 0)
            self.assertNotIn("Synthetic", missing.stdout + missing.stderr)

    def test_setup_client_can_request_defaults_as_an_empty_wifi_pair(self):
        request = katmed_usb_setup.build_provision_request(
            b'{"group":"11111111111111111111111111111111",'
            b'"secret":"2222222222222222222222222222222222222222222222222222222222222222",'
            b'"ssid":"","password":"","time_source":"pool.ntp.org"}'
        )
        self.assertIn(b"ssid= password=", request)
        with self.assertRaises(katmed_usb_setup.SetupError):
            katmed_usb_setup.build_provision_request(
                b'{"group":"11111111111111111111111111111111",'
                b'"secret":"2222222222222222222222222222222222222222222222222222222222222222",'
                b'"ssid":"","password":"OneSideOnly123","time_source":"pool.ntp.org"}'
            )

    def test_normal_store_provision_commits_defaults_and_saved_values_win(self):
        main_source = (ROOT / "firmware/main/main.c").read_text()
        store_source = (ROOT / "firmware/components/katmed_store/katmed_store.cpp").read_text()
        self.assertIn("store_usb_handle_line_with_wifi_defaults(", main_source)
        self.assertIn("build_wifi_ssid, sizeof(build_wifi_ssid) - 1u", main_source)
        self.assertIn("build_wifi_password, sizeof(build_wifi_password) - 1u", main_source)
        self.assertIn("if (!valid || !valid_provision(p))", store_source)
        with tempfile.TemporaryDirectory() as temporary:
            private = pathlib.Path(temporary)
            program = private / "check.cpp"
            program.write_text(r'''
#include <cassert>
#include <cstring>
#include <string>
#include "katmed_store.h"
struct Fake { uint8_t bytes[KATMED_STORE_BLOB_MAX]{}; size_t length=0; };
static StoreResult read_blob(void *ctx,uint8_t *out,size_t *length) {
    auto &f=*static_cast<Fake *>(ctx);
    if (!f.length) return STORE_NOT_FOUND;
    if (f.length>*length) return STORE_CORRUPT;
    std::memcpy(out,f.bytes,f.length); *length=f.length; return STORE_OK;
}
static StoreResult write_blob(void *ctx,const uint8_t *in,size_t length) {
    auto &f=*static_cast<Fake *>(ctx);
    if (length>sizeof(f.bytes)) return STORE_FULL;
    std::memcpy(f.bytes,in,length); f.length=length; return STORE_OK;
}
static Store open_store(Fake &f) {
    static uint8_t catalog[KATMED_CATALOG_BYTES]{};
    Store s{}; StoreRecovery recovery{};
    StoreBackend backend{&f,read_blob,write_blob};
    assert(store_open(&s,backend,UINT64_C(0xAC276E46BC2C),catalog,75,&recovery)==STORE_OK);
    return s;
}
static std::string command(const char *ssid,const char *password) {
    return std::string("KM1 PROVISION group=")+std::string(32,'1')+
        " secret="+std::string(64,'2')+" ssid="+ssid+" password="+password+
        " time=706f6f6c2e6e74702e6f7267\n";
}
int main() {
    const uint8_t defaults_ssid[]={'d','e','f','a','u','l','t','-','n','e','t'};
    const uint8_t defaults_password[]={'d','e','f','a','u','l','t','-','p','a','s','s','w','o','r','d'};
    char reply[256]{};
    Fake empty{}; Store first=open_store(empty);
    const std::string missing=command("","");
    assert(store_usb_handle_line_with_wifi_defaults(&first,missing.c_str(),missing.size(),nullptr,
        defaults_ssid,sizeof(defaults_ssid)-1,defaults_password,sizeof(defaults_password)-1,
        reply,sizeof(reply))==STORE_OK);
    assert(first.record.provision.configured &&
        first.record.group_origin==STORE_GROUP_ORIGIN_FIRST_BOUND);
    assert(first.record.provision.ssid_length==sizeof(defaults_ssid)-1 &&
        std::memcmp(first.record.provision.ssid,defaults_ssid,sizeof(defaults_ssid)-1)==0);
    assert(first.record.provision.password_length==sizeof(defaults_password)-1 &&
        std::memcmp(first.record.provision.password,defaults_password,sizeof(defaults_password)-1)==0);

    Fake saved{}; Store second=open_store(saved);
    const std::string existing=command("53617665642d6e6574","73617665642d70617373776f7264");
    assert(store_usb_handle_line_with_wifi_defaults(&second,existing.c_str(),existing.size(),nullptr,
        defaults_ssid,sizeof(defaults_ssid)-1,defaults_password,sizeof(defaults_password)-1,
        reply,sizeof(reply))==STORE_OK);
    assert(second.record.provision.ssid_length==9 &&
        std::memcmp(second.record.provision.ssid,"Saved-net",9)==0);
    assert(second.record.provision.password_length==14 &&
        std::memcmp(second.record.provision.password,"saved-password",14)==0);
    const std::string keep_saved=command("","");
    assert(store_usb_handle_line_with_wifi_defaults(&second,keep_saved.c_str(),keep_saved.size(),nullptr,
        defaults_ssid,sizeof(defaults_ssid),defaults_password,sizeof(defaults_password),
        reply,sizeof(reply))==STORE_OK);
    assert(second.record.provision.ssid_length==9 &&
        std::memcmp(second.record.provision.ssid,"Saved-net",9)==0);
    assert(second.record.provision.password_length==14 &&
        std::memcmp(second.record.provision.password,"saved-password",14)==0);

    Fake blocked{}; Store third=open_store(blocked);
    assert(store_usb_handle_line(&third,missing.c_str(),missing.size(),nullptr,
        reply,sizeof(reply))==STORE_INVALID);
    assert(!third.record.provision.configured &&
        third.record.group_origin==STORE_GROUP_ORIGIN_FRESH_UNBOUND);
    return 0;
}
''')
            binary = private / "check"
            built = subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(ROOT / "firmware/components/katmed_store/include"),
                 "-I", str(ROOT / "firmware/components/katmed_core/include"),
                 str(ROOT / "firmware/components/katmed_store/katmed_store.cpp"),
                 str(ROOT / "firmware/components/katmed_core/katmed_core.cpp"),
                 str(program), "-lz", "-o", str(binary)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(binary)], check=False)
            self.assertEqual(ran.returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
