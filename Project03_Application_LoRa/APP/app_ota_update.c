/*
 * app_ota_update.c - LoRa OTA 固件更新状态机实现
 *
 * 本文件实现了 App 侧 OTA 更新的核心状态机，是整个无线固件升级流程
 * 的控制中心。通过 LoRa 接收网关推送的固件数据，存储到 W25Q16，
 * 校验 CRC32，写 EEPROM 标志，最后系统复位让 Bootloader 完成烧录。
 *
 * 依赖模块：
 *   - lora_buf.c/h    : LoRa USART3 DMA+IDLE 帧收发
 *   - lora_proto.h    : 协议命令码和错误码定义
 *   - ota_storage.c/h : W25Q16 页缓冲写入 + EEPROM 标志管理
 *   - w25q16.c/h      : W25Q16 SPI Flash 底层驱动
 *   - at24c02.c/h     : AT24C02 EEPROM 底层驱动（通过 ota_storage 间接使用）
 *   - crc32.c/h       : STM32 硬件 CRC32 校验（通过 ota_storage 间接使用）
 */

#include "app_ota_update.h"
#include "lora_buf.h"
#include "lora_proto.h"
#include "ota_storage.h"
#include "w25q16.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>

/*
 * APP_OTA_Init - 初始化 OTA 更新模块
 *
 * 将 OTA 上下文恢复到初始状态，绑定 LoRa 通信和存储驱动。
 * 初始化后状态机处于 IDLE 态，主循环调用 APP_OTA_Process() 后
 * 会自动发送 UPDATE_REQ 发起更新流程。
 *
 * 参数：
 *   ctx      - OTA 上下文指针，调用者分配（静态/全局变量）
 *   lora_ctx - LoRa 收发上下文，必须已通过 LORA_Buf_Init() 初始化
 *              后续通过此指针调用 LORA_Buf_Send/Recv 收发协议帧
 *   w25q     - W25Q16 SPI Flash 驱动句柄，用于存储接收的固件数据
 *              通过 OTA_Storage 层间接使用，App 不直接操作 W25Q16
 *   eeprom   - AT24C02 EEPROM 驱动句柄，用于写入更新触发标志
 *              Bootloader 复位后读取此标志判断是否需要搬运固件
 */
void APP_OTA_Init(APP_OTA_t *ctx, LORA_Buf_t *lora_ctx,
                  W25Q16_t *w25q, AT24C02_t *eeprom)
{
    /* ---- 初始化状态机状态 ---- */
    ctx->state         = OTA_STATE_IDLE;  /* 初始状态：空闲 */
    ctx->lora_ctx      = lora_ctx;        /* 绑定 LoRa 收发上下文 */
    ctx->expect_seq    = 0;               /* 期望序号归零（首帧 seq=0） */
    ctx->state_tick    = 0;               /* 时间戳归零（非退避状态） */
    ctx->error_code    = 0;               /* 无错误 */
    ctx->crc_retry_cnt = 0;               /* CRC 重试计数归零 */

    /*
     * 初始化存储层
     * 绑定 W25Q16 和 EEPROM 驱动句柄，清零内部缓冲区状态。
     * 存储层的具体操作（页缓冲写入、CRC 校验、EEPROM 标志写入）
     * 由 APP_OTA_Process 通过 OTA_Storage_xxx API 间接调用。
     */
    OTA_Storage_Init(&ctx->storage, w25q, eeprom);
}

/*
 * APP_OTA_Process - OTA 状态机主处理函数
 *
 * 每次调用执行一轮状态机处理：
 *   1. 根据当前状态进入对应分支
 *   2. 尝试从 LoRa 接收一帧数据
 *   3. 根据帧内容执行相应操作（解析/存储/校验）
 *   4. 检测超时，必要时转换状态
 *
 * 设计要点：
 *   - 纯轮询模式，不使用 RTOS 或回调，简化调试
 *   - 每个 case 分支处理一个状态，逻辑清晰
 *   - 超时检测在每次调用末尾统一执行
 *
 * 参数：
 *   ctx - OTA 上下文指针，由 APP_OTA_Init() 初始化
 */
