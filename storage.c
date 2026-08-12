#include "storage.h"

#include <stdio.h>
#include <string.h>

#define STORAGE_FILE_HEADER_SIZE 16U
#define STORAGE_PATTERN_PAYLOAD_SIZE 133U
#define STORAGE_PATTERN_V1_PAYLOAD_SIZE 132U
#define STORAGE_SONG_PAYLOAD_SIZE 132U
#define STORAGE_FILE_READ_LIMIT 1024U

static void put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((value >> 16U) & 0xFFU);
    dst[3] = (uint8_t)(value >> 24U);
}

static uint16_t get_u16(const uint8_t *src)
{
    return (uint16_t)src[0] | (uint16_t)((uint16_t)src[1] << 8U);
}

static uint32_t get_u32(const uint8_t *src)
{
    return (uint32_t)src[0]
         | ((uint32_t)src[1] << 8U)
         | ((uint32_t)src[2] << 16U)
         | ((uint32_t)src[3] << 24U);
}

static uint32_t checksum32(const uint8_t *data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFU;
    size_t i;
    unsigned int bit;

    for (i = 0U; i < size; ++i) {
        crc ^= data[i];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (uint32_t)-(int)(crc & 1U));
        }
    }
    return ~crc;
}

static int valid_pattern(const Pattern *pattern)
{
    Pattern copy;
    unsigned int i;

    if (pattern == NULL) return 0;
    copy = *pattern;
    pattern_clamp(&copy);
    if (copy.length != pattern->length || copy.direction != pattern->direction
        || copy.transpose != pattern->transpose
        || memcmp(&copy.settings, &pattern->settings, sizeof(copy.settings)) != 0) {
        return 0;
    }
    for (i = 0U; i < PSP303_MAX_STEPS; ++i) {
        if (memcmp(&copy.steps[i], &pattern->steps[i], sizeof(PatternStep)) != 0) {
            return 0;
        }
    }
    return 1;
}

static void write_header(uint8_t *buffer,
                         const char magic[4],
                         uint16_t version,
                         uint32_t payload_length,
                         uint32_t checksum)
{
    memcpy(buffer, magic, 4U);
    put_u16(buffer + 4U, version);
    put_u16(buffer + 6U, (uint16_t)STORAGE_FILE_HEADER_SIZE);
    put_u32(buffer + 8U, payload_length);
    put_u32(buffer + 12U, checksum);
}

static int validate_header(const uint8_t *buffer,
                           size_t size,
                           const char magic[4],
                           uint16_t version,
                           uint32_t expected_payload,
                           const uint8_t **payload)
{
    uint32_t payload_length;

    if (size < STORAGE_FILE_HEADER_SIZE) return STORAGE_ERR_TRUNCATED;
    if (memcmp(buffer, magic, 4U) != 0) return STORAGE_ERR_MAGIC;
    if (get_u16(buffer + 4U) != version) return STORAGE_ERR_VERSION;
    if (get_u16(buffer + 6U) != STORAGE_FILE_HEADER_SIZE) return STORAGE_ERR_LENGTH;
    payload_length = get_u32(buffer + 8U);
    if (payload_length != expected_payload
        || size != STORAGE_FILE_HEADER_SIZE + (size_t)payload_length) {
        return size < STORAGE_FILE_HEADER_SIZE + (size_t)payload_length
            ? STORAGE_ERR_TRUNCATED : STORAGE_ERR_LENGTH;
    }
    *payload = buffer + STORAGE_FILE_HEADER_SIZE;
    if (checksum32(*payload, payload_length) != get_u32(buffer + 12U)) {
        return STORAGE_ERR_CHECKSUM;
    }
    return STORAGE_OK;
}

