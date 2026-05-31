#include <string.h>
#include <stdbool.h>
#include "diskio_sd_spi.h"
#include "diskio_impl.h"
#include "driver/spi.h"
#include "esp8266/spi_struct.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"

// Макрос для безопасного перевода миллисекунд в тики.
// Если таймаут меньше 1 тика, округляем до 1 тика,
// чтобы избежать нулевых таймаутов (бесконечного или нулевого ожидания).
#define MS_TO_TICKS_SAFE(ms) ((ms) < (1000 / configTICK_RATE_HZ) ? 1 : pdMS_TO_TICKS(ms))

// Конфигурация из Kconfig
#define SD_MUTEX_TIMEOUT_MS       CONFIG_SD_SPI_MUTEX_TIMEOUT_MS
#define SD_CMD0_RETRIES           CONFIG_SD_SPI_CMD0_RETRIES
#define SD_INIT_TIMEOUT_MS        CONFIG_SD_SPI_INIT_TIMEOUT_MS
#define SD_WAIT_READY_MS          CONFIG_SD_SPI_WAIT_READY_MS
#define SD_READ_TOKEN_TIMEOUT_MS  CONFIG_SD_SPI_READ_TOKEN_TIMEOUT_MS
#define SD_TRIM_TIMEOUT_MS        CONFIG_SD_SPI_TRIM_TIMEOUT_MS
#define SD_CMD42_TIMEOUT_MS       CONFIG_SD_SPI_CMD42_TIMEOUT_MS

#ifdef CONFIG_SD_SPI_CRC16_VERIFY
#define SD_DATA_CRC16_CALC_ON
#endif

static const char *TAG = "sd_spi";

// SD Card Commands
#define CMD0 (0)           /* GO_IDLE_STATE */
#define CMD1 (1)           /* SEND_OP_COND */
#define ACMD41 (0x80 + 41) /* SEND_OP_COND (SDC) */
#define CMD8 (8)           /* SEND_IF_COND */
#define CMD9 (9)           /* SEND_CSD */
#define CMD10 (10)         /* SEND_CID */
#define CMD12 (12)         /* STOP_TRANSMISSION */
#define CMD16 (16)         /* SET_BLOCKLEN */
#define CMD17 (17)         /* READ_SINGLE_BLOCK */
#define CMD18 (18)         /* READ_MULTIPLE_BLOCK */
#define CMD23 (23)         /* SET_BLOCK_COUNT */
#define ACMD23 (0x80 + 23) /* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24 (24)         /* WRITE_BLOCK */
#define CMD25 (25)         /* WRITE_MULTIPLE_BLOCK */
#define CMD32 (32)         /* ERASE_WR_BLK_START */
#define CMD33 (33)         /* ERASE_WR_BLK_END */
#define CMD38 (38)         /* ERASE */
#define CMD55 (55)         /* APP_CMD */
#define CMD58 (58)         /* READ_OCR */
#define CMD59 (59)         /* CRC_ON_OFF */
#define CMD13 (13)         /* SEND_STATUS */
#define CMD42 (42)         /* LOCK_UNLOCK */
#define ACMD13 (0x80 + 13) /* SD_STATUS */

// Card type flags
#define CT_MMC 0x01
#define CT_SD1 0x02
#define CT_SD2 0x04
#define CT_SDC (CT_SD1 | CT_SD2)
#define CT_BLOCK 0x08

// Биты управляющего байта CMD42
#define SD_LOCK_SET_PWD (1 << 0) // Установить новый пароль
#define SD_LOCK_CLR_PWD (1 << 1) // Удалить пароль
#define SD_LOCK_LOCK (1 << 2)    // Заблокировать карту
#define SD_LOCK_ERASE (1 << 3)   // Принудительное стирание заблокированной карты

// ─────────────────────────────────────────────────────────────────
// Архитектура многопоточной модели
// ─────────────────────────────────────────────────────────────────
//
// Два уровня ресурсов, каждый со своим жизненным циклом:
//
//   Уровень 0: SPI шина (s_spi_mutex + s_hw_initialized)
//     sd_spi_bus_init()  → создаёт мьютекс, инициализирует HSPI
//     sd_spi_bus_deinit() → деинициализирует HSPI, удаляет мьютекс
//
//   Уровень 1: Диск (cs_pin + Stat + CardType)
//     sd_spi_diskio_register()   → настраивает CS GPIO, регистрирует в FatFs
//     sd_spi_diskio_unregister() → отвязывает от FatFs, сбрасывает GPIO и состояние
//
// Жизненный цикл:
//   bus_init → diskio_register → [работа] → diskio_unregister → bus_deinit
//
// Контракт: sd_spi_bus_init() вызывается ДО запуска рабочих задач
// (из app_main или из однопоточного стартапа). Поэтому мьютекс создаётся
// просто — нет конкуренции, нет гонки, нет нужды в candidate/SuspendAll.
//
// Правило: мьютекс берётся на внешней границе каждого публичного API.
// Внутренние static-функции (send_cmd, xmit_datablock и т.д.) предполагают,
// что мьютекс уже удерживается вызывающей стороной.
//
// Исключение: ff_sd_spi_status() не берёт мьютекс — чтение volatile BYTE
// атомарно на Xtensa LX106 (ESP8266), а незначительно устаревшее значение
// допустимо для опроса состояния.
//
// FatFs diskio-коллбэки (ff_sd_spi_read/write/ioctl/initialize) берут
// мьютекс самостоятельно. Даже если FF_FS_REENTRANT включён, его мьютекс
// защищает только FatFs внутренние структуры (FAT cache, file objects),
// но НЕ SPI шину. При нескольких дисках на одной шине FatFs использует
// разные мьютексы для разных pdrv — одновременный доступ к шине возможен.
// Поэтому s_spi_mutex — единая точка синхронизации для ВСЕХ SPI операций.
//
// Почему mutex, а не semaphore/event group/queue:
//   - Mutex даёт priority inheritance → нет инверсии приоритетов
//   - Все SPI операции синхронные (polling) → нечего сигнализировать через events
//   - Нет producer/consumer → очереди не нужны
//   - Нет счётных ресурсов → counting semaphore не нужен
// ─────────────────────────────────────────────────────────────────

