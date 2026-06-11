/**
 * @file    app_update.c
 * @brief   LoRa 固件更新状态机实现（网关侧）
 *
 * 本文件实现了网关设备的 OTA 固件分发状态机。网关在内部 Flash 缓存区
 * (0x08004000) 中保存一份完整的固件 .bin 镜像，当远端 App 设备通过
 * LoRa 发送 UPDATE_REQ 请求时，网关将固件按 50 字节/帧逐帧发送，
 * 最后附带 CRC32 校验值供 App 验证完整性。
 *
 * 关键设计决策：
 *   1. CRC32 预计算：收到 REQ 时一次性计算整份固件的 CRC32（而非发送时逐帧累加），
 *      因为 STM32F1 硬件 CRC 计算速度很快（32KB 约 <1ms）。
 *   2. 阻塞式发送：DATA 帧之间使用 HAL_Delay(50ms) 控制间隔，确保 E32 LoRa
 *      模块有足够时间完成空中发送。网关作为专用固件分发设备，主循环可接受此延迟。
 *   3. 简化流控：仅使用 READY 流控（App 擦除完成后通知），DATA 发送期间不做
 *      逐帧应答（LoRa 半双工特性，发送时无法同时接收）。
 */

#include "app_update.h"
#include "lora_proto.h"
#include <stdio.h>
#include <string.h>
#include "crc32.h"

/**
 * @brief  初始化网关 OTA 更新状态机
 *
 * 清零上下文结构体，设置初始状态、LoRa 通信上下文和固件参数。
 *
 * @param  ctx        更新状态机上下文指针（调用者分配，通常为全局/静态变量）
 * @param  lora_ctx   LoRa 收发上下文指针（需已通过 LORA_Buf_Init() 初始化）
 * @param  fw_data    固件数据起始地址（指向 Flash 缓存区 0x08004000，
 *                    STM32 Flash 可直接按字节指针读取）
 * @param  fw_size    固件总大小（字节），0 表示无有效固件
 *
 * 初始化后的状态：
 *   - state = APP_WAIT_UPDATE_CMD（等待 App 发送 REQ）
 *   - fw_offset = 0, fw_seq = 0, fw_crc = 0（等待 REQ 时计算）
 */
void AppUpdate_Init(AppUpdate_t *ctx, LORA_Buf_t *lora_ctx,
                    const uint8_t *fw_data, uint32_t fw_size)
{
    /* 清零整个上下文，所有字段初始化为 0/NULL */
    memset(ctx, 0, sizeof(*ctx));

    /* 设置初始状态：等待远端 App 发送 UPDATE_REQ 命令 */
    ctx->state = APP_WAIT_UPDATE_CMD;

    /* 保存 LoRa 收发上下文，后续所有通信通过此指针操作 */
    ctx->lora_ctx = lora_ctx;

    /* 保存固件数据指针（Flash 缓存区首地址） */
    ctx->fw_data = fw_data;

    /* 保存固件大小：
     * 显式处理 0 值，虽然 memset 已清零，但这里保持防御性编程 */
    ctx->fw_size = (fw_size == 0) ? 0 : fw_size;
}

/**
 * @brief  发送 UPDATE_ACK 帧（静态内部函数）
 *
 * 收到 App 的 UPDATE_REQ 后调用，告知 App 即将传输的固件大小。
 * 载荷为 4 字节小端序的固件大小（fw_size）。
 *
 * @param  ctx  更新状态机上下文指针
 *
 * ACK 帧载荷格式：
 *   [0] = fw_size 的最低字节（bit 0~7）
 *   [1] = fw_size 的次低字节（bit 8~15）
 *   [2] = fw_size 的次高字节（bit 16~23）
 *   [3] = fw_size 的最高字节（bit 24~31）
 *
 * App 收到后根据此值判断：
 *   - 需要擦除多少 W25Q16 扇区
 *   - 需要接收多少个 DATA 帧
 *   - 最终接收总量是否与此值一致
 */
static void send_ack(AppUpdate_t *ctx)
{
    uint8_t payload[4];

    /* 将 fw_size 拆分为 4 字节小端序（低字节在前） */
    payload[0] = (uint8_t)(ctx->fw_size);         /* bit 0~7 */
    payload[1] = (uint8_t)(ctx->fw_size >> 8);    /* bit 8~15 */
    payload[2] = (uint8_t)(ctx->fw_size >> 16);   /* bit 16~23 */
    payload[3] = (uint8_t)(ctx->fw_size >> 24);   /* bit 24~31 */

    /* 通过 LoRa 发送 ACK 帧：命令码 0x81，载荷 4 字节 */
    LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_ACK, payload, 4);

    /* 调试日志：Printf 使用英文，因为 ARM Compiler V5 不支持字符串中的中文 UTF-8 */
    printf("[Host] ACK sent, size=%lu\r\n", ctx->fw_size);
}

