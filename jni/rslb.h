/*
 * rslb.h - RSLB/LZMA 容器格式公共头文件
 *
 * 逆向工程自 libSrc（新）.so (ELF64 AArch64)
 * 目标: 植物大战僵尸2 RSB 资源系统
 *
 * RSLB 文件布局:
 *   0x00  u32  magic = "RSLB" (0x424c5352)
 *   0x04  u32  version = 1
 *   0x08  u64  total_uncompressed_size
 *   0x10  u64  packed_after_header (file_size - 0x20)
 *   0x18  u32  block_size (0x02000000 = 32MiB)
 *   0x1C  u32  block_count
 *   0x20  块表 (block_count × 36 字节)
 *        每条目:
 *          +0x00 u64  cum_uncompressed_offset
 *          +0x08 u32  uncompressed_size
 *          +0x0C u32  reserved (0)
 *          +0x10 u64  data_offset (→本块5B props)
 *          +0x18 u32  compressed_size (含5B props)
 *          +0x1C u8   lzma_props_byte
 *          +0x1D u32  dict_size
 *          +0x21 3B   padding (0)
 *   块数据: 每块 = 5B props + LZMA1 流 (无 end marker)
 *
 * LZMA 参数 (与原始打包器一致):
 *   props_byte = 0x5D (lc=3, lp=0, pb=2)
 *   dict_size  = 块大小向上取整到 1MB
 *   fb(niceLen)= 64
 *   algo       = 1 (BT4 / normal)
 *   btMode     = 1
 *   numHashBytes = 4
 *   writeEndMark = 0 (无 EOS 标记)
 *   numThreads = 1
 */

#ifndef RSLB_H
#define RSLB_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 格式常量 ---- */
#define RSLB_MAGIC            0x424c5352u   /* "RSLB" 小端 */
#define RSLB_VERSION          1
#define RSLB_HEADER_SIZE      0x20          /* 32 字节容器头部 */
#define RSLB_BLOCK_ENTRY_SIZE 0x24          /* 36 字节块表条目 */
#define RSLB_DEFAULT_BLOCK_SIZE 0x02000000u /* 32 MiB */
#define RSLB_LZMA_PROPS_SIZE  5             /* 1B props + 4B dict_size */
#define RSLB_LZMA_PROPS_BYTE  0x5D          /* lc=3, lp=0, pb=2 */

/* ---- LZMA 编码参数 (与原始打包器一致) ---- */
#define RSLB_LZMA_LC          3
#define RSLB_LZMA_LP          0
#define RSLB_LZMA_PB          2
#define RSLB_LZMA_FB          64
#define RSLB_LZMA_ALGO        1
#define RSLB_LZMA_BT_MODE     1
#define RSLB_LZMA_NUM_HASH   4
#define RSLB_LZMA_END_MARK   0
#define RSLB_LZMA_NUM_THREADS 1

/* ---- 块表条目结构 ---- */
#pragma pack(push,1)
typedef struct {
    uint64_t cum_uncompressed_offset;  /* +0x00 */
    uint32_t uncompressed_size;         /* +0x08 */
    uint32_t reserved;                  /* +0x0C */
    uint64_t data_offset;               /* +0x10 */
    uint32_t compressed_size;           /* +0x18 */
    uint8_t  lzma_props_byte;           /* +0x1C */
    uint32_t dict_size;                 /* +0x1D */
    uint8_t  padding[3];                /* +0x21 */
} RslbBlockEntry;                       /* 36 bytes */
#pragma pack(pop)

/* ---- API ---- */

/*
 * 解压 RSLB 包到输出文件
 * 返回: 0=成功, 非0=错误码
 */
int rslb_unpack(const char *in_path, const char *out_path);

/*
 * 压缩数据文件为 RSLB 包
 * block_size: 分段大小 (默认 0x02000000=32MiB, 可传 0 用默认)
 * 返回: 0=成功, 非0=错误码
 */
int rslb_pack(const char *in_path, const char *out_path,
              uint32_t block_size);

/*
 * 查看RSLB包信息 (打印头部和块表)
 * 返回: 0=成功, 非0=错误码
 */
int rslb_inspect(const char *in_path);

/*
 * 计算 LZMA props_byte: (pb*5+lp)*9+lc
 */
static inline uint8_t rslb_props_byte(int lc, int lp, int pb) {
    return (uint8_t)((pb * 5 + lp) * 9 + lc);
}

/*
 * dict_size 向上取整到 1MB
 */
static inline uint32_t rslb_dict_size_round_up(uint32_t raw_size) {
    if (raw_size == 0) return 0x100000; /* 最小 1MB */
    return ((raw_size + 0xFFFFF) & ~0xFFFFFu);
}

#ifdef __cplusplus
}
#endif

#endif /* RSLB_H */