// Мьютекс для защиты SPI-шины и общего состояния драйвера.
// Создаётся в sd_spi_bus_init(), удаляется в sd_spi_bus_deinit().
static SemaphoreHandle_t s_spi_mutex = NULL;
static bool s_hw_initialized = false;

static volatile DSTATUS Stat = STA_NOINIT;
static BYTE CardType;
static gpio_num_t cs_pin;

// --- CRC Functions ---

// CRC7: Polynomial x^7 + x^3 + 1 (0x09)
// Используется для защиты команд SD-карты
static uint8_t crc7(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x80)
            {
                crc = (crc << 1) ^ 0x12; // 0x12 = 0x09 << 1
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return (crc | 0x01); // Добавляем stop-бит (LSB всегда 1)
}

// CRC16: CCITT Polynomial x^16 + x^12 + x^5 + 1 (0x1021)
// Используется для защиты блоков данных
static uint16_t crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0;
    for (uint16_t i = 0; i < len; i++)
    {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8; j++)
        {
            if (crc & 0x8000)
            {
                crc = (crc << 1) ^ 0x1021;
            }
            else
            {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// --- SPI Low Level Functions ---

// На ESP8266 HSPI при указании длины 8 бит (trans.bits.mosi = 8),
// аппаратный контроллер берет МЛАДШИЕ 8 бит (7:0) из 32-битного слова.
// Сдвигать данные влево на 24 бита НЕЛЬЗЯ, иначе отправятся нули!
static uint8_t spi_rw_byte(uint8_t data)
{
    spi_trans_t trans;
    memset(&trans, 0, sizeof(trans));
    trans.bits.mosi = 8;
    trans.bits.miso = 8;

    uint32_t mosi_data = (uint32_t)data; // БЕЗ СДВИГА!
    uint32_t miso_data = 0;

    trans.mosi = &mosi_data;
    trans.miso = &miso_data;
    spi_trans(HSPI_HOST, &trans);

    return (uint8_t)(miso_data & 0xFF); // БЕЗ СДВИГА!
}

static void cs_high(void)
{
    gpio_set_level(cs_pin, 1);
}

static void cs_low(void)
{
    gpio_set_level(cs_pin, 0);
}

static void spi_set_speed(bool high_speed)
{
    if (high_speed)
    {
        // Скорость из Kconfig
#if CONFIG_SD_SPI_SPEED_20MHZ
        spi_clk_div_t div = SPI_20MHz_DIV;
#elif CONFIG_SD_SPI_SPEED_10MHZ
        spi_clk_div_t div = SPI_10MHz_DIV;
#elif CONFIG_SD_SPI_SPEED_5MHZ
        spi_clk_div_t div = SPI_5MHz_DIV;
#else
        spi_clk_div_t div = SPI_2MHz_DIV;
#endif
        spi_set_clk_div(HSPI_HOST, &div);
    }
    else
    {
        // ~400 кГц для инициализации SD-карты.
        // spi_set_clk_div() в SDK жестко ставит clkdiv_pre = 0, поэтому макс делитель = 64.
        // Пишем в регистры напрямую для делителя 200 (400 кГц).
        CLEAR_PERI_REG_MASK(PERIPHS_IO_MUX_CONF_U, SPI1_CLK_EQU_SYS_CLK);
        SPI1.clock.clk_equ_sysclk = 0;
        SPI1.clock.clkdiv_pre = 9;
        SPI1.clock.clkcnt_n = 19;
        SPI1.clock.clkcnt_h = 9;
        SPI1.clock.clkcnt_l = 19;
    }
}

// --- SD Card Protocol Functions ---

static int wait_ready(UINT wt)
{
    uint8_t d;
    TickType_t t0 = xTaskGetTickCount();
    TickType_t timeout = MS_TO_TICKS_SAFE(wt);
    do
    {
        d = spi_rw_byte(0xFF);
        if (d == 0xFF)
            return 1;

        // Отдаём процессорное время каждые ~1 тик,
        // чтобы не сработал Watchdog на длинных таймаутах.
        if ((xTaskGetTickCount() - t0) >= 1)
        {
            vTaskDelay(1);
        }
    } while ((xTaskGetTickCount() - t0) < timeout);
    return 0;
}

static void deselect(void)
{
    cs_high();
    spi_rw_byte(0xFF);
}

static int select_card(void)
{
    cs_low();
    spi_rw_byte(0xFF);
    if (wait_ready(SD_WAIT_READY_MS))
        return 1;
    deselect();
    return 0;
}

static int rcvr_datablock(BYTE *buff, UINT btr)
{
    uint8_t token;
    TickType_t t0 = xTaskGetTickCount();
    TickType_t timeout = MS_TO_TICKS_SAFE(SD_READ_TOKEN_TIMEOUT_MS);

    do
    {
        token = spi_rw_byte(0xFF);
        if (token == 0xFF && (xTaskGetTickCount() - t0) >= 1)
        {
            vTaskDelay(1);
        }
    } while (token == 0xFF && (xTaskGetTickCount() - t0) < timeout);

    if (token != 0xFE)
        return 0; // Data token not received

    // Читаем блок данных
    for (UINT i = 0; i < btr; i++)
    {
        buff[i] = spi_rw_byte(0xFF);
    }

#ifdef SD_DATA_CRC16_CALC_ON
    // Читаем CRC16 от карты
    uint16_t crc_rcvd = ((uint16_t)spi_rw_byte(0xFF) << 8) | spi_rw_byte(0xFF);

    // Вычисляем наш CRC16
    uint16_t crc_calc = crc16(buff, btr);

    if (crc_rcvd != crc_calc)
    {
        ESP_LOGE(TAG, "Read CRC16 mismatch! Calc: %04X, Rcvd: %04X", crc_calc, crc_rcvd);
        return 0;
    }
#else
    spi_rw_byte(0xFF);
    spi_rw_byte(0xFF);

#endif

    return 1;
}

static int xmit_datablock(const BYTE *buff, BYTE token)
{
    if (!wait_ready(SD_WAIT_READY_MS))
        return 0;

    spi_rw_byte(token);

    if (token != 0xFD)
    {
        if (buff == NULL)
            return 0; // ← защита от случайного вызова
        // Отправляем данные
        for (int i = 0; i < 512; i++)
        {
            spi_rw_byte(buff[i]);
        }

        // Вычисляем и отправляем CRC16
        uint16_t crc = crc16(buff, 512);
        spi_rw_byte((uint8_t)(crc >> 8));
        spi_rw_byte((uint8_t)crc);

        uint8_t resp = spi_rw_byte(0xFF);
        if ((resp & 0x1F) != 0x05)
        {
            ESP_LOGE(TAG, "Write rejected by card, resp=0x%02X", resp);
            return 0;
        }
    }
    else
    {
        // После токена STOP (0xFD) карта уходит в запись —
        // нужно дождаться её готовности перед следующей командой.
        if (!wait_ready(SD_WAIT_READY_MS))
        {
            ESP_LOGE(TAG, "Card busy after STOP token");
            return 0;
        }
    }
    return 1;
}

static BYTE send_cmd(BYTE cmd, DWORD arg)
{
    BYTE n, res;
    uint8_t frame[5];

    if (cmd & 0x80)
    { // ACMD: сначала шлём CMD55
        cmd &= 0x7F;
        res = send_cmd(CMD55, 0);
        if (res > 1)
            return res;
    }

    // Для всех команд кроме CMD0 и CMD12: снимаем выбор и выбираем снова.
    // CMD0 должна отправляться с уже опущенным CS.
    // CMD12 отправляется во время передачи данных, CS дергать нельзя.
    if (cmd != CMD0 && cmd != CMD12)
    {
        deselect();
        if (!select_card())
            return 0xFF;
    }

    // Формируем пакет команды
    frame[0] = 0x40 | cmd;
    frame[1] = (BYTE)(arg >> 24);
    frame[2] = (BYTE)(arg >> 16);
    frame[3] = (BYTE)(arg >> 8);
    frame[4] = (BYTE)arg;

    // Вычисляем CRC7 для команды
    uint8_t crc = crc7(frame, 5);

    // Отправляем команду
    for (int i = 0; i < 5; i++)
    {
        spi_rw_byte(frame[i]);
    }
    spi_rw_byte(crc); // CRC + Stop bit

    if (cmd == CMD12)
        spi_rw_byte(0xFF); // Пропуск stuff-байта для CMD12

    // Ждем ответ (до 10 попыток)
    n = 10;
    do
    {
        res = spi_rw_byte(0xFF);
    } while ((res & 0x80) && --n);

    return res;
}

// --- FatFs DiskIO Interface ---

static DSTATUS ff_sd_spi_initialize(BYTE pdrv)
{
    BYTE n, ty, cmd, ocr[4];
    TickType_t t0;

    if (pdrv != 0)
        return STA_NOINIT;

    // Карта уже инициализирована — возвращаем текущий статус без повторной
    // инициализации. Это важно т.к. f_mount() с mount=1 вызывает
    // disk_initialize() повторно, а полная реинициализация не нужна.
    if (!(Stat & STA_NOINIT))
        return Stat;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        return STA_NOINIT;
    }

    spi_set_speed(false); // Низкая скорость для инициализации (~400 кГц)

    ty = 0;
    // До 10 попыток CMD0 — некоторые SD-карты требуют нескольких попыток
    // для корректного перехода из SD bus в SPI режим (особенно после hot-plug).
    // Каждая попытка: >=74 dummy-тактов (CS high) + NCS такт (CS low) + CMD0.
    {
        BYTE cmd0_retry;
        for (cmd0_retry = SD_CMD0_RETRIES; cmd0_retry; cmd0_retry--)
        {
            cs_high();
            for (n = 10; n; n--)
                spi_rw_byte(0xFF);
            cs_low();
            spi_rw_byte(0xFF); // NCS: минимум 1 такт после опускания CS
            if (send_cmd(CMD0, 0) == 1)
                break;
            deselect();
            vTaskDelay(1);
        }
        if (cmd0_retry == 0)
        {
            ESP_LOGE(TAG, "CMD0 failed after %d attempts", SD_CMD0_RETRIES);
            goto init_fail;
        }
    }
    // deselect после CMD0 перед следующими командами
    deselect();

    t0 = xTaskGetTickCount(); // Таймер для ветки SDv2

    if (send_cmd(CMD8, 0x1AA) == 1)
    { // SDv2
        for (n = 0; n < 4; n++)
            ocr[n] = spi_rw_byte(0xFF);
        if (ocr[2] == 0x01 && ocr[3] == 0xAA)
        {

            bool timed_out = false;
            while (send_cmd(ACMD41, 1UL << 30))
            {
                if ((xTaskGetTickCount() - t0) >= MS_TO_TICKS_SAFE(SD_INIT_TIMEOUT_MS))
                {
                    timed_out = true;
                    break;
                }
                if ((xTaskGetTickCount() - t0) >= 1)
                {
                    vTaskDelay(1);
                }
            }
            if (!timed_out && send_cmd(CMD58, 0) == 0)
            {

                for (n = 0; n < 4; n++)
                    ocr[n] = spi_rw_byte(0xFF);
                ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;
            }
        }
    }
    else
    {
        // Сбрасываем таймер здесь — до этого момента время уже
        // потрачено на попытки CMD8, и старый t0 давал некорректный таймаут.
        t0 = xTaskGetTickCount();

        // Тип карты определяем по первому ответу на ACMD41,
        // затем сразу запускаем цикл ожидания без повторной первой итерации.
        if (send_cmd(ACMD41, 0) <= 1)
        {
            ty = CT_SD1;
            cmd = ACMD41;
        }
        else
        {
            ty = CT_MMC;
            cmd = CMD1;
        }

        // Используем флаг — надёжно разграничивает успех и таймаут.
        bool timed_out = false;
        while (send_cmd(cmd, 0))
        {
            if ((xTaskGetTickCount() - t0) >= MS_TO_TICKS_SAFE(SD_INIT_TIMEOUT_MS))
            {
                timed_out = true;
                break;
            }
            if ((xTaskGetTickCount() - t0) >= 1)
            {
                vTaskDelay(1);
            }
        }

        if (timed_out)
        {
            ty = 0; // Карта не ответила за 1 секунду
        }
    }

    if (ty)
    {

#ifdef SD_DATA_CRC16_CALC_ON
        // Включаем проверку CRC на стороне SD-карты
        if (send_cmd(CMD59, 1) == 0)
        {
            ESP_LOGI(TAG, "CRC verification enabled on SD card");
        }
#endif
        // Устанавливаем размер блока 512 для карт стандартной емкости
        if (!(ty & CT_BLOCK) && send_cmd(CMD16, 512) != 0)
        {
            ty = 0;
        }
    }

    CardType = ty;
    deselect();

    if (ty)
    {
        Stat &= ~STA_NOINIT;
        spi_set_speed(true); // Высокая скорость для работы с данными
        ESP_LOGI(TAG, "SD card initialized, type=0x%02X", ty);
        // Проверяем не заблокирована ли карта паролем (CMD13 → R2 byte, бит 0 = CARD_IS_LOCKED)
        BYTE r1_lock = send_cmd(CMD13, 0);
        BYTE r2_lock = spi_rw_byte(0xFF);
        deselect();
        if (r1_lock <= 1 && (r2_lock & 0x01))
        {
            Stat |= STA_PROTECT; // Заблокированная карта — чтение/запись невозможны
            ESP_LOGW(TAG, "SD card is password-locked! Call sd_spi_unlock() before mounting.");
        }
    }
    else
    {
init_fail:
        Stat = STA_NOINIT;
        ESP_LOGE(TAG, "SD card initialization failed");
    }

    xSemaphoreGive(s_spi_mutex);
    return Stat;
}

static DSTATUS ff_sd_spi_status(BYTE pdrv)
{
    if (pdrv != 0)
        return STA_NOINIT;
    // Мьютекс НЕ берётся: чтение volatile BYTE атомарно на Xtensa LX106.
    // Незначительно устаревшее значение допустимо для опроса состояния.
    // FatFs вызывает disk_status() часто, и накладные расходы на мьютекс
    // здесь неоправданны.
    return Stat;
}

static DRESULT ff_sd_spi_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0 || !count)
        return RES_PARERR;
    if (Stat & STA_NOINIT)
        return RES_NOTRDY;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
        return RES_ERROR;

    if (!(CardType & CT_BLOCK))
        sector *= 512;

    if (count == 1)
    {
        if ((send_cmd(CMD17, sector) == 0) && rcvr_datablock(buff, 512))
        {
            count = 0;
        }
    }
    else
    {
        if (send_cmd(CMD18, sector) == 0)
        {
            do
            {
                if (!rcvr_datablock(buff, 512))
                {
                    count = 1;
                    break;
                }
                buff += 512;
            } while (--count);
            // CMD12 всегда — и при успехе, и при ошибке внутри цикла.
            send_cmd(CMD12, 0);
            wait_ready(SD_READ_TOKEN_TIMEOUT_MS);
        }
    }
    deselect();

    xSemaphoreGive(s_spi_mutex);
    return count ? RES_ERROR : RES_OK;
}

