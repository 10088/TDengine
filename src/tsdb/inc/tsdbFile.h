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

#ifndef _TS_TSDB_FILE_H_
#define _TS_TSDB_FILE_H_

#ifdef __cplusplus
extern "C" {
#endif

#define TSDB_FILE_HEAD_SIZE 512
#define TSDB_FILE_DELIMITER 0xF00AFA0F
#define TSDB_FILE_INIT_MAGIC 0xFFFFFFFF

#define TSDB_FILE_INFO(tf) (&((tf)->info))
#define TSDB_FILE_F(tf) (&((tf)->f))
#define TSDB_FILE_FD(tf) ((tf)->fd)
#define TSDB_FILE_FULL_NAME(f) TFILE_NAME(TSDB_FILE_F(f))
#define TSDB_FILE_OPENED(f) (TSDB_FILE_FD(f) >= 0)
#define TSDB_FILE_SET_CLOSED(f) (TSDB_FILE_FD(f) = -1)

typedef enum {
  TSDB_FILE_HEAD = 0,
  TSDB_FILE_DATA,
  TSDB_FILE_LAST,
  TSDB_FILE_MAX,
  TSDB_FILE_META,
  TSDB_FILE_MANIFEST
} TSDB_FILE_T;

#define tsdbOpenFile(T, f, flags) tsdbOpen##T(f, flags)
#define tsdbCloseFile(T, f) tsdbClose##T(f)
#define tsdbSeekFile(T, f, offset, whence) tsdbSeek##T(f, offset, whence)
#define tsdbWriteFile(T, f, buf, nbytes) tsdbWrite##T(f, buf, nbytes)
#define tsdbUpdateFileMagic(T, f, pCksum) tsdbUpdate##T##Magic(f, pCksum)
#define tsdbTellFile(T, f) tsdbTell##T(f)
#define tsdbEncodeFile(T, buf, f) tsdbEncode##T(buf, f)
#define tsdbDecodeFile(T, buf, f) tsdbDecode##T(buf, f)

// =============== SMFile
typedef struct {
  int64_t  size;
  int64_t  tombSize;
  int64_t  nRecords;
  int64_t  nDels;
  uint32_t magic;
} SMFInfo;

typedef struct {
  SMFInfo info;
  TFILE   f;
  int     fd;
} SMFile;

void  tsdbInitMFile(SMFile* pMFile, int vid, int ver, SMFInfo* pInfo);
int   tsdbEncodeSMFile(void** buf, SMFile* pMFile);
void* tsdbDecodeSMFile(void* buf, SMFile* pMFile);

static FORCE_INLINE int tsdbOpenSMFile(SMFile* pMFile, int flags) {
  ASSERT(!TSDB_FILE_OPENED(pMFile));

  pMFile->fd = open(TSDB_FILE_FULL_NAME(pMFile), flags);
  if (pMFile->fd < 0) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  return 0;
}

static FORCE_INLINE void tsdbCloseSMFile(SMFile* pMFile) {
  if (TSDB_FILE_OPENED(pMFile)) {
    close(pMFile->fd);
    TSDB_FILE_SET_CLOSED(pMFile);
  }
}

static FORCE_INLINE int64_t tsdbSeekSMFile(SMFile* pMFile, int64_t offset, int whence) {
  ASSERT(TSDB_FILE_OPENED(pMFile));

  int64_t loffset = taosLSeek(TSDB_FILE_FD(pMFile), offset, whence);
  if (loffset < 0) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  return loffset;
}

static FORCE_INLINE int64_t tsdbWriteSMFile(SMFile* pMFile, void* buf, int64_t nbyte) {
  ASSERT(TSDB_FILE_OPENED(pMFile));

  int64_t nwrite = taosWrite(pMFile->fd, buf, nbyte);
  if (nwrite < nbyte) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  return nwrite;
}

static FORCE_INLINE void tsdbUpdateSMFileMagic(SMFile* pMFile, void* pCksum) {
  pMFile->info.magic = taosCalcChecksum(pMFile->info.magic, (uint8_t*)(pCksum), sizeof(TSCKSUM));
}

static FORCE_INLINE int64_t tsdbTellSMFile(SMFile* pMFile) { return tsdbSeekSMFile(pMFile, 0, SEEK_CUR); }

// =============== SDFile
typedef struct {
  uint32_t magic;
  uint32_t len;
  uint32_t totalBlocks;
  uint32_t totalSubBlocks;
  uint32_t offset;
  uint64_t size;
  uint64_t tombSize;
} SDFInfo;

typedef struct {
  SDFInfo info;
  TFILE   f;
  int     fd;
} SDFile;

void  tsdbInitDFile(SDFile* pDFile, int vid, int fid, int ver, int level, int id, const SDFInfo* pInfo,
                    TSDB_FILE_T ftype);
void  tsdbInitDFileWithOld(SDFile* pDFile, SDFile* pOldDFile);
int   tsdbEncodeSDFile(void** buf, SDFile* pDFile);
void* tsdbDecodeSDFile(void* buf, SDFile* pDFile);

static FORCE_INLINE int tsdbOpenSDFile(SDFile *pDFile, int flags) {
  ASSERT(!TSDB_FILE_OPENED(pDFile));

  pDFile->fd = open(pDFile->f.aname, flags);
  if (pDFile->fd < 0) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  return 0;
}

static FORCE_INLINE void tsdbCloseSDFile(SDFile* pDFile) {
  if (TSDB_FILE_OPENED(pDFile)) {
    close(pDFile->fd);
    TSDB_FILE_SET_CLOSED(pDFile);
  }
}

static FORCE_INLINE int64_t tsdbSeekSDFile(SDFile *pDFile, int64_t offset, int whence) {
  ASSERT(TSDB_FILE_OPENED(pDFile));

  int64_t loffset = taosLSeek(pDFile->fd, offset, whence);
  if (loffset < 0) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  return loffset;
}

static FORCE_INLINE int64_t tsdbWriteSDFile(SDFile* pDFile, void* buf, int64_t nbyte) {
  ASSERT(TSDB_FILE_OPENED(pDFile));

  int64_t nwrite = taosWrite(pDFile->fd, buf, nbyte);
  if (nwrite < nbyte) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  return nwrite;
}

static FORCE_INLINE int64_t tsdbAppendSDFile(SDFile* pDFile, void* buf, int64_t nbyte, int64_t* offset) {
  ASSERT(TSDB_FILE_OPENED(pDFile));
  int64_t nwrite;

  *offset = tsdbSeekSDFile(pDFile, 0, SEEK_SET);
  if (*offset < 0) return -1;

  nwrite = tsdbWriteSDFile(pDFile, buf, nbyte);
  if (nwrite < 0) return nwrite;

  return nwrite;
}

static FORCE_INLINE int64_t tsdbReadSDFile(SDFile* pDFile, void* buf, int64_t nbyte) {
  ASSERT(TSDB_FILE_OPENED(pDFile));

  int64_t nread = taosRead(pDFile->fd, buf, nbyte);
  if (nread < 0) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }

  return nread;
}

