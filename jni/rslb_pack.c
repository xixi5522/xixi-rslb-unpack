/*
 * rslb_pack.c - RSLB/LZMA 压缩实现
 *
 * 使用 LZMA SDK 25.01 的 LzmaEncode() 压缩每个块。
 * 参数与原始打包器完全一致, 实现逐字节复现:
 *   lc=3, lp=0, pb=2, fb=64, algo=1, btMode=1, numHashBytes=4,
 *   writeEndMark=0, numThreads=1, dict_size=块大小向上取整1MB
 *
 * 多块分段: 按 block_size (32MiB) 切分, 每块独立压缩。
 */

#include "rslb.h"
#include "sdk/LzmaEnc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ISzAlloc 实现 */
static void *SzAlloc(ISzAllocPtr p, size_t size) {
    (void)p;
    return malloc(size);
}
static void SzFree(ISzAllocPtr p, void *addr) {
    (void)p;
    free(addr);
}
static const ISzAlloc g_Alloc = { SzAlloc, SzFree };
static const ISzAlloc g_BigAlloc = { SzAlloc, SzFree };

/*
 * 压缩单个块。
 * 调用 LzmaEncode, 参数与原始打包器一致。
 *
 * 返回: 压缩后的 payload (不含 5B props), 需调用者 free()
 *      props_out[5]: 输出 5 字节 LZMA props
 *      out_len: 输出 payload 长度
 */
static unsigned char *lzma_encode_block(const unsigned char *data, size_t data_len,
                                         uint32_t dict_size,
                                         unsigned char props_out[5],
                                         size_t *out_len) {
    /* 设置 LZMA 编码参数 */
    CLzmaEncProps props;
    LzmaEncProps_Init(&props);

    props.dictSize = dict_size;
    props.lc = RSLB_LZMA_LC;
    props.lp = RSLB_LZMA_LP;
    props.pb = RSLB_LZMA_PB;
    props.algo = RSLB_LZMA_ALGO;
    props.fb = RSLB_LZMA_FB;
    props.btMode = RSLB_LZMA_BT_MODE;
    props.numHashBytes = RSLB_LZMA_NUM_HASH;
    props.writeEndMark = RSLB_LZMA_END_MARK;
    props.numThreads = RSLB_LZMA_NUM_THREADS;
    props.reduceSize = data_len;

    /* 规范化参数 (SDK 内部可能调整 dictSize 等) */
    LzmaEncProps_Normalize(&props);

    /* 输出缓冲: 最坏情况 = data_len + 一点开销 */
    size_t dest_cap = data_len + (data_len >> 2) + 256;
    unsigned char *dest = (unsigned char *)malloc(dest_cap);
    if (!dest) return NULL;

    /* 编码 */
    SizeT props_size = 5;
    SizeT dest_len = dest_cap;
    SRes sres = LzmaEncode(dest, &dest_len, data, data_len,
                           &props, props_out, &props_size,
                           RSLB_LZMA_END_MARK,   /* writeEndMark */
                           NULL,                  /* progress */
                           &g_Alloc, &g_BigAlloc);

    if (sres != SZ_OK) {
        fprintf(stderr, "错误: LzmaEncode 失败 (sres=%d)\n", sres);
        free(dest);
        return NULL;
    }

    *out_len = (size_t)dest_len;
    return dest;
}

/* 从文件读取全部内容 */
static unsigned char *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "错误: 无法打开 %s\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if (len < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc(len);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, len, f) != (size_t)len) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)len;
    return buf;
}

