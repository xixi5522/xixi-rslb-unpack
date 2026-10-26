/*
 * rslb_unpack.c - RSLB/LZMA 解压实现
 *
 * 使用 LZMA SDK 25.01 的 LzmaDecode() 解码每个块。
 * 支持多块 (32MiB 分段)。
 */

#include "rslb.h"
#include "sdk/LzmaDec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ISzAlloc 实现: 标准 malloc/free */
static void *SzAlloc(ISzAllocPtr p, size_t size) {
    (void)p;
    return malloc(size);
}
static void SzFree(ISzAllocPtr p, void *addr) {
    (void)p;
    free(addr);
}
static const ISzAlloc g_Alloc = { SzAlloc, SzFree };

/* 从文件读取全部内容 */
static unsigned char *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "错误: 无法打开 %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc(len);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)len;
    return buf;
}

int rslb_unpack(const char *in_path, const char *out_path) {
    size_t file_size;
    unsigned char *blob = read_file(in_path, &file_size);
    if (!blob) return 1;

    if (file_size < RSLB_HEADER_SIZE) {
        fprintf(stderr, "错误: 文件过小 (%zu 字节)\n", file_size);
        free(blob);
        return 1;
    }

    /* 解析头部 */
    uint32_t magic = *(uint32_t *)(blob + 0x00);
    if (magic != RSLB_MAGIC) {
        fprintf(stderr, "错误: 魔数不匹配 (期望 RSLB, 得到 0x%08X)\n", magic);
        free(blob);
        return 1;
    }
    uint32_t version = *(uint32_t *)(blob + 0x04);
    uint64_t total_uncomp = *(uint64_t *)(blob + 0x08);
    /* uint64_t packed_after_hdr = *(uint64_t *)(blob + 0x10); */
    uint32_t block_size = *(uint32_t *)(blob + 0x18);
    uint32_t block_count = *(uint32_t *)(blob + 0x1C);

    (void)version; (void)block_size;

    printf("RSLB 解压: %s → %s\n", in_path, out_path);
    printf("  总大小: %llu, 块数: %u\n",
           (unsigned long long)total_uncomp, block_count);

    /* 打开输出文件 */
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "错误: 无法创建 %s\n", out_path);
        free(blob);
        return 1;
    }

    int ret = 0;
    uint64_t total_produced = 0;

    /* 逐块解压 */
    for (uint32_t i = 0; i < block_count; i++) {
        const RslbBlockEntry *entry =
            (const RslbBlockEntry *)(blob + RSLB_HEADER_SIZE + i * RSLB_BLOCK_ENTRY_SIZE);

        uint64_t cum_off  = entry->cum_uncompressed_offset;
        uint32_t u_size   = entry->uncompressed_size;
        uint64_t data_off = entry->data_offset;
        uint32_t c_size   = entry->compressed_size;

        if (cum_off != total_produced) {
            fprintf(stderr, "错误: 块 %u 累计偏移不连续 (%llu != %llu)\n",
                    i, (unsigned long long)cum_off,
                    (unsigned long long)total_produced);
            ret = 1;
            break;
        }

        /* 从 data_offset 读取 5B props */
        if (data_off + c_size > file_size) {
            fprintf(stderr, "错误: 块 %u 数据超出文件范围\n", i);
            ret = 1;
            break;
        }

        const unsigned char *props = blob + data_off;
        const unsigned char *src  = blob + data_off + RSLB_LZMA_PROPS_SIZE;
        size_t src_len = c_size - RSLB_LZMA_PROPS_SIZE;

        /* 分配解压缓冲 */
        unsigned char *dest = (unsigned char *)malloc(u_size);
        if (!dest) {
            fprintf(stderr, "错误: 内存不足 (块 %u, %u 字节)\n", i, u_size);
            ret = 1;
            break;
        }

        /* 调用 LZMA SDK 解压 */
        SizeT dest_len = u_size;
        SizeT src_len_in = src_len;
        ELzmaStatus status;
        SRes sres = LzmaDecode(dest, &dest_len, src, &src_len_in,
                               props, RSLB_LZMA_PROPS_SIZE,
                               LZMA_FINISH_END, &status,
                               &g_Alloc);

        if (sres != SZ_OK || dest_len != u_size) {
            fprintf(stderr, "错误: 块 %u 解压失败 (sres=%d, 产出=%llu, 期望=%u)\n",
                    i, sres, (unsigned long long)dest_len, u_size);
            free(dest);
            ret = 1;
            break;
        }

        /* 写入输出文件 */
        if (fwrite(dest, 1, u_size, out) != u_size) {
            fprintf(stderr, "错误: 写入 %s 失败\n", out_path);
            free(dest);
            ret = 1;
            break;
        }

        free(dest);
        total_produced += u_size;

        printf("  块[%u]: 偏移=%llu 未压=%u 压缩=%u props=0x%02X dict=%u\n",
               i, (unsigned long long)data_off, u_size, c_size,
               entry->lzma_props_byte, entry->dict_size);
    }

    if (ret == 0 && total_produced != total_uncomp) {
        fprintf(stderr, "错误: 总解压大小 %llu != 期望 %llu\n",
                (unsigned long long)total_produced,
                (unsigned long long)total_uncomp);
        ret = 1;
    }

    fclose(out);
    free(blob);

    if (ret == 0) {
        printf("  解压完成: %llu 字节\n", (unsigned long long)total_produced);
    }
    return ret;
}