static void encode_pattern_payload(const Pattern *pattern, uint8_t *payload)
{
    Pattern copy = *pattern;
    unsigned int i;
    size_t offset = 0U;

    pattern_clamp(&copy);
    payload[offset++] = copy.length;
    payload[offset++] = (uint8_t)copy.direction;
    payload[offset++] = (uint8_t)copy.transpose;
    payload[offset++] = 0U;
    put_u16(payload + offset, copy.settings.bpm);
    offset += 2U;
    payload[offset++] = copy.settings.waveform;
    payload[offset++] = copy.settings.cutoff;
    payload[offset++] = copy.settings.resonance;
    payload[offset++] = copy.settings.envelope;
    payload[offset++] = copy.settings.decay;
    payload[offset++] = copy.settings.drive;
    payload[offset++] = copy.settings.delay_time;
    payload[offset++] = copy.settings.delay_feedback;
    payload[offset++] = copy.settings.delay_mix;
    payload[offset++] = copy.settings.swing;
    payload[offset++] = (uint8_t)copy.settings.tune_cents;
    payload[offset++] = copy.settings.accent_depth;
    payload[offset++] = copy.settings.slide_time;
    payload[offset++] = copy.settings.key_tracking;
    payload[offset++] = copy.settings.attack;
    for (i = 0U; i < PSP303_MAX_STEPS; ++i) {
        const PatternStep *step = &copy.steps[i];
        payload[offset++] = step->note;
        payload[offset++] = step->active;
        payload[offset++] = step->accent;
        payload[offset++] = step->slide;
        payload[offset++] = step->probability;
        payload[offset++] = step->ratchet_count;
        payload[offset++] = step->gate;
    }
}

static int decode_pattern_payload(Pattern *pattern, const uint8_t *payload,
                                  uint16_t version)
{
    Pattern decoded;
    unsigned int i;
    size_t offset = 0U;

    pattern_init(&decoded);
    decoded.length = payload[offset++];
    decoded.direction = (PatternDirection)payload[offset++];
    decoded.transpose = (int8_t)payload[offset++];
    ++offset;
    decoded.settings.bpm = get_u16(payload + offset);
    offset += 2U;
    decoded.settings.waveform = payload[offset++];
    decoded.settings.cutoff = payload[offset++];
    decoded.settings.resonance = payload[offset++];
    decoded.settings.envelope = payload[offset++];
    decoded.settings.decay = payload[offset++];
    decoded.settings.drive = payload[offset++];
    decoded.settings.delay_time = payload[offset++];
    decoded.settings.delay_feedback = payload[offset++];
    decoded.settings.delay_mix = payload[offset++];
    decoded.settings.swing = payload[offset++];
    decoded.settings.tune_cents = (int8_t)payload[offset++];
    decoded.settings.accent_depth = payload[offset++];
    decoded.settings.slide_time = payload[offset++];
    decoded.settings.key_tracking = payload[offset++];
    if (version >= STORAGE_PATTERN_VERSION) decoded.settings.attack = payload[offset++];
    for (i = 0U; i < PSP303_MAX_STEPS; ++i) {
        PatternStep *step = &decoded.steps[i];
        step->note = payload[offset++];
        step->active = payload[offset++];
        step->accent = payload[offset++];
        step->slide = payload[offset++];
        step->probability = payload[offset++];
        step->ratchet_count = payload[offset++];
        step->gate = payload[offset++];
    }
    if (!valid_pattern(&decoded)) return STORAGE_ERR_DATA;
    *pattern = decoded;
    return STORAGE_OK;
}

static int valid_song(const StorageSong *song)
{
    unsigned int i;

    if (song == NULL || song->length > STORAGE_SONG_CHAIN_MAX
        || song->loop_enabled > 1U) return 0;
    if (song->loop_enabled != 0U
        && (song->length == 0U || song->loop_start > song->loop_end
            || song->loop_end >= song->length)) return 0;
    if (song->loop_enabled == 0U && (song->loop_start != 0U || song->loop_end != 0U)) {
        return 0;
    }
    for (i = 0U; i < song->length; ++i) {
        if (song->entries[i].pattern_slot >= STORAGE_PATTERN_SLOT_COUNT
            || song->entries[i].repeats == 0U) return 0;
    }
    return 1;
}