static DRESULT ff_sd_spi_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0 || !count)
        return RES_PARERR;
    if (Stat & STA_NOINIT)
        return RES_NOTRDY;
    if (Stat & STA_PROTECT)
        return RES_WRPRT;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
        return RES_ERROR;

    if (!(CardType & CT_BLOCK))
        sector *= 512;

    if (count == 1)
    {
        if ((send_cmd(CMD24, sector) == 0) && xmit_datablock(buff, 0xFE))
        {
            count = 0;
        }
    }
    else
    {
        if (CardType & CT_SDC)
            send_cmd(ACMD23, count);
        if (send_cmd(CMD25, sector) == 0)
        {
            do
            {
                if (!xmit_datablock(buff, 0xFC))
                {
                    count = 1;
                    break;
                }
                buff += 512;
            } while (--count);
            // STOP всегда — и при успехе, и при ошибке внутри цикла.
            if (!xmit_datablock(NULL, 0xFD))
                count = 1;
        }
    }
    deselect();

    xSemaphoreGive(s_spi_mutex);
    return count ? RES_ERROR : RES_OK;
}

static DRESULT ff_sd_spi_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    DRESULT res = RES_ERROR;
    BYTE n, csd[16];
    DWORD *dp, st, ed, csize;

    if (pdrv != 0)
        return RES_PARERR;
    if (Stat & STA_NOINIT)
        return RES_NOTRDY;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
        return RES_ERROR;

    switch (cmd)
    {
    case CTRL_SYNC:
        if (select_card())
            res = RES_OK;
        break;

    case GET_SECTOR_COUNT:
        if (buff == NULL)
        {
            res = RES_PARERR;
            break;
        }
        if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16))
        {
            if ((csd[0] >> 6) == 1)
            { // SDC ver 2.00
                csize = csd[9] + ((WORD)csd[8] << 8) +
                        ((DWORD)(csd[7] & 63) << 16) + 1;
                *(DWORD *)buff = csize << 10;
            }
            else
            { // SDC ver 1.XX or MMC
                n = (csd[5] & 15) + ((csd[10] & 128) >> 7) +
                    ((csd[9] & 3) << 1) + 2;
                csize = (csd[8] >> 6) + ((WORD)csd[7] << 2) +
                        ((WORD)(csd[6] & 3) << 10) + 1;
                if (n < 9)
                {
                    ESP_LOGE(TAG, "Invalid CSD: block_len=%d (min=9)", n);
                    break;
                }
                *(DWORD *)buff = csize << (n - 9);
            }
            res = RES_OK;
        }
        break;

    case GET_SECTOR_SIZE:
        if (buff == NULL)
        {
            res = RES_PARERR;
            break;
        }
        *(WORD *)buff = 512;
        res = RES_OK;
        break;

    case GET_BLOCK_SIZE:
        if (buff == NULL)
        {
            res = RES_PARERR;
            break;
        }
        if (CardType & CT_SD2)
        {
            BYTE sdst[64]; // Buffer for 64 bytes SD Status
            if (send_cmd(ACMD13, 0) == 0)
            { // ACMD13
                spi_rw_byte(0xFF);
                if (rcvr_datablock(sdst, 64))
                {
                    *(DWORD *)buff = 16UL << (sdst[10] >> 4);
                    res = RES_OK;
                }
            }
        }
        else
        {
            if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16))
            {
                if (CardType & CT_SD1)
                {
                    *(DWORD *)buff = (((csd[10] & 63) << 1) +
                                      ((WORD)(csd[11] & 128) >> 7) + 1)
                                     << ((csd[13] >> 6) - 1);
                }
                else
                {
                    *(DWORD *)buff = ((WORD)((csd[10] & 124) >> 2) + 1) *
                                     (((csd[11] & 3) << 3) +
                                      ((csd[11] & 224) >> 5) + 1);
                }
                res = RES_OK;
            }
        }
        break;

    case CTRL_TRIM:
        if (buff == NULL)
        {
            res = RES_PARERR;
            break;
        }
        if (!(CardType & CT_SDC))
        {
            res = RES_PARERR; // MMC использует другие команды, только SD
            break;
        }
        dp = (DWORD *)buff;
        st = dp[0];
        ed = dp[1];
        if (!(CardType & CT_BLOCK))
        {
            st *= 512;
            ed *= 512;
        }
        if (send_cmd(CMD32, st) == 0 &&
            send_cmd(CMD33, ed) == 0 &&
            send_cmd(CMD38, 0) == 0)
        {
            if (wait_ready(SD_TRIM_TIMEOUT_MS))
            {
                res = RES_OK;
            }
            else
            {
                ESP_LOGE(TAG, "CTRL_TRIM timeout");
            }
        }
        break;

    default:
        res = RES_PARERR;
        break;
    }
    deselect();

    xSemaphoreGive(s_spi_mutex);
    return res;
}