/**
 * @brief  发送一帧 UPDATE_DATA（静态内部函数）
 *
 * 从固件数据中读取最多 50 字节（LORA_MAX_DATA_PER_FRAME），
 * 附带当前序号（2 字节小端），组装为 DATA 帧发送。
 *
 * @param  ctx  更新状态机上下文指针
 *
 * DATA 帧载荷格式：
 *   [0] = seq 低字节
 *   [1] = seq 高字节
 *   [2..2+chunk-1] = 固件数据（chunk <= 50 字节）
 *
 * 内部逻辑：
 *   1. 计算剩余未发送字节数 remain = fw_size - fw_offset
 *   2. 取 chunk = min(remain, 50)，确保最后一帧不超过固件末尾
 *   3. 从 fw_data + fw_offset 处拷贝 chunk 字节到发送缓冲区
 *   4. 通过 LoRa 发送，总载荷长度 = 2(seq) + chunk(data)
 *   5. 更新 fw_offset（已发送偏移）和 fw_seq（帧序号）
 *   6. 每 10 帧或发送完成时打印进度日志
 */
static void send_data_frame(AppUpdate_t *ctx)
{
    /* 发送缓冲区：2 字节序号 + 最大 50 字节固件数据 */
    uint8_t buf[2 + LORA_MAX_DATA_PER_FRAME];

    /* 序号编码为 2 字节小端序 */
    buf[0] = (uint8_t)(ctx->fw_seq);         /* seq 低字节 */
    buf[1] = (uint8_t)(ctx->fw_seq >> 8);    /* seq 高字节 */

    /* 计算剩余未发送的固件字节数 */
    uint32_t remain = ctx->fw_size - ctx->fw_offset;

    /* 本帧发送的固件数据量：取剩余量和单帧最大量的较小值
     * （最后一帧可能不足 50 字节） */
    uint8_t chunk = (remain > LORA_MAX_DATA_PER_FRAME)
                  ? LORA_MAX_DATA_PER_FRAME : (uint8_t)remain;

    /* 从 Flash 缓存区拷贝 chunk 字节固件数据到发送缓冲区
     * fw_data 指向 Flash（0x08004000），STM32 Flash 可直接 memcpy 读取 */
    memcpy(&buf[2], &ctx->fw_data[ctx->fw_offset], chunk);

    /* 发送 DATA 帧：命令码 0x02，载荷 = 2 字节序号 + chunk 字节数据 */
    LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_DATA, buf, 2 + chunk);

    /* 更新发送进度 */
    ctx->fw_offset += chunk;  /* 已发送偏移量前移 */
    ctx->fw_seq++;            /* 序号递增 */

    /* 进度日志：每 10 帧或全部发送完毕时打印
     * （避免每帧都打印导致串口输出过多，影响发送节奏） */
    if (ctx->fw_seq % 10 == 0 || ctx->fw_offset >= ctx->fw_size)
        printf("[Host] %lu/%lu\r\n", ctx->fw_offset, ctx->fw_size);
}

/**
 * @brief  发送 UPDATE_END 帧（静态内部函数）
 *
 * 所有 DATA 帧发送完毕后调用，通知 App 固件传输结束，
 * 并附带预计算的 CRC32 校验值供 App 验证。
 *
 * @param  ctx  更新状态机上下文指针
 *
 * END 帧载荷格式：
 *   [0] = CRC32 最低字节（bit 0~7）
 *   [1] = CRC32 次低字节（bit 8~15）
 *   [2] = CRC32 次高字节（bit 16~23）
 *   [3] = CRC32 最高字节（bit 24~31）
 *
 * App 收到 END 后的流程：
 *   1. 回读 W25Q16 全部已写入的固件数据
 *   2. 使用相同的 CRC32 算法计算校验值
 *   3. 与 END 帧携带的 CRC32 比对
 *   4. 匹配：写 EEPROM 标志 -> 发 DONE -> SystemReset
 *   5. 不匹配：发 ERR(CRC_MISMATCH)，放弃本次更新
 */
