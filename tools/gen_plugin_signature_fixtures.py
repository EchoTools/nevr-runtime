#!/usr/bin/env python3
"""Generate the signed and unsigned DLLs the plugin-signature GTests read (ADR 0010, #741).

    gen_plugin_signature_fixtures.py <out-dir> [--now <unix seconds>]

Everything is made offline from a throwaway PKI: a pinned test root, a second root that is NOT pinned, leaf
certificates with chosen validity windows and EKUs, and timestamps from osslsigncode's built-in TSA at chosen
times. Writes <out-dir>/fixtures.json, the manifest the tests read: the pinned root and the facts about each
file (what a correct verifier must say). Needs openssl, osslsigncode and x86_64-w64-mingw32-gcc on PATH.
"""

import argparse
import copy
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

    def leaf(self, name, cn, issuer, ekus, not_before, not_after, serial, bits=2048):
        key, csr, pem, ext = (self.path(f"{name}.{x}") for x in ("key", "csr", "pem", "ext"))
        run(["openssl", "req", "-newkey", f"rsa:{bits}", "-nodes", "-keyout", key, "-out", csr, "-subj", f"/CN={cn}"])
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


def build_c(work, name, source, extra=(), defs=None):
    """Compile `source` (C text) into <work>/<name>.dll; `extra` are further linker inputs/flags, `defs` a .def text."""
    src = work / f"{name}.c"
    src.write_text(source)
    out = work / f"{name}.dll"
    cmd = ["x86_64-w64-mingw32-gcc", "-shared", "-O1", "-nostdlib", "-Wl,--entry=DllMain", "-o", str(out), str(src)]
    if defs is not None:
        def_file = work / f"{name}.def"
        def_file.write_text(defs)
        cmd.append(str(def_file))
    run(cmd + list(extra))
    return out


def set_data_dir(path, index, rva, size):
    """Point data directory `index` of a PE32+ file at (rva, size)."""
    data = bytearray(Path(path).read_bytes())
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0" and struct.unpack_from("<H", data, pe + 24)[0] == 0x20B
    struct.pack_into("<II", data, pe + 24 + 112 + index * 8, rva, size)
    Path(path).write_bytes(bytes(data))


def section_rva(path, name):
    data = Path(path).read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    first = pe + 24 + opt_size
    for i in range(count):
        at = first + i * 40
        if data[at:at + 8].rstrip(b"\0") == name.encode():
            return struct.unpack_from("<I", data, at + 12)[0]
    raise SystemExit(f"gen_plugin_signature_fixtures: FAIL no section {name} in {path}")


