/*------------------------------------------------------------------------*/
/* OS Dependent Functions for FatFs  (R0.16, FreeRTOS/ESP8266_RTOS_SDK)  */
/* (C)ChaN, 2025                                                          */
/* ESP-IDF port Copyright 2016-2025 Espressif Systems (Shanghai) PTE LTD  */
/*------------------------------------------------------------------------*/

#include <string.h>
#include <stdlib.h>
#include "ff.h"

#ifdef CONFIG_FATFS_ALLOC_EXTRAM_FIRST
#include "esp_heap_caps.h"
#endif


/*------------------------------------------------------------------------*/
/* Allocate a memory block                                                 */
/*------------------------------------------------------------------------*/

void* ff_memalloc (    /* Returns pointer to the allocated memory block (null on not enough core) */
    unsigned msize     /* Number of bytes to allocate */
)
{
#ifdef CONFIG_FATFS_ALLOC_EXTRAM_FIRST
    return heap_caps_malloc_prefer(msize, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM,
                                            MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
#else
    return malloc(msize);
#endif
}


/*------------------------------------------------------------------------*/
/* Free a memory block                                                    */
/*------------------------------------------------------------------------*/

void ff_memfree (
    void* mblock    /* Pointer to the memory block to free (nothing to do for null) */
)
{
    free(mblock);   /* Free the memory block with POSIX API */
}



#if FF_FS_REENTRANT    /* Mutual exclusion */

/*------------------------------------------------------------------------*/
/* Volume mutex table — one FreeRTOS mutex per logical drive              */
/*------------------------------------------------------------------------*/

static SemaphoreHandle_t s_mutexes[FF_VOLUMES] = { NULL };


/*------------------------------------------------------------------------*/
/* Create a Synchronization Object                                        */
/*------------------------------------------------------------------------*/
/* This function is called in f_mount() function to create a new
/  synchronization object for the volume, such as semaphore and mutex.
/  When a 0 is returned, the f_mount() function fails with FR_INT_ERR.
*/


int ff_mutex_create (   /* 1:Function succeeded, 0:Could not create the sync object */
    int vol              /* Corresponding volume (logical drive number) */
)
{
    if (vol < 0 || vol >= FF_VOLUMES) return 0;
    s_mutexes[vol] = xSemaphoreCreateMutex();
    return (s_mutexes[vol] != NULL) ? 1 : 0;
}


/*------------------------------------------------------------------------*/
/* Delete a Synchronization Object                                        */
/*------------------------------------------------------------------------*/
/* This function is called in f_mount() function to delete a synchronization
/  object that created with ff_mutex_create() function.
*/

void ff_mutex_delete (
    int vol              /* Volume number tied to the logical drive to be deleted */
)
{
    if (vol < 0 || vol >= FF_VOLUMES) return;
    if (s_mutexes[vol] == NULL) return;
    vSemaphoreDelete(s_mutexes[vol]);
    s_mutexes[vol] = NULL;
}


/*------------------------------------------------------------------------*/
/* Request Grant to Access the Volume                                     */
/*------------------------------------------------------------------------*/
/* This function is called on entering file functions to lock the volume.
/  When a 0 is returned, the file function fails with FR_TIMEOUT.
*/

int ff_mutex_take (     /* 1:Got a grant to access the volume, 0:Could not get a grant */
    int vol              /* Volume number to wait */
)
{
    if (vol < 0 || vol >= FF_VOLUMES) return 0;
    return (xSemaphoreTake(s_mutexes[vol], FF_FS_TIMEOUT) == pdTRUE) ? 1 : 0;
}


/*------------------------------------------------------------------------*/
/* Release Grant to Access the Volume                                     */
/*------------------------------------------------------------------------*/
/* This function is called on leaving file functions to unlock the volume.
*/

void ff_mutex_give (
    int vol              /* Volume number to be signaled */
)
{
    if (vol < 0 || vol >= FF_VOLUMES) return;
    xSemaphoreGive(s_mutexes[vol]);
}

#endif /* FF_FS_REENTRANT */