static void send_end(AppUpdate_t *ctx)
{
    uint8_t payload[4];

    /* 将 CRC32 拆分为 4 字节小端序（与 ACK 中 fw_size 编码方式一致） */
    payload[0] = (uint8_t)(ctx->fw_crc);         /* bit 0~7 */
    payload[1] = (uint8_t)(ctx->fw_crc >> 8);    /* bit 8~15 */
    payload[2] = (uint8_t)(ctx->fw_crc >> 16);   /* bit 16~23 */
    payload[3] = (uint8_t)(ctx->fw_crc >> 24);   /* bit 24~31 */

    /* 发送 END 帧：命令码 0x03，载荷 4 字节 CRC32 */
    LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_END, payload, 4);

    printf("[Host] Send END, crc=0x%08lX\r\n", ctx->fw_crc);
}

/**
 * @brief  APP_WAIT_UPDATE_CMD 状态处理函数
 *
 * 查询 LoRa 是否收到 UPDATE_REQ 命令。如果收到：
 *   1. 预计算整份固件的 CRC32 校验值
 *   2. 发送 UPDATE_ACK（含固件大小）给 App
 *   3. 重置发送偏移和序号
 *   4. 转入 APP_WAIT_READY 状态等待 App 擦除完成
 *
 * @param  ctx  更新状态机上下文指针
 *
 * 状态转换：APP_WAIT_UPDATE_CMD -> APP_WAIT_READY（收到 REQ 时）
 *
 * @note   CRC32 使用 STM32F1 硬件 CRC 外设计算（多项式 0x04C11DB7），
 *         通过 crc32.c/h 中的 CRC32_Calculate() 函数调用。
 *         32KB 固件的 CRC 计算时间 <1ms，不影响响应速度。
 */
void AppUpdate_WaitCmd(AppUpdate_t *ctx)
{
    uint8_t cmd, payload[55], len;

    /* 非阻塞查询：LORA_Buf_Recv 在无新帧时立即返回 0 */
    if (LORA_Buf_Recv(ctx->lora_ctx, &cmd, payload, &len))
    {
        /* 检查是否为 UPDATE_REQ 命令（0x01） */
        if (cmd == LORA_CMD_UPDATE_REQ)
        {
            /* 预计算固件 CRC32：
             * 在发送数据之前计算整份固件的校验值，
             * 这个值将在 END 帧中传给 App 做完整性校验 */
            ctx->fw_crc = CRC32_Calculate(ctx->fw_data, ctx->fw_size);
            printf("[Host] CRC calc: 0x%08lX\r\n", ctx->fw_crc);

            /* 发送 ACK 帧告知 App 固件大小 */
            send_ack(ctx);

            /* 重置发送进度：偏移归零、序号归零 */
            ctx->fw_offset = 0;
            ctx->fw_seq = 0;

            /* 状态转换：进入等待 App 就绪状态 */
            ctx->state = APP_WAIT_READY;

            /* 记录当前滴答计数，用于后续超时检测 */
            ctx->wait_ready_tick = HAL_GetTick();
        }
    }
}

/**
 * @brief  APP_WAIT_READY 状态处理函数
 *
 * 等待 App 擦除 W25Q16 SPI Flash 完成后发送的 UPDATE_READY 命令。
 * 包含 60 秒超时保护，防止 App 异常导致网关永久卡在此状态。
 *
 * @param  ctx  更新状态机上下文指针
 *
 * 状态转换：
 *   - 收到 UPDATE_READY -> APP_UPDATE_SEND（开始发送数据）
 *   - 超时 60 秒 -> APP_WAIT_UPDATE_CMD（放弃本次更新）
 *
 * 超时时间 60 秒的设计考虑：
 *   - W25Q16 4KB 扇区擦除约 100ms，32KB 固件需要擦除 8 个扇区 ≈ 800ms
 *   - 但 App 在擦除前可能还有其他初始化工作，留足余量
 *   - 60 秒足够覆盖最坏情况，同时不会让网关长时间无响应
 */
void AppUpdate_WaitReady(AppUpdate_t *ctx)
{
    /* 超时检查：从进入此状态起计时，超过 60 秒未收到 READY 则放弃 */
    if (HAL_GetTick() - ctx->wait_ready_tick > 60000)
    {
        printf("[Host] READY timeout\r\n");
        /* 超时回到空闲状态，等待下一次更新请求 */
        ctx->state = APP_WAIT_UPDATE_CMD;
        return;
    }

    uint8_t cmd, payload[55], len;

    /* 非阻塞查询 LoRa 接收缓冲区 */
    if (LORA_Buf_Recv(ctx->lora_ctx, &cmd, payload, &len))
    {
        /* 检查是否为 UPDATE_READY 命令（0x04） */
        if (cmd == LORA_CMD_UPDATE_READY)
        {
            /* App 已完成 W25Q16 擦除，可以开始发送固件数据 */
            ctx->state = APP_UPDATE_SEND;
        }
    }
    /* 注意：如果收到的是其他命令（非 READY），在此状态下忽略，继续等待 */
}

