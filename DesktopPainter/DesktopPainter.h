#pragma once

#include "resource.h"
#include <commctrl.h>
#pragma comment(lib, "Comctl32.lib")

// ← 加在這裡
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif
