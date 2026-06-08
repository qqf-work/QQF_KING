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

/* 流控命令 */
#define CAN_PROTO_CMD_UPDATE_READY 0x04   /* A -> Host, 擦除完成，可以开始发数据 */

#endif
