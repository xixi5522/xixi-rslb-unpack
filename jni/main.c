/*
 * main.c - RSLB/LZMA 交互式编解码工具
 * 增加 setbuf(stdout, NULL) 确保输出实时显示
 */

#include "rslb.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_PATH 512

static void print_help(void) {
    printf(
        "RSLB/LZMA 交互式编解码工具\n"
        "支持压缩（原始数据 → RSLB）和解压（RSLB → 原始数据）\n"
        "输出文件自动在原文件名后添加 '~' 字符\n"
        "\n"
        "命令：\n"
        "  <文件路径>           输入文件路径后按回车，然后选择操作\n"
        "  quit / exit          退出程序\n"
        "  help                 显示本帮助\n"
        "\n"
        "示例：\n"
        "  输入文件: dino.rsb.smf\n"
        "  选择操作: 2 (解压)\n"
        "  输出文件: dino.rsb.smf~\n"
    );
    fflush(stdout);
}

int main(void) {
    // 禁用标准输出缓冲，使提示立即显示
    setbuf(stdout, NULL);

    char line[MAX_PATH];
    char inputPath[MAX_PATH];
    char outputPath[MAX_PATH + 2];
    int choice;
    int ret;

    printf("RSLB/LZMA 交互式工具 (输入 help 查看帮助)\n");
    fflush(stdout);

    while (1) {
        printf("\n请输入文件路径 (或 quit 退出): ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("读取输入失败，退出。\n");
            break;
        }

        line[strcspn(line, "\n")] = '\0';  // 去除换行

        // 特殊命令
        if (strcmp(line, "quit") == 0 || strcmp(line, "exit") == 0) {
            printf("再见！\n");
            break;
        }
        if (strcmp(line, "help") == 0 || strcmp(line, "h") == 0 || strcmp(line, "?") == 0) {
            print_help();
            continue;
        }

        // 检查输入是否为空
        if (line[0] == '\0') {
            printf("路径不能为空，请重新输入。\n");
            continue;
        }

        // 复制输入路径
        strncpy(inputPath, line, sizeof(inputPath) - 1);
        inputPath[sizeof(inputPath) - 1] = '\0';

        // 询问操作
        printf("请选择操作: 1=压缩, 2=解压 : ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("读取输入失败。\n");
            break;
        }

        if (sscanf(line, "%d", &choice) != 1) {
            printf("无效输入，请输入 1 或 2。\n");
            continue;
        }

        // 生成输出路径
        snprintf(outputPath, sizeof(outputPath), "%s~", inputPath);

        switch (choice) {
            case 1:
                printf("压缩: %s → %s\n", inputPath, outputPath);
                fflush(stdout);
                ret = rslb_pack(inputPath, outputPath, RSLB_DEFAULT_BLOCK_SIZE);
                break;
            case 2:
                printf("解压: %s → %s\n", inputPath, outputPath);
                fflush(stdout);
                ret = rslb_unpack(inputPath, outputPath);
                break;
            default:
                printf("无效选择，请输入 1 或 2。\n");
                continue;
        }

        if (ret == 0)
            printf("操作成功完成。\n");
        else
            printf("操作失败，返回码 %d。\n", ret);
        fflush(stdout);
    }

    return 0;
}
