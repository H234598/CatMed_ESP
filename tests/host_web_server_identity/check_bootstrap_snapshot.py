#!/usr/bin/env python3
"""Run the firmware snapshot and listener lifecycle against synthetic DER."""
import datetime
import hashlib
import importlib
import os
from pathlib import Path
import subprocess  # nosec B404 - Runs the system compiler and the private fixture executable.
import sys
import tempfile

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/host_web_ui"))
startup = importlib.import_module("test_network_startup")

CRYPTO = r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "mbedtls/md.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
bool init_snapshot_crypto(void) { return psa_crypto_init() == PSA_SUCCESS; }
'''

LOCKS = r'''
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define portEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)
'''

ORIGIN_NVS = r'''
static char stored_authority[KATMED_WEB_IDENTIFIER_MAX];
static char pending_authority[KATMED_WEB_IDENTIFIER_MAX];
static bool has_authority, fail_set_authority, fail_commit_authority, fail_load_authority;
static bool observe_pending_authority, stop_during_authority_commit;
static unsigned authority_commits;
static void unavailable(void);

static int nvs_get_str(int nvs, const char *key, char *out, size_t *size)
{
    (void)nvs;
    assert(!strcmp(key, "acme_id"));
    if (fail_load_authority) return ESP_FAIL;
    if (!has_authority) return ESP_ERR_NVS_NOT_FOUND;
    assert(*size >= strlen(stored_authority) + 1);
    *size = strlen(stored_authority) + 1;
    memcpy(out, stored_authority, *size);
    return ESP_OK;
}

static int nvs_set_str(int nvs, const char *key, const char *value)
{
    (void)nvs;
    assert(!strcmp(key, "acme_id"));
    if (observe_pending_authority) unavailable();
    if (fail_set_authority) return ESP_FAIL;
    strcpy(pending_authority, value);
    return ESP_OK;
}

static int nvs_commit(int nvs)
{
    (void)nvs;
    ++authority_commits;
    strcpy(stored_authority, pending_authority);
    has_authority = true;
    if (observe_pending_authority) unavailable();
    if (stop_during_authority_commit) invalidate_bootstrap_snapshot();
    /* Failed commit is genuinely ambiguous: data may already be durable. */
    return fail_commit_authority ? ESP_FAIL : ESP_OK;
}

static bool katmed_source_writer_enter(void) { return true; }
static void katmed_source_writer_leave(void) {}
'''

DRIVER = r'''
#include <assert.h>
#include <pthread.h>
#include <sched.h>
bool init_snapshot_crypto(void);

static void unavailable(void)
{
    KatmedWebBootstrapSnapshot result;
    memset(&result, 0xab, sizeof(result));
    assert(!katmed_web_bootstrap_snapshot(&result));
    const unsigned char *bytes = (const unsigned char *)&result;
    for (size_t i = 0; i < sizeof(result); i++) assert(bytes[i] == 0);
}

static void load_leaf(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    web.identity.cert_len = fread(web.identity.cert, 1, sizeof(web.identity.cert), file);
    assert(feof(file) && !ferror(file) && fclose(file) == 0);
}

static KatmedWebBootstrapSnapshot expect(const char *dns, const char *sha)
{
    KatmedWebBootstrapSnapshot result;
    assert(https_servercert == web.identity.cert);
    assert(katmed_web_bootstrap_snapshot(&result) && result.generation);
    assert(!strcmp(result.dns_san, dns));
    char hex[65];
    for (size_t i = 0; i < sizeof(result.sha256); i++)
        snprintf(hex + 2*i, 3, "%02x", (unsigned)result.sha256[i]);
    assert(!strcmp(hex, sha));
    return result;
}

static bool write_authority(const char *authority)
{
    xSemaphoreTake(web.auth_mutex, portMAX_DELAY);
    const bool saved = save_identifier(&web, authority);
    xSemaphoreGive(web.auth_mutex);
    return saved;
}

static KatmedWebBootstrapSnapshot expect_authority(const char *authority)
{
    KatmedWebBootstrapSnapshot view;
    assert(katmed_web_bootstrap_snapshot(&view));
    assert(!strcmp(view.authority, authority) && view.origin_revision != 0);
    assert(katmed_web_bootstrap_snapshot_matches(view.generation, view.origin_revision));
    return view;
}