def build_dependency_dlls(work):
    """The unsigned DLLs the tree fixtures are made of. dep exports Dep; each other names dep.dll (or mid.dll) in
    the table the test is about: import, delay-load import, or forwarded export."""
    stub = ('int __stdcall DllMain(void*a,unsigned long b,void*c){(void)a;(void)b;(void)c;return 1;}\n')
    dep = build_c(work, "dep", '__declspec(dllexport) int Dep(void){return 7;}\n' + stub)
    build_c(work, "importer", '__declspec(dllimport) int Dep(void);\n'
            'int __stdcall DllMain(void*a,unsigned long b,void*c){(void)a;(void)b;(void)c;return Dep();}\n', [str(dep)])
    build_c(work, "mid", '__declspec(dllimport) int Dep(void);\n__declspec(dllexport) int Mid(void){return Dep();}\n' + stub,
            [str(dep)])
    mid = work / "mid.dll"
    build_c(work, "toplevel", '__declspec(dllimport) int Mid(void);\n'
            'int __stdcall DllMain(void*a,unsigned long b,void*c){(void)a;(void)b;(void)c;return Mid();}\n', [str(mid)])
    build_c(work, "forwarder", stub, defs="EXPORTS\n  Fwd = dep.Dep\n")
    # A plugin that loads another library from its own DllMain: the file named by NEVR_TEST_DLLMAIN_LOAD. It exports
    # no plugin entry points; what is under test is the load it makes while the loader still holds the loader lock.
    build_c(work, "dllmain_loader",
            '#include <stddef.h>\n'
            '__declspec(dllimport) unsigned long __stdcall GetEnvironmentVariableW(const wchar_t*, wchar_t*, unsigned long);\n'
            '__declspec(dllimport) void* __stdcall LoadLibraryW(const wchar_t*);\n'
            'int __stdcall DllMain(void*a,unsigned long b,void*c){(void)a;(void)c;\n'
            '  if(b==1){wchar_t path[520];unsigned long n=GetEnvironmentVariableW(L"NEVR_TEST_DLLMAIN_LOAD",path,520);\n'
            '    if(n>0&&n<520)LoadLibraryW(path);}\n'
            '  return 1;}\n', ["-lkernel32"])
    # A delay-load descriptor (data directory 13) written by hand: this linker has no delay-load support. The
    # descriptor is the attribute word (1: RVAs, not addresses), the DLL name RVA and six zero words; a zero
    # descriptor ends the table.
    build_c(work, "delayer",
            '__asm__(".section .didat,\\"dr\\"\\nnevr_delay_desc:\\n.long 1\\n.rva nevr_delay_name\\n'
            '.fill 24,1,0\\n.fill 32,1,0\\n.section .rdata,\\"dr\\"\\nnevr_delay_name:\\n.asciz \\"dep.dll\\"\\n.text\\n");\n' + stub)
    delayer = work / "delayer.dll"
    set_data_dir(delayer, 13, section_rva(delayer, ".didat"), 64)
    # An import directory that points outside the image: a file whose dependencies cannot be listed.
    build_c(work, "broken", stub)
    set_data_dir(work / "broken.dll", 1, 0x7FFFF000, 40)
    return dep


def pe_layout(data):
    """(offset of the PE header, sections [(name, va, virtual size, raw pointer, raw size)], data directory entries)."""
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    first = pe + 24 + opt_size
    sections = []
    for i in range(count):
        at = first + i * 40
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", data, at + 8)
        sections.append((data[at:at + 8].rstrip(b"\0").decode(), va, vsize, rawptr, rawsize))
    return pe, sections


def rva_to_offset(data, rva):
    _, sections = pe_layout(data)
    for _, va, vsize, rawptr, rawsize in sections:
        if va <= rva < va + max(vsize, rawsize):
            return rawptr + (rva - va)
    raise SystemExit(f"gen_plugin_signature_fixtures: FAIL rva {rva:#x} is in no section")


def data_dir(data, index):
    pe, _ = pe_layout(data)
    return struct.unpack_from("<II", data, pe + 24 + 112 + index * 8)


def patch_dir(data, index, rva, size):
    pe, _ = pe_layout(data)
    struct.pack_into("<II", data, pe + 24 + 112 + index * 8, rva, size)


