#ifndef VCD_H
#define VCD_H

// ---------------------------------------------------------------------------
// vcd.h  –  shared protocol definitions for vcd server and vc client.
//
// Wire protocol (line-based TCP):
//
//   C→S  HELLO <version>\n
//   S→C  OK vcd/<version>\n
//   C→S  AUTH <username> <sha256hex_of_password>\n
//   S→C  OK\n | ERROR <message>\n
//   C→S  <command>\n
//   S→C  OK [args]\n | ERROR <message>\n
//   [binary payload if applicable]
//   C→S  QUIT\n
//   S→C  OK\n
//
// Commands after AUTH:
//   PUSH <repopath> <relfile> <size>    push one file; binary data follows OK
//   PULL <repopath> <relfile>           pull one file; binary data follows OK <size>
//   LIST <repopath>                     list files in repo; one per line, END terminates
//   LISTREPOS                           list all repos the user can access
//   QUIT                                close connection
// ---------------------------------------------------------------------------

#include "vcVersion.h"
#define VCD_VERSION         APP_VERSION
#define VCD_DEFAULT_PORT    9876
#define VCD_MAX_LINE        4096
#define VCD_MAX_PATH        512
#define VCD_MAX_USERNAME    64
#define VCD_TRANSFER_BUF    65536

// Server directory layout under repoRoot:
//   users/<username>/           personal repos
//   shared/                     shared repos
#define VCD_USERS_DIR       "users"
#define VCD_SHARED_DIR      "shared"

// Protocol command strings.
#define CMD_HELLO       "HELLO"
#define CMD_AUTH        "AUTH"
#define CMD_PUSH        "PUSH"
#define CMD_PULL        "PULL"
#define CMD_LIST        "LIST"
#define CMD_LISTREPOS   "LISTREPOS"
#define CMD_INITREPO    "INITREPO"   // INITREPO <repopath> — create repo if not exists
#define CMD_MOVEREPO    "MOVEREPO"   // MOVEREPO <srcpath> <dstpath> — move repo
#define CMD_QUIT        "QUIT"

#define RESP_OK         "OK"
#define RESP_ERROR      "ERROR"

#endif // VCD_H