/*
 * 查看 RSLB 包信息
 */
int rslb_inspect(const char *in_path) {
    size_t file_size;
    unsigned char *blob = read_file(in_path, &file_size);
    if (!blob) return 1;

    if (file_size < RSLB_HEADER_SIZE) {
        fprintf(stderr, "错误: 文件过小\n");
        free(blob);
        return 1;
    }

    uint32_t magic = *(uint32_t *)(blob + 0x00);
    if (magic != RSLB_MAGIC) {
        fprintf(stderr, "错误: 魔数不匹配 (期望 RSLB)\n");
        free(blob);
        return 1;
    }

    uint32_t version    = *(uint32_t *)(blob + 0x04);
    uint64_t total_uncomp = *(uint64_t *)(blob + 0x08);
    uint64_t packed_after = *(uint64_t *)(blob + 0x10);
    uint32_t block_size = *(uint32_t *)(blob + 0x18);
    uint32_t block_count = *(uint32_t *)(blob + 0x1C);

    printf("=== RSLB 容器信息 ===\n");
    printf("文件: %s (%zu 字节)\n", in_path, file_size);
    printf("魔数: RSLB (0x%08X)\n", magic);
    printf("版本: %u\n", version);
    printf("总未压缩大小: %llu (%.2f MiB)\n",
           (unsigned long long)total_uncomp, (double)total_uncomp / (1 << 20));
    printf("packed_after_header: %llu\n", (unsigned long long)packed_after);
    printf("block_size: 0x%X (%u MiB)\n", block_size, block_size / (1 << 20));
    printf("block_count: %u\n", block_count);
    printf("\n");

    printf("=== 块表 ===\n");
    printf("%-6s %-20s %-14s %-14s %-14s %-12s %-10s %-10s\n",
           "块#", "cum_uncomp_off", "uncomp_size", "data_offset",
           "comp_size", "props_byte", "dict_size", "reserved");
    for (uint32_t i = 0; i < block_count && i < 100; i++) {
        const RslbBlockEntry *e =
            (const RslbBlockEntry *)(blob + RSLB_HEADER_SIZE + i * RSLB_BLOCK_ENTRY_SIZE);
        printf("%-6u %-20llu %-14u 0x%-12llX %-14u 0x%02X        0x%-8X 0x%08X\n",
               i,
               (unsigned long long)e->cum_uncompressed_offset,
               e->uncompressed_size,
               (unsigned long long)e->data_offset,
               e->compressed_size,
               e->lzma_props_byte,
               e->dict_size,
               e->reserved);
    }
    if (block_count > 100)
        printf("... (省略 %u 块)\n", block_count - 100);

    free(blob);
    return 0;
}