// --- Registration ---

static const ff_diskio_impl_t sd_spi_impl = {
    .init = &ff_sd_spi_initialize,
    .status = &ff_sd_spi_status,
    .read = &ff_sd_spi_read,
    .write = &ff_sd_spi_write,
    .ioctl = &ff_sd_spi_ioctl};

// ─────────────────────────────────────────────
// Внутренний обработчик CMD42 (LOCK/UNLOCK)
// ВЫЗЫВАЮЩИЙ ДОЛЖЕН ДЕРЖАТЬ s_spi_mutex!
// ─────────────────────────────────────────────
static esp_err_t sd_do_lock_unlock_locked(uint8_t control,
                                           const uint8_t *pwd1, uint8_t len1,
                                           const uint8_t *pwd2, uint8_t len2)
{
    if (Stat & STA_NOINIT)
    {
        ESP_LOGE(TAG, "CMD42: card not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (len1 > SD_PWD_MAX_LEN || len2 > SD_PWD_MAX_LEN)
        return ESP_ERR_INVALID_ARG;

    // CMD42 требует блок ровно 512 байт, остаток заполняем нулями
    uint8_t *block = calloc(512, 1);
    if (!block)
        return ESP_ERR_NO_MEM;

    block[0] = control;
    block[1] = (uint8_t)(len1 + len2);   // PWD_LEN по спецификации SD

    uint8_t *p = block + 2;
    if (len1 && pwd1) { memcpy(p, pwd1, len1); p += len1; }
    if (len2 && pwd2) { memcpy(p, pwd2, len2); }

    esp_err_t ret = ESP_FAIL;

    // Мьютекс НЕ берём — вызывающий уже его держит

    // Отправляем CMD42 (send_cmd сам делает deselect+select внутри)
    BYTE res = send_cmd(CMD42, 0);
    if (res != 0x00)
    {
        ESP_LOGE(TAG, "CMD42 rejected, R1=0x%02X", res);
        deselect();
        goto done;
    }

    // Отправляем 512-байтный блок данных
    if (!xmit_datablock(block, 0xFE))
    {
        ESP_LOGE(TAG, "CMD42 data block send failed");
        deselect();
        goto done;
    }

    // Ждём пока карта обработает операцию (до N секунд — требование спецификации)
    if (!wait_ready(SD_CMD42_TIMEOUT_MS))
        ESP_LOGW(TAG, "CMD42: card busy timeout");

    deselect();

    // Проверяем результат через CMD13 → R2
    // Бит 1 R2 = WP_ERASE_SKIP / LOCK_UNLOCK_FAILED (неверный пароль или ошибка операции)
    BYTE r1 = send_cmd(CMD13, 0);
    BYTE r2 = spi_rw_byte(0xFF);
    deselect();

    if (r2 & 0x02)
    {
        ESP_LOGE(TAG, "CMD42 operation failed (wrong password?), R2=0x%02X", r2);
        ret = ESP_ERR_INVALID_ARG;
    }
    else if (r1 <= 1)
    {
        ret = ESP_OK;
    }

done:
    // Мьютекс НЕ отпускаем — вызывающий сам отпустит после модификации Stat
    free(block);
    return ret;
}

// ─────────────────────────────────────────────
// Публичный API управления паролем
// Все функции берут s_spi_mutex сами и модифицируют
// Stat внутри захваченного мьютекса.
// ─────────────────────────────────────────────

bool sd_spi_is_locked(BYTE pdrv)
{
    if (pdrv != 0 || (Stat & STA_NOINIT))
        return false;
    if (s_spi_mutex == NULL)
        return false;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
        return false;

    BYTE r1 = send_cmd(CMD13, 0);
    BYTE r2 = spi_rw_byte(0xFF);
    deselect();

    xSemaphoreGive(s_spi_mutex);
    return (r1 <= 1) && ((r2 & 0x01) != 0);
}

esp_err_t sd_spi_unlock(BYTE pdrv, const uint8_t *password, uint8_t pwd_len)
{
    if (pdrv != 0 || !password || pwd_len == 0 || pwd_len > SD_PWD_MAX_LEN)
        return ESP_ERR_INVALID_ARG;
    if (s_spi_mutex == NULL)
        return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "Unlocking SD card...");
    esp_err_t ret = sd_do_lock_unlock_locked(0x00, password, pwd_len, NULL, 0);
    if (ret == ESP_OK)
    {
        Stat &= ~STA_PROTECT; // Под мьютексом — безопасно
        ESP_LOGI(TAG, "SD card unlocked successfully");
    }
    else
    {
        ESP_LOGE(TAG, "SD card unlock failed (wrong password?)");
    }

    xSemaphoreGive(s_spi_mutex);
    return ret;
}

esp_err_t sd_spi_lock(BYTE pdrv, const uint8_t *password, uint8_t pwd_len)
{
    if (pdrv != 0 || !password || pwd_len == 0 || pwd_len > SD_PWD_MAX_LEN)
        return ESP_ERR_INVALID_ARG;
    if (s_spi_mutex == NULL)
        return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "Locking SD card...");
    esp_err_t ret = sd_do_lock_unlock_locked(SD_LOCK_LOCK, password, pwd_len, NULL, 0);
    if (ret == ESP_OK)
    {
        Stat |= STA_PROTECT; // Под мьютексом — безопасно
        ESP_LOGI(TAG, "SD card locked successfully");
    }

    xSemaphoreGive(s_spi_mutex);
    return ret;
}

