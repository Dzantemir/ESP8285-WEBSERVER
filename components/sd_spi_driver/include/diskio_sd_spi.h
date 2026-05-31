#ifndef _DISKIO_SD_SPI_H_
#define _DISKIO_SD_SPI_H_

#include "ff.h"
#include "diskio.h"
#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SD_PWD_MAX_LEN 16   // SD спецификация: максимум 16 байт

/**
 * @brief Конфигурация SD SPI диска
 */
typedef struct {
    gpio_num_t cs_io_num;        /**< CS pin (GPIO5 по умолчанию) */
    const char *password;        /**< Пароль для разблокировки SD карты (NULL = не пытаться) */
    uint8_t pwd_len;             /**< Длина пароля (0 = пароль не задан) */
} sd_spi_config_t;

/* ─────────────────────────────────────────────────────────────────
 * Уровень 0: SPI шина
 *
 * Жизненный цикл:  bus_init → ... → bus_deinit
 * bus_init создаёт мьютекс и инициализирует HSPI.
 * Вызывается ДО запуска рабочих задач — без гонок, без candidate-mutex.
 * bus_deinit деинициализирует HSPI и удаляет мьютекс (потокобезопасна).
 * ───────────────────────────────────────────────────────────────── */

/**
 * @brief Инициализация SPI шины
 *
 * Создаёт мьютекс и инициализирует HSPI периферию.
 * Идемпотентна: повторный вызов возвращает ESP_OK.
 *
 * КОНТРАКТ: вызывается ДО запуска рабочих задач (из app_main
 * или однопоточного стартапа). Нет конкуренции — нет гонки —
 * мьютекс создаётся просто, без candidate/SuspendAll/танцев.
 *
 * @return ESP_OK — шина инициализирована
 *         ESP_ERR_NO_MEM — не хватило памяти на мьютекс
 */
esp_err_t sd_spi_bus_init(void);

/**
 * @brief Деинициализация SPI шины
 *
 * Деинициализирует HSPI периферию и удаляет мьютекс.
 * Вызывать ПОСЛЕ sd_spi_diskio_unregister() для всех дисков.
 *
 * Берёт мьютекс (ожидая завершения текущих операций),
 * затем деинициализирует SPI и удаляет мьютекс удерживая его.
 *
 * @return ESP_OK — шина деинициализирована
 *         ESP_ERR_INVALID_STATE — шина не была инициализирована
 *         ESP_ERR_TIMEOUT — мьютекс занят
 */
esp_err_t sd_spi_bus_deinit(void);

/* ─────────────────────────────────────────────────────────────────
 * Уровень 1: Диск
 *
 * Жизненный цикл:  diskio_register → ... → diskio_unregister
 * Требует предварительно инициализированной шины (bus_init).
 * ───────────────────────────────────────────────────────────────── */

/**
 * @brief Регистрация диска в FatFs
 *
 * Настраивает CS GPIO и регистрирует diskio-коллбэки в FatFs.
 * Требует предварительно инициализированной шины (sd_spi_bus_init).
 *
 * @param pdrv Номер физического диска (обычно 0)
 * @param config Настройки: CS пин + пароль для разблокировки
 * @return ESP_OK при успехе
 *         ESP_ERR_INVALID_STATE — шина не инициализирована
 *         ESP_ERR_INVALID_ARG — неверные аргументы
 */
esp_err_t sd_spi_diskio_register(BYTE pdrv, const sd_spi_config_t *config);

/**
 * @brief Снятие регистрации диска из FatFs
 *
 * Откатывает всё, что сделал sd_spi_diskio_register():
 * ff_diskio_unregister → CS GPIO в INPUT → сброс состояния драйвера.
 * НЕ затрагивает SPI шину и мьютекс — для этого вызовите sd_spi_bus_deinit().
 *
 * @param pdrv Номер физического диска (обычно 0)
 */
void sd_spi_diskio_unregister(BYTE pdrv);

/* ─────────────────────────────────────────────────────────────────
 * Уровень 2: VFS (удобные обёртки)
 *
 * Комбинируют все уровни в один вызов.
 * ───────────────────────────────────────────────────────────────── */

/**
 * @brief Монтирование VFS FatFs с использованием SD SPI
 *
 * Выполняет: bus_init → diskio_register → VFS register → f_mount.
 *
 * f_mount() внутри вызывает disk_initialize() (через find_volume()),
 * которая проверяет блокировку карты через CMD13 и устанавливает STA_PROTECT.
 * Если карта заблокирована — f_mount возвращает FR_WRITE_PROTECTED.
 * При наличии пароля в config — автоматически разблокирует и повторяет f_mount.
 *
 * @param base_path Путь монтирования (например, "/sdcard")
 * @param pdrv Номер физического диска (обычно 0)
 * @param config Настройки пинов + пароль для разблокировки
 * @param max_files Максимальное количество одновременно открытых файлов
 * @return esp_err_t ESP_OK при успехе
 */
esp_err_t esp_vfs_fat_sd_spi_mount(const char* base_path, BYTE pdrv,
                                    const sd_spi_config_t *config, size_t max_files);

/**
 * @brief Размонтирование VFS FatFs
 *
 * Выполняет: f_unmount → VFS unregister → diskio_unregister → bus_deinit.
 *
 * @param base_path Путь монтирования
 * @param pdrv Номер физического диска
 */
esp_err_t esp_vfs_fat_sd_spi_unmount(const char *base_path, BYTE pdrv);

/* ─────────────────────────────────────────────────────────────────
 * Управление паролем SD карты (CMD42)
 *
 * Все функции потокобезопасны — берут мьютекс SPI шины.
 * ───────────────────────────────────────────────────────────────── */

/**
 * @brief Проверить заблокирована ли карта паролем
 * @return true если карта заблокирована
 */
bool sd_spi_is_locked(BYTE pdrv);

/**
 * @brief Разблокировать карту
 */
esp_err_t sd_spi_unlock(BYTE pdrv, const uint8_t *password, uint8_t pwd_len);

/**
 * @brief Заблокировать карту (пароль должен быть предварительно установлен через sd_spi_set_password)
 */
esp_err_t sd_spi_lock(BYTE pdrv, const uint8_t *password, uint8_t pwd_len);

/**
 * @brief Установить или сменить пароль
 * @param old_pwd Текущий пароль (NULL и old_len=0 если пароля ещё нет)
 * @param new_pwd Новый пароль
 */
esp_err_t sd_spi_set_password(BYTE pdrv,
                               const uint8_t *old_pwd, uint8_t old_len,
                               const uint8_t *new_pwd, uint8_t new_len);

/**
 * @brief Удалить пароль с карты (карта должна быть разблокирована)
 */
esp_err_t sd_spi_clear_password(BYTE pdrv, const uint8_t *password, uint8_t pwd_len);

/**
 * @brief Принудительное стирание заблокированной карты. ВСЕ ДАННЫЕ БУДУТ УНИЧТОЖЕНЫ.
 *        Использовать только при утере пароля.
 */
esp_err_t sd_spi_force_erase(BYTE pdrv);

#ifdef __cplusplus
}
#endif

#endif // _DISKIO_SD_SPI_H_
