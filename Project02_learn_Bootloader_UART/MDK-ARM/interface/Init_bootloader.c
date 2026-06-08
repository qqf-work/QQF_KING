#include "Init_bootloader.h"

// 接收缓冲区
uint8_t uart_rec_buff[BOOTLOADER_UART_REC_BUFF_LEN] = {0};
uint16_t uart_rec_len = 0;
uint16_t uart_rec_full_len = 0;
//记录当期写入数据的偏移量
uint16_t uart_rec_offset = 0;
//末尾可能出现的单独字节
uint8_t last_byte_flag = 0;
uint8_t last_byte = 0;

static void Init_flash_erase(void)
{
    /* 计算需要覆盖的地址范围 */
    uint32_t start = APP_START_ADDR + uart_rec_offset;
    uint32_t end   = start + uart_rec_len;
    /* 考虑 last_byte 拼接可能需要额外 1 字节空间 */
    if (last_byte_flag) end += 1;

    /* 计算涉及的页范围（每页 FLASH_PAGE_SIZE 字节） */
    uint32_t first_page = (start - APP_START_ADDR) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE + APP_START_ADDR;
    uint32_t last_page  = ((end - APP_START_ADDR - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE + APP_START_ADDR;

    /* 擦除所有涉及的页 */
    for (uint32_t addr = first_page; addr <= last_page; addr += FLASH_PAGE_SIZE)
    {
        /* 检查该页是否需要擦除（是否含非 0xFF） */
        int need_erase = 0;
        for (uint16_t i = 0; i < FLASH_PAGE_SIZE; i++)
        {
            if (*(volatile uint8_t *)(addr + i) != 0xFF)
            {
                need_erase = 1;
                break;
            }
        }
        if (need_erase)
        {
            FLASH_EraseInitTypeDef erase_init_struct;
            erase_init_struct.TypeErase   = FLASH_TYPEERASE_PAGES;
            erase_init_struct.Banks       = FLASH_BANK_1;
            erase_init_struct.PageAddress = addr;
            erase_init_struct.NbPages     = 1;
            uint32_t page_error = 0;
            HAL_FLASHEx_Erase(&erase_init_struct, &page_error);
        }
    }
}
static void Init_flash_write_with_last(void)
{
    for (uint16_t i = 0; i < uart_rec_len; i += 2)
    {
        uint16_t data16;
        if (i == 0)
        {
            // 拼接上一个字节
            data16 = last_byte | (uart_rec_buff[i] << 8);
        }
        else
        {
            // 获取16位数据
            data16 = uart_rec_buff[i - 1] | (uart_rec_buff[i] << 8);
        }
        HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, APP_START_ADDR + i + uart_rec_offset, data16);
    }
}
static void Init_flash_write_without_last(void)
{
    for (uint16_t i = 0; i < uart_rec_len; i += 2)
    {
        uint16_t data16;
        // 获取16位数据
        if (i + 1 < uart_rec_len)
        {
            data16 = uart_rec_buff[i] | (uart_rec_buff[i + 1] << 8);
            // 16位数据写入flash
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, APP_START_ADDR + i + uart_rec_offset, data16);
        }
    }
}
static void Init_flash_write_halfworf(void)
{
    if ((uart_rec_len + last_byte_flag) % 2 == 0)
    {

        if (last_byte_flag)
        {
            // 上次遗留有一个字节->这次需要作为第一个字节写入
            Init_flash_write_with_last();
            // 记录偏移量
            uart_rec_offset += uart_rec_len + 1;
        }
        else
        {
            // 正好能够写入->不再有遗留字节
            Init_flash_write_without_last();
            // 记录偏移量
            uart_rec_offset += uart_rec_len;
        }
        last_byte_flag = 0;
    }
    // 还会剩余一个字节
    else
    {
        if (last_byte_flag)
        {
            // 上次遗留有一个字节->这次数量为偶数
            Init_flash_write_with_last();
            // 修改最后剩下的字节
            last_byte = uart_rec_buff[uart_rec_len - 1];
            // 记录偏移量
            uart_rec_offset += uart_rec_len;
        }
        else
        {
            // 上次没有遗留字节->这次会留下一个
            Init_flash_write_without_last();
            last_byte = uart_rec_buff[uart_rec_len - 1];
            // 记录偏移量
            uart_rec_offset += uart_rec_len - 1;
        }
        last_byte_flag = 1;
    }
}
/**
 * @brief  串口开启中断之后，触发空闲帧时使用的回调函数
 * @param  huart UART handle
 * @param  Size  Number of data available in application reception buffer (indicates a position in
 *               reception buffer until which, data are available)
 * @retval None
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART1)
    {
        // 保存接收数据的实际长度
        uart_rec_len = Size;
        if (uart_rec_len == 0) return;
        uart_rec_full_len += uart_rec_len;

        /* 地址溢出保护 */
        if (uart_rec_offset + uart_rec_len > APP_END_ADDR - APP_START_ADDR)
            return;

        // TODO:将接收的数据写入flash
        //1.解锁flash
        HAL_FLASH_Unlock();

        //2.判断当前页是否为新的一页->需要擦除
        Init_flash_erase();

        //2.3.使用16位写入数据
        Init_flash_write_halfworf();
        //3.锁定flash
        HAL_FLASH_Lock();

        // 使用完数据后，清空接收缓冲区，准备下一次接收
        memset(uart_rec_buff, 0, BOOTLOADER_UART_REC_BUFF_LEN);
        // 清空初始化串口使用之前的所有问题
        __HAL_UART_CLEAR_OREFLAG(&huart1);
        __HAL_UART_CLEAR_IDLEFLAG(&huart1);
        HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rec_buff, BOOTLOADER_UART_REC_BUFF_LEN);
    }
}
/**
 * @brief  串口通信->准备接收A程序
 */
void Init_bootloader(void)
{
    // 清空初始化串口使用之前的所有问题
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);

    // 带有串口中断的接收方式
    HAL_UARTEx_ReceiveToIdle_IT(&huart1, uart_rec_buff, BOOTLOADER_UART_REC_BUFF_LEN);
}