static void check_authority(const char *leaf)
{
    const KatmedWebBootstrapSnapshot empty = expect_authority("");
    observe_pending_authority = true;
    assert(write_authority("management.example"));
    const KatmedWebBootstrapSnapshot a = expect_authority("management.example");
    assert(a.generation == empty.generation && a.origin_revision > empty.origin_revision);
    assert(!memcmp(a.sha256, empty.sha256, sizeof(a.sha256)) &&
           !strcmp(a.dns_san, empty.dns_san));

    observe_pending_authority = false;
    const unsigned before_noop = authority_commits;
    assert(write_authority("management.example") && authority_commits == before_noop + 1);
    assert(expect_authority("management.example").origin_revision == a.origin_revision);
    assert(!write_authority("invalid!"));
    assert(expect_authority("management.example").origin_revision == a.origin_revision);

    observe_pending_authority = true;
    assert(write_authority("temporary.example"));
    const KatmedWebBootstrapSnapshot b = expect_authority("temporary.example");
    assert(b.origin_revision > a.origin_revision && b.generation == a.generation);
    assert(write_authority("management.example"));
    const KatmedWebBootstrapSnapshot again = expect_authority("management.example");
    assert(again.origin_revision > b.origin_revision && again.generation == a.generation);
    assert(!katmed_web_bootstrap_snapshot_matches(a.generation, a.origin_revision));

    fail_set_authority = true;
    assert(!write_authority("fail-set.example"));
    unavailable();
    fail_set_authority = false;
    assert(katmed_web_stop());
    load_leaf(leaf);
    assert(katmed_web_start(NULL) == ESP_OK);
    const KatmedWebBootstrapSnapshot reloaded = expect_authority("management.example");
    assert(reloaded.generation > again.generation && reloaded.origin_revision > again.origin_revision);

    fail_commit_authority = true;
    assert(!write_authority("unknown.example"));
    unavailable();
    fail_commit_authority = false;
    assert(katmed_web_stop());
    fail_load_authority = true;
    load_leaf(leaf);
    assert(katmed_web_start(NULL) != ESP_OK);
    unavailable();
    fail_load_authority = false;
    load_leaf(leaf);
    assert(katmed_web_start(NULL) == ESP_OK);
    assert(expect_authority("unknown.example").origin_revision > reloaded.origin_revision);

    stop_during_authority_commit = true;
    assert(write_authority("management.example"));
    unavailable(); /* Completion must not restore a stopped generation. */
    stop_during_authority_commit = observe_pending_authority = false;
    assert(katmed_web_stop());
    load_leaf(leaf);
    assert(katmed_web_start(NULL) == ESP_OK);
    assert(expect_authority("management.example").generation > reloaded.generation);
}

static bool stage_during_start;
static void before_ready(void)
{
    unavailable();
    if (stage_during_start) stage_identity();
}

static bool deny_stop(void *context) { (void)context; return false; }

static KatmedWebBootstrapSnapshot first, second;
static atomic_bool reader_ready, reader_done;
static void *observe(void *unused)
{
    (void)unused;
    uint64_t last = 0;
    do {
        KatmedWebBootstrapSnapshot result;
        if (katmed_web_bootstrap_snapshot(&result)) {
            assert(result.generation >= last);
            last = result.generation;
            const KatmedWebBootstrapSnapshot *expected =
                !strcmp(result.dns_san, first.dns_san) ? &first : &second;
            assert(!strcmp(result.dns_san, expected->dns_san));
            assert(!memcmp(result.sha256, expected->sha256, sizeof(result.sha256)));
            atomic_store(&reader_ready, true);
        }
    } while (!atomic_load(&reader_done));
    return NULL;
}