/**
 * @brief  APP_UPDATE_SEND 状态处理函数
 *
 * 每次 Poll 调用发送一帧固件数据（DATA 帧），如果所有数据已发送完毕
 * 则发送 END 帧（含 CRC32）并回到空闲状态。
 *
 * @param  ctx  更新状态机上下文指针
 *
 * 状态转换：
 *   - fw_offset < fw_size -> 继续发送 DATA 帧（保持 APP_UPDATE_SEND）
 *   - fw_offset >= fw_size -> 发送 END 帧，回到 APP_WAIT_UPDATE_CMD
 *
 * 发送节奏控制：
 *   - 每发送一帧 DATA 后调用 HAL_Delay(LORA_DATA_FRAME_DELAY=50ms)
 *   - 50ms 间隔匹配 E32-433T20D 在 9.6kbps 空中速率下的实际吞吐能力
 *   - 发送 END 帧后不延时（状态已转换，无后续帧需要节奏控制）
 *
 * @note   此函数包含 HAL_Delay 阻塞调用。在发送期间（APP_UPDATE_SEND 状态），
 *         主循环每轮会有 50ms 延迟。对于网关设备（专做固件分发），这是可接受的。
 *         32KB 固件约需 655 帧 × 50ms ≈ 33 秒发送完毕。
 */
void AppUpdate_Send(AppUpdate_t *ctx)
{
    if (ctx->fw_offset < ctx->fw_size)
    {
        /* 还有未发送的固件数据：发送一帧 DATA */
        send_data_frame(ctx);

        /* 帧间延时：防止 E32 LoRa 模块发送缓冲溢出
         * 这是整个 OTA 流程中最关键的时序参数 */
        HAL_Delay(LORA_DATA_FRAME_DELAY);
    }
    else
    {
        /* 所有固件数据已发送完毕：发送 END 帧（含 CRC32 校验值） */
        send_end(ctx);

        /* 状态回到空闲，等待下一次更新请求
         * （App 收到 END 后会校验 CRC，然后发 DONE 或 ERR） */
        ctx->state = APP_WAIT_UPDATE_CMD;
    }
}

/**
 * @brief  OTA 更新状态机主轮询函数
 *
 * 根据当前状态分发到对应的处理函数。应在 main() 的 while(1) 主循环中
 * 高频调用，由内部状态决定每轮执行的动作。
 *
 * @param  ctx  更新状态机上下文指针
 *              - 不能为 NULL
 *              - lora_ctx 成员不能为 NULL
 *              两者为 NULL 时函数直接返回（安全检查）。
 *
 * 状态分发逻辑：
 *   APP_WAIT_UPDATE_CMD -> AppUpdate_WaitCmd()  查询 REQ 命令
 *   APP_WAIT_READY      -> AppUpdate_WaitReady() 等待 READY 或超时
 *   APP_UPDATE_SEND     -> AppUpdate_Send()      发送一帧 DATA 或 END
 *   其他/非法           -> 回退到 APP_WAIT_UPDATE_CMD（防御性编程）
 *
 * @note   调用频率：
 *         - 在 APP_WAIT_UPDATE_CMD 和 APP_WAIT_READY 状态下，Poll 应尽可能
 *           高频调用（无阻塞），确保及时响应 App 的 REQ/READY 命令。
 *         - 在 APP_UPDATE_SEND 状态下，每次 Poll 会阻塞 50ms（HAL_Delay），
 *           调用频率由帧间隔决定，不影响 LoRa 接收（发送期间不接收）。
 */
void AppUpdate_Poll(AppUpdate_t *ctx)
{
    /* 空指针安全检查：防止传入无效上下文导致硬件异常 */
    if (ctx == NULL || ctx->lora_ctx == NULL)
        return;

    /* 根据当前状态分发到对应的处理函数 */
    switch (ctx->state)
    {
    case APP_WAIT_UPDATE_CMD:
        /* 空闲状态：查询是否收到 App 的更新请求 */
        AppUpdate_WaitCmd(ctx);
        break;

    case APP_WAIT_READY:
        /* 等待就绪状态：查询是否收到 App 的 READY 或检查超时 */
        AppUpdate_WaitReady(ctx);
        break;

    case APP_UPDATE_SEND:
        /* 发送状态：发送一帧固件数据 */
        AppUpdate_Send(ctx);
        break;

    default:
        /* 非法状态（可能是内存损坏导致的状态值异常）：
         * 回退到空闲状态作为恢复手段 */
        ctx->state = APP_WAIT_UPDATE_CMD;
        break;
    }
}
