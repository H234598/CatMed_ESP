#!/usr/bin/env python3
"""Exercise the HTTP import harness against freshly generated synthetic certificates."""
import datetime
import os
import pathlib
import stat
import subprocess  # nosec B404 - Executes the HTTP import fixture compiled in private scratch.
import sys
import tempfile
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

folder = pathlib.Path.cwd()
scratch = pathlib.Path(tempfile.gettempdir()).resolve(strict=True)
if len(sys.argv) != 1 or folder.parent != scratch or not folder.name.startswith("katmed-http-import-"):
    raise SystemExit("check_validation.py requires the private workspace from check_http_import.py")
owner = folder.stat()
if owner.st_uid != os.geteuid() or stat.S_IMODE(owner.st_mode) != 0o700:
    raise RuntimeError("HTTP import workspace must be private and owned by the test runner")
program = folder / "http-test"
info = program.lstat()
if (not stat.S_ISREG(info.st_mode) or info.st_uid != owner.st_uid
        or info.st_mode & 0o022 or not os.access(program, os.X_OK)):
    raise RuntimeError("HTTP import fixture must be a runner-owned executable in the workspace")
now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)

def issue(name, key, issuer, issuer_key, ca, path_length=None):
    subject = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, name)])
    builder = (x509.CertificateBuilder().subject_name(subject)
               .issuer_name(issuer.subject if issuer else subject)
               .public_key(key.public_key()).serial_number(x509.random_serial_number())
               .not_valid_before(now - datetime.timedelta(days=2))
               .not_valid_after(now + datetime.timedelta(days=2))
               .add_extension(x509.BasicConstraints(ca=ca, path_length=path_length), True)
               .add_extension(x509.KeyUsage(digital_signature=True, content_commitment=False,
                   key_encipherment=False, data_encipherment=False, key_agreement=False,
                   key_cert_sign=ca, crl_sign=ca, encipher_only=False, decipher_only=False), True))
    if not ca:
        builder = (builder.add_extension(x509.SubjectAlternativeName([x509.DNSName(name)]), False)
                   .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), False))
    return builder.sign(issuer_key or key, hashes.SHA256())

root_key = ec.generate_private_key(ec.SECP256R1())
root = issue("synthetic root", root_key, None, None, True, 1)
inter_key = ec.generate_private_key(ec.SECP256R1())
inter = issue("synthetic intermediate", inter_key, root, root_key, True, 0)
leaf_key = ec.generate_private_key(ec.SECP256R1())
leaf = issue("device.example", leaf_key, inter, inter_key, False)
second_key = ec.generate_private_key(ec.SECP256R1())
second_leaf = issue("device.example", second_key, inter, inter_key, False)
other_key = ec.generate_private_key(ec.SECP256R1())
other = issue("synthetic unrelated root", other_key, None, None, True, 1)
with tempfile.TemporaryDirectory(dir=folder) as directory:
    certificates = pathlib.Path(directory)
    entries = [leaf_key.private_numbers().private_value.to_bytes(32, "big"),
               leaf.public_bytes(serialization.Encoding.PEM) + inter.public_bytes(serialization.Encoding.PEM),
               root.public_bytes(serialization.Encoding.PEM), other.public_bytes(serialization.Encoding.PEM)]
    if os.environ.get("KATMED_SECOND_IDENTITY") == "1":
        entries.extend([second_key.private_numbers().private_value.to_bytes(32, "big"),
                        second_leaf.public_bytes(serialization.Encoding.PEM) + inter.public_bytes(serialization.Encoding.PEM)])
    paths = []
    for index, entry in enumerate(entries):
        path = certificates / str(index)
        path.write_bytes(entry)
        path.chmod(0o600)
        paths.append(str(path))
    command = [str(program), *paths[:4], str(int(now.timestamp())), *paths[4:]]
    subprocess.run(command, check=True, cwd=folder,  # nosec B603 - Private harness and synthetic certificate argv.
                   env={"PATH": "/usr/bin:/bin", "TMPDIR": directory})