int main(int argc, char **argv)
{
    assert(argc == 9 && init_snapshot_crypto() && !katmed_web_bootstrap_snapshot(NULL));
    unavailable();
    load_leaf(argv[1]);
    assert(katmed_web_start(NULL) == ESP_OK);
    first = expect("Observed-A.example", argv[3]);
    check_authority(argv[1]);

    stage_identity();
    unavailable();
    https_fail_once = true;
    katmed_web_poll(501);
    assert(expect("Observed-A.example", argv[3]).generation > first.generation);

    web.services.prepare_listener_stop = deny_stop;
    assert(!katmed_web_stop() && web.https);
    unavailable();
    katmed_web_set_enabled(false);
    assert(web.https);
    unavailable();
    web.services.prepare_listener_stop = NULL;
    https_stop_fails = true;
    assert(!katmed_web_stop() && web.https && !web.redirect);
    unavailable();
    https_stop_fails = false;
    assert(katmed_web_stop());
    katmed_web_set_enabled(true);

    load_leaf(argv[2]);
    stage_during_start = true;
    assert(katmed_web_start(NULL) == ESP_OK && web.server_pending);
    stage_during_start = false;
    unavailable();
    handshake_during_start = true;
    katmed_web_poll(501);
    assert(web.started && web.server_identity && !web.server_pending);
    unavailable();
    assert(katmed_web_stop());

    load_leaf(argv[1]);
    assert(katmed_web_start(NULL) == ESP_OK);
    expect("Observed-A.example", argv[3]);
    stage_identity();
    handshake_during_start = false;
    katmed_web_poll(501);
    assert(web.started && web.server_pending && atomic_load(&server_probe_active));
    unavailable();
    atomic_store(&server_probe_seen, true);
    mock_commit_failure = true;
    katmed_web_poll(502);
    assert(web.server_quarantined && !web.started);
    unavailable();
    mock_commit_failure = false;
    assert(katmed_web_stop());

    load_leaf(argv[1]);
    redirect_registration_fails = true;
    assert(katmed_web_start(NULL) != ESP_OK);
    unavailable();
    redirect_registration_fails = false;

    for (int i = 5; i < argc; i++) {
        load_leaf(argv[i]);
        assert(katmed_web_start(NULL) == ESP_OK);
        unavailable();
        assert(katmed_web_stop());
    }
    load_leaf(argv[2]);
    assert(katmed_web_start(NULL) == ESP_OK);
    second = expect("Observed-B.example", argv[4]);
    assert(second.generation > first.generation);
    pthread_t observer;
    assert(pthread_create(&observer, NULL, observe, NULL) == 0);
    while (!atomic_load(&reader_ready)) sched_yield();
    for (int i = 0; i < 64; i++) {
        assert(katmed_web_stop());
        unavailable();
        load_leaf(argv[1+i%2]);
        assert(katmed_web_start(NULL) == ESP_OK);
        expect(i%2 ? second.dns_san : first.dns_san, argv[3+i%2]);
    }
    atomic_store(&reader_done, true);
    assert(pthread_join(observer, NULL) == 0);
    assert(katmed_web_stop());
    unavailable();
    puts("Bootstrap listener snapshot DER, Origin revision, lifecycle and concurrent reads: PASS");
    return 0;
}
'''


def certificate(*names):
    key = ec.generate_private_key(ec.SECP256R1())
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "not-the-san.example")])
    now = datetime.datetime.now(datetime.timezone.utc)
    builder = (x509.CertificateBuilder().subject_name(subject).issuer_name(subject)
               .public_key(key.public_key()).serial_number(x509.random_serial_number())
               .not_valid_before(now - datetime.timedelta(days=1))
               .not_valid_after(now + datetime.timedelta(days=1)))
    if names:
        builder = builder.add_extension(
            x509.SubjectAlternativeName([x509.DNSName(name) for name in names]), False)
    return builder.sign(key, hashes.SHA256()).public_bytes(serialization.Encoding.DER)


def compile_fixture(folder):
    web = ROOT / "firmware/components/katmed_web"
    source = (web / "katmed_web.c").read_text()
    header = (web / "include/katmed_web.h").read_text()
    snapshot_type = ('#include "katmed_web_policy.h"\n' +
                     header[header.index("typedef struct {"):
                            header.index("} KatmedWebBootstrapSnapshot;") +
                            len("} KatmedWebBootstrapSnapshot;")])
    lock = source[source.index("static portMUX_TYPE bootstrap_snapshot_lock"):
                  source.index("static void invalidate_bootstrap_snapshot")]
    helper_names = ("void invalidate_bootstrap_snapshot", "bool bootstrap_snapshot_available",
                    "bool katmed_web_bootstrap_snapshot", "bool katmed_web_bootstrap_snapshot_matches",
                    "void publish_bootstrap_snapshot", "bool load_identifier", "bool save_identifier")
    helpers = [startup.function(source, name + "(") for name in helper_names]
    limits = "\n".join(row for row in source.splitlines()
                       if row.startswith(("#define PRE_", "#define POST_")))
    mocked = startup.PREFIX.replace("static bool tcpip_ready", "static void before_ready(void);\n"
                                    "static bool tcpip_ready", 1).replace(
        "++redirect_registrations;", "++redirect_registrations; before_ready();", 1).replace(
        "static struct {\n KatmedWebServices services;",
        "typedef struct {\n bool identifier_ready;\n KatmedWebServices services;", 1).replace(
        "} web;", "} WebContext;\nstatic WebContext web;", 1).replace(
        "static bool load_identifier(void *state){(void)state;return true;}", "", 1)
    lifecycle = ("static bool resources_allow", "static bool stop_listeners",
                 "static esp_err_t start_listeners", "void katmed_web_set_enabled",
                 "esp_err_t katmed_web_start", "void katmed_web_poll", "bool katmed_web_stop")
    stage = ("static void stage_identity(void) {\n"
             "    const bool staged = true; unsigned slot = 0;\n"
             "    KatmedServerRecord *candidate = calloc(1, sizeof(*candidate));\n"
             "    if (!candidate) abort();\n" + startup.function(source, "if (staged) {") + "\n}")
    driver = folder / "lifecycle.c"
    driver.write_text("\n".join([mocked, "#define ESP_PLATFORM 1", snapshot_type, LOCKS,
                                 "#include <assert.h>", lock, helpers[0], ORIGIN_NVS, *helpers[1:],
                                 "bool describe_bootstrap_leaf(const uint8_t *, size_t, KatmedWebBootstrapSnapshot *);",
                                 limits,
                                 "static bool management_enabled=true;", stage,
                                 *(startup.function(source, name + "(") for name in lifecycle), DRIVER]))
    crypto = folder / "snapshot.c"
    crypto.write_text("\n".join([CRYPTO, snapshot_type, '#include "katmed_web_identity.h"',
                                 startup.function(source, "static int sha256_bytes("),
                                 startup.function(source, "bool describe_bootstrap_leaf(")]))
    build = Path(os.environ["KATMED_MBEDTLS_HOST_BUILD"]).resolve(strict=True)
    mbed = (Path(os.environ["IDF_PATH"]) / "components/mbedtls/mbedtls").resolve(strict=True)
    includes = [web, web / "include", ROOT / "tests/host_security_import/stubs",
                ROOT / "tests/host_security/stubs",
                mbed / "include", mbed / "tf-psa-crypto/include",
                mbed / "tf-psa-crypto/drivers/builtin/include", build / "tf-psa-crypto/include"]
    libraries = [build / "library" / ("lib" + name + ".a") for name in ("mbedtls", "mbedx509", "tfpsacrypto")]
    binary = folder / "test-snapshot"
    compiler = Path("/usr/bin/cc").resolve(strict=True)
    if compiler.stat().st_uid != 0:
        raise RuntimeError("The host fixture requires a root-owned system compiler")
    command = [str(compiler), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
               "-Wno-unused-function", "-pthread", *["-I" + str(p) for p in includes],
               str(driver), str(crypto), str(web / "katmed_web_policy.c"),
               "-Wl,--start-group", *map(str, libraries), "-Wl,--end-group", "-o", str(binary)]
    subprocess.run(command, check=True)  # nosec B603 - Root-owned compiler, absolute paths, no shell.
    return binary


def main():
    first, second = (certificate("Observed-A.example"), certificate("Observed-B.example"))
    leaves = [first, second, b"broken DER", first + b"\x00", certificate(),
              certificate("first.example", "second.example")]
    with tempfile.TemporaryDirectory() as directory:
        folder = Path(directory)
        paths = [folder / ("leaf-" + str(i) + ".der") for i in range(len(leaves))]
        for path, der in zip(paths, leaves):
            path.write_bytes(der)
        hashes_hex = [hashlib.sha256(leaf).hexdigest() for leaf in (first, second)]
        command = [str(compile_fixture(folder)), *map(str, paths[:2]),
                   *hashes_hex, *map(str, paths[2:])]
        # The executable and DER inputs come from this private temporary directory.
        subprocess.run(command, check=True, timeout=30)  # nosec B603 - Fixture argv, no shell.


if __name__ == "__main__":
    main()
