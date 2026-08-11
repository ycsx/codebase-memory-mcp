#ifndef CBM_FOUNDATION_WIN_PROCESS_H
#define CBM_FOUNDATION_WIN_PROCESS_H

#ifdef _WIN32
#include <windows.h>

static inline DWORD cbm_win_background_creation_flags(DWORD extra_flags) {
    return CREATE_NO_WINDOW | extra_flags;
}
#endif

#endif /* CBM_FOUNDATION_WIN_PROCESS_H */
