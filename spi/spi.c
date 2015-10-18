#include "spi.h"
#include <stddef.h>
#include "utils/utils.h"
#include "dma/dma.h"
#include "defs/defs.h"


//#define SPI_BUS_DEBUG
#ifdef SPI_BUS_DEBUG
#include <stdio.h>
#endif


static uint8_t spi_dummy_data = 0;


#define SPI_STATE_IDLE             0
#define SPI_STATE_READING          1
#define SPI_STATE_WRITING          2
#define SPI_STATE_READING_WRITING  3



ALWAYS_INLINE static bool spi_bus_is_duplex(spi_bus_t* spi)
{
    return (spi->spi_device->CR1 & (SPI_CR1_BIDIMODE | SPI_CR1_RXONLY)) == 0;
}

ALWAYS_INLINE static bool spi_bus_can_rx(spi_bus_t* spi)
{
    return (spi->spi_device->CR1 & (SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE)) != (SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE);
}

ALWAYS_INLINE static bool spi_bus_can_tx(spi_bus_t* spi)
{
    return ((spi->spi_device->CR1 & (SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE)) == (SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE)) ||
           ((spi->spi_device->CR1 & SPI_CR1_RXONLY) == 0);
}

err_t spi_bus_init(spi_bus_t* spi, spi_bus_init_t* init)
{
    if(init == NULL) return E_NULL_POINTER;
    
    spi->spi_device = init->spi_device;
    spi->dma_rx_channel = init->dma_rx_channel;
    spi->dma_tx_channel = init->dma_tx_channel;
    spi->messages = NULL;
    spi->messages_count = 0;
    spi->message_index = 0;
    spi->status = SPI_STATUS_IDLE;
    spi->error = SPI_NO_ERROR;
    spi->transfer_id = SPI_BUS_DEFAULT_TRANSFER_ID;
    spi->callback = NULL;
    
    spi->state = SPI_STATE_IDLE;
    spi->dma_rx_locked = false;
    spi->dma_tx_locked = false;
    
    SPI_I2S_ITConfig(spi->spi_device, SPI_I2S_IT_ERR, ENABLE);
    
    return E_NO_ERROR;
}

ALWAYS_INLINE static void spi_bus_dma_config_shared(DMA_InitTypeDef* dma_is, spi_bus_t* spi, size_t size)
{
    dma_is->DMA_Priority = DMA_Priority_Medium;
    dma_is->DMA_M2M = DMA_M2M_Disable;
    dma_is->DMA_Mode = DMA_Mode_Normal;
    
    dma_is->DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma_is->DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma_is->DMA_PeripheralBaseAddr = (uint32_t)&spi->spi_device->DR;
    
    dma_is->DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    
    dma_is->DMA_BufferSize = size;
}

static void spi_bus_dma_rxtx_config(spi_bus_t* spi, void* rx_address, const void* tx_address, size_t size)
{
    DMA_InitTypeDef dma_is;
    
    // Общие значения.
    spi_bus_dma_config_shared(&dma_is, spi, size);
    
    // RX.
    if(spi->dma_rx_locked){
        dma_is.DMA_DIR = DMA_DIR_PeripheralSRC;
        if(rx_address){
            dma_is.DMA_MemoryInc = DMA_MemoryInc_Enable;
            dma_is.DMA_MemoryBaseAddr = (uint32_t)rx_address;
        }else{
            dma_is.DMA_MemoryInc = DMA_MemoryInc_Disable;
            dma_is.DMA_MemoryBaseAddr = (uint32_t)&spi_dummy_data;
        }

        DMA_DeInit(spi->dma_rx_channel);
        DMA_Init(spi->dma_rx_channel, &dma_is);
        DMA_ITConfig(spi->dma_rx_channel, DMA_IT_TC, ENABLE);
        DMA_ITConfig(spi->dma_rx_channel, DMA_IT_TE, ENABLE);
    }
    
    // TX.
    if(spi->dma_tx_locked){
        dma_is.DMA_DIR = DMA_DIR_PeripheralDST;
        if(tx_address){
            dma_is.DMA_MemoryInc = DMA_MemoryInc_Enable;
            dma_is.DMA_MemoryBaseAddr = (uint32_t)tx_address;
        }else{
            spi_dummy_data = 0;
            dma_is.DMA_MemoryInc = DMA_MemoryInc_Disable;
            dma_is.DMA_MemoryBaseAddr = (uint32_t)&spi_dummy_data;
        }

        DMA_DeInit(spi->dma_tx_channel);
        DMA_Init(spi->dma_tx_channel, &dma_is);
        DMA_ITConfig(spi->dma_tx_channel, DMA_IT_TC, ENABLE);
        DMA_ITConfig(spi->dma_tx_channel, DMA_IT_TE, ENABLE);
    }
}

