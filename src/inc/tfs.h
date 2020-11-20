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

typedef struct {
  int level;
  int id;
} SDiskID;

typedef struct {
  SDiskID did;
  char    fname[TSDB_FILENAME_LEN];
} STfsFile;

int  tfsInit(SDiskCfg *pDiskCfg, int ndisk);
void tfsDestroy();
int  tfsUpdateInfo();
void tfsPrimaryPath(char *dst);
int  tfsCreateDir(char *dirname);
int  tfsRemoveDir(char *dirname);
int  tfsRename(char *oldpath, char *newpath);

#ifdef __cplusplus
}
#endif

#endif