static void encode_song_payload(const StorageSong *song, uint8_t *payload)
{
    unsigned int i;
    size_t offset = 0U;

    payload[offset++] = song->length;
    payload[offset++] = song->loop_enabled;
    payload[offset++] = song->loop_start;
    payload[offset++] = song->loop_end;
    for (i = 0U; i < STORAGE_SONG_CHAIN_MAX; ++i) {
        payload[offset++] = song->entries[i].pattern_slot;
        payload[offset++] = song->entries[i].repeats;
    }
}

static int decode_song_payload(StorageSong *song, const uint8_t *payload)
{
    StorageSong decoded;
    unsigned int i;
    size_t offset = 0U;

    storage_song_init(&decoded);
    decoded.length = payload[offset++];
    decoded.loop_enabled = payload[offset++];
    decoded.loop_start = payload[offset++];
    decoded.loop_end = payload[offset++];
    for (i = 0U; i < STORAGE_SONG_CHAIN_MAX; ++i) {
        decoded.entries[i].pattern_slot = payload[offset++];
        decoded.entries[i].repeats = payload[offset++];
    }
    if (!valid_song(&decoded)) return STORAGE_ERR_DATA;
    *song = decoded;
    return STORAGE_OK;
}

static int read_file(const char *path, uint8_t *buffer, size_t capacity, size_t *size)
{
    FILE *file;
    size_t read_count;

    if (path == NULL || buffer == NULL || size == NULL) return STORAGE_ERR_ARGUMENT;
    file = fopen(path, "rb");
    if (file == NULL) return STORAGE_ERR_IO;
    read_count = fread(buffer, 1U, capacity, file);
    if (ferror(file) != 0) {
        (void)fclose(file);
        return STORAGE_ERR_IO;
    }
    if (fclose(file) != 0) return STORAGE_ERR_IO;
    *size = read_count;
    return read_count == capacity ? STORAGE_ERR_LENGTH : STORAGE_OK;
}

static int write_file(const char *path, const uint8_t *buffer, size_t size)
{
    FILE *file;
    int result = STORAGE_OK;

    if (path == NULL || buffer == NULL) return STORAGE_ERR_ARGUMENT;
    file = fopen(path, "wb");
    if (file == NULL) return STORAGE_ERR_IO;
    if (fwrite(buffer, 1U, size, file) != size) result = STORAGE_ERR_IO;
    if (fclose(file) != 0) result = STORAGE_ERR_IO;
    return result;
}

size_t storage_pattern_serialized_size(void)
{
    return STORAGE_FILE_HEADER_SIZE + STORAGE_PATTERN_PAYLOAD_SIZE;
}

size_t storage_song_serialized_size(void)
{
    return STORAGE_FILE_HEADER_SIZE + STORAGE_SONG_PAYLOAD_SIZE;
}

int storage_pattern_serialize(const Pattern *pattern,
                              uint8_t *buffer,
                              size_t capacity,
                              size_t *written)
{
    Pattern normalized;
    uint8_t *payload;
    size_t required = storage_pattern_serialized_size();

    if (written == NULL || pattern == NULL) return STORAGE_ERR_ARGUMENT;
    *written = required;
    if (buffer == NULL || capacity < required) return STORAGE_ERR_BUFFER;
    normalized = *pattern;
    pattern_clamp(&normalized);
    payload = buffer + STORAGE_FILE_HEADER_SIZE;
    encode_pattern_payload(&normalized, payload);
    write_header(buffer, "P3PT", STORAGE_PATTERN_VERSION,
                 STORAGE_PATTERN_PAYLOAD_SIZE, checksum32(payload, STORAGE_PATTERN_PAYLOAD_SIZE));
    return STORAGE_OK;
}