static void spi_bus_dma_start(spi_bus_t* spi)
{
    if(spi->dma_rx_locked){
        SPI_I2S_DMACmd(spi->spi_device, SPI_I2S_DMAReq_Rx, ENABLE);
        DMA_Cmd(spi->dma_rx_channel, ENABLE);
    }
    if(spi->dma_tx_locked){
        SPI_I2S_DMACmd(spi->spi_device, SPI_I2S_DMAReq_Tx, ENABLE);
        DMA_Cmd(spi->dma_tx_channel, ENABLE);
    }
}


static void spi_bus_dma_stop_rx(spi_bus_t* spi)
{
    if(spi->dma_rx_locked){
        SPI_I2S_DMACmd(spi->spi_device, SPI_I2S_DMAReq_Rx, DISABLE);
        DMA_Cmd(spi->dma_rx_channel, DISABLE);
    }
}

static void spi_bus_dma_stop_tx(spi_bus_t* spi)
{
    if(spi->dma_tx_locked){
        SPI_I2S_DMACmd(spi->spi_device, SPI_I2S_DMAReq_Tx, DISABLE);
        DMA_Cmd(spi->dma_tx_channel, DISABLE);
    }
}

static void spi_bus_dma_stop(spi_bus_t* spi)
{
    spi_bus_dma_stop_rx(spi);
    spi_bus_dma_stop_tx(spi);
}

static bool spi_bus_dma_lock_channels(spi_bus_t* spi, bool lock_rx, bool lock_tx)
{
    if(lock_rx){
        spi->dma_rx_locked = dma_channel_trylock(spi->dma_rx_channel);
        if(!spi->dma_rx_locked) return false;
    }
    if(lock_tx){
        spi->dma_tx_locked = dma_channel_trylock(spi->dma_tx_channel);
        if(!spi->dma_tx_locked){
            if(spi->dma_rx_locked){
                dma_channel_unlock(spi->dma_rx_channel);
                spi->dma_rx_locked = false;
            }
            return false;
        }
    }
    return true;
}

static void spi_bus_dma_unlock_channels(spi_bus_t* spi)
{
    if(spi->dma_rx_locked){
        DMA_DeInit(spi->dma_rx_channel);
        dma_channel_unlock(spi->dma_rx_channel);
        spi->dma_rx_locked = false;
    }
    if(spi->dma_tx_locked){
        DMA_DeInit(spi->dma_tx_channel);
        dma_channel_unlock(spi->dma_tx_channel);
        spi->dma_tx_locked = false;
    }
}

static void spi_bus_setup_message(spi_bus_t* spi)
{
    spi_message_t* msg = &spi->messages[spi->message_index];
    switch(msg->direction){
        case SPI_WRITE:
            spi->state = SPI_STATE_WRITING;
            spi_bus_dma_rxtx_config(spi, NULL, msg->tx_data, msg->data_size);
            break;
        case SPI_READ:
            spi->state = SPI_STATE_READING;
            spi_bus_dma_rxtx_config(spi, msg->rx_data, NULL, msg->data_size);
            break;
        case SPI_READ_WRITE:
            spi->state = SPI_STATE_READING_WRITING;
            spi_bus_dma_rxtx_config(spi, msg->rx_data, msg->tx_data, msg->data_size);
            break;
    }
}

static ALWAYS_INLINE void spi_bus_on_message_sent(spi_bus_t* spi)
{
    if(spi->messages[spi->message_index].callback != NULL){
        spi->messages[spi->message_index].callback(&spi->messages[spi->message_index]);
    }
}

static bool spi_bus_setup_next_message(spi_bus_t* spi)
{
    spi_bus_on_message_sent(spi);
    if(++ spi->message_index >= spi->messages_count) return false;
    spi_bus_setup_message(spi);
    return true;
}

static ALWAYS_INLINE bool spi_bus_done(spi_bus_t* spi)
{
    if(spi->callback) spi->callback();
    return spi->state == SPI_STATE_IDLE;
}

