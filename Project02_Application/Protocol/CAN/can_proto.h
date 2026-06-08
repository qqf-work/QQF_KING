#ifndef __CAN_PROTO_H__
#define __CAN_PROTO_H__

#include <stdint.h>

/* CAN ID */
#define CAN_PROTO_ID_A      0x000   /* A ID */
#define CAN_PROTO_ID_HOST   0x001   /* Host ID */

/* CMD */
#define CAN_PROTO_CMD_UPDATE_REQ   0x01   /* A -> Host */
#define CAN_PROTO_CMD_UPDATE_ACK   0x81   /* Host -> A, payload: size(4B LE) */
#define CAN_PROTO_CMD_UPDATE_DATA  0x02   /* Host -> A, payload: seq(2B LE) + data(<=5B) */
#define CAN_PROTO_CMD_UPDATE_END   0x03   /* Host -> A */
#define CAN_PROTO_CMD_UPDATE_DONE  0x83   /* A -> Host */

#define CAN_PROTO_MAX_DATA_PER_FRAME  5   /* DATA max payload bytes */

/* 错误命令 */
#define CAN_PROTO_CMD_UPDATE_ERR   0x84   /* A -> Host, payload: error_code(1B) */

/* 流控命令 */
#define CAN_PROTO_CMD_UPDATE_READY 0x04   /* A -> Host, 擦除完成，可以开始发数据 */

/* 错误码 */
#define OTA_ERR_SEQ_MISMATCH  0x01  /* 序号不连续 */
#define OTA_ERR_TIMEOUT       0x04  /* 接收超时 */

#endif