def build_corpus(work):
    """Files a parser must refuse, or survive, made by damaging a good DLL's own tables. `lists` is whether
    ListDependencies must succeed (False: it must fail and say something), None: only that it returns."""
    corpus = {}

    def put(name, data, lists, error_contains=""):
        (work / f"corpus_{name}.dll").write_bytes(bytes(data))
        corpus[name] = {"file": f"corpus_{name}.dll", "lists": lists, "error_contains": error_contains}

    importer = bytearray((work / "importer.dll").read_bytes())
    rva, _ = data_dir(importer, 1)
    table = rva_to_offset(importer, rva)
    put("truncated_import_table", importer[:table + 10], False, "does not end")

    data = bytearray(importer)
    pe, sections = pe_layout(data)
    text = next(sec for sec in sections if sec[0] == ".text")
    patch_dir(data, 1, text[1], 40)                      # a table that is really code: no end, no sane names
    put("import_table_is_code", data, False)

    data = bytearray(importer)
    struct.pack_into("<I", data, table + 12, 0x7FFFF000)  # the first descriptor's DLL name, past the image
    put("import_name_past_image", data, False, "outside the file")

    delayer = bytearray((work / "delayer.dll").read_bytes())
    drva, _ = data_dir(delayer, 13)
    doff = rva_to_offset(delayer, drva)
    data = bytearray(delayer)
    struct.pack_into("<I", data, doff + 4, 0x7FFFF000)
    put("delay_name_past_image", data, False, "outside the file")
    data = bytearray(delayer)
    struct.pack_into("<I", data, doff, 0)                # attributes 0: the descriptor holds addresses, not RVAs
    put("delay_virtual_addresses", data, False, "virtual addresses")

    forwarder = bytearray((work / "forwarder.dll").read_bytes())
    erva, _ = data_dir(forwarder, 0)
    eoff = rva_to_offset(forwarder, erva)
    functions = rva_to_offset(forwarder, struct.unpack_from("<I", forwarder, eoff + 28)[0])
    string_at = rva_to_offset(forwarder, struct.unpack_from("<I", forwarder, functions)[0])
    assert forwarder[string_at:string_at + 8] == b"dep.Dep\0"
    data = bytearray(forwarder)
    data[string_at + 3] = ord("x")                       # "depxDep": a forwarder with no DLL name
    put("forward_without_dll", data, False, "no DLL name")
    data = bytearray(forwarder)
    struct.pack_into("<I", data, eoff + 28, 0x7FFFF000)  # AddressOfFunctions
    put("export_table_past_image", data, False, "outside the file")
    data = bytearray(forwarder)
    struct.pack_into("<I", data, eoff + 20, 0x7FFFFFFF)  # NumberOfFunctions
    put("export_count_huge", data, False, "outside the file")

    # The import directory laid over the export directory: its "descriptors" are export-directory bytes, so the
    # parser reads fields that were never import descriptors. It must return, not crash or run on.
    data = bytearray(forwarder)
    erva, esize = data_dir(forwarder, 0)
    patch_dir(data, 1, erva, esize)
    put("import_directory_overlaps_export_directory", data, None)

    # A dependency named by a path: the loader resolves that relative to something else, so it cannot be checked.
    data = bytearray(importer)
    assert data.count(b"dep.dll\0") == 1
    put("import_named_by_path", data.replace(b"dep.dll\0", b"a\\b.dll\0"), True)

    # A dependency named without an extension: the loader appends ".dll", so "dep" is dep.dll. A name of the same
    # length keeps every table where it is ("dep.dll" and "dep" plus padding are both eight bytes with the NUL).
    data = bytearray(importer)
    put("import_without_extension", data.replace(b"dep.dll\0", b"dep\0\0\0\0\0"), True)
    # The same, naming a library the system supplies: hid.dll is in System32 and is not loaded by the test process.
    data = bytearray(importer)
    put("import_system_without_extension", data.replace(b"dep.dll\0", b"hid\0\0\0\0\0"), True)
    return corpus


def build_cap_dlls(work, count):
    """capdep_001.dll .. capdep_<count>.dll, each exporting one function, and two importers: cap64.dll names the
    first 64, cap65.dll names all 65."""
    names = [f"capdep_{i:03d}" for i in range(1, count + 1)]
    for i, name in enumerate(names, 1):
        build_c(work, name, f'__declspec(dllexport) int F{i}(void){{return {i};}}\n'
                'int __stdcall DllMain(void*a,unsigned long b,void*c){(void)a;(void)b;(void)c;return 1;}\n')

    def importer(out_name, used):
        decl = "".join(f"__declspec(dllimport) int F{i}(void);\n" for i in range(1, used + 1))
        body = "+".join(f"F{i}()" for i in range(1, used + 1))
        build_c(work, out_name, decl + f'int __stdcall DllMain(void*a,unsigned long b,void*c){{(void)a;(void)b;(void)c;return {body};}}\n',
                [str(work / f"{n}.dll") for n in names[:used]])

    importer("cap64", 64)
    importer("cap65", 65)
    return names


