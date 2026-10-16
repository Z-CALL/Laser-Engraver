import struct, zlib, sys
SEC=512; RESV=32; NFATS=2; FATSZ=520; SPC=1
TOTAL=66602
data_start = RESV + NFATS*FATSZ
clusters = (TOTAL - data_start)//SPC
assert clusters >= 65525, (clusters, "should be FAT32")
assert FATSZ*SEC//4 >= clusters, "FAT too small"
print("FAT32: clusters=%d dataStart=%d (partition-relative)" % (clusters, data_start))

PART_LBA = 131
img = bytearray(TOTAL*SEC)

bs = bytearray(SEC)
bs[0:3]=b'\xEB\x58\x90'; bs[3:11]=b'MSDOS5.0'
struct.pack_into('<H',bs,11,SEC); bs[13]=SPC
struct.pack_into('<H',bs,14,RESV); bs[16]=NFATS
struct.pack_into('<H',bs,17,0)              # FAT32: root entries = 0
struct.pack_into('<H',bs,19,0)
bs[21]=0xF8
struct.pack_into('<H',bs,22,0)              # FAT size 16 = 0  <- FAT32 标志
struct.pack_into('<I',bs,32,TOTAL)
struct.pack_into('<I',bs,36,FATSZ)          # FAT size 32
struct.pack_into('<I',bs,44,2)              # root cluster = 2
bs[54:62]=b'FAT32   '
bs[510:512]=b'\x55\xAA'
img[PART_LBA*SEC:(PART_LBA+1)*SEC]=bs

# FSInfo 扇区（内容不重要，但真实卡有；我们不用它）
img[(PART_LBA+1)*SEC:(PART_LBA+2)*SEC]=b'\x00'*SEC

mbr=bytearray(SEC); e=446
mbr[e]=0x80; mbr[e+4]=0x0C               # FAT32 LBA
struct.pack_into('<I',mbr,e+8,PART_LBA); struct.pack_into('<I',mbr,e+12,TOTAL)
mbr[510:512]=b'\x55\xAA'; img[0:SEC]=mbr

fat_base=(PART_LBA+RESV)*SEC
W,H=33,17
payload=bytes(((i*11+5)&0xFF) for i in range(W*H))
crc=zlib.crc32(payload)&0xFFFFFFFF
content=b'GRY1'+struct.pack('<HHI',W,H,len(payload))+struct.pack('<I',crc)+payload   # ★ 别漏了 payload
nclu=(len(content)+SEC-1)//SEC
print("file=%d bytes, %d clusters, crc=%08X" % (len(content), nclu, crc))

def setfat32(clu,val):
    struct.pack_into('<I', img, fat_base+clu*4, val & 0x0FFFFFFF)
for i in range(nclu):
    setfat32(2+i, (3+i) if i<nclu-1 else 0x0FFFFFFF)
setfat32(0,0x0FFFFFF8); setfat32(1,0x0FFFFFFF)
img[fat_base+FATSZ*SEC:fat_base+2*FATSZ*SEC]=img[fat_base:fat_base+FATSZ*SEC]

# 数据从簇 2 开始（簇 2 = 根目录！所以数据放簇 3 起）
DATA_CLU = 3
for i in range(nclu):
    lba = PART_LBA + data_start + (DATA_CLU+i-2)*SPC
    ch = content[i*SEC:(i+1)*SEC]
    img[lba*SEC:lba*SEC+len(ch)] = ch
setfat32(2, DATA_CLU)                    # 根目录簇 2 的链：指向数据簇
setfat32(DATA_CLU, DATA_CLU+1 if nclu>1 else 0x0FFFFFFF)
for i in range(1,nclu):
    setfat32(DATA_CLU+i, (DATA_CLU+i+1) if i<nclu-1 else 0x0FFFFFFF)

# 根目录（簇 2）里放目录项：LFN + IMG.BIN
root_lba = PART_LBA + data_start + (2-2)*SPC
ro = root_lba*SEC
lfn=bytearray(32); lfn[0]=0x41; lfn[11]=0x0F; img[ro:ro+32]=lfn
ent=bytearray(32); ent[0:11]=b'IMG     BIN'; ent[11]=0x20
struct.pack_into('<H',ent,20,(DATA_CLU>>16)&0xFFFF)
struct.pack_into('<H',ent,26,DATA_CLU&0xFFFF)
struct.pack_into('<I',ent,28,len(content))
img[ro+32:ro+64]=ent
open(sys.argv[1],'wb').write(img)
print("wrote", sys.argv[1], len(img), "bytes; expect payload %d crc %08X" % (len(payload), crc))
