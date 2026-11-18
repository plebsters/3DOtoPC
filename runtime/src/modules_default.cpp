// Defaults for games converted before other programs on the disc were
// translated: their generated table has no module list. A game converted now
// defines these symbols itself, and its definitions take precedence.
#include "tdort/runtime.h"

#if defined(__GNUC__) || defined(__clang__)
extern "C" __attribute__((weak)) const tdort::ModuleInfo tdort_modules[] = {{"", "", 0, 0, nullptr}};
extern "C" __attribute__((weak)) const uint32_t tdort_module_count = 0;
#endif
