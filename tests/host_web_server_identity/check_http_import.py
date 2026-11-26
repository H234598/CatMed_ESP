#!/usr/bin/env python3
"""Compile production HTTP and poll functions against real crypto and modeled I/O."""
import importlib.util
import os
import pathlib
import stat
import subprocess  # nosec B404 - Compiles and runs the synthetic HTTP import fixture.
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
startup_spec = importlib.util.spec_from_file_location(
    "test_network_startup", root / "tests/host_web_ui/test_network_startup.py")
if startup_spec is None or startup_spec.loader is None:
    raise RuntimeError("Cannot load the repository's Web startup fixture")
startup = importlib.util.module_from_spec(startup_spec)
startup_spec.loader.exec_module(startup)

compiler = pathlib.Path("/usr/bin/cc").resolve(strict=True)
python = pathlib.Path(sys.executable)
for tool in (compiler, python):
    if not tool.is_absolute():
        raise RuntimeError(f"Host tool path must be absolute: {tool}")
    info = tool.stat()
    if (not stat.S_ISREG(info.st_mode) or info.st_uid != 0
            or info.st_mode & 0o022 or not os.access(tool, os.X_OK)):
        raise RuntimeError(f"Host tool must be a root-owned executable: {tool}")

idf = pathlib.Path(os.environ["IDF_PATH"]).resolve(strict=True)
build = pathlib.Path(os.environ["KATMED_MBEDTLS_HOST_BUILD"]).resolve(strict=True)
cjson = pathlib.Path(os.environ["KATMED_CJSON_DIR"]).resolve(strict=True)
web = root / "firmware/components/katmed_web"
test = root / "tests/host_web_server_identity"
mbed = idf / "components/mbedtls/mbedtls"
source = (web / "katmed_web.c").read_text()
signatures = ["bool katmed_web_acme_identifier(", "static bool request_host(",
              "static bool same_origin(", "static bool get_cookie(",
              "static KatmedWebSession *session_from_request(",
              "static KatmedWebSession *require_session(", "static void wipe_json(",
              "static cJSON *read_json(", "static bool keys_only(",
              "static const char *json_string(", "static esp_err_t server_anchor_post(",
              "static int hex_digit(", "static esp_err_t server_identity_get(",
              "static esp_err_t server_identity_post(", "void katmed_web_poll("]
scratch = pathlib.Path(tempfile.gettempdir()).resolve(strict=True)
with tempfile.TemporaryDirectory(prefix="katmed-http-import-", dir=scratch) as tmp:
    folder = pathlib.Path(tmp)
    program = folder / "http.c"
    program.write_text((test / "check_http_prefix.c").read_text() + "\n" +
        "\n".join(startup.function(source, sig) for sig in signatures) + "\n" +
        (test / "check_http_suffix.c").read_text())
    exe = folder / "http-test"
    include = [test, root / "tests/host_security_import/stubs", root / "tests/host_security/stubs",
               root / "firmware/components/katmed_security/include", web, web / "include", cjson,
               mbed / "include", mbed / "tf-psa-crypto/include", mbed / "tf-psa-crypto/drivers/builtin/include",
               build / "tf-psa-crypto/include"]
    libraries = [build / "library/libmbedtls.a", build / "library/libmbedx509.a", build / "library/libtfpsacrypto.a"]
    command = [str(compiler), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
               "-DKATMED_WEB_ANCHOR_RACE_TEST", "-pthread",
               *["-I" + str(p) for p in include], str(program), str(web / "katmed_web_policy.c"),
               str(web / "katmed_web_auth.c"), str(web / "katmed_web_server_validation.c"),
               str(web / "katmed_web_server_journal.c"), str(cjson / "cJSON.c"),
               "-Wl,--start-group", *map(str, libraries), "-Wl,--end-group", "-lm", "-o", str(exe)]
    env = {"PATH": "/usr/bin:/bin", "TMPDIR": str(folder)}
    subprocess.run(command, check=True, cwd=folder, env=env)  # nosec B603 - System compiler, absolute fixture argv.
    # Keep the venv entry point so the child uses the same cryptography dependency.
    validation = [str(python), "-I", "-B", str(test / "check_validation.py")]
    subprocess.run(validation, check=True, cwd=folder,  # nosec B603 - Pinned Python and repository fixture.
                   env={**env, "TMPDIR": str(scratch), "KATMED_SECOND_IDENTITY": "1"})