esp_err_t sd_spi_set_password(BYTE pdrv,
                               const uint8_t *old_pwd, uint8_t old_len,
                               const uint8_t *new_pwd, uint8_t new_len)
{
    if (pdrv != 0 || !new_pwd || new_len == 0 || new_len > SD_PWD_MAX_LEN)
        return ESP_ERR_INVALID_ARG;
    if (old_len > SD_PWD_MAX_LEN)
        return ESP_ERR_INVALID_ARG;
    if (s_spi_mutex == NULL)
        return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "Setting SD card password...");
    esp_err_t ret = sd_do_lock_unlock_locked(SD_LOCK_SET_PWD, old_pwd, old_len, new_pwd, new_len);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "SD card password set successfully");

    xSemaphoreGive(s_spi_mutex);
    return ret;
}

esp_err_t sd_spi_clear_password(BYTE pdrv, const uint8_t *password, uint8_t pwd_len)
{
    if (pdrv != 0 || !password || pwd_len == 0 || pwd_len > SD_PWD_MAX_LEN)
        return ESP_ERR_INVALID_ARG;
    if (s_spi_mutex == NULL)
        return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "Clearing SD card password...");
    esp_err_t ret = sd_do_lock_unlock_locked(SD_LOCK_CLR_PWD, password, pwd_len, NULL, 0);
    if (ret == ESP_OK)
        ESP_LOGI(TAG, "SD card password cleared successfully");

    xSemaphoreGive(s_spi_mutex);
    return ret;
}

