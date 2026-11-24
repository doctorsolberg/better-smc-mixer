"""Turn the USB key firmware's flash image into a UF2 file for the RP2040 boot ROM.

Usage: python3 -I tools/usbkey/uf2.py IMAGE.bin OUT.uf2

IMAGE.bin starts at flash address 0x10000000 with the 256-byte second-stage boot. Its last 4
bytes get the CRC-32 the boot ROM checks (polynomial 0x04C11DB7, initial value 0xFFFFFFFF, no
reflection, no final XOR, over the first 252 bytes). The UF2 file is what the board takes when it
shows up as the RPI-RP2 drive (BOOTSEL held at power-up): 256 bytes of payload per 512-byte block,
with the RP2040 family ID. Standard library only.
"""
import struct
import sys

FLASH_BASE = 0x10000000
FAMILY_RP2040 = 0xE48BFF56
MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY_ID = 0x00002000
PAYLOAD = 256


def crc32_boot2(data):
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc


def seal_boot2(image):
    """The image with the boot2 CRC filled in."""
    if len(image) < 256:
        raise ValueError("image shorter than the 256-byte second-stage boot")
    out = bytearray(image)
    out[252:256] = struct.pack("<I", crc32_boot2(out[:252]))
    return bytes(out)


def uf2(image, base=FLASH_BASE):
    blocks = [image[i:i + PAYLOAD] for i in range(0, len(image), PAYLOAD)]
    out = bytearray()
    for n, chunk in enumerate(blocks):
        data = chunk.ljust(476, b"\0")
        out += struct.pack("<IIIIIIII", MAGIC0, MAGIC1, FLAG_FAMILY_ID, base + n * PAYLOAD,
                           PAYLOAD, n, len(blocks), FAMILY_RP2040)
        out += data + struct.pack("<I", MAGIC_END)
    return bytes(out)


def main(argv):
    if len(argv) != 2:
        sys.exit(__doc__.strip())
    image = seal_boot2(open(argv[0], "rb").read())
    open(argv[1], "wb").write(uf2(image))
    print(f"{argv[1]}: {len(image)} bytes, {(len(image) + PAYLOAD - 1) // PAYLOAD} blocks, "
          f"boot2 CRC {struct.unpack('<I', image[252:256])[0]:#010x}")


if __name__ == "__main__":
    main(sys.argv[1:])
