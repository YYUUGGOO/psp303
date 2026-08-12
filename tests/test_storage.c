#include "storage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        ++failures; \
    } \
} while (0)

static void make_pattern(Pattern *pattern)
{
    unsigned int i;

    pattern_init(pattern);
    pattern->length = 7U;
    pattern->direction = PATTERN_PING_PONG;
    pattern->transpose = -7;
    pattern->settings.bpm = 173U;
    pattern->settings.swing = 67U;
    pattern->settings.tune_cents = -31;
    pattern->settings.attack = 63U;
    for (i = 0U; i < PSP303_MAX_STEPS; ++i) {
        pattern->steps[i].note = (uint8_t)(36U + i);
        pattern->steps[i].active = (uint8_t)(i & 1U);
        pattern->steps[i].accent = (uint8_t)(i == 2U);
        pattern->steps[i].slide = (uint8_t)(i == 5U);
        pattern->steps[i].probability = (uint8_t)(i * 6U);
        pattern->steps[i].ratchet_count = (uint8_t)(1U + (i % 4U));
        pattern->steps[i].gate = (uint8_t)(25U + i * 4U);
    }
    pattern_clamp(pattern);
}

static void test_pattern_round_trip_and_capacity(void)
{
    Pattern original;
    Pattern decoded;
    uint8_t buffer[STORAGE_PATTERN_FILE_SIZE];
    size_t written = 0U;

    make_pattern(&original);
    CHECK(storage_pattern_serialized_size() == STORAGE_PATTERN_FILE_SIZE);
    CHECK(storage_pattern_serialize(&original, buffer, sizeof(buffer), &written) == STORAGE_OK);
    CHECK(written == STORAGE_PATTERN_FILE_SIZE);
    CHECK(storage_pattern_deserialize(&decoded, buffer, written) == STORAGE_OK);
    CHECK(memcmp(&original, &decoded, sizeof(original)) == 0);
    CHECK(storage_pattern_serialize(&original, buffer, sizeof(buffer) - 1U, &written)
        == STORAGE_ERR_BUFFER);
    CHECK(written == STORAGE_PATTERN_FILE_SIZE);
    CHECK(storage_pattern_serialize(&original, NULL, 0U, &written) == STORAGE_ERR_BUFFER);
    CHECK(storage_pattern_deserialize(NULL, buffer, sizeof(buffer)) == STORAGE_ERR_ARGUMENT);
}

static void test_corruption_truncation_and_version(void)
{
    Pattern pattern;
    Pattern decoded;
    uint8_t buffer[STORAGE_PATTERN_FILE_SIZE];
    size_t written;

    make_pattern(&pattern);
    CHECK(storage_pattern_serialize(&pattern, buffer, sizeof(buffer), &written) == STORAGE_OK);
    buffer[0] ^= 1U;
    CHECK(storage_pattern_deserialize(&decoded, buffer, written) == STORAGE_ERR_MAGIC);
    CHECK(storage_pattern_serialize(&pattern, buffer, sizeof(buffer), &written) == STORAGE_OK);
    buffer[4] = (uint8_t)(STORAGE_PATTERN_VERSION + 1U);
    CHECK(storage_pattern_deserialize(&decoded, buffer, written) == STORAGE_ERR_VERSION);
    CHECK(storage_pattern_serialize(&pattern, buffer, sizeof(buffer), &written) == STORAGE_OK);
    buffer[20] ^= 0x80U;
    CHECK(storage_pattern_deserialize(&decoded, buffer, written) == STORAGE_ERR_CHECKSUM);
    CHECK(storage_pattern_serialize(&pattern, buffer, sizeof(buffer), &written) == STORAGE_OK);
    CHECK(storage_pattern_deserialize(&decoded, buffer, written - 1U) == STORAGE_ERR_TRUNCATED);
    CHECK(storage_pattern_deserialize(&decoded, buffer, written + 1U) == STORAGE_ERR_LENGTH);
}

static void test_song_repeats_and_loop_ranges(void)
{
    StorageSong song;
    StorageSong decoded;
    uint8_t buffer[STORAGE_SONG_FILE_SIZE];
    size_t written;
    unsigned int i;

    storage_song_init(&song);
    song.length = 5U;
    song.loop_enabled = 1U;
    song.loop_start = 1U;
    song.loop_end = 3U;
    for (i = 0U; i < STORAGE_SONG_CHAIN_MAX; ++i) {
        song.entries[i].pattern_slot = (uint8_t)(i % STORAGE_PATTERN_SLOT_COUNT);
        song.entries[i].repeats = (uint8_t)(1U + (i % 7U));
    }
    CHECK(storage_song_serialize(&song, buffer, sizeof(buffer), &written) == STORAGE_OK);
    CHECK(written == STORAGE_SONG_FILE_SIZE);
    CHECK(storage_song_deserialize(&decoded, buffer, written) == STORAGE_OK);
    CHECK(memcmp(&song, &decoded, sizeof(song)) == 0);
    song.entries[5].repeats = 0U; /* Unused chain entries are irrelevant. */
    CHECK(storage_song_serialize(&song, buffer, sizeof(buffer), &written) == STORAGE_OK);
    song.entries[0].repeats = 0U;
    CHECK(storage_song_serialize(&song, buffer, sizeof(buffer), &written) == STORAGE_ERR_DATA);
    song.entries[0].repeats = 1U;
    song.loop_start = 4U;
    CHECK(storage_song_serialize(&song, buffer, sizeof(buffer), &written) == STORAGE_ERR_DATA);
}

static void test_pattern_bank_slots(void)
{
    StoragePatternBank bank;
    Pattern pattern;
    Pattern decoded;

    make_pattern(&pattern);
    storage_pattern_bank_init(&bank);
    CHECK(storage_pattern_bank_get(&bank, 2U, &decoded) == STORAGE_ERR_DATA);
    CHECK(storage_pattern_bank_set(&bank, 2U, &pattern) == STORAGE_OK);
    CHECK(storage_pattern_bank_get(&bank, 2U, &decoded) == STORAGE_OK);
    CHECK(memcmp(&pattern, &decoded, sizeof(pattern)) == 0);
    CHECK(storage_pattern_bank_set(&bank, STORAGE_PATTERN_SLOT_COUNT, &pattern)
        == STORAGE_ERR_DATA);
}

int main(void)
{
    test_pattern_round_trip_and_capacity();
    test_corruption_truncation_and_version();
    test_song_repeats_and_loop_ranges();
    test_pattern_bank_slots();
    if (failures != 0U) {
        fprintf(stderr, "%u storage test(s) failed\n", failures);
        return 1;
    }
    puts("storage tests passed");
    return 0;
}
