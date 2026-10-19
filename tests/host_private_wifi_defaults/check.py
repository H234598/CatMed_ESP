#!/usr/bin/env python3
import os
import json
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
    def test_omitting_external_profile_emits_neutral_empty_defaults(self):
        with tempfile.TemporaryDirectory() as temporary:
            private = pathlib.Path(temporary)
            os.chmod(private, 0o700)
            output_dir = private / "generated"
            output = output_dir / "private_wifi_defaults.h"
            ui_output = output_dir / "katmed_private_ui_profile.h"
            result = subprocess.run(
                ["python3", str(TOOL), "--output", str(output),
                 "--ui-profile-output", str(ui_output)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            generated = output.read_text()
            self.assertIn('#define KATMED_WIFI_DEFAULT_SSID ""', generated)
            self.assertIn('#define KATMED_WIFI_DEFAULT_PASSWORD ""', generated)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            neutral_ui = ui_output.read_text()
            self.assertNotIn("KATMED_DEFAULT_ALARM_TEXT", neutral_ui)
            self.assertNotIn("KATMED_DEFAULT_SELTSAM_MORSE_TEXT", neutral_ui)

    def test_private_config_generates_c_escaped_header_and_fails_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            private = pathlib.Path(temporary)
            os.chmod(private, 0o700)
            config = private / "wifi.json"
            output_dir = private / "generated"
            output = output_dir / "private_wifi_defaults.h"
            ui_output = output_dir / "katmed_private_ui_profile.h"
            config.write_text(json.dumps({
                "schema_version": 1,
                "provenance": "synthetic host test",
                "wifi": {"ssid": 'Synthetic "Net"', "password": "Synth\\Pass123"},
                "display": {
                    "medication_alarm_text": "Synthetic alarm",
                    "seltsam_morse_text": "Synthetic Seltsam!",
                },
                "legacy_personal_documentation_text": "Synthetic README alt text",
            }) + "\n")
            os.chmod(config, 0o600)

            good = subprocess.run(
                ["python3", str(TOOL), "--config", str(config), "--output", str(output),
                 "--ui-profile-output", str(ui_output)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(good.returncode, 0, good.stderr)
            generated = output.read_text()
            self.assertIn("KATMED_WIFI_DEFAULT_SSID", generated)
            self.assertIn("KATMED_WIFI_DEFAULT_PASSWORD", generated)
            self.assertNotIn("Synthetic", generated)
            private_ui = ui_output.read_text()
            self.assertIn("KATMED_DEFAULT_ALARM_TEXT", private_ui)
            self.assertIn("KATMED_DEFAULT_SELTSAM_MORSE_TEXT", private_ui)
            self.assertEqual(output.stat().st_mode & 0o777, 0o600)
            self.assertEqual(ui_output.stat().st_mode & 0o777, 0o600)
            self.assertEqual(output_dir.stat().st_mode & 0o777, 0o700)

            multiline = {
                "schema_version": 1,
                "provenance": "synthetic host test",
                "wifi": {"ssid": "Synthetic", "password": "SynthPass123"},
                "display": {
                    "medication_alarm_text": "Line one\r\nLine two",
                    "seltsam_morse_text": "A " * 300,
                },
                "legacy_personal_documentation_text": "Synthetic README alt text",
            }
            config.write_text(json.dumps(multiline) + "\n")
            allowed_text = subprocess.run(
                ["python3", str(TOOL), "--config", str(config), "--output", str(output),
                 "--ui-profile-output", str(ui_output)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(allowed_text.returncode, 0, allowed_text.stderr)
            self.assertIn(r"\015\012", ui_output.read_text())
            self.assertEqual(ui_output.stat().st_mode & 0o600, 0o600)

            for invalid_display in (
                {"medication_alarm_text": "bad\x00text", "seltsam_morse_text": "A"},
                {"medication_alarm_text": "ok", "seltsam_morse_text": "A" * 4097},
            ):
                multiline["display"] = invalid_display
                config.write_text(json.dumps(multiline) + "\n")
                rejected = subprocess.run(
                    ["python3", str(TOOL), "--config", str(config), "--output", str(output),
                     "--ui-profile-output", str(ui_output)],
                    text=True, capture_output=True, check=False,
                )
                self.assertNotEqual(rejected.returncode, 0)
                self.assertIn("private display", rejected.stderr)

            config.write_text(json.dumps({
                "schema_version": 1,
                "provenance": "synthetic host test",
                "wifi": {"ssid": 'Synthetic "Net"', "password": "Synth\\Pass123"},
                "display": {
                    "medication_alarm_text": "Synthetic alarm",
                    "seltsam_morse_text": "Synthetic Seltsam!",
                },
                "legacy_personal_documentation_text": "Synthetic README alt text",
            }) + "\n")
            restored = subprocess.run(
                ["python3", str(TOOL), "--config", str(config), "--output", str(output),
                 "--ui-profile-output", str(ui_output)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(restored.returncode, 0, restored.stderr)

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

            ui_check = private / "ui_profile_check.c"
            ui_check.write_text(r'''
#include <string.h>
#include "katmed_private_ui_profile.h"
#include "katmed_ui_defaults.h"
int main(void) {
    return strcmp(KATMED_DEFAULT_ALARM_TEXT, "Synthetic alarm") != 0 ||
           strcmp(KATMED_DEFAULT_SELTSAM_MORSE_TEXT, "Synthetic Seltsam!") != 0;
}
''')
            ui_binary = private / "ui_profile_check"
            ui_built = subprocess.run(
                ["cc", "-Wall", "-Wextra", "-Werror", "-I", str(output_dir),
                 "-I", str(ROOT / "firmware/components/katmed_ui/include"),
                 str(ui_check), "-o", str(ui_binary)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(ui_built.returncode, 0, ui_built.stderr)
            self.assertEqual(subprocess.run([str(ui_binary)], check=False).returncode, 0)

            missing = subprocess.run(
                ["python3", str(TOOL), "--config", str(private / "missing.json"),
                 "--output", str(output), "--ui-profile-output", str(ui_output)],
                text=True, capture_output=True, check=False,
            )
            self.assertNotEqual(missing.returncode, 0)
            self.assertNotIn("Synthetic", missing.stdout + missing.stderr)

    def test_wifi_profiles_are_bounded_atomic_and_survive_reopen(self):
        store_dir = ROOT / "firmware/components/katmed_store"
        core_dir = ROOT / "firmware/components/katmed_core"
        with tempfile.TemporaryDirectory() as temporary:
            private = pathlib.Path(temporary)
            program = private / "profiles.cpp"
            program.write_text(r'''
#include <cassert>
#include <cstring>
#include "katmed_store.h"
struct Fake {
    uint8_t bytes[KATMED_STORE_BLOB_MAX]{};
    size_t length=0;
    bool fail=false;
};
static StoreResult read_blob(void *ctx,uint8_t *out,size_t *length) {
    auto &f=*static_cast<Fake *>(ctx);
    if (!f.length) return STORE_NOT_FOUND;
    if (f.length>*length) return STORE_CORRUPT;
    std::memcpy(out,f.bytes,f.length); *length=f.length; return STORE_OK;
}
static StoreResult write_blob(void *ctx,const uint8_t *in,size_t length) {
    auto &f=*static_cast<Fake *>(ctx);
    if (f.fail) { f.fail=false; return STORE_IO; }
    if (length>sizeof(f.bytes)) return STORE_FULL;
    std::memcpy(f.bytes,in,length); f.length=length; return STORE_OK;
}
static Store open_store(Fake &f) {
    static uint8_t catalog[KATMED_CATALOG_BYTES]{};
    Store s{}; StoreRecovery recovery{};
    assert(store_open(&s,StoreBackend{&f,read_blob,write_blob},UINT64_C(0xAC276E46BC2C),
                      catalog,75,&recovery)==STORE_OK);
    return s;
}
int main() {
    assert(store_wifi_profile_limit()==4);
    Fake disk{}; Store s=open_store(disk);
    StoreWifiProfileUpdate input[KATMED_STORE_WIFI_PROFILE_MAX]{};
    for (unsigned i=0;i<KATMED_STORE_WIFI_PROFILE_MAX;++i) {
        input[i].priority=i+1;
        input[i].ssid_length=2;
        input[i].ssid[0]='n'; input[i].ssid[1]=static_cast<uint8_t>('0'+i);
        input[i].credential_action=STORE_WIFI_CREDENTIAL_REPLACE;
        input[i].password_length=9;
        std::memcpy(input[i].password,"pass1234x",9);
    }
    assert(store_wifi_profiles_save_atomic(&s,input,KATMED_STORE_WIFI_PROFILE_MAX)==STORE_OK);
    assert(store_wifi_profiles_save_atomic(&s,input,KATMED_STORE_WIFI_PROFILE_MAX+1)==STORE_INVALID);
    StoreWifiProfile loaded[8]{}; size_t count=0;
    assert(store_wifi_profiles_load(&s,loaded,KATMED_STORE_WIFI_PROFILE_MAX,&count)==STORE_OK && count==4);
    assert(loaded[0].priority==1 && loaded[3].priority==4 && loaded[0].secured);
    Fake fail_disk{}; Store fail_store=open_store(fail_disk);
    assert(store_wifi_profiles_save_atomic(&fail_store,input,KATMED_STORE_WIFI_PROFILE_MAX)==STORE_OK);
    assert(store_wifi_profiles_load(&fail_store,loaded,KATMED_STORE_WIFI_PROFILE_MAX,&count)==STORE_OK && count==4);
    input[0].id=loaded[0].id; input[0].credential_action=STORE_WIFI_CREDENTIAL_KEEP;
    fail_disk.fail=true;
    assert(store_wifi_profiles_save_atomic(&fail_store,input,KATMED_STORE_WIFI_PROFILE_MAX)==STORE_IO);
    assert(fail_store.record.wifi_profile_count==4);
    Store reopened=open_store(fail_disk);
    assert(store_wifi_profiles_load(&reopened,loaded,KATMED_STORE_WIFI_PROFILE_MAX,&count)==STORE_OK && count==4);
    assert(store_wifi_profile_delete_atomic(&reopened,loaded[3].id)==STORE_OK);
    assert(store_wifi_profiles_load(&reopened,loaded,KATMED_STORE_WIFI_PROFILE_MAX,&count)==STORE_OK && count==3);
    for (size_t i=0;i<count;++i) assert(loaded[i].priority==i+1);
    return 0;
}
''')
            binary = private / "profiles"
            built = subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(store_dir / "include"), "-I", str(core_dir / "include"),
                 str(store_dir / "katmed_store.cpp"), str(core_dir / "katmed_core.cpp"),
                 str(program), "-lz", "-o", str(binary)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(built.returncode, 0, built.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 0)

    def test_wifi_reselection_advances_only_after_loss_and_explicitly_resets_priority(self):
        header = ROOT / "firmware/components/katmed_mesh/include"
        with tempfile.TemporaryDirectory() as temporary:
            private = pathlib.Path(temporary)
            program = private / "wifi_policy.cpp"
            program.write_text(r'''
#include <cassert>
#include "katmed_wifi_profile_policy.h"
int main() {
    uint8_t current = 1; // Unreachable AP produces STA disconnect.
    current = katmed_wifi_profile_reselect(true, false, current, 3);
    assert(current == 2);
    // Authentication failure advances to the next ordered candidate.
    current = katmed_wifi_profile_reselect(true, false, current, 3);
    assert(current == 3);
    // A stable associated link does not trigger profile switching.
    assert(katmed_wifi_profile_reselect(false, false, current, 3) == 0);
    // Explicit reconnect starts a fresh priority evaluation at 1.
    current = katmed_wifi_profile_reselect(false, true, current, 3);
    assert(current == 1);
    // No configured profiles leaves the current/fallback path inactive.
    assert(katmed_wifi_profile_reselect(true, false, 1, 0) == 0);
    return 0;
}
''')
            binary = private / "wifi_policy"
            built = subprocess.run(
                ["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-I", str(header), str(program), "-o", str(binary)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(built.returncode, 0, built.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 0)

    def test_factory_reset_journal_recovers_each_failed_stage_and_tokens_are_one_use(self):
        reset_dir = ROOT / "firmware/components/katmed_reset"
        with tempfile.TemporaryDirectory() as temporary:
            private = pathlib.Path(temporary)
            program = private / "reset_journal.c"
            program.write_text(r'''
#include <assert.h>
#include <string.h>
#include "katmed_reset_journal.h"
typedef struct {
    KatmedResetJournal journal;
    int fail_save;
    int fail_stage;
    unsigned runs[5];
    unsigned mutations;
} Fake;
static bool load(void *ctx,KatmedResetJournal *out) {
    *out=((Fake *)ctx)->journal; return true;
}
static bool save(void *ctx,const KatmedResetJournal *in) {
    Fake *f=ctx;
    if(f->fail_save){f->fail_save=0;return false;}
    f->journal=*in;return true;
}
static bool run(void *ctx,KatmedResetStage stage) {
    Fake *f=ctx; ++f->runs[stage];
    if((int)stage==f->fail_stage){f->fail_stage=0;return false;}
    ++f->mutations;return true;
}
static bool random_bytes(void *ctx,uint8_t *out,size_t length) {
    uint8_t *seed=ctx; for(size_t i=0;i<length;++i)out[i]=++*seed; return true;
}
int main(void) {
    KatmedResetJournalBackend ops; Fake f={0}; ops.context=&f;ops.load=load;ops.save=save;ops.run_stage=run;
    f.fail_save=1;
    assert(!katmed_reset_begin(ops)); assert(!f.journal.pending && f.mutations==0);
    assert(katmed_reset_begin(ops) && f.journal.pending && f.journal.stage==KATMED_RESET_FILES);
    bool pending=false;
    for(int stage=KATMED_RESET_FILES;stage<KATMED_RESET_DONE;++stage){
        f.fail_stage=stage;
        assert(!katmed_reset_resume(ops,&pending));
        assert(pending && f.journal.pending && f.journal.stage==stage);
        assert(katmed_reset_resume(ops,&pending));
        assert(!pending && !f.journal.pending);
        if(stage!=KATMED_RESET_DONE) {
            if(stage<KATMED_RESET_ACME_NVS) {
                assert(katmed_reset_begin(ops));
                /* Move the durable marker directly to each later failure point. */
                f.journal.stage=(uint8_t)stage;
            }
        }
    }
    assert(katmed_reset_begin(ops));
    f.fail_save=1; assert(!katmed_reset_resume(ops,&pending));
    assert(f.journal.pending); /* Progress commit failure is recoverable. */
    assert(katmed_reset_resume(ops,&pending) && !pending);

    KatmedResetConfirmation confirmation={0}; char token[33]; size_t length=0;
    uint8_t seed=0;
    assert(katmed_reset_confirmation_prepare(&confirmation,random_bytes,&seed,100,1000,
                                              token,sizeof(token),&length));
    assert(length==32);
    assert(!katmed_reset_confirmation_consume(&confirmation,token,length,1101));
    assert(confirmation.valid); /* Expired token never triggers reset action. */
    assert(katmed_reset_confirmation_prepare(&confirmation,random_bytes,&seed,200,1000,
                                              token,sizeof(token),&length));
    assert(!katmed_reset_confirmation_consume(&confirmation,"bad",3,201));
    assert(katmed_reset_confirmation_consume(&confirmation,token,length,201));
    assert(!katmed_reset_confirmation_consume(&confirmation,token,length,202));
    return 0;
}
''')
            binary = private / "reset_journal"
            built = subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I", str(reset_dir / "include"),
                 str(reset_dir / "katmed_reset_journal.c"), str(program), "-o", str(binary)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(built.returncode, 0, built.stderr)
            self.assertEqual(subprocess.run([str(binary)], check=False).returncode, 0)

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
