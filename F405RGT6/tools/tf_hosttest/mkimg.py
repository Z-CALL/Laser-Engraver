import struct, zlib, sys

SEC = 512
TOTAL = 8192          # 4MB
RESV  = 1
NFATS = 2
FATSZ = 32            # 扇区/FAT
ROOTE = 512           # 根目录项数 -> 32 扇区
SPC   = 1             # 1 扇区/簇  <- 故意设小，好把簇链走起来
root_dir_sectors = (ROOTE*32 + SEC-1)//SEC
data_start = RESV + NFATS*FATSZ + root_dir_sectors
clusters = (TOTAL - data_start)//SPC
assert 4085 <= clusters < 65525, clusters
print("clusters=%d -> FAT16, dataStart=%d" % (clusters, data_start))

img = bytearray(TOTAL*SEC)

# ---- BPB ----
bs = bytearray(SEC)
bs[0:3] = b'\xEB\x3C\x90'
bs[3:11] = b'MSDOS5.0'
struct.pack_into('<H', bs, 11, SEC)      # bytes/sector
bs[13] = SPC                             # sectors/cluster
struct.pack_into('<H', bs, 14, RESV)     # reserved
bs[16] = NFATS                           # num FATs
struct.pack_into('<H', bs, 17, ROOTE)    # root entries
struct.pack_into('<H', bs, 19, 0)        # total16 = 0
bs[21] = 0xF8                            # media
struct.pack_into('<H', bs, 22, FATSZ)    # FAT size 16
struct.pack_into('<H', bs, 24, 32)       # sectors/track
struct.pack_into('<H', bs, 26, 64)       # heads
struct.pack_into('<I', bs, 28, 0)        # hidden
struct.pack_into('<I', bs, 32, TOTAL)    # total32
bs[36] = 0x80                            # drive
bs[38] = 0x29                            # boot sig
struct.pack_into('<I', bs, 39, 0x12345678)
bs[43:54] = b'TESTVOL    '
bs[54:62] = b'FAT16   '
bs[510:512] = b'\x55\xAA'
img[0:SEC] = bs

# ---- 分区表：一个分区，起始 LBA 131（和用户那张卡一样的偏置）----
PART_LBA = 131
mbr = bytearray(SEC)
e = 446
mbr[e+0] = 0x80
mbr[e+4] = 0x06                      # FAT16
struct.pack_into('<I', mbr, e+8,  PART_LBA)
struct.pack_into('<I', mbr, e+12, TOTAL)
mbr[510:512] = b'\x55\xAA'
img[0:SEC] = mbr                     # MBR 在 LBA 0

# 把 BPB 写到分区起始
img[PART_LBA*SEC : (PART_LBA+1)*SEC] = bs

fat_base = (PART_LBA + RESV)*SEC
# ---- 数据：payload 故意不是 512 的整数倍，逼出"尾巴"那条路径 ----
W, H = 70, 71
payload = bytes(((i*7 + 3) & 0xFF) for i in range(W*H))
crc = zlib.crc32(payload) & 0xFFFFFFFF
hdr = b'GRY1' + struct.pack('<HHI', W, H, len(payload)) + struct.pack('<I', crc)
content = hdr + payload
print("file size=%d (hdr 16 + payload %d), crc32=%08X" % (len(content), len(payload), crc))

# ---- 写数据簇（簇 2 起），spc=1 -> 一簇一扇区 ----
nclu = (len(content) + SEC - 1)//SEC
first = 2
for i in range(nclu):
    lba = PART_LBA + data_start + (first + i - 2)*SPC   # ★ data_start 是分区内偏移，要加 PART_LBA
    chunk = content[i*SEC:(i+1)*SEC]
    img[lba*SEC : lba*SEC + len(chunk)] = chunk

# ---- 写 FAT 链 ----
def set_fat16(clu, val):
    off = fat_base + clu*2
    struct.pack_into('<H', img, off, val)
for i in range(nclu):
    set_fat16(first+i, (first+i+1) if i < nclu-1 else 0xFFFF)
set_fat16(0, 0xFFF8)
set_fat16(1, 0xFFFF)
# 第二个 FAT 复制一份
img[fat_base + FATSZ*SEC : fat_base + 2*FATSZ*SEC] = img[fat_base : fat_base + FATSZ*SEC]

# ---- 根目录项：先放一个长文件名项（考验跳过逻辑），再放 IMG.BIN ----
root_lba = PART_LBA + RESV + NFATS*FATSZ
ro = root_lba*SEC
# LFN 项（attr=0x0F），应被跳过
lfn = bytearray(32)
lfn[0] = 0x41; lfn[11] = 0x0F; lfn[12] = 0; lfn[13] = 0x00; lfn[26:28] = b'\x00\x00'
img[ro:ro+32] = lfn
# IMG.BIN
ent = bytearray(32)
ent[0:11] = b'IMG     BIN'
ent[11] = 0x20                       # archive
struct.pack_into('<H', ent, 20, 0)   # 起始簇高
struct.pack_into('<H', ent, 26, first)
struct.pack_into('<I', ent, 28, len(content))
img[ro+32:ro+64] = ent
# 一个卷标项，也应被跳过
vol = bytearray(32)
vol[0:11] = b'TESTVOL    '
vol[11] = 0x08
img[ro+64:ro+96] = vol

open(sys.argv[1], 'wb').write(img)
print("wrote", sys.argv[1], len(img), "bytes")
