#include "platform_3ds.h"
#include "updater.h"
#include "port_second_screen_theme.h"

#include "port_audio.h"
#include "port_ppu.h"
#include "port_rom.h"
#include "port_rom_profile.h"
#include "port_runtime_config.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_DIR "sdmc:/3ds/The Minish Cap 3DS"
#define ROM_PATH_SIZE 512

extern void AgbMain(void);

static const PortRomProfile* sSelectedRomProfile;
static PortRomHashes sSelectedRomHashes;
static const PortRomProfile* sMismatchedRomProfile;

static const char* BuildBaselineName(void) {
#if defined(JP)
    return "JP";
#elif defined(EU)
    return "EU";
#else
    return "USA";
#endif
}

/* The original 3DS target is a proven USA/EU multi-region build. JP still
 * carries compile-time data/layout choices that require the JP asset baseline,
 * so do not silently boot either family with the other's binary. */
static int RomMatchesBuildFamily(const PortRomProfile* profile) {
#if defined(JP)
    return profile->region == ROM_REGION_JP;
#else
    return profile->region == ROM_REGION_USA || profile->region == ROM_REGION_EU;
#endif
}

static const char* RequiredBuildBaseline(const PortRomProfile* profile) {
    if (profile != NULL && profile->region == ROM_REGION_JP)
        return "JP";
    return "USA";
}

/* ROM hash cache.
 *
 * Hashing a 16 MiB ROM with SHA-1 and SHA-256 costs more than the rest of the
 * boot put together, and the answer cannot change unless the file does. The
 * cache lives in the app directory and is keyed by name + size + mtime, so a
 * normal second launch skips hashing entirely. */
#define ROM_HASH_CACHE_PATH "romhash.cache"
#define ROM_HASH_CACHE_MAX 8

typedef struct {
    char path[ROM_PATH_SIZE];
    long long size;
    long long mtime;
    char sha1[41];
    char sha256[65];
} RomHashCacheEntry;

static int LoadRomHashCache(RomHashCacheEntry* entries, int capacity) {
    FILE* f = fopen(ROM_HASH_CACHE_PATH, "r");
    int n = 0;
    if (f == NULL)
        return 0;
    while (n < capacity && fscanf(f, "%255s %lld %lld %40s %64s", entries[n].path, &entries[n].size, &entries[n].mtime,
                                  entries[n].sha1, entries[n].sha256) == 5) {
        ++n;
    }
    fclose(f);
    return n;
}

static void StoreRomHashCache(RomHashCacheEntry* entries, int n) {
    FILE* f = fopen(ROM_HASH_CACHE_PATH, "w");
    if (f == NULL)
        return;
    for (int i = 0; i < n; ++i) {
        fprintf(f, "%s %lld %lld %s %s\n", entries[i].path, entries[i].size, entries[i].mtime, entries[i].sha1,
                entries[i].sha256);
    }
    fclose(f);
}

static int PrepareStorage(void) {
    mkdir("sdmc:/3ds", 0777);
    if (mkdir(APP_DIR, 0777) != 0 && errno != EEXIST)
        return 0;
    return chdir(APP_DIR) == 0;
}

static int HasGbaExtension(const char* name) {
    const size_t length = strlen(name);
    if (length < 5)
        return 0;
    const char* ext = name + length - 4;
    return tolower((unsigned char)ext[0]) == '.' && tolower((unsigned char)ext[1]) == 'g' &&
           tolower((unsigned char)ext[2]) == 'b' && tolower((unsigned char)ext[3]) == 'a';
}

static RomHashCacheEntry sRomHashCache[ROM_HASH_CACHE_MAX];
static int sRomHashCacheCount;
static int sRomHashCacheDirty;

