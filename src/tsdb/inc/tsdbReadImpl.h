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

#ifndef _TD_TSDB_READ_IMPL_H_
#define _TD_TSDB_READ_IMPL_H_

#include "taosdef.h"
#include "tdataformat.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SReadH SReadH;

typedef struct {
  int32_t  tid;
  uint32_t len;
  uint32_t offset;
  uint32_t hasLast : 2;
  uint32_t numOfBlocks : 30;
  uint64_t uid;
  TSKEY    maxKey;
} SBlockIdx;

typedef struct {
  int64_t last : 1;
  int64_t offset : 63;
  int32_t algorithm : 8;
  int32_t numOfRows : 24;
  int32_t len;
  int32_t keyLen;     // key column length, keyOffset = offset+sizeof(SBlockData)+sizeof(SBlockCol)*numOfCols
  int16_t numOfSubBlocks;
  int16_t numOfCols; // not including timestamp column
  TSKEY   keyFirst;
  TSKEY   keyLast;
} SBlock;

typedef struct {
  int32_t    delimiter;  // For recovery usage
  int32_t    tid;
  uint64_t   uid;
  SBlock blocks[];
} SBlockInfo;

typedef struct {
  int16_t colId;
  int32_t len;
  int32_t type : 8;
  int32_t offset : 24;
  int64_t sum;
  int64_t max;
  int64_t min;
  int16_t maxIndex;
  int16_t minIndex;
  int16_t numOfNull;
  char    padding[2];
} SBlockCol;

typedef struct {
  int32_t  delimiter;  // For recovery usage
  int32_t  numOfCols;  // For recovery usage
  uint64_t uid;        // For recovery usage
  SBlockCol cols[];
} SBlockData;

struct SReadH {
  STsdbRepo * pRepo;
  SDFileSet   rSet;  // File set
  SArray *    aBlkIdx;
  STable *    pTable;  // Table info
  SBlockIdx * pBlkIdx;
  int         cidx;
  SBlockInfo *pBlkInfo;
  SBlockData *pBlkData;  // Block info
  SDataCols * pDCols[2];
  void *      pBuf;
  void *      pCBuf;
};

#define TSDB_READ_REPO(rh) ((rh)->pRepo)
#define TSDB_READ_REPO_ID(rh) REPO_ID(TSDB_READ_REPO(rh))
#define TSDB_READ_FSET(rh) &((rh)->rSet)
#define TSDB_READ_TABLE(ch) ((rh)->pTable)
#define TSDB_READ_HEAD_FILE(rh) TSDB_DFILE_IN_SET(TSDB_READ_FSET(rh), TSDB_FILE_HEAD)
#define TSDB_READ_DATA_FILE(rh) TSDB_DFILE_IN_SET(TSDB_READ_FSET(rh), TSDB_FILE_DATA)
#define TSDB_READ_LAST_FILE(rh) TSDB_DFILE_IN_SET(TSDB_READ_FSET(rh), TSDB_FILE_LAST)
#define TSDB_READ_BUF(rh) ((rh)->pBuf)
#define TSDB_READ_COMP_BUF(rh) ((rh)->pCBuf)

#define TSDB_BLOCK_STATIS_SIZE(ncols) (sizeof(SBlockData) + sizeof(SBlockCol) * (ncols) + sizeof(TSCKSUM))

int   tsdbInitReadH(SReadH *pReadh, STsdbRepo *pRepo);
void  tsdbDestroyReadH(SReadH *pReadh);
int   tsdbSetAndOpenReadFSet(SReadH *pReadh, SDFileSet *pSet);
void  tsdbCloseAndUnsetFSet(SReadH *pReadh);
int   tsdbLoadBlockIdx(SReadH *pReadh);
int   tsdbSetReadTable(SReadH *pReadh, STable *pTable);
int   tsdbLoadBlockInfo(SReadH *pReadh, void *pTarget);
int   tsdbLoadBlockData(SReadH *pReadh, const SBlock *pBlock, const SBlockInfo *pBlockInfo);
int   tsdbLoadBlockDataCols(SReadH *pReadh, const SBlock *pBlock, const SBlockInfo *pBlockInfo, const int16_t *colIds,
                            const int numOfColsIds);
int   tsdbLoadBlockStatis(SReadH *pReadh, SBlock *pBlock);
int   tsdbEncodeSBlockIdx(void **buf, SBlockIdx *pIdx);
void *tsdbDecodeSBlockIdx(void *buf, SBlockIdx *pIdx);
void  tsdbGetBlockStatis(SReadH *pReadh, SDataStatis *pStatis, int numOfCols);

static FORCE_INLINE int tsdbMakeRoom(void **ppBuf, size_t size) {
  void * pBuf = *ppBuf;
  size_t tsize = taosTSizeof(pBuf);

  if (tsize < size) {
    if (tsize == 0) tsize = 1024;

    while (tsize < size) {
      tsize *= 2;
    }

    *ppBuf = taosTRealloc(pBuf, tsize);
    if (*ppBuf == NULL) {
      terrno = TSDB_CODE_TDB_OUT_OF_MEMORY;
      return -1;
    }
  }

  return 0;
}

#ifdef __cplusplus
}
#endif

#endif /*_TD_TSDB_READ_IMPL_H_*/