def sign(unsigned, out, leaf, chain_pems, tsa=None, tsa_time=None, digest="sha256"):
    """osslsigncode sign; chain_pems are concatenated behind the leaf into the -certs file."""
    certs = Path(str(out) + ".chain.pem")
    certs.write_text("".join(Path(p).read_text() for p in [leaf["pem"], *chain_pems]))
    cmd = ["osslsigncode", "sign", "-certs", str(certs), "-key", leaf["key"], "-h", digest,
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


def pkcs7_of(pe_bytes):
    """The PKCS#7 DER of a signed PE (WIN_CERTIFICATE header and padding removed)."""
    _, off, size = pe_cert_table(pe_bytes)
    length = struct.unpack_from("<I", pe_bytes, off)[0]
    return pe_bytes[off + 8:off + length]


def with_pkcs7(pe_bytes, pkcs7, out):
    """The same PE with its certificate table replaced by `pkcs7` (the table is the last thing in the file)."""
    dir_off, off, size = pe_cert_table(pe_bytes)
    padded = pkcs7 + b"\0" * (-len(pkcs7) % 8)
    win = struct.pack("<IHH", len(pkcs7) + 8, 0x0200, 0x0002) + padded
    result = bytearray(pe_bytes[:off]) + win
    struct.pack_into("<II", result, dir_off, off, len(win))
    out.write_bytes(bytes(result))


def spc_digest(pkcs7):
    """The Authenticode digest inside SpcIndirectDataContent: encapContentInfo -> [0] -> SEQ -> DigestInfo -> OCTET."""
    content_info, _ = der_parse(pkcs7)
    signed_data = content_info["kids"][1]["kids"][0]
    spc = signed_data["kids"][2]["kids"][1]["kids"][0]
    return spc["kids"][1]["kids"][1]["raw"]


OID_MESSAGE_DIGEST = bytes.fromhex("2a864886f70d010904")   # 1.2.840.113549.1.9.4
OID_CONTENT_TYPE = bytes.fromhex("2a864886f70d010903")     # 1.2.840.113549.1.9.3
OID_DATA = bytes.fromhex("2a864886f70d010701")             # 1.2.840.113549.1.7.1
OID_SHA1_WITH_RSA = bytes.fromhex("2a864886f70d010105")    # 1.2.840.113549.1.1.5


def resigned(pe_bytes, leaf_, mutate, work, name):
    """The signature of an unstamped file, re-made with the leaf's throwaway key over MUTATED signed attributes, so
    the RSA signature is genuine and only the one structural fact under test is wrong. `mutate(signer)` edits the
    signerInfo node in place (its kids: version, sid, digestAlgorithm, [0] signedAttrs, signatureAlgorithm,
    signature)."""
    content_info, _ = der_parse(pkcs7_of(pe_bytes))
    signer = find_signer_info(content_info["kids"][1]["kids"][0])
    assert signer["kids"][3]["tag"] == 0xA0 and signer["kids"][-1]["tag"] == 0x04, "expected an unstamped signature"
    mutate(signer)
    attrs = {"tag": 0x31, "raw": b"", "kids": signer["kids"][3]["kids"]}
    to_sign = Path(work) / f"{name}.attrs"
    to_sign.write_bytes(der_dump(attrs))
    signature = subprocess.run(["openssl", "dgst", "-sha256", "-sign", leaf_["key"], str(to_sign)],
                               capture_output=True, check=True).stdout
    signer["kids"][5]["raw"] = signature
    return der_dump(content_info)


def attr_named(signer, oid):
    return next((a for a in signer["kids"][3]["kids"] if a["kids"][0]["raw"] == oid), None)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out")
    parser.add_argument("--now", type=int, default=int(time.time()))
    parser.add_argument("--plugin-dll", action="append", default=[],
                        help="a real plugin DLL: also written signed by the good signer (<stem>.signed.dll), signed "
                             "with another signer's EKU (<stem>.wrong_eku.dll) and unsigned (<stem>.unsigned.dll)")
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
    # The forgery that matters: same subject and serial as the pinned root, a fresh key, and a leaf it issued.
    forged_root = pki.root("forged_root_ca", "Test Pinned Root", now - 400 * DAY, now + 3000 * DAY)
    forged = leaf("forged", forged_root, [CODE_SIGNING, PROFILE_EKU], now - DAY, now + 3 * DAY, 0x1004)
    weak = pki.leaf("weak", "Test weak", pinned, [CODE_SIGNING, PROFILE_EKU], now - DAY, now + 3 * DAY, 0x1006, bits=1024)
    old = leaf("old", pinned, [CODE_SIGNING, PROFILE_EKU], now - 10 * DAY, now - 5 * DAY, 0x1005)

    tsa_leaf = leaf("tsa", pinned, [TIME_STAMPING], now - 30 * DAY, now + 3000 * DAY, 0x2001)
    tsa = {"key": tsa_leaf["key"], "pem_chain": pki.path("tsa.chain.pem")}
    Path(tsa["pem_chain"]).write_text(Path(tsa_leaf["pem"]).read_text() + Path(pinned["pem"]).read_text())

    tsa_foreign_leaf = leaf("tsa_foreign", other, [TIME_STAMPING], now - 30 * DAY, now + 3000 * DAY, 0x2002)
    tsa_foreign = {"key": tsa_foreign_leaf["key"], "pem_chain": pki.path("tsa_foreign.chain.pem")}
    Path(tsa_foreign["pem_chain"]).write_text(Path(tsa_foreign_leaf["pem"]).read_text() + Path(other["pem"]).read_text())

    tsa_weak_leaf = pki.leaf("tsa_weak", "Test tsa_weak", pinned, [TIME_STAMPING], now - 30 * DAY, now + 3000 * DAY,
                             0x2003, bits=1024)
    tsa_weak = {"key": tsa_weak_leaf["key"], "pem_chain": pki.path("tsa_weak.chain.pem")}
    Path(tsa_weak["pem_chain"]).write_text(Path(tsa_weak_leaf["pem"]).read_text() + Path(pinned["pem"]).read_text())

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
    make("forged_root", forged, [forged_root["pem"]], now)      # the file carries a copy of the root's name and serial, not its key
    make("weak_key", weak, pinned_chain, now)
    make("tsa_weak", good, pinned_chain, now, tsa_=tsa_weak)    # stamped by an authority with a 1024 bit key
    sign(plain, out / "sha1_digest.dll", good, pinned_chain, digest="sha1")   # SHA-1 digest: refused whole
    files["sha1_digest"] = "sha1_digest.dll"
    make("expired_stamped_inside", old, pinned_chain, now - 7 * DAY)
    make("expired_unstamped", old, pinned_chain, None)
    make("stamped_after_expiry", old, pinned_chain, now - 2 * DAY)
    make("good_unstamped", good, pinned_chain, None)
    make("tsa_untrusted", good, pinned_chain, now, tsa_=tsa_foreign)   # stamped by an authority the pinned root did not issue

    plugins = {}
    for source in args.plugin_dll:
        stem = Path(source).stem
        for kind, leaf_ in (("signed", good), ("wrong_eku", wrong_eku)):
            name = f"{stem}.{kind}"
            sign(Path(source), out / f"{name}.dll", leaf_, pinned_chain, tsa, now)
            files[name] = f"{name}.dll"
        shutil.copy(source, out / f"{stem}.unsigned.dll")
        files[f"{stem}.unsigned"] = f"{stem}.unsigned.dll"
        plugins[stem] = {k: f"{stem}.{k}" for k in ("signed", "wrong_eku", "unsigned")}

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

    # content_swap: a genuine signature (good's), its Authenticode digest replaced by the digest of ANOTHER file
    # (plain2, signed as good2), attached to that other file. The signed attributes are untouched, so the RSA
    # signature still verifies; only the binding of messageDigest to the content is left to catch it.
    good_p7 = pkcs7_of((out / "good.dll").read_bytes())
    old_digest = spc_digest(good_p7)
    new_digest = spc_digest(pkcs7_of((out / "good2.dll").read_bytes()))
    assert good_p7.count(old_digest) == 1 and len(old_digest) == len(new_digest)
    with_pkcs7((out / "good2.dll").read_bytes(), good_p7.replace(old_digest, new_digest), out / "content_swap.dll")

    # backdated_token: good's timestamp token with only genTime edited (same length), inside a certificate that
    # was already expired at the real time. The token's own signature no longer covers its TSTInfo.
    sign(plain, out / "stamped_after_expiry_copy.dll", old, pinned_chain, tsa, now - 2 * DAY)
    data = (out / "stamped_after_expiry_copy.dll").read_bytes()
    real, fake = stamp(now - 2 * DAY).encode(), stamp(now - 7 * DAY).encode()
    assert data.count(real) == 1, data.count(real)
    (out / "backdated_token.dll").write_bytes(data.replace(real, fake))

    # Structure faults under a GENUINE signature (re-signed with the throwaway leaf key over the edited attributes).
    unstamped = (out / "good_unstamped.dll").read_bytes()

    def two_message_digests(signer):
        attrs = signer["kids"][3]["kids"]
        attrs.append(copy.deepcopy(attr_named(signer, OID_MESSAGE_DIGEST)))

    def no_content_type(signer):
        signer["kids"][3]["kids"].remove(attr_named(signer, OID_CONTENT_TYPE))

    def other_content_type(signer):
        attr_named(signer, OID_CONTENT_TYPE)["kids"][1]["kids"][0]["raw"] = OID_DATA

    def sha1_signature_algorithm(signer):
        signer["kids"][4]["kids"][0]["raw"] = OID_SHA1_WITH_RSA

    for name, edit in (("two_message_digests", two_message_digests), ("no_content_type", no_content_type),
                       ("other_content_type", other_content_type), ("sha1_signature_algorithm", sha1_signature_algorithm)):
        with_pkcs7(unstamped, resigned(unstamped, good, edit, out, name), out / f"{name}.dll")
        files[name] = f"{name}.dll"

    # A token that is genuine but dated in the future (the authority's clock, or a forged time, ahead of now).
    make("token_future", good, pinned_chain, now + 2 * DAY)

    for name in ("unsigned", "tampered_code", "tampered_signature", "token_copied", "content_swap", "backdated_token"):
        files[name] = f"{name}.dll"


    # Dependency trees (PR 4): a directory per case, holding the plugin and what it names. `signed` files carry the
    # good signature; the manifest says which file a correct tree walk refuses, how it reached it, and why.
    build_dependency_dlls(out)
    trees = {}

    def tree(name, root, members, refuses=None, via=None, reason=None):
        """members: {file name in the directory: (built dll stem, signer leaf or None for unsigned)}."""
        directory = out / f"tree_{name}"
        directory.mkdir(exist_ok=True)
        for fname, (stem, signer) in members.items():
            if signer is None:
                shutil.copy(out / f"{stem}.dll", directory / fname)
            else:
                sign(out / f"{stem}.dll", directory / fname, signer, pinned_chain, tsa, now)
        trees[name] = {"dir": f"tree_{name}", "root": root, "refused_file": refuses, "via": via, "reason": reason}

    tree("import_ok", "importer.dll", {"importer.dll": ("importer", good), "dep.dll": ("dep", good)})
    tree("import_unsigned", "importer.dll", {"importer.dll": ("importer", good), "dep.dll": ("dep", None)},
         "dep.dll", "import", "Unsigned")
    tree("import_wrong_signer", "importer.dll", {"importer.dll": ("importer", good), "dep.dll": ("dep", wrong_eku)},
         "dep.dll", "import", "MissingEku")
    # dep.dll is in the directory of no one and is not the system's: a file put there before the load would map unchecked.
    tree("import_absent", "importer.dll", {"importer.dll": ("importer", good)}, "dep.dll", "import", "Unreadable")
    tree("delay_ok", "delayer.dll", {"delayer.dll": ("delayer", good), "dep.dll": ("dep", good)})
    tree("delay_unsigned", "delayer.dll", {"delayer.dll": ("delayer", good), "dep.dll": ("dep", None)},
         "dep.dll", "delay-load", "Unsigned")
    tree("forwarder_ok", "forwarder.dll", {"forwarder.dll": ("forwarder", good), "dep.dll": ("dep", good)})
    tree("forwarder_unsigned", "forwarder.dll", {"forwarder.dll": ("forwarder", good), "dep.dll": ("dep", None)},
         "dep.dll", "forwarder", "Unsigned")
    tree("transitive_ok", "toplevel.dll",
         {"toplevel.dll": ("toplevel", good), "mid.dll": ("mid", good), "dep.dll": ("dep", good)})
    tree("transitive_unsigned", "toplevel.dll",
         {"toplevel.dll": ("toplevel", good), "mid.dll": ("mid", good), "dep.dll": ("dep", None)},
         "dep.dll", "import", "Unsigned")
    tree("malformed", "broken.dll", {"broken.dll": ("broken", good)}, "the plugin", "", "MalformedSignature")
    tree("dllmain_load", "dllmain_loader.dll", {"dllmain_loader.dll": ("dllmain_loader", good)})


    # The 64-file cap: 64 signed dependencies pass, a 65th is refused (and the walk does not fail open).
    cap_names = build_cap_dlls(out, 65)
    signed_cap = out / "signed_cap"
    signed_cap.mkdir(exist_ok=True)
    for n in cap_names:
        sign(out / f"{n}.dll", signed_cap / f"{n}.dll", good, pinned_chain, tsa, now)
    for tree_name, root_stem, used in (("cap_64", "cap64", 64), ("cap_65", "cap65", 65)):
        directory = out / f"tree_{tree_name}"
        directory.mkdir(exist_ok=True)
        sign(out / f"{root_stem}.dll", directory / f"{root_stem}.dll", good, pinned_chain, tsa, now)
        for n in cap_names[:used]:
            shutil.copy(signed_cap / f"{n}.dll", directory / f"{n}.dll")
        trees[tree_name] = {"dir": f"tree_{tree_name}", "root": f"{root_stem}.dll", "refused_file": None if used == 64 else "capdep_065.dll",
                            "via": "import", "reason": "Unreadable" if used == 65 else None, "cap": used}
    shutil.rmtree(signed_cap)

    corpus = build_corpus(out)
    tree("import_named_by_path", "corpus_import_named_by_path.dll",
         {"corpus_import_named_by_path.dll": ("corpus_import_named_by_path", good)},
         "a\\b.dll", "import", "Unreadable")

    tree("import_without_extension", "corpus_import_without_extension.dll",
         {"corpus_import_without_extension.dll": ("corpus_import_without_extension", good), "dep.dll": ("dep", None)},
         "dep.dll", "import", "Unsigned")
    tree("import_without_extension_absent", "corpus_import_without_extension.dll",
         {"corpus_import_without_extension.dll": ("corpus_import_without_extension", good)},
         "dep.dll", "import", "Unreadable")
    tree("import_system_without_extension", "corpus_import_system_without_extension.dll",
         {"corpus_import_system_without_extension.dll": ("corpus_import_system_without_extension", good)})

    manifest = {
        "now": now,
        "root_der": "pinned_root.der",
        "profile_eku": PROFILE_EKU,
        "other_eku": OTHER_EKU,
        "good_subject": "CN=Test good",
        "serials": {"good": good["serial_hex"], "old": old["serial_hex"]},
        "stamp_time": {"good": now, "expired_stamped_inside": now - 7 * DAY, "stamped_after_expiry": now - 2 * DAY},
        "files": files,
        "plugins": plugins,
        "trees": trees,
        "corpus": corpus,
    }
    (out / "fixtures.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"gen_plugin_signature_fixtures: wrote {len(files)} files and fixtures.json to {out}")


if __name__ == "__main__":
    main()