esp_err_t sd_spi_force_erase(BYTE pdrv)
{
    if (pdrv != 0)
        return ESP_ERR_INVALID_ARG;
    if (s_spi_mutex == NULL)
        return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    // Только для заблокированных карт, пароль от которых утерян.
    // Все данные будут уничтожены безвозвратно!
    ESP_LOGW(TAG, "*** FORCE ERASE: ALL DATA ON SD CARD WILL BE LOST! ***");
    esp_err_t ret = sd_do_lock_unlock_locked(SD_LOCK_ERASE, NULL, 0, NULL, 0);
    if (ret == ESP_OK)
    {
        Stat &= ~STA_PROTECT; // Под мьютексом — безопасно
        ESP_LOGI(TAG, "SD card force erase complete. Card is now unlocked.");
    }

    xSemaphoreGive(s_spi_mutex);
    return ret;
}

// ─────────────────────────────────────────────────────────────────
// Уровень 0: SPI шина
// ─────────────────────────────────────────────────────────────────

esp_err_t sd_spi_bus_init(void)
{
    // Шина уже инициализирована — идемпотентно возвращаем OK.
    if (s_spi_mutex != NULL)
        return ESP_OK;

    // Контракт: sd_spi_bus_init() вызывается из app_main() ДО запуска
    // рабочих задач. Нет конкуренции — нет гонки на создании мьютекса.
    // Просто создаём мьютекс, без candidate/SuspendAll/танцев.
    //
    // Даже если вызывается через esp_vfs_fat_sd_spi_mount() из задачи —
    // это однопоточный стартап, вторая задача ещё не запущена.
    s_spi_mutex = xSemaphoreCreateMutex();
    if (s_spi_mutex == NULL)
        return ESP_ERR_NO_MEM;

    spi_config_t spi_cfg;
    memset(&spi_cfg, 0, sizeof(spi_cfg));
    spi_cfg.interface.val = SPI_DEFAULT_INTERFACE;
    spi_cfg.interface.cs_en = 0;
    spi_cfg.mode = SPI_MASTER_MODE;
    // Начальная скорость; реальные ~400 кГц выставляются через
    // регистры в spi_set_speed(false) внутри ff_sd_spi_initialize().
    spi_cfg.clk_div = SPI_2MHz_DIV;

    spi_init(HSPI_HOST, &spi_cfg);
    s_hw_initialized = true;

    return ESP_OK;
}