void spi_bus_irq_handler(spi_bus_t* spi)
{
#ifdef SPI_BUS_DEBUG
    printf("[SPI] ERR\r\n");
#endif
    
    spi->state = SPI_STATE_IDLE;
    spi->status = SPI_STATUS_ERROR;
    
    spi_bus_dma_stop(spi);
    
    spi_bus_dma_unlock_channels(spi);
    
    uint16_t SR = spi->spi_device->SR;
    
    if(SR & SPI_SR_OVR || SR & SPI_SR_UDR){

        spi->error = SPI_ERROR_OVERRUN;
        // Clear flag.
        SPI_I2S_ReceiveData(spi->spi_device);
        SPI_I2S_GetITStatus(spi->spi_device, SPI_I2S_IT_OVR);

    }else if(SR & SPI_SR_MODF){

        spi->error = SPI_ERROR_MASTER_MODE_FAULT;
        // Clear flag.
        SPI_Cmd(spi->spi_device, ENABLE);
        // Restore master.
        spi->spi_device->CR1 |= SPI_CR1_MSTR;

    }else if(SR & SPI_SR_CRCERR){

        spi->error = SPI_ERROR_CRC;
        // Clear flag.
        SPI_I2S_ClearITPendingBit(spi->spi_device, SPI_IT_CRCERR);

    }

    spi_bus_done(spi);
}

bool spi_bus_dma_rx_channel_irq_handler(spi_bus_t* spi)
{
    //bool duplex = spi_bus_is_duplex(spi);
    bool can_rx = spi_bus_can_rx(spi);
    //bool can_tx = spi_bus_can_tx(spi);
    
#ifdef SPI_BUS_DEBUG
    printf("[SPI] DMA RX\r\n");
#endif
    
    // Если мы не можем принимать - возврат.
    if(!can_rx || !spi->dma_rx_locked) return false;
        
    uint32_t dma_tc_flag = dma_channel_it_flag(spi->dma_rx_channel, DMA_IT_TC);
    uint32_t dma_te_flag = dma_channel_it_flag(spi->dma_tx_channel, DMA_IT_TE);

    if(DMA_GetITStatus(dma_tc_flag)){

        DMA_ClearITPendingBit(dma_tc_flag);
        
        if(spi_bus_setup_next_message(spi)){
            spi_bus_dma_start(spi);
        }else{
            spi_bus_dma_stop(spi);
            spi_bus_dma_unlock_channels(spi);

            spi->state = SPI_STATE_IDLE;
            spi->status = SPI_STATUS_TRANSFERED;

            spi_bus_done(spi);
        }

    }else if(DMA_GetITStatus(dma_te_flag)){

        DMA_ClearITPendingBit(dma_te_flag);

        spi_bus_dma_stop(spi);

        spi_bus_dma_unlock_channels(spi);

        spi->state = SPI_STATE_IDLE;
        spi->status = SPI_STATUS_ERROR;
        spi->error = SPI_ERROR_DMA;

        spi_bus_done(spi);
    }

    return true;
}

bool spi_bus_dma_tx_channel_irq_handler(spi_bus_t* spi)
{
    //bool duplex = spi_bus_is_duplex(spi);
    bool can_rx = spi_bus_can_rx(spi);
    bool can_tx = spi_bus_can_tx(spi);
    
#ifdef SPI_BUS_DEBUG
    printf("[SPI] DMA TX\r\n");
#endif
    
    // Если мы не можем передавать - возврат.
    if(!can_tx || !spi->dma_tx_locked) return false;
        
    uint32_t dma_tc_flag = dma_channel_it_flag(spi->dma_tx_channel, DMA_IT_TC);
    uint32_t dma_te_flag = dma_channel_it_flag(spi->dma_tx_channel, DMA_IT_TE);

    if(DMA_GetITStatus(dma_tc_flag)){

        DMA_ClearITPendingBit(dma_tc_flag);

        if(!can_rx){            
            if(spi_bus_setup_next_message(spi)){
                spi_bus_dma_start(spi);
            }else{
                spi_bus_dma_stop(spi);
                spi_bus_dma_unlock_channels(spi);

                spi->state = SPI_STATE_IDLE;
                spi->status = SPI_STATUS_TRANSFERED;

                spi_bus_done(spi);
            }
        }

    }else if(DMA_GetITStatus(dma_te_flag)){

        DMA_ClearITPendingBit(dma_te_flag);

        spi_bus_dma_stop(spi);

        spi_bus_dma_unlock_channels(spi);

        spi->state = SPI_STATE_IDLE;
        spi->status = SPI_STATUS_ERROR;
        spi->error = SPI_ERROR_DMA;

        spi_bus_done(spi);
    }
    return true;
}

