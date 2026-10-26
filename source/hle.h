/**
 * @file hle.h
 * @brief Canonical storage path constants for PS Vita high-level emulation.
 */
#pragma once

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/blockbreaker3unlimited/"
#endif

#define BUNDLE_PATH DATA_PATH "game_app/"
#define DOCS_PATH   DATA_PATH "saves/"
#define LOGS_PATH   DATA_PATH "logs/"

// Identidad del bundle iOS (Info.plist de game_app/)
#define EXE_NAME     "BBD3"
#define EXE_PATH     BUNDLE_PATH EXE_NAME
#define BUNDLE_ID    "com.gameloft.BBD3"
#define APP_DELEGATE "BBD3AppDelegate"