esp_err_t sd_spi_bus_deinit(void)
{
    if (s_spi_mutex == NULL)
        return ESP_ERR_INVALID_STATE; // Шина не была инициализирована

    // Берём мьютекс — ожидаем завершения текущих SPI операций.
    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Cannot deinit bus — SPI operations in progress");
        return ESP_ERR_TIMEOUT;
    }

    // Деинициализация SPI (под мьютексом — никто не трогает шину)
    spi_deinit(HSPI_HOST);
    s_hw_initialized = false;

    // Удаляем мьютекс ПОСЛЕДНИМ — мы его держим, никто другой не сможет взять.
    // vSemaphoreDelete безопасна при удержании — просто освобождает память.
    vSemaphoreDelete(s_spi_mutex);
    s_spi_mutex = NULL;

    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────
// Уровень 1: Диск
// ─────────────────────────────────────────────────────────────────

esp_err_t sd_spi_diskio_register(BYTE pdrv, const sd_spi_config_t *config)
{
    if (pdrv >= FF_VOLUMES || config == NULL)
        return ESP_ERR_INVALID_ARG;

    // Шина должна быть инициализирована ДО регистрации диска
    if (s_spi_mutex == NULL)
        return ESP_ERR_INVALID_STATE;

    // Настраиваем CS GPIO под мьютексом
    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    cs_pin = config->cs_io_num;

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << cs_pin),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);
    gpio_set_level(cs_pin, 1); // Деактивируем CS (High)

    xSemaphoreGive(s_spi_mutex);

    // Регистрация diskio-коллбэков в FatFs.
    // ff_diskio_register только обновляет таблицу указателей —
    // нет SPI операций, мьютекс не нужен.
    ff_diskio_register(pdrv, &sd_spi_impl);

    return ESP_OK;
}