bool spi_bus_busy(spi_bus_t* spi)
{
    return (SPI_I2S_GetFlagStatus(spi->spi_device, SPI_I2S_FLAG_BSY) != RESET) && spi->state != SPI_STATE_IDLE;
}

void spi_bus_wait(spi_bus_t* spi)
{
    WAIT_WHILE_TRUE(spi_bus_busy(spi));
}

spi_callback_t spi_bus_callback(spi_bus_t* spi)
{
    return spi->callback;
}

void spi_bus_set_callback(spi_bus_t* spi, spi_callback_t callback)
{
    spi->callback = callback;
}

spi_transfer_id_t spi_bus_transfer_id(spi_bus_t* spi)
{
    return spi->transfer_id;
}

bool spi_bus_set_transfer_id(spi_bus_t* spi, spi_transfer_id_t id)
{
    if(spi_bus_busy(spi)) return false;
    spi->transfer_id = id;
    return true;
}

spi_status_t spi_bus_status(spi_bus_t* spi)
{
    return spi->status;
}

spi_error_t spi_bus_error(spi_bus_t* spi)
{
    return spi->error;
}

err_t spi_message_init(spi_message_t* message, spi_direction_t direction, const void* tx_data, void* rx_data, size_t data_size)
{
    if(data_size == 0) return E_INVALID_VALUE;
    
    switch(direction){
        case SPI_READ:
            if(rx_data == NULL) return E_NULL_POINTER;
            if(tx_data != NULL) return E_INVALID_VALUE;
            break;
        case SPI_WRITE:
            if(tx_data == NULL) return E_NULL_POINTER;
            if(rx_data != NULL) return E_INVALID_VALUE;
            break;
        case SPI_READ_WRITE:
            if(tx_data == NULL || rx_data == NULL) return E_NULL_POINTER;
            break;
    }
    
    message->direction = direction;
    message->tx_data = tx_data;
    message->rx_data = rx_data;
    message->data_size = data_size;
    message->callback = NULL;
    message->sender_data = NULL;
    
    return E_NO_ERROR;
}

spi_message_callback_t spi_message_callback(spi_message_t* message)
{
    return message->callback;
}

void spi_message_set_callback(spi_message_t* message, spi_message_callback_t callback)
{
    message->callback = callback;
}

void* spi_message_sender_data(spi_message_t* message)
{
    return message->sender_data;
}

void spi_message_set_sender_data(spi_message_t* message, void* sender_data)
{
    message->sender_data = sender_data;
}

err_t spi_bus_transfer(spi_bus_t* spi, spi_message_t* messages, size_t messages_count)
{
    if(spi_bus_busy(spi)) return E_BUSY;
    if(messages == NULL) return E_NULL_POINTER;
    if(messages_count == 0) return E_INVALID_VALUE;
    
    bool need_rx_channel = false;
    bool need_tx_channel = false;
    
    size_t i = 0;
    
    bool duplex = spi_bus_is_duplex(spi);
    bool can_rx = spi_bus_can_rx(spi);
    bool can_tx = spi_bus_can_tx(spi);
    
    spi_message_t* msg = NULL;
    // Если режим передачи по обоим линиям.
    if(duplex){
        need_rx_channel = can_rx;
        need_tx_channel = can_tx;
    // Иначе.
    }else{
        // Проверим все сообщения.
        for(; i < messages_count; i ++){
            msg = &messages[i];
            switch(msg->direction){
                case SPI_READ:
                    // Принимать данные можно только при BIDIOE == 0.
                    if(!can_rx) return E_SPI_INVALID_MESSAGE;
                    need_rx_channel = true;
                    break;
                case SPI_WRITE:
                    // Передавать данные можно только при BIDIOE == 1.
                    if(!can_tx) return E_SPI_INVALID_MESSAGE;
                    need_tx_channel = true;
                    break;
                case SPI_READ_WRITE:
                    // Передавать и принимать данные одновременно нельзя
                    // в режиме однопроводной шины.
                    return E_SPI_INVALID_MESSAGE;
            }
            if(need_rx_channel && need_tx_channel) break;
        }
    }
    
    if(!spi_bus_dma_lock_channels(spi, need_rx_channel, need_tx_channel)) return E_BUSY;
    
    spi->messages = messages;
    spi->messages_count = messages_count;
    spi->message_index = 0;
    spi->error = E_NO_ERROR;
    spi->status = SPI_STATUS_TRANSFERING;
    
    spi_bus_setup_message(spi);
    
    spi_bus_dma_start(spi);
    
    return E_NO_ERROR;
}
