#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
rslb_unpack.py - RSLB/LZMA 解包工具

逆向工程自 libSrc.so (ELF64 AArch64)
使用 Python 标准库 lzma 模块，无任何外部依赖。

用法:
    python3 rslb_unpack.py <输入.rsb.smf> <输出文件>      解压
    python3 rslb_unpack.py info  <输入.rsb.smf>             查看信息

作者: 嘻嘻哈哈嘿嘿
QQ群: 1038054570
"""

import struct
import sys
import lzma

# ---- 格式常量 ----
RSLB_MAGIC            = 0x424C5352   # "RSLB" 小端
RSLB_VERSION          = 1
RSLB_HEADER_SIZE      = 0x20         # 32 字节容器头部
RSLB_BLOCK_ENTRY_SIZE = 0x24         # 36 字节块表条目
RSLB_LZMA_PROPS_SIZE  = 5            # 1B props + 4B dict_size


def props_split(props_byte):
    """将 LZMA 属性字节解码为 (lc, lp, pb)"""
    pb = props_byte // (9 * 5)
    rem = props_byte % (9 * 5)
    lp = rem // 9
    lc = rem % 9
    return lc, lp, pb


def parse_header(blob):
    """
    解析 RSLB 容器头部和块表。
    返回 dict: version, uncompressed_size, block_size,
              block_count, blocks[]
    """
    if len(blob) < RSLB_HEADER_SIZE:
        raise ValueError("文件过短")
    if struct.unpack_from("<I", blob, 0)[0] != RSLB_MAGIC:
        magic = blob[:4]
        raise ValueError("魔数不匹配: 期望 b'RSLB', "
                         "得到 %r" % magic)

    version     = struct.unpack_from("<I", blob, 0x04)[0]
    total_uncomp = struct.unpack_from("<Q", blob, 0x08)[0]
    total_comp   = struct.unpack_from("<Q", blob, 0x10)[0]
    block_size   = struct.unpack_from("<I", blob, 0x18)[0]
    block_count  = struct.unpack_from("<I", blob, 0x1C)[0]

    blocks = []
    for i in range(block_count):
        eoff = RSLB_HEADER_SIZE + i * RSLB_BLOCK_ENTRY_SIZE
        cum_uoff = struct.unpack_from("<Q", blob, eoff)[0]
        u_size   = struct.unpack_from("<I", blob, eoff + 0x08)[0]
        data_off = struct.unpack_from("<Q", blob, eoff + 0x10)[0]
        c_size   = struct.unpack_from("<I", blob, eoff + 0x18)[0]

        # 从 data_offset 读取实际 LZMA props
        props_byte = blob[data_off]
        dict_sz    = struct.unpack_from(
            "<I", blob, data_off + 1)[0]
        lc, lp, pb = props_split(props_byte)

        blocks.append(dict(
            index=i,
            cum_uncompressed_offset=cum_uoff,
            uncompressed_size=u_size,
            data_offset=data_off,
            compressed_size=c_size,
            lzma_props=props_byte,
            dict_size=dict_sz,
            lc=lc, lp=lp, pb=pb,
        ))

    return dict(
        version=version,
        uncompressed_size=total_uncomp,
        total_compressed_size=total_comp,
        block_size=block_size,
        block_count=block_count,
        blocks=blocks,
    )


def decompress_block(blob, block):
    """解压单个 LZMA1 块"""
    data_off  = block["data_offset"]
    comp_size = block["compressed_size"]
    u_size    = block["uncompressed_size"]
    dict_sz   = block["dict_size"]
    lc, lp, pb = block["lc"], block["lp"], block["pb"]

    # 5B props 在 data_offset, 压缩数据在 data_offset+5
    comp_data = blob[data_off + RSLB_LZMA_PROPS_SIZE :
                     data_off + comp_size]

    filt = [{"id": lzma.FILTER_LZMA1,
             "dict_size": dict_sz,
             "lc": lc, "lp": lp, "pb": pb}]
    dec = lzma.LZMADecompressor(
        format=lzma.FORMAT_RAW, filters=filt)
    out = dec.decompress(comp_data, max_length=u_size)
    out = out[:u_size]
    if len(out) != u_size:
        raise ValueError(
            "块 %d 解压不足: 得到 %d, 期望 %d"
            % (block["index"], len(out), u_size))
    return out


def decompress(blob):
    """
    解压 RSLB/.rsb.smf 资源包为原始数据流。
    支持多块 (32 MiB 分段): 逐块独立解压并拼接。
    """
    h = parse_header(blob)
    result = bytearray()
    for blk in h["blocks"]:
        result.extend(decompress_block(blob, blk))
    if len(result) != h["uncompressed_size"]:
        raise ValueError(
            "总解压大小 %d, 期望 %d"
            % (len(result), h["uncompressed_size"]))
    return bytes(result)


def print_info(blob):
    """打印 RSLB 包信息"""
    h = parse_header(blob)
    print("=== RSLB 容器信息 ===")
    print("魔数: RSLB")
    print("版本: %d" % h["version"])
    print("总未压缩大小: %d (%.2f MiB)"
          % (h["uncompressed_size"],
             h["uncompressed_size"] / (1 << 20)))
    print("block_size: 0x%X (%d MiB)"
          % (h["block_size"], h["block_size"] // (1 << 20)))
    print("block_count: %d" % h["block_count"])
    print()
    print("=== 块表 ===")
    for blk in h["blocks"]:
        print("  块[%d]: cum_off=%d uncomp=%d "
              "data_off=0x%X comp=%d "
              "props=0x%02X dict=0x%X "
              "lc=%d lp=%d pb=%d"
              % (blk["index"],
                 blk["cum_uncompressed_offset"],
                 blk["uncompressed_size"],
                 blk["data_offset"],
                 blk["compressed_size"],
                 blk["lzma_props"],
                 blk["dict_size"],
                 blk["lc"], blk["lp"], blk["pb"]))


def main():
    if len(sys.argv) < 2:
        print("用法:")
        print("  python3 rslb_unpack.py "
              "<输入.rsb.smf> <输出文件>  解压")
        print("  python3 rslb_unpack.py info "
              "<输入.rsb.smf>             查看信息")
        sys.exit(1)

    if sys.argv[1] == "info":
        if len(sys.argv) < 3:
            print("用法: rslb_unpack.py info <文件>")
            sys.exit(1)
        blob = open(sys.argv[2], "rb").read()
        print_info(blob)
        return

    if len(sys.argv) < 3:
        print("用法: rslb_unpack.py "
              "<输入.rsb.smf> <输出文件>")
        sys.exit(1)

    in_path  = sys.argv[1]
    out_path = sys.argv[2]
    blob = open(in_path, "rb").read()
    h = parse_header(blob)
    print("RSLB 解压: %s -> %s" % (in_path, out_path))
    print("  总大小: %d, 块数: %d"
          % (h["uncompressed_size"], h["block_count"]))

    out = decompress(blob)
    open(out_path, "wb").write(out)
    print("  解压完成: %d 字节" % len(out))

    # MD5 校验
    import hashlib
    print("  MD5: %s"
          % hashlib.md5(out).hexdigest())


if __name__ == "__main__":
    main()