int rslb_pack(const char *in_path, const char *out_path,
              uint32_t block_size) {
    if (block_size == 0) block_size = RSLB_DEFAULT_BLOCK_SIZE;

    size_t data_size;
    unsigned char *data = read_file(in_path, &data_size);
    if (!data) return 1;

    printf("RSLB 压缩: %s → %s\n", in_path, out_path);
    printf("  输入: %zu 字节, block_size: 0x%X (%u MiB)\n",
           data_size, block_size, block_size / (1 << 20));

    /* 按 block_size 切分 */
    uint32_t block_count = (uint32_t)((data_size + block_size - 1) / block_size);
    if (block_count == 0) block_count = 1; /* 空文件至少1块 */

    printf("  块数: %u\n", block_count);

    /* 逐块压缩 */
    unsigned char **payloads = (unsigned char **)calloc(block_count, sizeof(unsigned char *));
    unsigned char (*props_arr)[5] = malloc(block_count * 5);
    size_t *payload_lens = (size_t *)calloc(block_count, sizeof(size_t));
    uint32_t *chunk_sizes = (uint32_t *)calloc(block_count, sizeof(uint32_t));
    uint32_t *dict_sizes = (uint32_t *)calloc(block_count, sizeof(uint32_t));
    uint64_t *data_offsets = NULL;
    int ret = 0;

    if (!payloads || !props_arr || !payload_lens || !chunk_sizes || !dict_sizes) {
        fprintf(stderr, "错误: 内存不足\n");
        goto cleanup;
    }

    for (uint32_t i = 0; i < block_count; i++) {
        size_t chunk_off = (size_t)i * block_size;
        size_t chunk_len = (i == block_count - 1)
            ? (data_size - chunk_off)
            : block_size;
        chunk_sizes[i] = (uint32_t)chunk_len;

        /* dict_size = chunk_len 向上取整到 1MB */
        dict_sizes[i] = rslb_dict_size_round_up((uint32_t)chunk_len);

        printf("  块[%u]: 未压=%zu dict=0x%X ...", i, chunk_len, dict_sizes[i]);
        fflush(stdout);

        payloads[i] = lzma_encode_block(data + chunk_off, chunk_len,
                                         dict_sizes[i],
                                         props_arr[i],
                                         &payload_lens[i]);
        if (!payloads[i]) {
            ret = 1;
            goto cleanup;
        }

        printf(" 压缩=%zu props=%02X%02X%02X%02X%02X\n",
               payload_lens[i],
               props_arr[i][0], props_arr[i][1], props_arr[i][2],
               props_arr[i][3], props_arr[i][4]);
    }

    /* 计算各块 data_offset */
    uint32_t block_table_size = block_count * RSLB_BLOCK_ENTRY_SIZE;
    uint64_t data_start = RSLB_HEADER_SIZE + block_table_size;
    data_offsets = (uint64_t *)calloc(block_count, sizeof(uint64_t));
    uint64_t cur = data_start;
    for (uint32_t i = 0; i < block_count; i++) {
        data_offsets[i] = cur;
        cur += RSLB_LZMA_PROPS_SIZE + payload_lens[i];
    }

    /* total packed = block_table + all block data */
    uint64_t packed_after_hdr = block_table_size;
    for (uint32_t i = 0; i < block_count; i++)
        packed_after_hdr += RSLB_LZMA_PROPS_SIZE + payload_lens[i];

    /* 写入输出文件 */
    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "错误: 无法创建 %s\n", out_path);
        ret = 1;
        goto cleanup;
    }

    /* 容器头部 (32 字节) */
    unsigned char hdr[RSLB_HEADER_SIZE];
    memset(hdr, 0, sizeof(hdr));
    *(uint32_t *)(hdr + 0x00) = RSLB_MAGIC;
    *(uint32_t *)(hdr + 0x04) = RSLB_VERSION;
    *(uint64_t *)(hdr + 0x08) = (uint64_t)data_size;
    *(uint64_t *)(hdr + 0x10) = packed_after_hdr;
    *(uint32_t *)(hdr + 0x18) = block_size;
    *(uint32_t *)(hdr + 0x1C) = block_count;
    fwrite(hdr, 1, RSLB_HEADER_SIZE, out);

    /* 块表 (每条 36 字节) */
    for (uint32_t i = 0; i < block_count; i++) {
        RslbBlockEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.cum_uncompressed_offset = (uint64_t)i * block_size;
        entry.uncompressed_size = chunk_sizes[i];
        entry.reserved = 0;
        entry.data_offset = data_offsets[i];
        entry.compressed_size = (uint32_t)(RSLB_LZMA_PROPS_SIZE + payload_lens[i]);
        entry.lzma_props_byte = props_arr[i][0];
        /* 从 SDK 输出的 props 中提取真实 dict_size (SDK 可能因 reduceSize 调整) */
        entry.dict_size = (uint32_t)props_arr[i][1]
                        | ((uint32_t)props_arr[i][2] << 8)
                        | ((uint32_t)props_arr[i][3] << 16)
                        | ((uint32_t)props_arr[i][4] << 24);
        /* padding 已经 memset 为 0 */
        fwrite(&entry, 1, sizeof(entry), out);
    }

    /* 块数据区 */
    for (uint32_t i = 0; i < block_count; i++) {
        /* 5B props */
        fwrite(props_arr[i], 1, RSLB_LZMA_PROPS_SIZE, out);
        /* LZMA1 流 */
        fwrite(payloads[i], 1, payload_lens[i], out);
    }

    fclose(out);

    /* 验证输出大小 */
    size_t out_size = RSLB_HEADER_SIZE + packed_after_hdr;
    printf("  输出: %zu 字节 (packed_after_hdr=%llu)\n",
           out_size, (unsigned long long)packed_after_hdr);

cleanup:
    for (uint32_t i = 0; i < block_count; i++)
        free(payloads[i]);
    free(payloads);
    free(props_arr);
    free(payload_lens);
    free(chunk_sizes);
    free(dict_sizes);
    free(data_offsets);
    free(data);
    return ret;
}
