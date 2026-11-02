/* the folder lookup for the profile/save directory; the guest platform layer answers it
   with the app's external data folder (BO1_DATA_ROOT) */
#pragma once
#include <windows.h>

#define CSIDL_PERSONAL 0x0005
#define CSIDL_LOCAL_APPDATA 0x001c
#define CSIDL_FLAG_CREATE 0x8000
HRESULT SHGetFolderPathA(HWND owner, int folder, HANDLE token, DWORD flags, LPSTR path);