static FORCE_INLINE int64_t tsdbTellSDFile(SDFile *pDFile) { return tsdbSeekSDFile(pDFile, 0, SEEK_CUR); }

static FORCE_INLINE void tsdbUpdateSDFileMagic(SDFile* pDFile, void* pCksm) {
  pDFile->info.magic = taosCalcChecksum(pDFile->info.magic, (uint8_t*)(pCksm), sizeof(TSCKSUM));
}

// =============== SDFileSet
typedef struct {
  int    fid;
  int    state;
  SDFile files[TSDB_FILE_MAX];
} SDFileSet;

#define TSDB_FSET_FID(s) ((s)->fid)
#define TSDB_DFILE_IN_SET(s, t) ((s)->files + (t))

void tsdbInitDFileSet(SDFileSet* pSet, int vid, int fid, int ver, int level, int id);
void tsdbInitDFileSetWithOld(SDFileSet* pSet, SDFileSet* pOldSet);
int  tsdbOpenDFileSet(SDFileSet* pSet, int flags);
void tsdbCloseDFileSet(SDFileSet* pSet);
int  tsdbUpdateDFileSetHeader(SDFileSet* pSet);
int  tsdbCopyDFileSet(SDFileSet* pFromSet, SDFileSet* pToSet);

#ifdef __cplusplus
}
#endif

#endif /* _TS_TSDB_FILE_H_ */