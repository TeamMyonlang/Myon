"""Optional exhaustive reference-data check for a native shared library (Linux)."""
import argparse
import ctypes
import hashlib
from pathlib import Path
import random
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from generate_unicode_case import FILES


def reference_data(directory):
    for name, expected_hash in FILES.items():
        actual = hashlib.sha256((directory / name).read_bytes()).hexdigest()
        if actual != expected_hash:
            raise ValueError(f'{name}: expected the pinned Unicode 17.0.0 data')
    lower, upper = {}, {}
    for line in (directory / 'UnicodeData.txt').read_text(encoding='utf-8').splitlines():
        fields = line.split(';')
        cp = int(fields[0], 16)
        if fields[12]:
            upper[cp] = chr(int(fields[12], 16)).encode('utf-8')
        if fields[13]:
            lower[cp] = chr(int(fields[13], 16)).encode('utf-8')
    for line in (directory / 'SpecialCasing.txt').read_text(encoding='utf-8').splitlines():
        body = line.split('#', 1)[0].strip()
        if not body:
            continue
        fields = [part.strip() for part in body.split(';')]
        if fields[4]:
            continue  # A standalone scalar has no Final_Sigma/language context.
        cp = int(fields[0], 16)
        lower[cp] = ''.join(chr(int(x, 16)) for x in fields[1].split()).encode('utf-8')
        upper[cp] = ''.join(chr(int(x, 16)) for x in fields[3].split()).encode('utf-8')
    return lower, upper


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ucd_dir', type=Path)
    parser.add_argument('library', type=Path)
    args = parser.parse_args()
    reference = reference_data(args.ucd_dir)
    lib = ctypes.CDLL(str(args.library.resolve()))
    lib.myon_unicode_case.argtypes = [ctypes.c_char_p, ctypes.c_bool]
    lib.myon_unicode_case.restype = ctypes.c_void_p
    libc = ctypes.CDLL(None)
    libc.free.argtypes = [ctypes.c_void_p]

    def convert(source, upper):
        result = lib.myon_unicode_case(source, upper)
        assert result, 'unexpected size overflow'
        try:
            return ctypes.string_at(result)
        finally:
            libc.free(result)

    count = 0
    for cp in range(1, 0x110000):
        if 0xD800 <= cp <= 0xDFFF:
            continue
        source = chr(cp).encode('utf-8')
        for upper in [False, True]:
            expected = reference[upper].get(cp, source)
            actual = convert(source, upper)
            assert actual == expected, (hex(cp), upper, actual, expected)
            count += 1
    print(f'Unicode 17 single-scalar conformance: {count} conversions passed', flush=True)

    # Older, stable characters let Python act as an independent context oracle
    # even when the Python installation uses a different Unicode data version.
    rng = random.Random(1700)
    chars = list("aAΣσς Iİıißﬃǰΐ日本語!-.'") + ['\u0301', '\u0345', '\u200d', '𐐀', '𐐨']
    for _ in range(10000):
        source = ''.join(rng.choices(chars, k=rng.randrange(64)))
        assert convert(source.encode('utf-8'), False) == source.lower().encode('utf-8'), repr(source)
        assert convert(source.encode('utf-8'), True) == source.upper().encode('utf-8'), repr(source)
    print('Context corpus vs Python independent casing: 20000 conversions passed', flush=True)


if __name__ == '__main__':
    main()