/* Returns 1 when the cache answered for `path`. */
static int RomHashesFromCache(const char* path, const struct stat* info, PortRomHashes* out) {
    for (int i = 0; i < sRomHashCacheCount; ++i) {
        const RomHashCacheEntry* e = &sRomHashCache[i];
        if (e->size != (long long)info->st_size || e->mtime != (long long)info->st_mtime ||
            strcmp(e->path, path) != 0) {
            continue;
        }
        memset(out, 0, sizeof(*out));
        out->size = (uint32_t)info->st_size;
        snprintf(out->sha1, sizeof(out->sha1), "%s", e->sha1);
        snprintf(out->sha256, sizeof(out->sha256), "%s", e->sha256);
        return 1;
    }
    return 0;
}

static void RomHashesStoreCache(const char* path, const struct stat* info, const PortRomHashes* hashes) {
    int slot = -1;
    for (int i = 0; i < sRomHashCacheCount; ++i) {
        if (strcmp(sRomHashCache[i].path, path) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (sRomHashCacheCount < ROM_HASH_CACHE_MAX) {
            slot = sRomHashCacheCount++;
        } else {
            slot = 0; /* evict the oldest slot; the list is tiny and unordered */
        }
    }
    snprintf(sRomHashCache[slot].path, sizeof(sRomHashCache[slot].path), "%s", path);
    sRomHashCache[slot].size = (long long)info->st_size;
    sRomHashCache[slot].mtime = (long long)info->st_mtime;
    snprintf(sRomHashCache[slot].sha1, sizeof(sRomHashCache[slot].sha1), "%s", hashes->sha1);
    snprintf(sRomHashCache[slot].sha256, sizeof(sRomHashCache[slot].sha256), "%s", hashes->sha256);
    sRomHashCacheDirty = 1;
}

static int RomIsSupported(const char* path, const struct stat* info) {
    PortRomHashes hashes = { 0 };
    const PortRomProfile* profile;
    if (info != NULL && RomHashesFromCache(path, info, &hashes)) {
        char gameCode[4] = { 0 };
        FILE* gc = fopen(path, "rb");
        if (gc != NULL) {
            if (fseek(gc, 0xAC, SEEK_SET) == 0)
                (void)fread(gameCode, 1, sizeof(gameCode), gc);
            fclose(gc);
        }
        profile = Port_IdentifyRomHashes(&hashes, gameCode);
    } else {
        profile = Port_IdentifyRomFile(path, &hashes);
        if (info != NULL && profile != NULL)
            RomHashesStoreCache(path, info, &hashes);
    }
    if (!Port_RomProfileIsPlayable(profile)) {
        if (hashes.sha256[0] != '\0') {
            printf("Skipping unsupported ROM: %s\nSHA-256: %s\n", path, hashes.sha256);
        }
        return 0;
    }
    if (!RomMatchesBuildFamily(profile)) {
        if (sMismatchedRomProfile == NULL)
            sMismatchedRomProfile = profile;
        printf("Skipping ROM for a different build family: %s\n"
               "Profile: %s\nBuild baseline: %s\nRequired baseline: %s\n",
               path, profile->displayName, BuildBaselineName(), RequiredBuildBaseline(profile));
        return 0;
    }
    sSelectedRomProfile = profile;
    sSelectedRomHashes = hashes;
    return 1;
}

static int FindRom(char* out, size_t outSize) {
    DIR* dir = opendir(".");
    if (!dir)
        return 0;
    sRomHashCacheCount = LoadRomHashCache(sRomHashCache, ROM_HASH_CACHE_MAX);
    sRomHashCacheDirty = 0;

    int foundGba = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        if (!HasGbaExtension(entry->d_name))
            continue;

        struct stat info;
        if (stat(entry->d_name, &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        foundGba = 1;
        if (!RomIsSupported(entry->d_name, &info))
            continue;
        snprintf(out, outSize, "%s", entry->d_name);
        closedir(dir);
        if (sRomHashCacheDirty)
            StoreRomHashCache(sRomHashCache, sRomHashCacheCount);
        return 1;
    }

    closedir(dir);
    if (sRomHashCacheDirty)
        StoreRomHashCache(sRomHashCache, sRomHashCacheCount);
    if (sMismatchedRomProfile != NULL)
        return -2;
    return foundGba ? -1 : 0;
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    if (!Platform3DS_Init())
        return 1;
    Platform3DS_ShowSplash();

    printf("The Minish Cap 3DS v" TMC_PORT_VERSION "\n\n");
    printf("System: %s\n", Platform3DS_IsNew3DS() ? "New Nintendo 3DS" : "Nintendo 3DS");
    printf("Performance: %s\n",
           Platform3DS_IsNew3DS() ? "New 3DS full presentation" : "Old 3DS adaptive presentation skip (max 3)");
    printf("PPU worker core 1: %u%%\n", Platform3DS_Core1TimeLimit());
    printf("Extra New 3DS core: %s\n\n", Platform3DS_IsNew3DS() ? "enabled" : "unavailable");
    printf("Preparing storage...\n");
    if (!PrepareStorage()) {
        Platform3DS_ShowFatal("Storage error", "Could not open " APP_DIR ".");
        Platform3DS_Shutdown();
        return 1;
    }

    char romPath[ROM_PATH_SIZE];
    int romResult = FindRom(romPath, sizeof(romPath));
    if (romResult <= 0) {
        char message[512];
        if (romResult == -2) {
            snprintf(message, sizeof(message),
                     "Found %s (%s), but this package uses the %s compile-time baseline.\n\n"
                     "Use a build with TMC3DS_REGION=%s for this ROM. JP builds accept clean Japan and Angel SP4; "
                     "USA/EU builds accept the original USA and Europe ROMs.",
                     sMismatchedRomProfile->displayName, sMismatchedRomProfile->id, BuildBaselineName(),
                     RequiredBuildBaseline(sMismatchedRomProfile));
        } else if (romResult < 0) {
            snprintf(message, sizeof(message),
                     "None of the .gba files in:\n%s\n"
                     "matches a supported ROM profile.\n\n"
                     "Expected SHA-1:\nUSA: b4bd50e4131b027c334547b4524e2dbbd4227130\n"
                     "Europe: cff199b36ff173fb6faf152653d1bccf87c26fb7\n"
                     "Japan: 6c5404a1effb17f481f352181d0f1c61a2765c5d\n"
                     "Angel SP4: ba04cfbe93d12d2ad684c52234472fa12a5b53d7",
                     APP_DIR);
        } else {
            snprintf(message, sizeof(message),
                     "Copy a supported Minish Cap ROM to:\n%s\n\nAny .gba filename is accepted.\n\n"
                     "Supported: USA, Europe, clean Japan, and Angel Chinese SP4.",
                     APP_DIR);
        }
        Platform3DS_ShowFatal(romResult == -2 ? "ROM/build mismatch"
                              : romResult < 0 ? "Unsupported ROM"
                                              : "ROM not found",
                              message);
        Platform3DS_Shutdown();
        return 1;
    }

    FILE* rom = fopen(romPath, "rb");
    if (!rom) {
        Platform3DS_ShowFatal("ROM error", "Could not open the selected .gba file.");
        Platform3DS_Shutdown();
        return 1;
    }
    fclose(rom);

    printf("ROM profile: %s (%s)\n", sSelectedRomProfile->displayName, sSelectedRomProfile->id);
    printf("ROM SHA-1: %s\n", sSelectedRomHashes.sha1);
    printf("Loading ROM and tables...\n");
    Port_Config_Load("tmc3ds.ini");
    Port_LoadRom(romPath);
    Port_SecondScreenTheme_Ready();
    Port_PPU_Init(NULL);
    if (!Port_Audio_Init()) {
        printf("Warning: audio is unavailable.\n");
    }

    printf("Starting engine...\n");
    Platform3DS_EnterGameplayDisplay();
    Updater_Init(argc > 0 ? argv[0] : NULL);
    AgbMain();
    Updater_Shutdown();

    Port_PPU_Shutdown();
    Platform3DS_Shutdown();
    return 0;
}
