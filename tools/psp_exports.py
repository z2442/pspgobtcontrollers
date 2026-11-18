"""Inspect function export tables in an unrelocated PSP ELF/PRX image."""
import hashlib
import struct


def function_nid(name):
    return int.from_bytes(hashlib.sha1(name.encode("ascii")).digest()[:4], "little")


def function_exports(binary):
    if binary[:6] != b"\x7fELF\x01\x01":
        raise ValueError("Expected a little-endian ELF32 PSP image")
    section_offset = struct.unpack_from("<I", binary, 32)[0]
    section_size, count, names_index = struct.unpack_from("<HHH", binary, 46)
    sections = [struct.unpack_from("<10I", binary, section_offset + i * section_size) for i in range(count)]
    names_section = sections[names_index]
    names = binary[names_section[4]:names_section[4] + names_section[5]]

    def at(address, size):
        for _, kind, flags, start, offset, length, *_ in sections:
            if kind != 8 and flags & 2 and start <= address and address + size <= start + length:
                first = offset + address - start
                return binary[first:first + size]
        raise ValueError(f"Export address outside allocated image: {address:08x}")

    exports = []
    for name, _, _, _, offset, length, *_ in sections:
        if names[name:].split(b"\0", 1)[0] != b".lib.ent":
            continue
        cursor, end = offset, offset + length
        while cursor < end:
            library, _, attributes, words, variables, functions, table = struct.unpack_from("<IHHBBHI", binary, cursor)
            if words < 4 or cursor + words * 4 > end:
                raise ValueError("Invalid PSP export descriptor")
            total = variables + functions
            entries = struct.unpack(f"<{total * 2}I", at(table, total * 8))
            library_name = "syslib"
            if library:
                chars = bytearray()
                while True:
                    char = at(library + len(chars), 1)[0]
                    if not char:
                        break
                    chars.append(char)
                    if len(chars) > 255:
                        raise ValueError("Invalid PSP export library name")
                library_name = chars.decode("ascii")
            for i in range(functions):
                exports.append((library_name, entries[i], entries[total + i], attributes))
            cursor += words * 4
    return exports
