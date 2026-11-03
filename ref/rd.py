import struct, sys
REGS = ["PC","SP","AF","BC","DE","HL","IX","IY","AF2","BC2","DE2","HL2","WZ","R","I","IM","IFF1","IFF2"]
RS = 4 + 8 + 2 * len(REGS) + 4 + 3072 + 2048
def frames(path):
    d = open(path, "rb").read()
    for o in range(0, len(d), RS):
        n, t = struct.unpack_from("<Id", d, o)
        regs = dict(zip(REGS, struct.unpack_from("<%dH" % len(REGS), d, o + 12)))
        p = o + 12 + 2 * len(REGS)
        ports = tuple(d[p:p + 4]); ram = d[p + 4:p + 4 + 3072]; vram = d[p + 4 + 3072:p + 4 + 5120]
        yield n, t, regs, ports, ram, vram
if __name__ == "__main__":
    prev = None
    for n, t, r, ports, ram, vram in frames(sys.argv[1]):
        if n <= 5 or n % 500 == 0 or (prev and ports != prev):
            print(f"f{n:5d} t={t:9.5f} PC={r['PC']:04x} SP={r['SP']:04x} R={r['R']:02x} IFF1={r['IFF1']} ports={['%02x'%x for x in ports]} mode={ram[0xA]:02x}")
        prev = ports