int storage_pattern_deserialize(Pattern *pattern, const uint8_t *buffer, size_t size)
{
    const uint8_t *payload = NULL;
    int result;
    uint16_t version;
    uint32_t payload_length;

    if (pattern == NULL) return STORAGE_ERR_ARGUMENT;
    pattern_init(pattern);
    if (buffer == NULL) return STORAGE_ERR_ARGUMENT;
    if (size < STORAGE_FILE_HEADER_SIZE || memcmp(buffer, "P3PT", 4U) != 0) {
        return size < STORAGE_FILE_HEADER_SIZE ? STORAGE_ERR_TRUNCATED : STORAGE_ERR_MAGIC;
    }
    version = get_u16(buffer + 4U);
    payload_length = get_u32(buffer + 8U);
    if ((version != 1U && version != STORAGE_PATTERN_VERSION)
        || get_u16(buffer + 6U) != STORAGE_FILE_HEADER_SIZE
        || payload_length != (version == 1U ? STORAGE_PATTERN_V1_PAYLOAD_SIZE
                                            : STORAGE_PATTERN_PAYLOAD_SIZE)
        || size != STORAGE_FILE_HEADER_SIZE + (size_t)payload_length) {
        if (size < STORAGE_FILE_HEADER_SIZE + (size_t)payload_length) {
            return STORAGE_ERR_TRUNCATED;
        }
        return version != 1U && version != STORAGE_PATTERN_VERSION
            ? STORAGE_ERR_VERSION : STORAGE_ERR_LENGTH;
    }
    payload = buffer + STORAGE_FILE_HEADER_SIZE;
    if (checksum32(payload, payload_length) != get_u32(buffer + 12U)) {
        return STORAGE_ERR_CHECKSUM;
    }
    result = decode_pattern_payload(pattern, payload, version);
    return result;
}

int storage_pattern_save(const char *path, const Pattern *pattern)
{
    uint8_t buffer[STORAGE_PATTERN_FILE_SIZE];
    size_t written;
    int result = storage_pattern_serialize(pattern, buffer, sizeof(buffer), &written);
    if (result != STORAGE_OK) return result;
    /* File I/O is deliberately kept out of audio/MIDI realtime callbacks. */
    return write_file(path, buffer, written);
}

int storage_pattern_load(const char *path, Pattern *pattern)
{
    uint8_t buffer[STORAGE_FILE_READ_LIMIT];
    size_t size = 0U;
    int result;

    if (pattern == NULL) return STORAGE_ERR_ARGUMENT;
    pattern_init(pattern);
    /* File I/O is deliberately kept out of audio/MIDI realtime callbacks. */
    result = read_file(path, buffer, sizeof(buffer), &size);
    if (result != STORAGE_OK) return result;
    return storage_pattern_deserialize(pattern, buffer, size);
}

void storage_pattern_bank_init(StoragePatternBank *bank)
{
    unsigned int i;
    if (bank == NULL) return;
    for (i = 0U; i < STORAGE_PATTERN_SLOT_COUNT; ++i) {
        pattern_init(&bank->slots[i]);
        bank->used[i] = 0U;
    }
}

int storage_pattern_bank_set(StoragePatternBank *bank,
                             uint8_t slot,
                             const Pattern *pattern)
{
    if (bank == NULL || pattern == NULL) return STORAGE_ERR_ARGUMENT;
    if (slot >= STORAGE_PATTERN_SLOT_COUNT) return STORAGE_ERR_DATA;
    bank->slots[slot] = *pattern;
    pattern_clamp(&bank->slots[slot]);
    bank->used[slot] = 1U;
    return STORAGE_OK;
}

