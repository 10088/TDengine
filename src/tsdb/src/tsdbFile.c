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
#define _DEFAULT_SOURCE
#define TAOS_RANDOM_FILE_FAIL_TEST
#include <regex.h>
#include "os.h"
#include "talgo.h"
#include "tchecksum.h"
#include "tsdbMain.h"
#include "tutil.h"
#include "tfs.h"

const char *tsdbFileSuffix[] = {".head", ".data", ".last", ".stat", ".h", ".d", ".l", ".s"};

// STsdbFileH ===========================================
STsdbFileH *tsdbNewFileH(STsdbCfg *pCfg) {
  STsdbFileH *pFileH = (STsdbFileH *)calloc(1, sizeof(*pFileH));
  if (pFileH == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  int code = pthread_rwlock_init(&(pFileH->fhlock), NULL);
  if (code != 0) {
    tsdbError("vgId:%d failed to init file handle lock since %s", pCfg->tsdbId, strerror(code));
    terrno = TAOS_SYSTEM_ERROR(code);
    goto _err;
  }

  pFileH->maxFGroups = TSDB_MAX_FILE(pCfg->keep, pCfg->daysPerFile);

  pFileH->pFGroup = (SFileGroup *)calloc(pFileH->maxFGroups, sizeof(SFileGroup));
  if (pFileH->pFGroup == NULL) {
    terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
    goto _err;
  }

  return pFileH;

_err:
  tsdbFreeFileH(pFileH);
  return NULL;
}

void tsdbFreeFileH(STsdbFileH *pFileH) {
  if (pFileH) {
    pthread_rwlock_destroy(&pFileH->fhlock);
    tfree(pFileH->pFGroup);
    free(pFileH);
  }
}

int tsdbOpenFileH(STsdbRepo *pRepo) { // TODO
  ASSERT(pRepo != NULL && pRepo->tsdbFileH != NULL);
  char dataDir[TSDB_FILENAME_LEN] = "\0";

  // 1. scan and get all files corresponds
  TDIR *tdir = NULL;
  char    fname[TSDB_FILENAME_LEN] = "\0";
  regex_t regex = {0};
  int     code = 0;
  int     vid = 0;
  int     fid = 0;

  const TFILE *pfile = NULL;

  code = regcomp(&regex, "^v[0-9]+f[0-9]+\\.(head|data|last|h|d|l)$", REG_EXTENDED);
  if (code != 0) {
    // TODO: deal the error
  }

  snprintf(dataDir, TSDB_FILENAME_LEN, "vnode/vnode%d/tsdb/data", REPO_ID(pRepo));
  tdir = tfsOpendir(dataDir);
  if (tdir == NULL) {
    // TODO: deal the error
  }

  while ((pfile = tfsReaddir(tdir)) != NULL) {
    tfsBaseName(pfile, fname);

    if (strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0) continue;

    code = regexec(&regex, fname, 0, NULL, 0);
    if (code == 0) {
      sscanf(fname, "v%df%d", &vid, &fid);

      if (vid != REPO_ID(pRepo)) {
        tfsAbsName(pfile, fname);
        tsdbError("vgId:%d invalid file %s exists, ignore", REPO_ID(pRepo), fname);
        continue;
      }

      // TODO 
      {}
    } else if (code == REG_NOMATCH) {
      tfsAbsName(pfile, fname);
      tsdbWarn("vgId:%d unrecognizable file %s exists, ignore", REPO_ID(pRepo), fname);
      continue;
    } else {
      tsdbError("vgId:%d regexec failed since %s", REPO_ID(pRepo), strerror(code));
      // TODO: deal with error
    }
  }

  // 2. Sort all files according to fid

  // 3. Recover all files of each fid
  while (true) {
    // TODO
  }

  return 0;
}

void tsdbCloseFileH(STsdbRepo *pRepo) { // TODO
  STsdbFileH *pFileH = pRepo->tsdbFileH;

  for (int i = 0; i < pFileH->nFGroups; i++) {
    SFileGroup *pFGroup = pFileH->pFGroup + i;
    for (int type = 0; type < TSDB_FILE_TYPE_MAX; type++) {
      tsdbCloseFile(&(pFGroup->files[type]));
    }
  }
  // TODO: delete each files
}

// SFileGroup ===========================================
SFileGroup *tsdbCreateFGroup(STsdbRepo *pRepo, int fid, int level) {
  STsdbFileH *pFileH = pRepo->tsdbFileH;
  char        fname[TSDB_FILENAME_LEN] = "\0";
  SFileGroup  fg = {0};
  SFileGroup *pfg = &fg;
  SFile *     pFile = NULL;
  int         id = TFS_UNDECIDED_ID;

  ASSERT(tsdbSearchFGroup(pFileH, fid, TD_EQ) == NULL && pFileH->nFGroups < pFileH->maxFGroups);

  // 1. Create each files
  for (int type = 0; type < TSDB_FILE_TYPE_MAX; type++) {
    pFile = &(pfg->files[type]);

    tsdbGetDataFileName(pRepo->rootDir, REPO_ID(pRepo), fid, type, pFile->file.rname);
    pFile->file.level = level;
    pFile->file.id = id;

    if (tsdbOpenFile(pFile, O_WRONLY|O_CREAT) < 0); {
      tsdbError("vgId:%d failed to create file group %d since %s", REPO_ID(pRepo), fid, tstrerror(terrno));
      return NULL;
    }

    if (tsdbUpdateFileHeader(pFile) < 0) {
      tsdbError("vgId:%d failed to update file %s header since %s", REPO_ID(pRepo), TSDB_FILE_NAME(pFile),
                tstrerror(terrno));
      tsdbCloseFile(pFile);
      return NULL;
    }

    tsdbCloseFile(pFile);

    level = pFile->file.level;
    id = pFile->file.id;
  }

  // Set fg
  pfg->fileId = fid;
  pfg->state = 0;

  // Register fg to the repo
  pthread_rwlock_wrlock(&pFileH->fhlock);
  pFileH->pFGroup[pFileH->nFGroups++] = fGroup;
  qsort((void *)(pFileH->pFGroup), pFileH->nFGroups, sizeof(SFileGroup), compFGroup);
  pthread_rwlock_unlock(&pFileH->fhlock);

  pfg = tsdbSearchFGroup(pFileH, fid, TD_EQ);
  ASSERT(pfg != NULL);
  return pfg;
}

void tsdbRemoveFileGroup(STsdbRepo *pRepo, SFileGroup *pFGroup) {
  ASSERT(pFGroup != NULL);
  STsdbFileH *pFileH = pRepo->tsdbFileH;

  SFileGroup fileGroup = *pFGroup;

  int nFilesLeft = pFileH->nFGroups - (int)(POINTER_DISTANCE(pFGroup, pFileH->pFGroup) / sizeof(SFileGroup) + 1);
  if (nFilesLeft > 0) {
    memmove((void *)pFGroup, POINTER_SHIFT(pFGroup, sizeof(SFileGroup)), sizeof(SFileGroup) * nFilesLeft);
  }

  pFileH->nFGroups--;
  ASSERT(pFileH->nFGroups >= 0);

  for (int type = 0; type < TSDB_FILE_TYPE_MAX; type++) {
    SFile *pFile = &(pFGroup->files[type]);
    tfsremove(&(pFile->file));
  }
}

SFileGroup *tsdbSearchFGroup(STsdbFileH *pFileH, int fid, int flags) {
  void *ptr 
      taosbsearch((void *)(&fid), (void *)(pFileH->pFGroup), pFileH->nFGroups, sizeof(SFileGroup), keyFGroupCompFunc, flags);
  if (ptr == NULL) return NULL;
  return (SFileGroup *)ptr;
}

static int compFGroup(const void *arg1, const void *arg2) {
  int val1 = ((SFileGroup *)arg1)->fileId;
  int val2 = ((SFileGroup *)arg2)->fileId;

  if (val1 < val2) {
    return -1;
  } else if (val1 > val2) {
    return 1;
  } else {
    return 0;
  }
}

static int keyFGroupCompFunc(const void *key, const void *fgroup) {
  int         fid = *(int *)key;
  SFileGroup *pFGroup = (SFileGroup *)fgroup;
  if (fid == pFGroup->fileId) {
    return 0;
  } else {
    return fid > pFGroup->fileId ? 1 : -1;
  }
}

// SFileGroupIter ===========================================
void tsdbInitFileGroupIter(STsdbFileH *pFileH, SFileGroupIter *pIter, int direction) {
  pIter->pFileH = pFileH;
  pIter->direction = direction;

  if (pFileH->nFGroups == 0) {
    pIter->index = -1;
    pIter->fileId = -1;
  } else {
    if (direction == TSDB_FGROUP_ITER_FORWARD) {
      pIter->index = 0;
    } else {
      pIter->index = pFileH->nFGroups - 1;
    }
    pIter->fileId = pFileH->pFGroup[pIter->index].fileId;
  }
}

void tsdbSeekFileGroupIter(SFileGroupIter *pIter, int fid) {
  STsdbFileH *pFileH = pIter->pFileH;

  if (pFileH->nFGroups == 0) {
    pIter->index = -1;
    pIter->fileId = -1;
    return;
  }

  int   flags = (pIter->direction == TSDB_FGROUP_ITER_FORWARD) ? TD_GE : TD_LE;
  void *ptr = taosbsearch(&fid, (void *)pFileH->pFGroup, pFileH->nFGroups, sizeof(SFileGroup), keyFGroupCompFunc, flags);
  if (ptr == NULL) {
    pIter->index = -1;
    pIter->fileId = -1;
  } else {
    pIter->index = (int)(POINTER_DISTANCE(ptr, pFileH->pFGroup) / sizeof(SFileGroup));
    pIter->fileId = ((SFileGroup *)ptr)->fileId;
  }
}

SFileGroup *tsdbGetFileGroupNext(SFileGroupIter *pIter) {
  STsdbFileH *pFileH = pIter->pFileH;
  SFileGroup *pFGroup = NULL;

  if (pIter->index < 0 || pIter->index >= pFileH->nFGroups || pIter->fileId < 0) return NULL;

  pFGroup = &pFileH->pFGroup[pIter->index];
  if (pFGroup->fileId != pIter->fileId) {
    tsdbSeekFileGroupIter(pIter, pIter->fileId);
  }

  if (pIter->index < 0) return NULL;

  pFGroup = &pFileH->pFGroup[pIter->index];
  ASSERT(pFGroup->fileId == pIter->fileId);

  if (pIter->direction == TSDB_FGROUP_ITER_FORWARD) {
    pIter->index++;
  } else {
    pIter->index--;
  }

  if (pIter->index >= 0 && pIter->index < pFileH->nFGroups) {
    pIter->fileId = pFileH->pFGroup[pIter->index].fileId;
  } else {
    pIter->fileId = -1;
  }

  return pFGroup;
}

// SFile ===========================================
int tsdbOpenFile(SFile *pFile, int oflag) {
  ASSERT(!TSDB_IS_FILE_OPENED(pFile));

  pFile->fd = tfsopen(&(pFile->file), oflag);
  if (pFile->fd < 0) {
    tsdbError("failed to open file %s since %s", TSDB_FILE_NAME(pFile), tstrerror(terrno));
    return -1;
  }

  tsdbTrace("open file %s, fd %d", TSDB_FILE_NAME(pFile), pFile->fd);

  return 0;
}

void tsdbCloseFile(SFile *pFile) {
  if (TSDB_IS_FILE_OPENED(pFile)) {
    tsdbTrace("close file %s, fd %d", TSDB_FILE_NAME(pFile), pFile->fd);
    close(pFile->fd);
    pFile->fd = -1;
  }
}

int tsdbUpdateFileHeader(SFile *pFile) {
  char buf[TSDB_FILE_HEAD_SIZE] = "\0";

  void *pBuf = (void *)buf;
  taosEncodeFixedU32((void *)(&pBuf), TSDB_FILE_VERSION);
  tsdbEncodeSFileInfo((void *)(&pBuf), &(pFile->info));

  taosCalcChecksumAppend(0, (uint8_t *)buf, TSDB_FILE_HEAD_SIZE);

  if (lseek(pFile->fd, 0, SEEK_SET) < 0) {
    tsdbError("failed to lseek file %s since %s", TSDB_FILE_NAME(pFile), strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }
  if (taosWrite(pFile->fd, (void *)buf, TSDB_FILE_HEAD_SIZE) < TSDB_FILE_HEAD_SIZE) {
    tsdbError("failed to write %d bytes to file %s since %s", TSDB_FILE_HEAD_SIZE, TSDB_FILE_NAME(pFile),
              strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  return 0;
}

int tsdbEncodeSFileInfo(void **buf, const STsdbFileInfo *pInfo) {
  int tlen = 0;
  tlen += taosEncodeFixedU32(buf, pInfo->magic);
  tlen += taosEncodeFixedU32(buf, pInfo->len);
  tlen += taosEncodeFixedU32(buf, pInfo->totalBlocks);
  tlen += taosEncodeFixedU32(buf, pInfo->totalSubBlocks);
  tlen += taosEncodeFixedU32(buf, pInfo->offset);
  tlen += taosEncodeFixedU64(buf, pInfo->size);
  tlen += taosEncodeFixedU64(buf, pInfo->tombSize);

  return tlen;
}

void *tsdbDecodeSFileInfo(void *buf, STsdbFileInfo *pInfo) {
  buf = taosDecodeFixedU32(buf, &(pInfo->magic));
  buf = taosDecodeFixedU32(buf, &(pInfo->len));
  buf = taosDecodeFixedU32(buf, &(pInfo->totalBlocks));
  buf = taosDecodeFixedU32(buf, &(pInfo->totalSubBlocks));
  buf = taosDecodeFixedU32(buf, &(pInfo->offset));
  buf = taosDecodeFixedU64(buf, &(pInfo->size));
  buf = taosDecodeFixedU64(buf, &(pInfo->tombSize));

  return buf;
}

int tsdbLoadFileHeader(SFile *pFile, uint32_t *version) {
  char buf[TSDB_FILE_HEAD_SIZE] = "\0";

  if (lseek(pFile->fd, 0, SEEK_SET) < 0) {
    tsdbError("failed to lseek file %s to start since %s", TSDB_FILE_NAME(pFile), strerror(errno));
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  if (taosRead(pFile->fd, buf, TSDB_FILE_HEAD_SIZE) < TSDB_FILE_HEAD_SIZE) {
    tsdbError("failed to read file %s header part with %d bytes, reason:%s", TSDB_FILE_NAME(pFile), TSDB_FILE_HEAD_SIZE,
              strerror(errno));
    terrno = TSDB_CODE_TDB_FILE_CORRUPTED;
    return -1;
  }

  if (!taosCheckChecksumWhole((uint8_t *)buf, TSDB_FILE_HEAD_SIZE)) {
    tsdbError("file %s header part is corrupted with failed checksum", TSDB_FILE_NAME(pFile));
    terrno = TSDB_CODE_TDB_FILE_CORRUPTED;
    return -1;
  }

  void *pBuf = (void *)buf;
  pBuf = taosDecodeFixedU32(pBuf, version);
  pBuf = tsdbDecodeSFileInfo(pBuf, &(pFile->info));

  return 0;
}

void tsdbGetFileInfoImpl(char *fname, uint32_t *magic, int64_t *size) { // TODO
  uint32_t      version = 0;
  SFile         file;
  SFile *       pFile = &file;

  strncpy(TSDB_FILE_NAME(pFile), fname, TSDB_FILENAME_LEN - 1);
  pFile->fd = -1;

  if (tsdbOpenFile(pFile, O_RDONLY) < 0) goto _err;
  if (tsdbLoadFileHeader(pFile, &version) < 0) goto _err;

  off_t offset = lseek(pFile->fd, 0, SEEK_END);
  if (offset < 0) goto _err;
  tsdbCloseFile(pFile);

  *magic = pFile->info.magic;
  *size = offset;

  return;

_err:
  tsdbCloseFile(pFile);
  *magic = TSDB_FILE_INIT_MAGIC;
  *size = 0;
}

// Retention ===========================================
void tsdbRemoveFilesBeyondRetention(STsdbRepo *pRepo, SFidGroup *pFidGroup) {
  STsdbFileH *pFileH = pRepo->tsdbFileH;
  SFileGroup *pGroup = pFileH->pFGroup;

  pthread_rwlock_wrlock(&(pFileH->fhlock));

  while (pFileH->nFGroups > 0 && pGroup[0].fileId < pFidGroup->minFid) {
    tsdbRemoveFileGroup(pRepo, pGroup);
  }

  pthread_rwlock_unlock(&(pFileH->fhlock));
}

void tsdbGetFidGroup(STsdbCfg *pCfg, SFidGroup *pFidGroup) {
  TSKEY now = taosGetTimestamp(pCfg->precision);

  pFidGroup->minFid =
      TSDB_KEY_FILEID(now - pCfg->keep * tsMsPerDay[pCfg->precision], pCfg->daysPerFile, pCfg->precision);
  pFidGroup->midFid =
      TSDB_KEY_FILEID(now - pCfg->keep2 * tsMsPerDay[pCfg->precision], pCfg->daysPerFile, pCfg->precision);
  pFidGroup->maxFid =
      TSDB_KEY_FILEID(now - pCfg->keep1 * tsMsPerDay[pCfg->precision], pCfg->daysPerFile, pCfg->precision);
}

int tsdbApplyRetention(STsdbRepo *pRepo, SFidGroup *pFidGroup) {
  STsdbFileH *pFileH = pRepo->tsdbFileH;
  SFileGroup *pGroup = NULL;
  SFileGroup  nFileGroup = {0};
  SFileGroup  oFileGroup = {0};
  int         level = 0;

  if (tsDnodeTier->nTiers == 1 || (pFidGroup->minFid == pFidGroup->midFid && pFidGroup->midFid == pFidGroup->maxFid)) {
    return 0;
  }

  for (int gidx = pFileH->nFGroups - 1; gidx >= 0; gidx--) {
    pGroup = pFileH->pFGroup + gidx;

    level = tsdbGetFidLevel(pGroup->fileId, pFidGroup);

    if (level == pGroup->level) continue;
    if (level > pGroup->level && level < tsDnodeTier->nTiers) {
      SDisk *pODisk = tdGetDisk(tsDnodeTier, pGroup->level, pGroup->did);
      SDisk *pDisk = tdAssignDisk(tsDnodeTier, level);
      tsdbCreateVnodeDataDir(pDisk->dir, REPO_ID(pRepo));
      oFileGroup = *pGroup;
      nFileGroup = *pGroup;
      nFileGroup.level = level;
      nFileGroup.did = pDisk->did;

      char tsdbRootDir[TSDB_FILENAME_LEN];
      tdGetTsdbRootDir(pDisk->dir, REPO_ID(pRepo), tsdbRootDir);
      for (int type = 0; type < TSDB_FILE_TYPE_MAX; type++) {
        tsdbGetDataFileName(tsdbRootDir, REPO_ID(pRepo), pGroup->fileId, type, nFileGroup.files[type].fname);
      }

      for (int type = 0; type < TSDB_FILE_TYPE_MAX; type++) {
        if (taosCopy(oFileGroup.files[type].fname, nFileGroup.files[type].fname) < 0) return -1;
      }

      pthread_rwlock_wrlock(&(pFileH->fhlock)); 
      *pGroup = nFileGroup;
      pthread_rwlock_unlock(&(pFileH->fhlock));

      for (int type = 0; type < TSDB_FILE_TYPE_MAX; type++) {
        (void)remove(oFileGroup.files[type].fname);
      }

      tdLockTiers(tsDnodeTier);
      tdDecDiskFiles(tsDnodeTier, pODisk, false);
      tdIncDiskFiles(tsDnodeTier, pDisk, false);
      tdUnLockTiers(tsDnodeTier);
    }
  }

  return 0;
}