void APP_OTA_Process(APP_OTA_t *ctx)
{
    /* 局部变量：用于接收 LoRa 帧解析结果 */
    uint8_t cmd, payload[55], len;

    switch (ctx->state)
    {
    /* ============================================================
     * IDLE 状态：空闲态
     *
     * 两个入口：
     *   a) 初始化后直接进入（state_tick == 0），立即发 REQ
     *   b) ERROR 退避后进入（state_tick != 0），等待退避时间过后再发 REQ
     *
     * 唯一出口：发送 REQ 后转入 WAIT_ACK
     * ============================================================ */
    case OTA_STATE_IDLE:
        /*
         * 退避等待检查
         *
         * state_tick != 0 表示从 ERROR 态转入，需要等待 OTA_ERROR_BACKOFF(5s)。
         * state_tick == 0 表示首次进入或 CRC 重试达上限后重置，直接发 REQ。
         *
         * 退避期间直接 break，不执行后续的 REQ 发送。
         */
        if (ctx->state_tick != 0 &&
            (HAL_GetTick() - ctx->state_tick) < OTA_ERROR_BACKOFF)
        {
            break;
        }

        /*
         * 向网关发送 UPDATE_REQ（无载荷帧）
         *
         * 网关收到后检查 Flash 缓存区是否有固件：
         *   - 有固件：回复 UPDATE_ACK（含 fw_size）
         *   - 无固件：不回复，App 会超时后重发 REQ
         */
        LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_REQ, NULL, 0);

        /* 状态转换：IDLE -> WAIT_ACK，记录时间戳用于超时检测 */
        ctx->state      = OTA_STATE_WAIT_ACK;
        ctx->state_tick = HAL_GetTick();
        break;

    /* ============================================================
     * WAIT_ACK 状态：等待网关回复 UPDATE_ACK
     *
     * 入口：IDLE 态发送 REQ 后
     * 出口：
     *   a) 收到合法 ACK -> 擦除 W25Q16 -> 转 RECV_DATA
     *   b) 超时 5s -> 转 ERROR
     *   c) 收到非 ACK 帧或格式错误 -> 忽略，继续等待
     * ============================================================ */
    case OTA_STATE_WAIT_ACK:
        /* 尝试从 LoRa 接收一帧 */
        if (LORA_Buf_Recv(ctx->lora_ctx, &cmd, payload, &len))
        {
            /* 检查是否是 UPDATE_ACK 且载荷包含 4 字节固件大小 */
            if (cmd == LORA_CMD_UPDATE_ACK && len >= 4)
            {
                /*
                 * 解析固件大小（4 字节小端序）
                 *
                 * 小端序：低字节在前，高字节在后
                 * 例如 0x00008000 (32KB) 存储为 [0x00, 0x80, 0x00, 0x00]
                 */
                uint32_t fw_size = (uint32_t)payload[0]
                                 | ((uint32_t)payload[1] << 8)
                                 | ((uint32_t)payload[2] << 16)
                                 | ((uint32_t)payload[3] << 24);

                /*
                 * fw_size 合法性校验
                 *
                 * - fw_size == 0：网关异常，不应返回空固件
                 * - fw_size > 2MB：超过 W25Q16 容量，无法存储
                 *   W25Q16_TOTAL_SIZE = 2 * 1024 * 1024 = 2097152 字节
                 */
                if (fw_size == 0 || fw_size > W25Q16_TOTAL_SIZE)
                {
                    printf("[OTA] Invalid fw_size: %lu\r\n", fw_size);
                    ctx->error_code = OTA_ERR_FLASH_WRITE;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /*
                 * 擦除 W25Q16 中用于存储固件的扇区
                 *
                 * OTA_Storage_Start() 内部会：
                 *   1. 重置写入偏移和缓冲区状态
                 *   2. 根据 fw_size 计算需要擦除的 4KB 扇区数量
                 *   3. 逐扇区调用 W25Q16_EraseSector()（每扇区约 100ms）
                 * 对于 32KB 固件需要擦除 8 个扇区，耗时约 800ms
                 *
                 * 注意：此处是阻塞操作，但因为 LoRa 版的 READY 流控设计，
                 * 网关不会在 App 擦除期间发数据，所以没有 FIFO 溢出风险
                 */
                int ret = OTA_Storage_Start(&ctx->storage, fw_size);
                if (ret != 0)
                {
                    printf("[OTA] Storage start failed\r\n");
                    ctx->error_code = OTA_ERR_FLASH_WRITE;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /*
                 * 擦除完成，发送 UPDATE_READY 通知网关
                 *
                 * READY 流控的作用：
                 *   类似 CAN 版的流控机制，防止网关在 App 擦除 W25Q16
                 *   （阻塞 ~100ms/扇区）期间发送数据帧导致丢失。
                 *   网关必须等到收到 READY 才开始发 UPDATE_DATA。
                 */
                LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_READY, NULL, 0);

                /* 初始化数据接收所需的计数器 */
                ctx->expect_seq    = 0;      /* 期望首帧 seq = 0 */
                ctx->crc_retry_cnt = 0;      /* 重置 CRC 重试计数 */

                /* 状态转换：WAIT_ACK -> RECV_DATA */
                ctx->state         = OTA_STATE_RECV_DATA;
                ctx->state_tick    = HAL_GetTick();
                break;
            }
            /* 收到非 ACK 帧或长度不足 4 字节：忽略，继续等待 ACK */
        }

        /*
         * WAIT_ACK 超时检测
         *
         * 如果在 OTA_WAIT_ACK_TIMEOUT(5s) 内未收到有效 ACK，
         * 可能原因：网关无固件、LoRa 通信失败、帧损坏。
         * 进入 ERROR 态，退避后重试。
         *
         * 注意：先检查 state 是否仍为 WAIT_ACK（防止上面已转换状态后
         *       又误判超时）
         */
        if (ctx->state == OTA_STATE_WAIT_ACK &&
            (HAL_GetTick() - ctx->state_tick) >= OTA_WAIT_ACK_TIMEOUT)
        {
            printf("[OTA] WAIT_ACK timeout\r\n");
            ctx->error_code = OTA_ERR_TIMEOUT;
            ctx->state      = OTA_STATE_ERROR;
        }
        break;

    /* ============================================================
     * RECV_DATA 状态：接收固件数据帧
     *
     * 入口：WAIT_ACK 收到 ACK 且 W25Q16 已擦除
     * 出口：
     *   a) 收到 END 帧 + CRC 校验通过 -> 发 DONE + SystemReset
     *   b) 收到 END 帧 + CRC 校验失败 -> 转 ERROR（CRC 重试）
     *   c) seq 不匹配 -> 转 ERROR
     *   d) W25Q16 写入失败 -> 转 ERROR
     *   e) 超时 30s -> 转 ERROR
     *
     * 正常处理路径（不包含 printf 或 HAL_Delay）：
     *   DATA 帧 -> 校验 seq -> 写 W25Q16 页缓冲 -> 更新 expect_seq
     *   这确保了即使高频接收也不会阻塞
     * ============================================================ */
    case OTA_STATE_RECV_DATA:
        /* 尝试接收一帧 LoRa 数据 */
        if (LORA_Buf_Recv(ctx->lora_ctx, &cmd, payload, &len))
        {
            /* ---- 处理 UPDATE_DATA 帧 ---- */
            if (cmd == LORA_CMD_UPDATE_DATA && len >= 2)
            {
                /*
                 * 解析帧序号（2 字节小端序）
                 *
                 * DATA 帧载荷结构：[seq_lo][seq_hi][data_0]...[data_N]
                 * 序号从 0 开始，每帧递增 1。
                 * 理论上最大 65535 帧（uint16_t 上限），足够传输 2MB 固件。
                 */
                uint16_t seq = (uint16_t)payload[0]
                             | ((uint16_t)payload[1] << 8);

                /*
                 * 序号连续性校验（丢帧检测）
                 *
                 * 如果收到的 seq != 期望值 expect_seq，说明：
                 *   - LoRa 信道丢帧（最常见原因）
                 *   - 帧序被打乱（极少发生）
                 * 直接报错，不做部分重传（当前协议不支持 NACK 指定 seq 重发）
                 */
                if (seq != ctx->expect_seq)
                {
                    printf("[OTA] seq mismatch: got %u, expected %u\r\n",
                           seq, ctx->expect_seq);
                    ctx->error_code = OTA_ERR_SEQ_MISMATCH;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /*
                 * 提取固件数据并写入 W25Q16
                 *
                 * payload[2..] = 实际固件数据（最多 50 字节/帧）
                 * dlen = len - 2 = 本帧固件数据字节数
                 *
                 * OTA_Storage_Write() 内部使用 256 字节页缓冲：
                 *   - 累积到 256 字节后自动写入 W25Q16 页
                 *   - 不足 256 字节时暂存缓冲区等待后续帧填满
                 *   - 写入 W25Q16 页耗时 0.7~3ms（阻塞）
                 *
                 * 注意：此处不在 printf 或 HAL_Delay，确保接收路径不阻塞
                 */
                uint8_t dlen = len - 2;
                int ret = OTA_Storage_Write(&ctx->storage,
                                            &payload[2], dlen);
                if (ret != 0)
                {
                    printf("[OTA] Storage write failed\r\n");
                    ctx->error_code = OTA_ERR_FLASH_WRITE;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 序号递增，刷新超时计时器（收到有效数据说明通信正常） */
                ctx->expect_seq++;
                ctx->state_tick = HAL_GetTick();
            }
            /* ---- 处理 UPDATE_END 帧 ---- */
            else if (cmd == LORA_CMD_UPDATE_END && len >= 4)
            {
                /*
                 * 解析网关计算的 CRC32（4 字节小端序）
                 *
                 * 这是网关对完整固件二进制预计算的 CRC32 校验值。
                 * App 收到后需要：回读 W25Q16 中已写入的固件数据，
                 * 计算本地 CRC32，与此值比对，验证传输完整性。
                 */
                uint32_t expected_crc = (uint32_t)payload[0]
                                      | ((uint32_t)payload[1] << 8)
                                      | ((uint32_t)payload[2] << 16)
                                      | ((uint32_t)payload[3] << 24);

                /* 将期望 CRC 保存到存储层上下文，供 Finish 时校验使用 */
                OTA_Storage_SetExpectedCRC(&ctx->storage, expected_crc);

                printf("[OTA] END received, total_recv=%lu\r\n",
                       ctx->storage.total_recv);

                /*
                 * OTA_Storage_Finish() 完成三步关键操作：
                 *
                 * 1. 刷新页缓冲：将缓冲区中剩余不足 256 字节的数据
                 *    写入 W25Q16（最后一次页编程）
                 *
                 * 2. CRC32 校验：回读 W25Q16 中已写入的全部固件数据
                 *    （通过 STM32 硬件 CRC 外设计算），与 expected_crc 比对
                 *    32KB 固件回读约需 500ms（SPI 逐字节读取）
                 *
                 * 3. 写 EEPROM 标志：CRC 校验通过后，向 AT24C02 写入
                 *    更新标志（状态=0x01, 密钥=0xA5A5, fw_size, CRC32）
                 *    EEPROM 写入是掉电安全的关键：只有 CRC 校验通过才写
                 *
                 * 返回值：
                 *   0          - 全部成功（缓冲已刷、CRC 校验通过、EEPROM 已写）
                 *   0x05       - 接收量与声明大小不匹配（total_recv != fw_size）
                 *   0x06       - CRC32 不匹配（回读校验失败）
                 *   0x03       - EEPROM 写入失败
                 */
                int ret = OTA_Storage_Finish(&ctx->storage);
                if (ret != 0)
                {
                    printf("[OTA] Storage finish failed: %d\r\n", ret);
                    ctx->error_code = (uint8_t)ret;  /* 直接使用 OTA_Storage_Finish 的返回值作为错误码 */
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /*
                 * ---- 更新成功，准备复位 ----
                 */

                /* 发送 UPDATE_DONE 通知网关更新完成 */
                LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_DONE, NULL, 0);
                printf("[APP] Update complete, resetting\r\n");

                /* 翻转 LED2 指示更新成功（视觉反馈） */
                HAL_GPIO_TogglePin(LED2_GPIO_Port, LED2_Pin);

                /*
                 * 延时 100ms 确保：
                 *   1. LoRa 的 DONE 帧完全发送到空中（58 字节 / 9.6kbps ≈ 48ms）
                 *   2. printf 通过 USART1 调试串口输出完毕（115200 波特率）
                 *
                 * 延时后调用 NVIC_SystemReset() 执行软件复位。
                 * 复位后 Bootloader 启动，读取 EEPROM 标志发现需要更新，
                 * 将 W25Q16 中的固件搬运到内部 Flash A 区，然后跳转到新 App。
                 */
                HAL_Delay(100);
                NVIC_SystemReset();
            }
            /* 收到非 DATA/END 帧或长度不足：忽略，继续等待 */
        }

        /*
         * RECV_DATA 超时检测
         *
         * 如果连续 30 秒未收到新的 DATA 或 END 帧，说明传输中断：
         *   - 网关异常停止发送
         *   - LoRa 信道持续干扰
         *   - 网关掉电
         * 进入 ERROR 态处理。
         *
         * 注意：只有当状态仍为 RECV_DATA 时才检测超时
         * （防止在上面已转入 ERROR 后又误触发）
         */
        if (ctx->state == OTA_STATE_RECV_DATA &&
            (HAL_GetTick() - ctx->state_tick) >= OTA_RECV_DATA_TIMEOUT)
        {
            printf("[OTA] RECV_DATA timeout\r\n");
            ctx->error_code = OTA_ERR_TIMEOUT;
            ctx->state      = OTA_STATE_ERROR;
        }
        break;

    /* ============================================================
     * DONE 状态：预留完成态
     *
     * 当前流程在 RECV_DATA 中收到 END + CRC 校验通过后
     * 直接发送 DONE + HAL_Delay + NVIC_SystemReset()，
     * 不会进入此状态。保留此枚举值是为了未来可能的异步完成流程。
     * ============================================================ */
    case OTA_STATE_DONE:
        /* 当前流程在 RECV_DATA 中直接 SystemReset，不进入此状态 */
        break;

    /* ============================================================
     * ERROR 状态：错误处理
     *
     * 入口：任何状态发生错误时转入
     * 出口：
     *   a) CRC 失败且未达重试上限 -> 退避后回 IDLE（重试）
     *   b) CRC 失败且已达重试上限 -> 回 IDLE（state_tick=0，放弃重试）
     *   c) 非 CRC 错误 -> 退避后回 IDLE（重试）
     *
     * 错误处理策略：
     *   1. 通过 LoRa 发送 UPDATE_ERR 帧通知网关
     *   2. 如果是 CRC 校验失败，累计重试计数器
     *   3. 重置存储层状态（清空页缓冲，不写 EEPROM）
     *   4. 退避 5s 后回到 IDLE 重新发起 REQ
     * ============================================================ */
    case OTA_STATE_ERROR:
        {
            /* 发送 UPDATE_ERR 帧，载荷为 1 字节错误码 */
            LORA_Buf_Send(ctx->lora_ctx, LORA_CMD_UPDATE_ERR,
                          &ctx->error_code, 1);
            printf("[OTA] ERROR: code=0x%02X\r\n", ctx->error_code);

            /*
             * CRC 校验失败的特殊处理
             *
             * CRC 失败意味着 W25Q16 中的固件数据与网关提供的不一致，
             * 最可能的原因是 LoRa 信道干扰导致部分帧数据损坏。
             * 策略是全量重试（重新发 REQ，从头接收全部固件）。
             *
             * 重试限制（OTA_MAX_CRC_RETRY = 3）：
             *   避免在固件本身损坏（网关端问题）时无限重试浪费信道资源。
             *   达到上限后清零计数器，设置 state_tick=0 使 IDLE 态
             *   立即发 REQ（但此时实际是"放弃"语义，因为 IDLE 退避检查
             *   state_tick==0 时直接发 REQ，不过由于 CRC 计数器已清零，
             *   下次 CRC 失败又会重新从 0 计数，所以这里是"重置并继续尝试"）。
             *
             * 注意：当前实现中 CRC 达到上限后的行为是清零重试计数器
             * 并回到 IDLE（state_tick=0），实际上会立即重发 REQ 继续
             * 尝试。如果需要真正"放弃更新"，应在此处设一个标志跳过
             * OTA_Process 调用或进入长期等待态。
             */
            if (ctx->error_code == OTA_ERR_CRC_MISMATCH)
            {
                ctx->crc_retry_cnt++;
                printf("[OTA] CRC fail, retry %d/%d\r\n",
                       ctx->crc_retry_cnt, OTA_MAX_CRC_RETRY);

                if (ctx->crc_retry_cnt >= OTA_MAX_CRC_RETRY)
                {
                    printf("[OTA] CRC retry limit reached, abort\r\n");
                    ctx->crc_retry_cnt = 0;
                    ctx->state         = OTA_STATE_IDLE;
                    ctx->state_tick    = 0;  /* 清零表示不再退避，立即重试 */
                    break;
                }
            }

            /*
             * 重置存储层状态
             *
             * 清空页缓冲区，重置写入偏移和接收计数器。
             * 不写 EEPROM（确保掉电安全：EEPROM 中没有更新标志，
             * 即使意外复位 Bootloader 也不会尝试搬运不完整的固件）。
             */
            OTA_Storage_Reset(&ctx->storage);

            /* 回到 IDLE 态，记录时间戳用于退避等待 */
            ctx->state      = OTA_STATE_IDLE;
            ctx->state_tick = HAL_GetTick();  /* 非零，IDLE 中会等待 5s 退避 */
        }
        break;
    }
}
