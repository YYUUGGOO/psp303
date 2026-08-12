#ifndef PSP303_STORAGE_H
#define PSP303_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#include "pattern.h"

#define STORAGE_PATTERN_VERSION 2U
#define STORAGE_SONG_VERSION 1U
#define STORAGE_PATTERN_SLOT_COUNT 32U
#define STORAGE_SONG_CHAIN_MAX 64U

#define STORAGE_PATTERN_FILE_SIZE 149U
#define STORAGE_SONG_FILE_SIZE 148U

typedef enum StorageResult {
    STORAGE_OK = 0,
    STORAGE_ERR_ARGUMENT = -1,
    STORAGE_ERR_IO = -2,
    STORAGE_ERR_BUFFER = -3,
    STORAGE_ERR_TRUNCATED = -4,
    STORAGE_ERR_MAGIC = -5,
    STORAGE_ERR_VERSION = -6,
    STORAGE_ERR_LENGTH = -7,
    STORAGE_ERR_CHECKSUM = -8,
    STORAGE_ERR_DATA = -9
} StorageResult;

typedef struct StoragePatternBank {
    Pattern slots[STORAGE_PATTERN_SLOT_COUNT];
    uint8_t used[STORAGE_PATTERN_SLOT_COUNT];
} StoragePatternBank;

typedef struct StorageSongEntry {
    uint8_t pattern_slot;
    uint8_t repeats;
} StorageSongEntry;

typedef struct StorageSong {
    StorageSongEntry entries[STORAGE_SONG_CHAIN_MAX];
    uint8_t length;
    uint8_t loop_enabled;
    uint8_t loop_start;
    uint8_t loop_end;
} StorageSong;

/* The returned size includes the fixed header and payload. */
size_t storage_pattern_serialized_size(void);
size_t storage_song_serialized_size(void);

int storage_pattern_serialize(const Pattern *pattern,
                              uint8_t *buffer,
                              size_t capacity,
                              size_t *written);
int storage_pattern_deserialize(Pattern *pattern,
                                const uint8_t *buffer,
                                size_t size);
int storage_pattern_save(const char *path, const Pattern *pattern);
int storage_pattern_load(const char *path, Pattern *pattern);

void storage_pattern_bank_init(StoragePatternBank *bank);
int storage_pattern_bank_set(StoragePatternBank *bank,
                             uint8_t slot,
                             const Pattern *pattern);
int storage_pattern_bank_get(const StoragePatternBank *bank,
                             uint8_t slot,
                             Pattern *pattern);
int storage_pattern_slot_save(const char *path,
                              const StoragePatternBank *bank,
                              uint8_t slot);
int storage_pattern_slot_load(const char *path,
                              StoragePatternBank *bank,
                              uint8_t slot);

void storage_song_init(StorageSong *song);
int storage_song_serialize(const StorageSong *song,
                           uint8_t *buffer,
                           size_t capacity,
                           size_t *written);
int storage_song_deserialize(StorageSong *song,
                             const uint8_t *buffer,
                             size_t size);
int storage_song_save(const char *path, const StorageSong *song);
int storage_song_load(const char *path, StorageSong *song);

#endif
