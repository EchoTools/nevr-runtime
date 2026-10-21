#!/usr/bin/env python3
"""Generate the signed and unsigned DLLs the plugin-signature GTests read (ADR 0010, #741).

    gen_plugin_signature_fixtures.py <out-dir> [--now <unix seconds>]

Everything is made offline from a throwaway PKI: a pinned test root, a second root that is NOT pinned, leaf
certificates with chosen validity windows and EKUs, and timestamps from osslsigncode's built-in TSA at chosen
times. Writes <out-dir>/fixtures.json, the manifest the tests read: the pinned root and the facts about each
file (what a correct verifier must say). Needs openssl, osslsigncode and x86_64-w64-mingw32-gcc on PATH.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

DAY = 86400
CODE_SIGNING = "1.3.6.1.5.5.7.3.3"
TIME_STAMPING = "1.3.6.1.5.5.7.3.8"
PROFILE_EKU = "1.3.6.1.4.1.311.97.1.1.1"      # test stand-in for the profile EKU
OTHER_EKU = "1.3.6.1.4.1.311.97.9.9.9"
RFC3161_COUNTERSIGN = bytes.fromhex("2b0601040182370303 01".replace(" ", ""))  # 1.3.6.1.4.1.311.3.3.1


def run(args, **kw):
    result = subprocess.run(args, capture_output=True, text=True, **kw)
    if result.returncode != 0:
        sys.exit(f"gen_plugin_signature_fixtures: FAIL {' '.join(map(str, args))}\n{result.stdout}{result.stderr}")
    return result.stdout


def stamp(unix):
    return time.strftime("%Y%m%d%H%M%SZ", time.gmtime(unix))


class Pki:
    def __init__(self, work):
        self.work = work

    def path(self, name):
        return str(self.work / name)

    def root(self, name, cn, not_before, not_after):
        """A self-signed CA valid over [not_before, not_after]: fixtures are dated back, so the root must be too."""
        key, csr, pem, ext = (self.path(f"{name}.{x}") for x in ("key", "csr", "pem", "ext"))
        run(["openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", csr, "-subj", f"/CN={cn}"])
        Path(ext).write_text("basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n")
        run(["openssl", "x509", "-req", "-in", csr, "-signkey", key, "-out", pem, "-not_before", stamp(not_before),
             "-not_after", stamp(not_after), "-extfile", ext, "-set_serial", "1"])
        der = self.path(f"{name}.der")
        run(["openssl", "x509", "-in", pem, "-outform", "DER", "-out", der])
        return {"key": key, "pem": pem, "der": der}

    def leaf(self, name, cn, issuer, ekus, not_before, not_after, serial):
        key, csr, pem, ext = (self.path(f"{name}.{x}") for x in ("key", "csr", "pem", "ext"))
        run(["openssl", "req", "-newkey", "rsa:2048", "-nodes", "-keyout", key, "-out", csr, "-subj", f"/CN={cn}"])
        Path(ext).write_text("keyUsage=critical,digitalSignature\nextendedKeyUsage=critical," + ",".join(ekus) + "\n")
        run(["openssl", "x509", "-req", "-in", csr, "-CA", issuer["pem"], "-CAkey", issuer["key"],
             "-set_serial", str(serial), "-out", pem, "-not_before", stamp(not_before),
             "-not_after", stamp(not_after), "-extfile", ext])
        return {"key": key, "pem": pem, "serial_hex": format(serial, "x"), "cn": cn}


def build_dll(work, name, marker):
    src = work / f"{name}.c"
    # A distinct body per file so two DLLs never share a digest; DllMain only, no imports beyond the CRT start-up.
    src.write_text(f'const char kMarker[] = "{marker}";\nint __stdcall DllMain(void*a,unsigned long b,void*c)'
                   f'{{(void)a;(void)b;(void)c;return kMarker[0]!=0;}}\n')
    out = work / f"{name}.dll"
    run(["x86_64-w64-mingw32-gcc", "-shared", "-O1", "-nostdlib", "-Wl,--entry=DllMain", "-o", str(out), str(src)])
    return out


def sign(unsigned, out, leaf, chain_pems, tsa=None, tsa_time=None):
    """osslsigncode sign; chain_pems are concatenated behind the leaf into the -certs file."""
    certs = Path(str(out) + ".chain.pem")
    certs.write_text("".join(Path(p).read_text() for p in [leaf["pem"], *chain_pems]))
    cmd = ["osslsigncode", "sign", "-certs", str(certs), "-key", leaf["key"], "-h", "sha256",
           "-in", str(unsigned), "-out", str(out)]
    if tsa is not None:
        cmd += ["-TSA-certs", tsa["pem_chain"], "-TSA-key", tsa["key"], "-TSA-time", str(tsa_time)]
    run(cmd)


# ---- a minimal DER tree, enough to move a timestamp token between two signatures ------------------------------

def der_parse(data, off=0):
    tag = data[off]
    length = data[off + 1]
    hdr = 2
    if length & 0x80:
        n = length & 0x7F
        length = int.from_bytes(data[off + 2:off + 2 + n], "big")
        hdr = 2 + n
    body = data[off + hdr:off + hdr + length]
    constructed = bool(tag & 0x20)
    node = {"tag": tag, "raw": body, "kids": None}
    if constructed:
        kids, pos = [], 0
        while pos < len(body):
            kid, used = der_parse(body, pos)
            kids.append(kid)
            pos += used
        node["kids"] = kids
    return node, hdr + length


def der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def der_dump(node):
    body = b"".join(der_dump(k) for k in node["kids"]) if node["kids"] is not None else node["raw"]
    return bytes([node["tag"]]) + der_len(len(body)) + body


def pe_cert_table(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    magic = struct.unpack_from("<H", data, pe + 24)[0]
    dir_off = pe + 24 + (112 if magic == 0x20B else 96) + 4 * 8
    off, size = struct.unpack_from("<II", data, dir_off)
    return dir_off, off, size


def find_signer_info(signed_data):
    """SignedData = SEQUENCE{version, digestAlgs, encapContent, [0] certs, [1] crls?, signerInfos SET}."""
    return signed_data["kids"][-1]["kids"][0]


def swap_timestamp(donor, recipient, out):
    """Copy donor's RFC 3161 token attribute value into recipient's signature, re-encoding the lengths."""
    def token_attr(pe_bytes):
        _, off, size = pe_cert_table(pe_bytes)
        win = pe_bytes[off:off + size]
        length = struct.unpack_from("<I", win, 0)[0]
        content_info, _ = der_parse(win[8:length])
        signed_data = content_info["kids"][1]["kids"][0]
        signer = find_signer_info(signed_data)
        unauth = signer["kids"][-1]
        assert unauth["tag"] == 0xA1, "signer has no unauthenticated attributes"
        for attr in unauth["kids"]:
            if attr["kids"][0]["raw"] == RFC3161_COUNTERSIGN:
                return content_info, unauth, attr
        raise SystemExit("no RFC 3161 attribute")

    _, _, donor_attr = token_attr(donor)
    content_info, unauth, attr = token_attr(recipient)
    attr["kids"][1] = donor_attr["kids"][1]
    new_pkcs7 = der_dump(content_info)
    _, off, size = pe_cert_table(recipient)
    padded = new_pkcs7 + b"\0" * (-len(new_pkcs7) % 8)
    win = struct.pack("<IHH", len(new_pkcs7) + 8, 0x0200, 0x0002) + padded
    dir_off = pe_cert_table(recipient)[0]
    result = bytearray(recipient[:off]) + win
    struct.pack_into("<II", result, dir_off, off, len(win))
    out.write_bytes(bytes(result))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out")
    parser.add_argument("--now", type=int, default=int(time.time()))
    args = parser.parse_args()
    for tool in ("openssl", "osslsigncode", "x86_64-w64-mingw32-gcc"):
        if shutil.which(tool) is None:
            sys.exit(f"gen_plugin_signature_fixtures: FAIL {tool} not found on PATH")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    now = args.now
    pki = Pki(out)
    pinned = pki.root("pinned_root", "Test Pinned Root", now - 400 * DAY, now + 3000 * DAY)
    other = pki.root("other_root", "Test Other Root", now - 400 * DAY, now + 3000 * DAY)

    def leaf(name, issuer, ekus, nb, na, serial):
        return pki.leaf(name, f"Test {name}", issuer, ekus, nb, na, serial)

    good = leaf("good", pinned, [CODE_SIGNING, PROFILE_EKU], now - DAY, now + 3 * DAY, 0x1001)
    wrong_eku = leaf("wrong_eku", pinned, [CODE_SIGNING, OTHER_EKU], now - DAY, now + 3 * DAY, 0x1002)
    foreign = leaf("foreign", other, [CODE_SIGNING, PROFILE_EKU], now - DAY, now + 3 * DAY, 0x1003)
    forged = leaf("forged", other, [CODE_SIGNING, PROFILE_EKU], now - DAY, now + 3 * DAY, 0x1004)
    old = leaf("old", pinned, [CODE_SIGNING, PROFILE_EKU], now - 10 * DAY, now - 5 * DAY, 0x1005)

    tsa_leaf = leaf("tsa", pinned, [TIME_STAMPING], now - 30 * DAY, now + 3000 * DAY, 0x2001)
    tsa = {"key": tsa_leaf["key"], "pem_chain": pki.path("tsa.chain.pem")}
    Path(tsa["pem_chain"]).write_text(Path(tsa_leaf["pem"]).read_text() + Path(pinned["pem"]).read_text())

    tsa_foreign_leaf = leaf("tsa_foreign", other, [TIME_STAMPING], now - 30 * DAY, now + 3000 * DAY, 0x2002)
    tsa_foreign = {"key": tsa_foreign_leaf["key"], "pem_chain": pki.path("tsa_foreign.chain.pem")}
    Path(tsa_foreign["pem_chain"]).write_text(Path(tsa_foreign_leaf["pem"]).read_text() + Path(other["pem"]).read_text())

    plain = build_dll(out, "plain", "plugin-one")
    plain2 = build_dll(out, "plain2", "plugin-two")
    shutil.copy(plain, out / "unsigned.dll")

    pinned_chain = [pinned["pem"]]
    files = {}

    def make(name, leaf_, chain, tsa_time, src=plain, tsa_=None):
        sign(src, out / f"{name}.dll", leaf_, chain, (tsa_ or tsa) if tsa_time is not None else None, tsa_time)
        files[name] = f"{name}.dll"

    make("good", good, pinned_chain, now)
    make("good2", good, pinned_chain, now, plain2)
    make("wrong_eku", wrong_eku, pinned_chain, now)
    make("other_root", foreign, [], now)                        # the root is not in the file nor the store
    make("forged_root", forged, [other["pem"]], now)            # the file carries its own root
    make("expired_stamped_inside", old, pinned_chain, now - 7 * DAY)
    make("expired_unstamped", old, pinned_chain, None)
    make("stamped_after_expiry", old, pinned_chain, now - 2 * DAY)
    make("good_unstamped", good, pinned_chain, None)
    make("tsa_untrusted", good, pinned_chain, now, tsa_=tsa_foreign)   # stamped by an authority the pinned root did not issue

    data = bytearray((out / "good.dll").read_bytes())
    data[1024] ^= 0x01                                           # a code byte (the first section starts at 0x400)
    (out / "tampered_code.dll").write_bytes(bytes(data))

    data = bytearray((out / "good.dll").read_bytes())
    _, off, size = pe_cert_table(bytes(data))
    length = struct.unpack_from("<I", data, off)[0]
    content_info, _ = der_parse(bytes(data[off + 8:off + length]))
    signer = find_signer_info(content_info["kids"][1]["kids"][0])
    rsa_signature = next(k["raw"] for k in signer["kids"] if k["tag"] == 0x04)
    at = bytes(data).find(rsa_signature, off)
    assert at > 0 and len(rsa_signature) >= 256
    data[at + 100] ^= 0x01                                       # a byte of the RSA signature itself
    (out / "tampered_signature.dll").write_bytes(bytes(data))

    swap_timestamp((out / "good2.dll").read_bytes(), (out / "good.dll").read_bytes(), out / "token_copied.dll")

    for name in ("unsigned", "tampered_code", "tampered_signature", "token_copied"):
        files[name] = f"{name}.dll"

    manifest = {
        "now": now,
        "root_der": "pinned_root.der",
        "profile_eku": PROFILE_EKU,
        "other_eku": OTHER_EKU,
        "good_subject": "CN=Test good",
        "serials": {"good": good["serial_hex"], "old": old["serial_hex"]},
        "stamp_time": {"good": now, "expired_stamped_inside": now - 7 * DAY, "stamped_after_expiry": now - 2 * DAY},
        "files": files,
    }
    (out / "fixtures.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"gen_plugin_signature_fixtures: wrote {len(files)} files and fixtures.json to {out}")


if __name__ == "__main__":
    main()
