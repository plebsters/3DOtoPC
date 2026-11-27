/* tdo/api.h - C API of the converter core (consumed by the WPF GUI via P/Invoke).
 * All strings are UTF-8. Everything runs locally; no function touches the network. */
#ifndef TDO_API_H
#define TDO_API_H
#include <stdint.h>

#if defined(_WIN32) && defined(TDO_BUILD_DLL)
#define TDO_API __declspec(dllexport)
#elif defined(_WIN32) && defined(TDO_USE_DLL)
#define TDO_API __declspec(dllimport)
#else
#define TDO_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum tdo_log_level { TDO_LOG_INFO = 0, TDO_LOG_WARN = 1, TDO_LOG_ERROR = 2 };

enum tdo_stage {
    TDO_STAGE_OPEN = 0, TDO_STAGE_EXTRACT = 1, TDO_STAGE_ANALYZE = 2,
    TDO_STAGE_EMIT = 3, TDO_STAGE_CONFIGURE = 4, TDO_STAGE_BUILD = 5, TDO_STAGE_DONE = 6
};

typedef void (*tdo_log_cb)(int level, const char* message, void* user);
typedef void (*tdo_progress_cb)(int stage, float fraction, void* user);

typedef struct tdo_convert_options {
    const char* disc_path;        /* .iso (2048) or .bin (2352) image */
    const char* output_dir;       /* project + build output goes here */
    const char* game_name;        /* window title / exe name; NULL = volume label */
    const char* runtime_dir;      /* tdort sources; NULL = <module dir>/runtime */
    const char* hints_path;       /* optional interp_hits.txt from a previous run */
    const char* launch_me;        /* executable on disc; NULL = "LaunchMe" */
    int32_t     build;            /* 1 = compile the exe with the local toolchain */
    int32_t     scan_prologues;   /* 1 = extra function discovery heuristics */
    int32_t     release;          /* 1 = Release build, 0 = Debug */
    const char* cover_path;       /* cover image (PNG/JPEG/BMP); NULL = an image beside the
                                     disc, else the disc's own title picture (BannerScreen) */
} tdo_convert_options;

TDO_API const char* tdo_version(void);

/* Lists volume info and files through the log callback. Returns 0 on success. */
TDO_API int32_t tdo_inspect(const char* disc_path, tdo_log_cb log, void* user);

/* Full pipeline: extract, analyze, emit C++, optionally build. 0 on success. */
TDO_API int32_t tdo_convert(const tdo_convert_options* opts, tdo_log_cb log,
                            tdo_progress_cb progress, void* user);

/* Extracts another disc of a multi-disc game (image or folder) into dest_dir,
 * normally <out_dir>/data_disc2, data_disc3, ... next to the game's data folder.
 * The runtime switches to a disc when the game asks for a file only it has. */
TDO_API int32_t tdo_extract_disc(const char* disc_path, const char* dest_dir, tdo_log_cb log, void* user);

/* Finds the cover art a conversion would use: an image beside the disc
 * (<disc name>.png/.jpg, cover.*, folder.*, boxart.* in its folder), else the
 * title picture on the disc (BannerScreen; the developer-kit sample is skipped).
 * Writes it to out_png as PNG and its origin ("cover.jpg", "the disc") to
 * source_buf. Returns 0 when one was found, 1 when there is none, 2 on error. */
TDO_API int32_t tdo_find_cover(const char* disc_path, const char* out_png, char* source_buf, int32_t source_len);

/* Describes the compiler that would be used, e.g. "Bundled LLVM (C:\...\clang-cl.exe)".
 * Writes into buf (UTF-8). Returns 0 if a usable toolchain exists. */
TDO_API int32_t tdo_detect_toolchain(char* buf, int32_t buf_len);

#ifdef __cplusplus
}
#endif
#endif