void sd_spi_diskio_unregister(BYTE pdrv)
{
    if (pdrv != 0)
        return;
    if (s_spi_mutex == NULL)
        return; // Шина не инициализирована — нечего откатывать

    // Берём мьютекс — ожидаем завершения текущих операций.
    if (xSemaphoreTake(s_spi_mutex, pdMS_TO_TICKS(SD_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Cannot unregister — driver busy, another task holds the mutex");
        return;
    }

    // Ждём готовности карты (завершение внутренних операций записи)
    if (select_card())
        deselect();
    else
        ESP_LOGW(TAG, "SD card not ready before unregister");

    // Отвязываем diskio от FatFs
    ff_diskio_unregister(pdrv);

    // CS пин в дефолт: input, без подтяжек
    if (cs_pin != GPIO_NUM_MAX) {
        gpio_set_level(cs_pin, 0);
        gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = (1ULL << cs_pin),
            .pull_down_en = 0,
            .pull_up_en = 0
        };
        gpio_config(&io_conf);
    }
    cs_pin = GPIO_NUM_MAX;

    // Сброс состояния драйвера (под мьютексом — безопасно)
    Stat = STA_NOINIT;
    CardType = 0;

    xSemaphoreGive(s_spi_mutex);
    // Мьютекс НЕ удаляем — это компетенция sd_spi_bus_deinit()
}

// ─────────────────────────────────────────────────────────────────
// Уровень 2: VFS (удобные обёртки)
// ─────────────────────────────────────────────────────────────────

esp_err_t esp_vfs_fat_sd_spi_mount(const char *base_path, BYTE pdrv,
                                   const sd_spi_config_t *config,
                                   size_t max_files)
{
    // Step 1: Инициализация SPI шины (мьютекс + HSPI)
    esp_err_t err = sd_spi_bus_init();
    if (err != ESP_OK)
        return err;

    // Step 2: Регистрация диска (CS GPIO + ff_diskio_register)
    err = sd_spi_diskio_register(pdrv, config);
    if (err != ESP_OK) {
        sd_spi_bus_deinit();
        return err;
    }

    // Step 3: Регистрация VFS + монтирование FAT
    FATFS *fs = NULL;
    char drv[3] = {(char)('0' + pdrv), ':', 0};

    err = esp_vfs_fat_register(base_path, drv, max_files, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register FATFS (%d)", err);
        sd_spi_diskio_unregister(pdrv);
        sd_spi_bus_deinit();
        return err;
    }

    // f_mount(fs, drv, 1) → find_volume() → disk_initialize() → CMD13 → STA_PROTECT
    // Если карта заблокирована паролем → FR_WRITE_PROTECTED
    FRESULT res = f_mount(fs, drv, 1);

    if (res == FR_WRITE_PROTECTED) {
        ESP_LOGW(TAG, "SD card is password-locked!");

        if (config->password && config->pwd_len > 0 && config->pwd_len <= SD_PWD_MAX_LEN) {
            ESP_LOGI(TAG, "Attempting to unlock (len=%d)...", config->pwd_len);
            esp_err_t unlock_ret = sd_spi_unlock(pdrv,
                (const uint8_t *)config->password, config->pwd_len);
            if (unlock_ret == ESP_OK) {
                ESP_LOGI(TAG, "SD card unlocked, retrying mount...");
                // sd_spi_unlock() уже сбросил STA_PROTECT в Stat.
                // ff_sd_spi_initialize() видит STA_NOINIT снят и вернёт Stat как есть.
                res = f_mount(fs, drv, 1);
            } else {
                ESP_LOGE(TAG, "SD card unlock failed (wrong password?): %s",
                         esp_err_to_name(unlock_ret));
            }
        } else {
            ESP_LOGW(TAG, "No SD card password configured — cannot unlock locked card. "
                         "Set password in sd_spi_config_t if the card requires a password.");
        }
    }

    if (res != FR_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS (%d)", res);
        esp_vfs_fat_unregister_path(base_path);
        sd_spi_diskio_unregister(pdrv);
        sd_spi_bus_deinit();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SD Card mounted successfully at %s", base_path);
    return ESP_OK;
}

esp_err_t esp_vfs_fat_sd_spi_unmount(const char *base_path, BYTE pdrv)
{
    // 1. Размонтируем файловую систему — FatFs сбросит все буферы на карту
    char drv[3] = {(char)('0' + pdrv), ':', 0};
    FRESULT res = f_mount(NULL, drv, 0); // NULL = размонтировать
    if (res != FR_OK)
    {
        ESP_LOGE(TAG, "Failed to unmount FATFS (%d)", res);
        // Продолжаем даже при ошибке — всё равно уходим
    }

    // 2. Снимаем регистрацию VFS — после этого файловые операции невозможны
    esp_vfs_fat_unregister_path(base_path);

    // 3. Откат диска: ff_diskio_unregister + CS GPIO reset + сброс состояния
    sd_spi_diskio_unregister(pdrv);

    // 4. Деинициализация шины: spi_deinit + удаление мьютекса
    sd_spi_bus_deinit();

    ESP_LOGI(TAG, "SD card safely unmounted");
    return ESP_OK;
}
