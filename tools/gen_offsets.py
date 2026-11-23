"""Generate dmsubs/offsets.ini for the installed UnityPlayer.dll.

Downloads Unity's public PDB for the exact UnityPlayer.dll build from
symbolserver.unity3d.com, resolves the native functions the mod hooks, and
derives the AudioSource -> clip PPtr field offset from disassembly.

The mod does the same thing by itself at startup when the game updates
(src/symbols.cpp); this script is for development / inspecting a build.
"""
import argparse
import ctypes
import os
import struct
import sys
import urllib.request
from ctypes import wintypes as W
from pathlib import Path

import capstone
import pefile

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_GAME = Path(r"D:\SteamLibrary\steamapps\common\Desktop Mate")

# key in ini -> exact (undecorated) symbol name. Keep in sync with kWanted in src/symbols.cpp.
WANTED = {
    "AudioSource_Play": "AudioSource::Play",
    "AudioSource_PlayOneShot": "AudioSource::PlayOneShot",
    "AudioSource_Stop": "AudioSource::Stop",
    "PPtr_Deref": "PPtr<GameObject>::operator GameObject * __ptr64",
    "NamedObject_GetName": "NamedObject::GetName",
    "GameObject_GetName": "GameObject::GetName",
    "Component_GetName": "Unity::Component::GetName",
    "AudioClip_GetLengthSec": "AudioClip::GetLengthSec",
    "Transform_GetPosition": "Transform::GetPosition",
    "FindMainCamera": "FindMainCamera",
    "Camera_GetWorldToClipMatrix": "Camera::GetWorldToClipMatrix",
    "AudioManager_Update": "AudioManager::Update",
    "FindObjectsOfType": "FindObjectsOfTypeImplementation",
    "RTTI_AudioClip": "TypeContainer<AudioClip>::rtti",
}


def pdb_info(dll):
    pe = pefile.PE(str(dll), fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DEBUG"]])
    for d in pe.DIRECTORY_ENTRY_DEBUG:
        if d.struct.Type == 2:
            raw = pe.get_data(d.struct.AddressOfRawData, d.struct.SizeOfData)
            g = raw[4:20]
            age, = struct.unpack("<I", raw[20:24])
            name = raw[24:].split(b"\0")[0].decode()
            d1, d2, d3 = struct.unpack("<IHH", g[:8])
            guid = "%08X%04X%04X%s" % (d1, d2, d3, g[8:].hex().upper())
            return Path(name.replace("\\", "/")).name, guid, age
    raise SystemExit("UnityPlayer.dll has no CodeView record")


def fetch_pdb(name, guid, age, cache):
    dst = cache / f"{guid}{age:X}" / name
    if dst.exists():
        return dst
    dst.parent.mkdir(parents=True, exist_ok=True)
    url = f"https://symbolserver.unity3d.com/{name}/{guid}{age:X}/{name}"
    print("downloading", url)
    with urllib.request.urlopen(url) as r, open(dst, "wb") as f:
        while chunk := r.read(1 << 20):
            f.write(chunk)
    return dst


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [("SizeOfStruct", W.ULONG), ("TypeIndex", W.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                ("Index", W.ULONG), ("Size", W.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", W.ULONG),
                ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", W.ULONG),
                ("Scope", W.ULONG), ("Tag", W.ULONG), ("NameLen", W.ULONG), ("MaxNameLen", W.ULONG),
                ("Name", W.WCHAR * 1)]


def all_symbols(dll, pdb_dir):
    dbg = ctypes.WinDLL(os.path.join(os.environ["SystemRoot"], "System32", "dbghelp.dll"))
    CB = ctypes.WINFUNCTYPE(W.BOOL, ctypes.POINTER(SYMBOL_INFOW), W.ULONG, ctypes.c_void_p)
    h = W.HANDLE(0x5EED)
    dbg.SymSetOptions(0x2 | 0x4)
    dbg.SymInitializeW.argtypes = [W.HANDLE, W.LPCWSTR, W.BOOL]
    if not dbg.SymInitializeW(h, str(pdb_dir), False):
        raise SystemExit("SymInitialize failed")
    dbg.SymLoadModuleExW.restype = ctypes.c_uint64
    dbg.SymLoadModuleExW.argtypes = [W.HANDLE, W.HANDLE, W.LPCWSTR, W.LPCWSTR, ctypes.c_uint64, W.DWORD,
                                     ctypes.c_void_p, W.DWORD]
    base = dbg.SymLoadModuleExW(h, None, str(dll), None, 0x10000000, 0, None, 0)
    out = {}

    def cb(p, size, ctx):
        s = p.contents
        name = ctypes.wstring_at(ctypes.addressof(s) + SYMBOL_INFOW.Name.offset, s.NameLen).rstrip("\0").strip()
        out.setdefault(name, s.Address - base)
        return True

    dbg.SymEnumSymbolsW.argtypes = [W.HANDLE, ctypes.c_uint64, W.LPCWSTR, CB, ctypes.c_void_p]
    dbg.SymEnumSymbolsW(h, base, "*", CB(cb), None)
    dbg.SymCleanup(h)
    return out


def clip_field_offset(dll, stop_rva, deref_rva):
    """AudioSource::Stop does `add rcx, <off>; call PPtr::operator T*` on the clip PPtr."""
    pe = pefile.PE(str(dll), fast_load=True)
    code = pe.get_memory_mapped_image()[stop_rva:stop_rva + 0x200]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    last_add = None
    for i in md.disasm(code, stop_rva):
        if i.mnemonic == "add" and i.op_str.startswith("rcx, "):
            last_add = int(i.op_str.split(",")[1], 0)
        if i.mnemonic == "call" and i.op_str.startswith("0x") and int(i.op_str, 16) == deref_rva:
            if last_add is not None:
                return last_add
    raise SystemExit("could not derive AudioSource clip offset")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", type=Path, default=DEFAULT_GAME)
    ap.add_argument("--out", type=Path, default=ROOT / "mod" / "dmsubs" / "offsets.ini")
    args = ap.parse_args()
    dll = args.game / "UnityPlayer.dll"
    name, guid, age = pdb_info(dll)
    pdb = fetch_pdb(name, guid, age, ROOT / "tools" / "symcache")
    syms = all_symbols(dll, pdb.parent)
    print(f"{len(syms)} symbols")
    lines = ["[unityplayer]", f"pdb_guid={guid}{age:X}", "", "[rva]"]
    missing = []
    for key, sym in WANTED.items():
        if sym in syms:
            lines.append(f"{key}=0x{syms[sym]:X}")
        else:
            missing.append(sym)
    off = clip_field_offset(dll, syms["AudioSource::Stop"], syms[WANTED["PPtr_Deref"]])
    lines += ["", "[layout]", f"AudioSource_ClipPPtr=0x{off:X}"]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("wrote", args.out)
    if missing:
        print("missing (optional features disabled):", ", ".join(missing))


if __name__ == "__main__":
    main()
