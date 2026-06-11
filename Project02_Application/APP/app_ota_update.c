#include "app_ota_update.h"
#include "can_buf.h"
#include "can_proto.h"
#include "ota_storage.h"
#include "stm32f1xx_hal.h"
#include <stdio.h>

void APP_OTA_Init(APP_OTA_t *ctx, CAN_Buf_t *can_ctx,
                  W25Q16_t *w25q, AT24C02_t *eeprom)
{
    ctx->state       = OTA_STATE_IDLE;
    ctx->can_ctx     = can_ctx;
    ctx->expect_seq  = 0;
    ctx->state_tick  = 0;
    ctx->error_code  = 0;
    ctx->crc_retry_cnt = 0;

    OTA_Storage_Init(&ctx->storage, w25q, eeprom);
}

void APP_OTA_Process(APP_OTA_t *ctx)
{
    CAN_RxMsg_t rx_msg[3];
    uint8_t msg_count = 0;

    switch (ctx->state)
    {
    case OTA_STATE_IDLE:
        /* ERROR 退避等待：避免错误后立即重试刷爆总线 */
        if (ctx->state_tick != 0 &&
            (HAL_GetTick() - ctx->state_tick) < OTA_ERROR_BACKOFF)
        {
            break;
        }

        {
            /* 发送更新请求 */
            uint8_t req[1] = { CAN_PROTO_CMD_UPDATE_REQ };
            CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_A, req, 1);

            ctx->state      = OTA_STATE_WAIT_ACK;
            ctx->state_tick = HAL_GetTick();
        }
        break;

    case OTA_STATE_WAIT_ACK:
        CAN_Buf_Recv(ctx->can_ctx, rx_msg, &msg_count);

        for (uint8_t i = 0; i < msg_count; i++)
        {
            if (rx_msg[i].data[0] == CAN_PROTO_CMD_UPDATE_ACK)
            {
                /* 解析固件大小（4B 小端） */
                uint32_t fw_size = (uint32_t)rx_msg[i].data[1]
                                 | ((uint32_t)rx_msg[i].data[2] << 8)
                                 | ((uint32_t)rx_msg[i].data[3] << 16)
                                 | ((uint32_t)rx_msg[i].data[4] << 24);

                /* fw_size 合法性校验 */
                if (fw_size == 0 || fw_size > W25Q16_TOTAL_SIZE)
                {
                    printf("[OTA] Invalid fw_size: %lu\r\n", fw_size);
                    ctx->error_code = OTA_ERR_FLASH_WRITE;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 擦除 W25Q16 扇区 */
                int ret = OTA_Storage_Start(&ctx->storage, fw_size);
                if (ret != 0)
                {
                    printf("[OTA] Storage start failed\r\n");
                    ctx->error_code = OTA_ERR_FLASH_WRITE;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 擦除完成，发 READY 通知网关开始发数据 */
                uint8_t ready[1] = { CAN_PROTO_CMD_UPDATE_READY };
                CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_A, ready, 1);

                ctx->expect_seq = 0;
                ctx->crc_retry_cnt = 0;
                ctx->state      = OTA_STATE_RECV_DATA;
                ctx->state_tick = HAL_GetTick();
                break;
            }
        }

        /* 超时检查 */
        if (ctx->state == OTA_STATE_WAIT_ACK &&
            (HAL_GetTick() - ctx->state_tick) >= OTA_WAIT_ACK_TIMEOUT)
        {
            printf("[OTA] WAIT_ACK timeout\r\n");
            ctx->error_code = OTA_ERR_TIMEOUT;
            ctx->state      = OTA_STATE_ERROR;
        }
        break;

    case OTA_STATE_RECV_DATA:
        CAN_Buf_Recv(ctx->can_ctx, rx_msg, &msg_count);

        for (uint8_t i = 0; i < msg_count; i++)
        {
            uint8_t cmd = rx_msg[i].data[0];

            if (cmd == CAN_PROTO_CMD_UPDATE_DATA)
            {
                /* 校验序号 */
                uint16_t seq = (uint16_t)rx_msg[i].data[1]
                             | ((uint16_t)rx_msg[i].data[2] << 8);

                if (seq != ctx->expect_seq)
                {
                    printf("[OTA] seq mismatch: got %u, expected %u\r\n",
                           seq, ctx->expect_seq);
                    ctx->error_code = OTA_ERR_SEQ_MISMATCH;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* 写入数据（跳过 cmd 1B + seq 2B） */
                uint8_t dlen = rx_msg[i].rxHeader.DLC - 3;
                int ret = OTA_Storage_Write(&ctx->storage,
                                            &rx_msg[i].data[3], dlen);
                if (ret != 0)
                {
                    printf("[OTA] Storage write failed\r\n");
                    ctx->error_code = OTA_ERR_FLASH_WRITE;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                ctx->expect_seq++;
                ctx->state_tick = HAL_GetTick();
            }
            else if (cmd == CAN_PROTO_CMD_UPDATE_END)
            {
                /* Parse CRC32 from END frame (bytes 1-4, LE) */
                uint32_t expected_crc = 0;
                if (rx_msg[i].rxHeader.DLC >= 5)
                {
                    expected_crc = (uint32_t)rx_msg[i].data[1]
                                 | ((uint32_t)rx_msg[i].data[2] << 8)
                                 | ((uint32_t)rx_msg[i].data[3] << 16)
                                 | ((uint32_t)rx_msg[i].data[4] << 24);
                }
                OTA_Storage_SetExpectedCRC(&ctx->storage, expected_crc);

                printf("[OTA] END received, total_recv=%lu\r\n",
                       ctx->storage.total_recv);

                /* Flush buffer + read-back W25Q16 CRC verify + write EEPROM */
                int ret = OTA_Storage_Finish(&ctx->storage);
                if (ret != 0)
                {
                    printf("[OTA] Storage finish failed: %d\r\n", ret);
                    ctx->error_code = (uint8_t)ret;
                    ctx->state      = OTA_STATE_ERROR;
                    break;
                }

                /* Send DONE */
                uint8_t done[1] = { CAN_PROTO_CMD_UPDATE_DONE };
                CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_A, done, 1);
                printf("[APP] Update complete, resetting\r\n");

                /* LED2 toggle */
                HAL_GPIO_TogglePin(LED2_GPIO_Port, LED2_Pin);

                /* Delay for CAN frame + printf, then reset */
                HAL_Delay(100);
                NVIC_SystemReset();
            }
        }

        /* 超时检查（10s 无数据） */
        if (ctx->state == OTA_STATE_RECV_DATA &&
            (HAL_GetTick() - ctx->state_tick) >= 10000)
        {
            printf("[OTA] RECV_DATA timeout\r\n");
            ctx->error_code = OTA_ERR_TIMEOUT;
            ctx->state      = OTA_STATE_ERROR;
        }
        break;

    case OTA_STATE_DONE:
        /* 等待 2s 后重新请求下一轮 */
        if ((HAL_GetTick() - ctx->state_tick) >= OTA_DONE_DELAY)
        {
            ctx->state      = OTA_STATE_IDLE;
            ctx->state_tick = 0;  /* 清零，避免进入 ERROR 退避逻辑 */
        }
        break;

    case OTA_STATE_ERROR:
        {
            /* 发送错误帧 */
            uint8_t err[2] = { CAN_PROTO_CMD_UPDATE_ERR, ctx->error_code };
            CAN_Buf_Send(ctx->can_ctx, CAN_PROTO_ID_A, err, 2);
            printf("[OTA] ERROR: code=0x%02X\r\n", ctx->error_code);

            /* CRC mismatch: accumulate retry counter */
            if (ctx->error_code == OTA_ERR_CRC_MISMATCH)
            {
                ctx->crc_retry_cnt++;
                printf("[OTA] CRC fail, retry %d/%d\r\n",
                       ctx->crc_retry_cnt, OTA_MAX_CRC_RETRY);

                if (ctx->crc_retry_cnt >= OTA_MAX_CRC_RETRY)
                {
                    printf("[OTA] CRC retry limit reached, abort\r\n");
                    /* Give up update, continue running current firmware */
                    ctx->crc_retry_cnt = 0;
                    ctx->state         = OTA_STATE_IDLE;
                    ctx->state_tick    = 0;  /* No backoff */
                    break;
                }
            }

            OTA_Storage_Reset(&ctx->storage);
            ctx->state      = OTA_STATE_IDLE;
            ctx->state_tick = HAL_GetTick();
        }
        break;
    }
}
