/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TD_TFS_H
#define TD_TFS_H

#include "tglobal.h"

#ifdef __cplusplus
extern "C" {
#endif

// tfs.c
int  tfsInit(SDiskCfg *pDiskCfg, int ndisk);
void tfsDestroy();
int  tfsUpdateInfo();
void tfsPrimaryPath(char *dst);
int  tfsCreateDir(char *dirname);
int  tfsRemoveDir(char *dirname);
int  tfsRename(char *oldpath, char *newpath);

// tfcntl.c
typedef struct TFSFILE TFSFILE;
typedef struct TFSDIR  TFSDIR;

TFSDIR *       tfsOpenDir(char *dir);
void           tfsCloseDir(TFSDIR *tdir);
const TFSFILE *tfsReadDir(TFSDIR *tdir);

const char *tfsAbsName(TFSFILE *pfile, char dest[]);
const char *tfsRelName(TFSFILE *pfile, char dest[]);
void        tfsDirName(TFSFILE *pfile, char dest[]);
void        tfsBaseName(TFSFILE *pfile, char dest[]);

int tfsopen(TFSFILE *pfile);
int tfsclose(int, fd);

const char *tfsGetDiskName(int level, int id);

#ifdef __cplusplus
}
#endif

#endif