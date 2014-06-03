//
// Copyright (c) Microsoft Corporation.  All rights reserved.
//
//
// Use of this source code is subject to the terms of the Microsoft end-user
// license agreement (EULA) under which you licensed this SOFTWARE PRODUCT.
// If you did not accept the terms of the EULA, you are not authorized to use
// this source code. For a copy of the EULA, please see the LICENSE.RTF on your
// install media.
//
/*++
THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
PARTICULAR PURPOSE.

Module Name:  

mwinreg.h

Abstract:

Private version of winreg.h. This module contains the function prototypes
and constant, type and structure definitions for the WINCE Implementeation
of the Windows 32-Bit Registry API.

Notes: 


--*/
#ifndef _MACRO_WINREG_H_
#define _MACRO_WINREG_H_

#ifndef WINCEMACRO
#error WINCEMACRO not defined __FILE__
#endif

#ifdef __cplusplus
extern "C" {
#endif

// SDK exports
#ifdef WIN32_FS_CALL
#define RegCloseKey WIN32_FS_CALL(LONG, 17, (HKEY hKey))

#define RegFlushKey WIN32_FS_CALL(LONG, 49, (HKEY hKey))

#define RegCreateKeyExW WIN32_FS_CALL(LONG, 18, (HKEY hKey, LPCWSTR lpSubKey,  \
    DWORD Reserved, LPWSTR lpClass, DWORD dwOptions, REGSAM samDesired,        \
    LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult, LPDWORD lpdwDisposition))

#define RegDeleteKeyW WIN32_FS_CALL(LONG, 19, (HKEY hKey, LPCWSTR lpSubKey))

#define RegDeleteValueW WIN32_FS_CALL(LONG, 20, (HKEY hKey, LPCWSTR lpValueName))

#define RegEnumValueW WIN32_FS_CALL(LONG, 21, (HKEY hKey, DWORD dwIndex,       \
    LPWSTR lpValueName, LPDWORD lpcbValueName, LPDWORD lpReserved, LPDWORD lpType, \
    LPBYTE lpData, LPDWORD lpcbData))

#define RegEnumKeyExW WIN32_FS_CALL(LONG, 22, (HKEY hKey, DWORD dwIndex,       \
    LPWSTR lpName, LPDWORD lpcbName, LPDWORD lpReserved, LPWSTR lpClass,       \
    LPDWORD lpcbClass, PFILETIME lpftLastWriteTime))

#define RegOpenKeyExW WIN32_FS_CALL(LONG, 23, (HKEY hKey, LPCWSTR lpSubKey,    \
    DWORD ulOptions, REGSAM samDesired, PHKEY phkResult))

#define RegQueryInfoKeyW WIN32_FS_CALL(LONG, 24, (HKEY hKey, LPWSTR lpClass,   \
    LPDWORD lpcbClass, LPDWORD lpReserved, LPDWORD lpcSubKeys,                 \
    LPDWORD lpcbMaxSubKeyLen, LPDWORD lpcbMaxClassLen, LPDWORD lpcValues,      \
    LPDWORD lpcbMaxValueNameLen, LPDWORD lpcbMaxValueLen, \
    LPDWORD lpcbSecurityDescriptor, PFILETIME lpftLastWriteTime))

#define RegQueryValueExW WIN32_FS_CALL(LONG, 25, (HKEY hKey, LPCWSTR lpValueName, \
    LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData))

#define RegSetValueExW WIN32_FS_CALL(LONG, 26, (HKEY hKey, LPCWSTR lpValueName, \
    DWORD Reserved, DWORD dwType, CONST BYTE* lpData, DWORD cbData))

#define GetUserInformation  WIN32_FS_CALL(BOOL, 69, (LPWSTR lpszBuffer,        \
    LPDWORD lpdwSize, DWORD dwFlags))

// This definition is from wincrypt.h
typedef struct _CRYPTOAPI_BLOB
{
    DWORD cbData;
    PBYTE pbData;
} DATA_BLOB;

#define CryptProtectData WIN32_FS_CALL(BOOL, 85, (DATA_BLOB*      pDataIn,  \
    LPCWSTR         szDataDescr, \
    DATA_BLOB*      pOptionalEntropy, \
    PVOID           pvReserved, \
    PVOID  pPromptStruct, \
    DWORD           dwFlags, \
    DATA_BLOB*      pDataOut ))
    
#define CryptUnprotectData WIN32_FS_CALL(BOOL, 86, (DATA_BLOB*      pDataIn,    \
    LPWSTR*         ppszDataDescr, \
    DATA_BLOB*      pOptionalEntropy, \
    PVOID           pvReserved, \
    PVOID   pPromptStruct, \
    DWORD           dwFlags, \
    DATA_BLOB*      pDataOut ))

// OAK exports
#define RegCopyFile    WIN32_FS_CALL(BOOL, 41, (LPCWSTR lpszFile))
#define RegRestoreFile WIN32_FS_CALL(BOOL, 44, (LPCWSTR lpszFile))
#define RegSaveKey     WIN32_FS_CALL(LONG, 64, (HKEY hKey, LPCWSTR lpszFile,   \
    LPSECURITY_ATTRIBUTES lpSecurityAttributes))
#define RegReplaceKey  WIN32_FS_CALL(LONG, 65, (HKEY hKey, LPCWSTR lpszSubKey, \
    LPCWSTR lpszNewFile, LPCWSTR lpszOldFile))