int storage_pattern_bank_get(const StoragePatternBank *bank,
                             uint8_t slot,
                             Pattern *pattern)
{
    if (pattern == NULL) return STORAGE_ERR_ARGUMENT;
    pattern_init(pattern);
    if (bank == NULL) return STORAGE_ERR_ARGUMENT;
    if (slot >= STORAGE_PATTERN_SLOT_COUNT || bank->used[slot] == 0U) {
        return STORAGE_ERR_DATA;
    }
    *pattern = bank->slots[slot];
    return STORAGE_OK;
}

int storage_pattern_slot_save(const char *path,
                              const StoragePatternBank *bank,
                              uint8_t slot)
{
    Pattern pattern;
    int result = storage_pattern_bank_get(bank, slot, &pattern);
    if (result != STORAGE_OK) return result;
    return storage_pattern_save(path, &pattern);
}

int storage_pattern_slot_load(const char *path,
                              StoragePatternBank *bank,
                              uint8_t slot)
{
    Pattern pattern;
    int result;
    if (bank == NULL) return STORAGE_ERR_ARGUMENT;
    if (slot >= STORAGE_PATTERN_SLOT_COUNT) return STORAGE_ERR_DATA;
    result = storage_pattern_load(path, &pattern);
    if (result != STORAGE_OK) return result;
    return storage_pattern_bank_set(bank, slot, &pattern);
}

void storage_song_init(StorageSong *song)
{
    unsigned int i;
    if (song == NULL) return;
    memset(song, 0, sizeof(*song));
    for (i = 0U; i < STORAGE_SONG_CHAIN_MAX; ++i) song->entries[i].repeats = 1U;
}

int storage_song_serialize(const StorageSong *song,
                           uint8_t *buffer,
                           size_t capacity,
                           size_t *written)
{
    uint8_t *payload;
    size_t required = storage_song_serialized_size();

    if (written == NULL || song == NULL) return STORAGE_ERR_ARGUMENT;
    *written = required;
    if (!valid_song(song)) return STORAGE_ERR_DATA;
    if (buffer == NULL || capacity < required) return STORAGE_ERR_BUFFER;
    payload = buffer + STORAGE_FILE_HEADER_SIZE;
    encode_song_payload(song, payload);
    write_header(buffer, "P3SG", STORAGE_SONG_VERSION,
                 STORAGE_SONG_PAYLOAD_SIZE, checksum32(payload, STORAGE_SONG_PAYLOAD_SIZE));
    return STORAGE_OK;
}

int storage_song_deserialize(StorageSong *song,
                             const uint8_t *buffer,
                             size_t size)
{
    const uint8_t *payload = NULL;
    int result;

    if (song == NULL) return STORAGE_ERR_ARGUMENT;
    storage_song_init(song);
    if (buffer == NULL) return STORAGE_ERR_ARGUMENT;
    result = validate_header(buffer, size, "P3SG", STORAGE_SONG_VERSION,
                             STORAGE_SONG_PAYLOAD_SIZE, &payload);
    if (result != STORAGE_OK) return result;
    return decode_song_payload(song, payload);
}

int storage_song_save(const char *path, const StorageSong *song)
{
    uint8_t buffer[STORAGE_SONG_FILE_SIZE];
    size_t written;
    int result = storage_song_serialize(song, buffer, sizeof(buffer), &written);
    if (result != STORAGE_OK) return result;
    /* File I/O is deliberately kept out of audio/MIDI realtime callbacks. */
    return write_file(path, buffer, written);
}

int storage_song_load(const char *path, StorageSong *song)
{
    uint8_t buffer[STORAGE_FILE_READ_LIMIT];
    size_t size = 0U;
    int result;

    if (song == NULL) return STORAGE_ERR_ARGUMENT;
    storage_song_init(song);
    /* File I/O is deliberately kept out of audio/MIDI realtime callbacks. */
    result = read_file(path, buffer, sizeof(buffer), &size);
    if (result != STORAGE_OK) return result;
    return storage_song_deserialize(song, buffer, size);
}