#define SetCurrentUser WIN32_FS_CALL(BOOL, 67, (LPCWSTR lpszUserName,          \
    LPBYTE lpbUserData, DWORD dwDataSize, BOOL bCreateIfNew))
#define SetUserData    WIN32_FS_CALL(BOOL, 68, (LPBYTE lpbUserData,            \
    DWORD dwDataSize))
#define GenRandom  WIN32_FS_CALL(BOOL, 87, (DWORD dwLen, LPBYTE pbBuffer))


#define CeFindFirstRegChange    +++ Do Not Use +++ 
#define CeFindFirstRegChange_Trap WIN32_FS_CALL(HANDLE, 104, (HKEY hKey, BOOL bSubTree, DWORD dwFlags))

#define CeFindNextRegChange      +++ Do Not Use +++ 
#define CeFindNextRegChange_Trap  WIN32_FS_CALL(BOOL, 105, (HANDLE hNotify))

#define CeFindCloseRegChange     +++ Do Not Use +++ 
#define CeFindCloseRegChange_Trap WIN32_FS_CALL(BOOL, 106, (HANDLE hNotify))

#elif defined(COREDLL)

LONG xxx_RegCloseKey (HKEY hKey);
#define RegCloseKey xxx_RegCloseKey
LONG xxx_RegFlushKey (HKEY hKey);
#define RegFlushKey xxx_RegFlushKey
LONG xxx_RegCreateKeyExW (HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved, LPWSTR lpClass, DWORD dwOptions, REGSAM samDesired, LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult, LPDWORD lpdwDisposition);
#define RegCreateKeyExW xxx_RegCreateKeyExW
LONG xxx_RegDeleteKeyW (HKEY hKey, LPCWSTR lpSubKey);
#define RegDeleteKeyW xxx_RegDeleteKeyW
LONG xxx_RegDeleteValueW (HKEY hKey, LPCWSTR lpValueName);
#define RegDeleteValueW xxx_RegDeleteValueW
LONG xxx_RegEnumValueW (HKEY hKey, DWORD dwIndex, LPWSTR lpValueName, LPDWORD lpcbValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
#define RegEnumValueW xxx_RegEnumValueW
LONG xxx_RegEnumKeyExW (HKEY hKey, DWORD dwIndex, LPWSTR lpName, LPDWORD lpcbName, LPDWORD lpReserved, LPWSTR lpClass, LPDWORD lpcbClass, PFILETIME lpftLastWriteTime);
#define RegEnumKeyExW xxx_RegEnumKeyExW
LONG xxx_RegOpenKeyExW (HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult);
#define RegOpenKeyExW xxx_RegOpenKeyExW
LONG xxx_RegQueryInfoKeyW (HKEY hKey, LPWSTR lpClass, LPDWORD lpcbClass, LPDWORD lpReserved, LPDWORD lpcSubKeys, LPDWORD lpcbMaxSubKeyLen, LPDWORD lpcbMaxClassLen, LPDWORD lpcValues, LPDWORD lpcbMaxValueNameLen, LPDWORD lpcbMaxValueLen, LPDWORD lpcbSecurityDescriptor, PFILETIME lpftLastWriteTime);
#define RegQueryInfoKeyW xxx_RegQueryInfoKeyW
LONG xxx_RegQueryValueExW (HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
#define RegQueryValueExW xxx_RegQueryValueExW
LONG xxx_RegSetValueExW (HKEY hKey, LPCWSTR lpValueName, DWORD Reserved, DWORD dwType, CONST BYTE* lpData, DWORD cbData);
#define RegSetValueExW xxx_RegSetValueExW


HANDLE  xxx_CeFindFirstRegChange(HKEY hKey, BOOL bWatchSubTree, DWORD dwNotifyFilter);
#define CeFindFirstRegChange   xxx_CeFindFirstRegChange

BOOL    xxx_CeFindNextRegChange(HANDLE hNotify);
#define CeFindNextRegChange      xxx_CeFindNextRegChange

BOOL    xxx_CeFindCloseRegChange(HANDLE hNotify);
#define CeFindCloseRegChange     xxx_CeFindCloseRegChange


// OAK exports
BOOL xxx_RegCopyFile (LPCWSTR lpszFile);
#define RegCopyFile xxx_RegCopyFile
BOOL xxx_RegRestoreFile (LPCWSTR lpszFile);
#define RegRestoreFile xxx_RegRestoreFile
LONG xxx_RegSaveKey(HKEY hKey, LPCWSTR lpszFile, LPSECURITY_ATTRIBUTES lpSecurityAttributes);
#define RegSaveKey xxx_RegSaveKey
LONG xxx_RegReplaceKey (HKEY hKey, LPCWSTR lpszSubKey, LPCWSTR lpszNewFile, LPCWSTR lpszOldFile);
#define RegReplaceKey xxx_RegReplaceKey
BOOL xxx_SetCurrentUser (LPCWSTR lpszUserName, LPBYTE lpbUserData, DWORD dwDataSize, BOOL bCreateIfNew);
#define SetCurrentUser xxx_SetCurrentUser
BOOL xxx_SetUserData (LPBYTE lpbUserData, DWORD dwDataSize);
#define SetUserData xxx_SetUserData
BOOL xxx_GetUserDirectory(LPWSTR lpszBuffer, LPDWORD lpdwSize);
#define GetUserDirectory xxx_GetUserDirectory

#endif

#ifdef __cplusplus
}
#endif

#endif // _MACRO_WINREG_
