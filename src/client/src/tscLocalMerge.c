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

#include "tscLocalMerge.h"
#include "tscSubquery.h"
#include "os.h"
#include "texpr.h"
#include "tlosertree.h"
#include "tscLog.h"
#include "tscUtil.h"
#include "tschemautil.h"
#include "tsclient.h"

typedef struct SCompareParam {
  SLocalDataSource **pLocalData;
  tOrderDescriptor * pDesc;
  int32_t            num;
  int32_t            groupOrderType;
} SCompareParam;

bool needToMergeRv(SSDataBlock* pBlock, SLocalMerger *pLocalMerge, int32_t index, char **buf);

int32_t treeComparator(const void *pLeft, const void *pRight, void *param) {
  int32_t pLeftIdx = *(int32_t *)pLeft;
  int32_t pRightIdx = *(int32_t *)pRight;

  SCompareParam *    pParam = (SCompareParam *)param;
  tOrderDescriptor * pDesc = pParam->pDesc;
  SLocalDataSource **pLocalData = pParam->pLocalData;

  /* this input is exhausted, set the special value to denote this */
  if (pLocalData[pLeftIdx]->rowIdx == -1) {
    return 1;
  }

  if (pLocalData[pRightIdx]->rowIdx == -1) {
    return -1;
  }

  if (pParam->groupOrderType == TSDB_ORDER_DESC) {  // desc
    return compare_d(pDesc, pParam->num, pLocalData[pLeftIdx]->rowIdx, pLocalData[pLeftIdx]->filePage.data,
                     pParam->num, pLocalData[pRightIdx]->rowIdx, pLocalData[pRightIdx]->filePage.data);
  } else {
    return compare_a(pDesc, pParam->num, pLocalData[pLeftIdx]->rowIdx, pLocalData[pLeftIdx]->filePage.data,
                     pParam->num, pLocalData[pRightIdx]->rowIdx, pLocalData[pRightIdx]->filePage.data);
  }
}

// todo merge with vnode side function
void tsCreateSQLFunctionCtx(SQueryInfo* pQueryInfo, SQLFunctionCtx* pCtx, SSchema* pSchema) {
  size_t size = tscSqlExprNumOfExprs(pQueryInfo);
  
  for (int32_t i = 0; i < size; ++i) {
    SExprInfo *pExpr = tscSqlExprGet(pQueryInfo, i);

    pCtx[i].order = pQueryInfo->order.order;
    pCtx[i].functionId = pExpr->base.functionId;

    pCtx[i].order = pQueryInfo->order.order;
    pCtx[i].functionId = pExpr->base.functionId;

    // input data format comes from pModel
    pCtx[i].inputType = pSchema[i].type;
    pCtx[i].inputBytes = pSchema[i].bytes;

    pCtx[i].outputBytes = pExpr->base.resBytes;
    pCtx[i].outputType  = pExpr->base.resType;

    // input buffer hold only one point data
    pCtx[i].size = 1;
    pCtx[i].hasNull = true;
    pCtx[i].currentStage = MERGE_STAGE;

    // for top/bottom function, the output of timestamp is the first column
    int32_t functionId = pExpr->base.functionId;
    if (functionId == TSDB_FUNC_TOP || functionId == TSDB_FUNC_BOTTOM || functionId == TSDB_FUNC_DIFF) {
      pCtx[i].ptsOutputBuf = pCtx[0].pOutput;
      pCtx[i].param[2].i64 = pQueryInfo->order.order;
      pCtx[i].param[2].nType  = TSDB_DATA_TYPE_BIGINT;
      pCtx[i].param[1].i64 = pQueryInfo->order.orderColId;
      pCtx[i].param[0].i64 = pExpr->base.param[0].i64;  // top/bot parameter
    } else if (functionId == TSDB_FUNC_APERCT) {
      pCtx[i].param[0].i64 = pExpr->base.param[0].i64;
      pCtx[i].param[0].nType  = pExpr->base.param[0].nType;
    } else if (functionId == TSDB_FUNC_BLKINFO) {
      pCtx[i].param[0].i64 = pExpr->base.param[0].i64;
      pCtx[i].param[0].nType = pExpr->base.param[0].nType;
      pCtx[i].numOfParams = 1;
    }

    pCtx[i].interBufBytes = pExpr->base.interBytes;
    pCtx[i].resultInfo = calloc(1, pCtx[i].interBufBytes + sizeof(SResultRowCellInfo));
    pCtx[i].stableQuery = true;
  }

  int16_t          n = 0;
  int16_t          tagLen = 0;
  SQLFunctionCtx **pTagCtx = calloc(pQueryInfo->fieldsInfo.numOfOutput, POINTER_BYTES);

  SQLFunctionCtx *pCtx1 = NULL;
  for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutput; ++i) {
    SExprInfo *pExpr = tscSqlExprGet(pQueryInfo, i);
    if (pExpr->base.functionId == TSDB_FUNC_TAG_DUMMY || pExpr->base.functionId == TSDB_FUNC_TS_DUMMY) {
      tagLen += pExpr->base.resBytes;
      pTagCtx[n++] = &pCtx[i];
    } else if ((aAggs[pExpr->base.functionId].status & TSDB_FUNCSTATE_SELECTIVITY) != 0) {
      pCtx1 = &pCtx[i];
    }
  }

  if (n == 0 || pCtx == NULL) {
    free(pTagCtx);
  } else {
    pCtx1->tagInfo.pTagCtxList = pTagCtx;
    pCtx1->tagInfo.numOfTagCols = n;
    pCtx1->tagInfo.tagsLen = tagLen;
  }
}

static UNUSED_FUNC void setCtxInputOutputBuffer(SQueryInfo *pQueryInfo, SQLFunctionCtx *pCtx, SLocalMerger *pMerger,
                                    tOrderDescriptor *pDesc) {
  size_t size = tscSqlExprNumOfExprs(pQueryInfo);

  for (int32_t i = 0; i < size; ++i) {
    SExprInfo *pExpr = tscSqlExprGet(pQueryInfo, i);
    pCtx[i].pOutput = pMerger->pResultBuf->data + pExpr->base.offset * pMerger->resColModel->capacity;

    // input buffer hold only one point data
    int16_t offset = getColumnModelOffset(pDesc->pColumnModel, i);
    pCtx[i].pInput = pMerger->pTempBuffer->data + offset;

    int32_t functionId = pCtx[i].functionId;
    if (functionId == TSDB_FUNC_TOP || functionId == TSDB_FUNC_BOTTOM || functionId == TSDB_FUNC_DIFF) {
      pCtx[i].ptsOutputBuf = pCtx[0].pOutput;
    }
  }
}

static SFillColInfo* createFillColInfo(SQueryInfo* pQueryInfo) {
  int32_t numOfCols = (int32_t)tscNumOfFields(pQueryInfo);
  int32_t offset = 0;
  
  SFillColInfo* pFillCol = calloc(numOfCols, sizeof(SFillColInfo));
  for(int32_t i = 0; i < numOfCols; ++i) {
    SInternalField* pIField = taosArrayGet(pQueryInfo->fieldsInfo.internalField, i);

    if (pIField->pExpr->pExpr == NULL) {
      SExprInfo* pExpr = pIField->pExpr;

      pFillCol[i].col.bytes  = pExpr->base.resBytes;
      pFillCol[i].col.type   = (int8_t)pExpr->base.resType;
      pFillCol[i].col.colId  = pExpr->base.colInfo.colId;
      pFillCol[i].flag       = pExpr->base.colInfo.flag;
      pFillCol[i].col.offset = offset;
      pFillCol[i].functionId = pExpr->base.functionId;
      pFillCol[i].fillVal.i  = pQueryInfo->fillVal[i];
    } else {
      pFillCol[i].col.bytes  = pIField->field.bytes;
      pFillCol[i].col.type   = (int8_t)pIField->field.type;
      pFillCol[i].col.colId  = -100;
      pFillCol[i].flag       = TSDB_COL_NORMAL;
      pFillCol[i].col.offset = offset;
      pFillCol[i].functionId = -1;
      pFillCol[i].fillVal.i  = pQueryInfo->fillVal[i];
    }

    offset += pFillCol[i].col.bytes;
  }
  
  return pFillCol;
}

void tscCreateLocalMerger(tExtMemBuffer **pMemBuffer, int32_t numOfBuffer, tOrderDescriptor *pDesc,
                          SColumnModel *finalmodel, SColumnModel *pFFModel, SSqlObj *pSql) {
  SSqlCmd* pCmd = &pSql->cmd;
  SSqlRes* pRes = &pSql->res;
  
  if (pMemBuffer == NULL) {
    tscLocalReducerEnvDestroy(pMemBuffer, pDesc, finalmodel, pFFModel, numOfBuffer);
    tscError("%p pMemBuffer is NULL", pMemBuffer);
    pRes->code = TSDB_CODE_TSC_APP_ERROR;
    return;
  }
 
  if (pDesc->pColumnModel == NULL) {
    tscLocalReducerEnvDestroy(pMemBuffer, pDesc, finalmodel, pFFModel, numOfBuffer);
    tscError("%p no local buffer or intermediate result format model", pSql);
    pRes->code = TSDB_CODE_TSC_APP_ERROR;
    return;
  }

  int32_t numOfFlush = 0;
  for (int32_t i = 0; i < numOfBuffer; ++i) {
    int32_t len = pMemBuffer[i]->fileMeta.flushoutData.nLength;
    if (len == 0) {
      tscDebug("%p no data retrieved from orderOfVnode:%d", pSql, i + 1);
      continue;
    }

    numOfFlush += len;
  }

  if (numOfFlush == 0 || numOfBuffer == 0) {
    tscLocalReducerEnvDestroy(pMemBuffer, pDesc, finalmodel, pFFModel, numOfBuffer);
    pCmd->command = TSDB_SQL_RETRIEVE_EMPTY_RESULT; // no result, set the result empty
    tscDebug("%p retrieved no data", pSql);
    return;
  }

  if (pDesc->pColumnModel->capacity >= pMemBuffer[0]->pageSize) {
    tscError("%p Invalid value of buffer capacity %d and page size %d ", pSql, pDesc->pColumnModel->capacity,
             pMemBuffer[0]->pageSize);

    tscLocalReducerEnvDestroy(pMemBuffer, pDesc, finalmodel, pFFModel, numOfBuffer);
    pRes->code = TSDB_CODE_TSC_APP_ERROR;
    return;
  }

  size_t size = sizeof(SLocalMerger) + POINTER_BYTES * numOfFlush;
  
  SLocalMerger *pMerger = (SLocalMerger *) calloc(1, size);
  if (pMerger == NULL) {
    tscError("%p failed to create local merge structure, out of memory", pSql);

    tscLocalReducerEnvDestroy(pMemBuffer, pDesc, finalmodel, pFFModel, numOfBuffer);
    pRes->code = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return;
  }

  pMerger->pExtMemBuffer = pMemBuffer;
  pMerger->pLocalDataSrc = (SLocalDataSource **)&pMerger[1];
  assert(pMerger->pLocalDataSrc != NULL);

  pMerger->numOfBuffer = numOfFlush;
  pMerger->numOfVnode = numOfBuffer;

  pMerger->pDesc = pDesc;
  tscDebug("%p the number of merged leaves is: %d", pSql, pMerger->numOfBuffer);

  int32_t idx = 0;
  for (int32_t i = 0; i < numOfBuffer; ++i) {
    int32_t numOfFlushoutInFile = pMemBuffer[i]->fileMeta.flushoutData.nLength;

    for (int32_t j = 0; j < numOfFlushoutInFile; ++j) {
      SLocalDataSource *ds = (SLocalDataSource *)malloc(sizeof(SLocalDataSource) + pMemBuffer[0]->pageSize);
      if (ds == NULL) {
        tscError("%p failed to create merge structure", pSql);
        pRes->code = TSDB_CODE_TSC_OUT_OF_MEMORY;
        tfree(pMerger);
        return;
      }
      
      pMerger->pLocalDataSrc[idx] = ds;

      ds->pMemBuffer = pMemBuffer[i];
      ds->flushoutIdx = j;
      ds->filePage.num = 0;
      ds->pageId = 0;
      ds->rowIdx = 0;

      tscDebug("%p load data from disk into memory, orderOfVnode:%d, total:%d", pSql, i + 1, idx + 1);
      tExtMemBufferLoadData(pMemBuffer[i], &(ds->filePage), j, 0);
#ifdef _DEBUG_VIEW
      printf("load data page into mem for build loser tree: %" PRIu64 " rows\n", ds->filePage.num);
      SSrcColumnInfo colInfo[256] = {0};
      SQueryInfo *   pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);

      tscGetSrcColumnInfo(colInfo, pQueryInfo);

      tColModelDisplayEx(pDesc->pColumnModel, ds->filePage.data, ds->filePage.num,
                         pMemBuffer[0]->numOfElemsPerPage, colInfo);
#endif
      
      if (ds->filePage.num == 0) {  // no data in this flush, the index does not increase
        tscDebug("%p flush data is empty, ignore %d flush record", pSql, idx);
        tfree(ds);
        continue;
      }
      
      idx += 1;
    }
  }
  
  // no data actually, no need to merge result.
  if (idx == 0) {
    tfree(pMerger);
    return;
  }

  pMerger->numOfBuffer = idx;

  SCompareParam *param = malloc(sizeof(SCompareParam));
  if (param == NULL) {
    tfree(pMerger);
    return;
  }

  param->pLocalData = pMerger->pLocalDataSrc;
  param->pDesc = pMerger->pDesc;
  param->num = pMerger->pLocalDataSrc[0]->pMemBuffer->numOfElemsPerPage;
  SQueryInfo *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);

  param->groupOrderType = pQueryInfo->groupbyExpr.orderType;
  pMerger->orderPrjOnSTable = tscOrderedProjectionQueryOnSTable(pQueryInfo, 0);

  pRes->code = tLoserTreeCreate(&pMerger->pLoserTree, pMerger->numOfBuffer, param, treeComparator);
  if (pMerger->pLoserTree == NULL || pRes->code != 0) {
    tfree(param);
    tfree(pMerger);
    return;
  }

  // the input data format follows the old format, but output in a new format.
  // so, all the input must be parsed as old format
  pMerger->pCtx = (SQLFunctionCtx *)calloc(tscSqlExprNumOfExprs(pQueryInfo), sizeof(SQLFunctionCtx));
  pMerger->rowSize = pMemBuffer[0]->nElemSize;

//  tscRestoreFuncForSTableQuery(pQueryInfo);
  tscFieldInfoUpdateOffset(pQueryInfo);

  if (pMerger->rowSize > pMemBuffer[0]->pageSize) {
    assert(false);  // todo fixed row size is larger than the minimum page size;
  }

  pMerger->hasPrevRow = false;
  pMerger->hasUnprocessedRow = false;

  pMerger->prevRowOfInput = (char *)calloc(1, pMerger->rowSize);

  // used to keep the latest input row
  pMerger->pTempBuffer = (tFilePage *)calloc(1, pMerger->rowSize + sizeof(tFilePage));
  pMerger->discardData = (tFilePage *)calloc(1, pMerger->rowSize + sizeof(tFilePage));
  pMerger->discard = false;

  pMerger->nResultBufSize = pMemBuffer[0]->pageSize * 16;
  pMerger->pResultBuf = (tFilePage *)calloc(1, pMerger->nResultBufSize + sizeof(tFilePage));

  pMerger->resColModel = finalmodel;
  pMerger->resColModel->capacity = pMerger->nResultBufSize;
  pMerger->finalModel = pFFModel;

  if (finalmodel->rowSize > 0) {
    pMerger->resColModel->capacity /= finalmodel->rowSize;
  }

  assert(finalmodel->rowSize > 0 && finalmodel->rowSize <= pMerger->rowSize);
  pMerger->pFinalRes = calloc(1, pMerger->rowSize * pMerger->resColModel->capacity);

  if (pMerger->pTempBuffer == NULL || pMerger->discardData == NULL || pMerger->pResultBuf == NULL ||
      pMerger->pFinalRes == NULL || pMerger->prevRowOfInput == NULL) {
    tfree(pMerger->pTempBuffer);
    tfree(pMerger->discardData);
    tfree(pMerger->pResultBuf);
    tfree(pMerger->pFinalRes);
    tfree(pMerger->prevRowOfInput);
    tfree(pMerger->pLoserTree);
    tfree(param);
    tfree(pMerger);
    pRes->code = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return;
  }
  
  pMerger->pTempBuffer->num = 0;

  tscCreateResPointerInfo(pRes, pQueryInfo);

  SSchema* pschema = calloc(pDesc->pColumnModel->numOfCols, sizeof(SSchema));
  for(int32_t i = 0; i < pDesc->pColumnModel->numOfCols; ++i) {
    pschema[i] = pDesc->pColumnModel->pFields[i].field;
  }

  tsCreateSQLFunctionCtx(pQueryInfo, pMerger->pCtx, pschema);
//  setCtxInputOutputBuffer(pQueryInfo, pMerger->pCtx, pMerger, pDesc);

  tfree(pschema);

  int32_t maxBufSize = 0;
  for (int32_t k = 0; k < tscSqlExprNumOfExprs(pQueryInfo); ++k) {
    SExprInfo *pExpr = tscSqlExprGet(pQueryInfo, k);
    if (maxBufSize < pExpr->base.resBytes && pExpr->base.functionId == TSDB_FUNC_TAG) {
      maxBufSize = pExpr->base.resBytes;
    }
  }

  pMerger->tagBuf = calloc(1, maxBufSize);

  // we change the capacity of schema to denote that there is only one row in temp buffer
  pMerger->pDesc->pColumnModel->capacity = 1;

  // restore the limitation value at the last stage
  if (tscOrderedProjectionQueryOnSTable(pQueryInfo, 0)) {
    pQueryInfo->limit.limit = pQueryInfo->clauseLimit;
    pQueryInfo->limit.offset = pQueryInfo->prjOffset;
  }

  pMerger->offset = (int32_t)pQueryInfo->limit.offset;

  pRes->pLocalMerger = pMerger;
  pRes->numOfGroups = 0;

  STableMetaInfo *pTableMetaInfo = tscGetTableMetaInfoFromCmd(pCmd, pCmd->clauseIndex, 0);
  STableComInfo tinfo = tscGetTableInfo(pTableMetaInfo->pTableMeta);
  
  TSKEY stime = (pQueryInfo->order.order == TSDB_ORDER_ASC)? pQueryInfo->window.skey : pQueryInfo->window.ekey;
  int64_t revisedSTime = taosTimeTruncate(stime, &pQueryInfo->interval, tinfo.precision);
  
  if (pQueryInfo->fillType != TSDB_FILL_NONE) {
    SFillColInfo* pFillCol = createFillColInfo(pQueryInfo);
    pMerger->pFillInfo =
        taosCreateFillInfo(pQueryInfo->order.order, revisedSTime, pQueryInfo->groupbyExpr.numOfGroupCols, 4096,
                           (int32_t)pQueryInfo->fieldsInfo.numOfOutput, pQueryInfo->interval.sliding,
                           pQueryInfo->interval.slidingUnit, tinfo.precision, pQueryInfo->fillType, pFillCol, pSql);
  }
}

static int32_t tscFlushTmpBufferImpl(tExtMemBuffer *pMemoryBuf, tOrderDescriptor *pDesc, tFilePage *pPage,
                                     int32_t orderType) {
  if (pPage->num == 0) {
    return 0;
  }

  assert(pPage->num <= pDesc->pColumnModel->capacity);

  // sort before flush to disk, the data must be consecutively put on tFilePage.
  if (pDesc->orderInfo.numOfCols > 0) {
    tColDataQSort(pDesc, (int32_t)pPage->num, 0, (int32_t)pPage->num - 1, pPage->data, orderType);
  }

#ifdef _DEBUG_VIEW
  printf("%" PRIu64 " rows data flushed to disk after been sorted:\n", pPage->num);
  tColModelDisplay(pDesc->pColumnModel, pPage->data, pPage->num, pPage->num);
#endif

  // write to cache after being sorted
  if (tExtMemBufferPut(pMemoryBuf, pPage->data, (int32_t)pPage->num) < 0) {
    tscError("failed to save data in temporary buffer");
    return -1;
  }

  pPage->num = 0;
  return 0;
}

int32_t tscFlushTmpBuffer(tExtMemBuffer *pMemoryBuf, tOrderDescriptor *pDesc, tFilePage *pPage, int32_t orderType) {
  int32_t ret = 0;
  if ((ret = tscFlushTmpBufferImpl(pMemoryBuf, pDesc, pPage, orderType)) != 0) {
    return ret;
  }

  if ((ret = tExtMemBufferFlush(pMemoryBuf)) != 0) {
    return ret;
  }

  return 0;
}

int32_t saveToBuffer(tExtMemBuffer *pMemoryBuf, tOrderDescriptor *pDesc, tFilePage *pPage, void *data,
                     int32_t numOfRows, int32_t orderType) {
  SColumnModel *pModel = pDesc->pColumnModel;

  if (pPage->num + numOfRows <= pModel->capacity) {
    tColModelAppend(pModel, pPage, data, 0, numOfRows, numOfRows);
    return 0;
  }

  // current buffer is overflow, flush data to extensive buffer
  int32_t numOfRemainEntries = pModel->capacity - (int32_t)pPage->num;
  tColModelAppend(pModel, pPage, data, 0, numOfRemainEntries, numOfRows);

  // current buffer is full, need to flushed to disk
  assert(pPage->num == pModel->capacity);
  int32_t code = tscFlushTmpBuffer(pMemoryBuf, pDesc, pPage, orderType);
  if (code != 0) {
    return code;
  }

  int32_t remain = numOfRows - numOfRemainEntries;

  while (remain > 0) {
    int32_t numOfWriteElems = 0;
    if (remain > pModel->capacity) {
      numOfWriteElems = pModel->capacity;
    } else {
      numOfWriteElems = remain;
    }

    tColModelAppend(pModel, pPage, data, numOfRows - remain, numOfWriteElems, numOfRows);

    if (pPage->num == pModel->capacity) {
      if ((code = tscFlushTmpBuffer(pMemoryBuf, pDesc, pPage, orderType)) != TSDB_CODE_SUCCESS) {
        return code;
      }
    } else {
      pPage->num = numOfWriteElems;
    }

    remain -= numOfWriteElems;
    numOfRemainEntries += numOfWriteElems;
  }

  return 0;
}

void tscDestroyLocalMerger(SSqlObj *pSql) {
  if (pSql == NULL) {
    return;
  }

  SSqlRes *pRes = &(pSql->res);
  if (pRes->pLocalMerger == NULL) {
    return;
  }

  SSqlCmd *   pCmd = &pSql->cmd;
  SQueryInfo *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);

  // there is no more result, so we release all allocated resource
  SLocalMerger *pLocalMerge = (SLocalMerger *)atomic_exchange_ptr(&pRes->pLocalMerger, NULL);
  if (pLocalMerge != NULL) {
    pLocalMerge->pFillInfo = taosDestroyFillInfo(pLocalMerge->pFillInfo);

    if (pLocalMerge->pCtx != NULL) {
      int32_t numOfExprs = (int32_t) tscSqlExprNumOfExprs(pQueryInfo);
      for (int32_t i = 0; i < numOfExprs; ++i) {
        SQLFunctionCtx *pCtx = &pLocalMerge->pCtx[i];

        tVariantDestroy(&pCtx->tag);
        tfree(pCtx->resultInfo);

        if (pCtx->tagInfo.pTagCtxList != NULL) {
          tfree(pCtx->tagInfo.pTagCtxList);
        }
      }

      tfree(pLocalMerge->pCtx);
    }

    tfree(pLocalMerge->prevRowOfInput);

    tfree(pLocalMerge->pTempBuffer);
    tfree(pLocalMerge->pResultBuf);

    if (pLocalMerge->pLoserTree) {
      tfree(pLocalMerge->pLoserTree->param);
      tfree(pLocalMerge->pLoserTree);
    }

    tfree(pLocalMerge->pFinalRes);
    tfree(pLocalMerge->discardData);

    tscLocalReducerEnvDestroy(pLocalMerge->pExtMemBuffer, pLocalMerge->pDesc, pLocalMerge->resColModel, pLocalMerge->finalModel,
                              pLocalMerge->numOfVnode);
    for (int32_t i = 0; i < pLocalMerge->numOfBuffer; ++i) {
      tfree(pLocalMerge->pLocalDataSrc[i]);
    }

    pLocalMerge->numOfBuffer = 0;
    pLocalMerge->numOfCompleted = 0;
    free(pLocalMerge);
  } else {
    tscDebug("%p already freed or another free function is invoked", pSql);
  }

  tscDebug("%p free local reducer finished", pSql);
}

static int32_t createOrderDescriptor(tOrderDescriptor **pOrderDesc, SSqlCmd *pCmd, SColumnModel *pModel) {
  int32_t     numOfGroupByCols = 0;
  SQueryInfo *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);

  if (pQueryInfo->groupbyExpr.numOfGroupCols > 0) {
    numOfGroupByCols = pQueryInfo->groupbyExpr.numOfGroupCols;
  }

  // primary timestamp column is involved in final result
  if (pQueryInfo->interval.interval != 0 || tscOrderedProjectionQueryOnSTable(pQueryInfo, 0)) {
    numOfGroupByCols++;
  }

  int32_t *orderColIndexList = (int32_t *)calloc(numOfGroupByCols, sizeof(int32_t));
  if (orderColIndexList == NULL) {
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }

  if (numOfGroupByCols > 0) {

    if (pQueryInfo->groupbyExpr.numOfGroupCols > 0) {
      int32_t numOfInternalOutput = (int32_t) tscSqlExprNumOfExprs(pQueryInfo);
      int32_t startCols = numOfInternalOutput - pQueryInfo->groupbyExpr.numOfGroupCols;

      // the last "pQueryInfo->groupbyExpr.numOfGroupCols" columns are order-by columns
      for (int32_t i = 0; i < pQueryInfo->groupbyExpr.numOfGroupCols; ++i) {
        orderColIndexList[i] = startCols++;
      }

      if (pQueryInfo->interval.interval != 0) {
        // the first column is the timestamp, handles queries like "interval(10m) group by tags"
        orderColIndexList[numOfGroupByCols - 1] = PRIMARYKEY_TIMESTAMP_COL_INDEX; //TODO ???
      }
    } else {
      /*
       * 1. the orderby ts asc/desc projection query for the super table
       * 2. interval query without groupby clause
       */
      if (pQueryInfo->interval.interval != 0) {
        orderColIndexList[0] = PRIMARYKEY_TIMESTAMP_COL_INDEX;
      } else {
        size_t size = tscSqlExprNumOfExprs(pQueryInfo);
        for (int32_t i = 0; i < size; ++i) {
          SExprInfo *pExpr = tscSqlExprGet(pQueryInfo, i);
          if (pExpr->base.functionId == TSDB_FUNC_PRJ && pExpr->base.colInfo.colId == PRIMARYKEY_TIMESTAMP_COL_INDEX) {
            orderColIndexList[0] = i;
          }
        }
      }

      assert(pQueryInfo->order.orderColId == PRIMARYKEY_TIMESTAMP_COL_INDEX);
    }
  }

  *pOrderDesc = tOrderDesCreate(orderColIndexList, numOfGroupByCols, pModel, pQueryInfo->order.order);
  tfree(orderColIndexList);

  if (*pOrderDesc == NULL) {
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  } else {
    return TSDB_CODE_SUCCESS;
  }
}

bool isSameGroup(SSqlCmd *pCmd, SLocalMerger *pMerger, char *pPrev, tFilePage *tmpBuffer) {
  SQueryInfo *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);

  // disable merge procedure for column projection query
  int16_t functionId = pMerger->pCtx[0].functionId;
  if (pMerger->orderPrjOnSTable) {
    return true;
  }

  if (functionId == TSDB_FUNC_PRJ || functionId == TSDB_FUNC_ARITHM) {
    return false;
  }

  tOrderDescriptor *pOrderDesc = pMerger->pDesc;
  SColumnOrderInfo* orderInfo = &pOrderDesc->orderInfo;

  // no group by columns, all data belongs to one group
  int32_t numOfCols = orderInfo->numOfCols;
  if (numOfCols <= 0) {
    return true;
  }

  if (orderInfo->colIndex[numOfCols - 1] == PRIMARYKEY_TIMESTAMP_COL_INDEX) {
    /*
     * super table interval query
     * if the order columns is the primary timestamp, all result data belongs to one group
     */
    assert(pQueryInfo->interval.interval > 0);
    if (numOfCols == 1) {
      return true;
    }
  } else {  // simple group by query
    assert(pQueryInfo->interval.interval == 0);
  }

  // only one row exists
  int32_t index = orderInfo->colIndex[0];
  int32_t offset = (pOrderDesc->pColumnModel)->pFields[index].offset;

  int32_t ret = memcmp(pPrev + offset, tmpBuffer->data + offset, pOrderDesc->pColumnModel->rowSize - offset);
  return ret == 0;
}

int32_t tscLocalReducerEnvCreate(SSqlObj *pSql, tExtMemBuffer ***pMemBuffer, tOrderDescriptor **pOrderDesc,
                                 SColumnModel **pFinalModel, SColumnModel** pFFModel, uint32_t nBufferSizes) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  SSchema *     pSchema = NULL;
  SColumnModel *pModel = NULL;
  *pFinalModel = NULL;

  SQueryInfo *    pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);
  STableMetaInfo *pTableMetaInfo = tscGetMetaInfo(pQueryInfo, 0);

  (*pMemBuffer) = (tExtMemBuffer **)malloc(POINTER_BYTES * pSql->subState.numOfSub);
  if (*pMemBuffer == NULL) {
    tscError("%p failed to allocate memory", pSql);
    pRes->code = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return pRes->code;
  }
  
  size_t size = tscSqlExprNumOfExprs(pQueryInfo);
  
  pSchema = (SSchema *)calloc(1, sizeof(SSchema) * size);
  if (pSchema == NULL) {
    tscError("%p failed to allocate memory", pSql);
    pRes->code = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return pRes->code;
  }

  int32_t rlen = 0;
  for (int32_t i = 0; i < size; ++i) {
    SExprInfo *pExpr = tscSqlExprGet(pQueryInfo, i);

    pSchema[i].bytes = pExpr->base.resBytes;
    pSchema[i].type = (int8_t)pExpr->base.resType;
    tstrncpy(pSchema[i].name, pExpr->base.aliasName, tListLen(pSchema[i].name));

    rlen += pExpr->base.resBytes;
  }

  int32_t capacity = 0;
  if (rlen != 0) {
    capacity = nBufferSizes / rlen;
  }
  
  pModel = createColumnModel(pSchema, (int32_t)size, capacity);

  int32_t pg = DEFAULT_PAGE_SIZE;
  int32_t overhead = sizeof(tFilePage);
  while((pg - overhead) < pModel->rowSize * 2) {
    pg *= 2;
  }

  size_t numOfSubs = pSql->subState.numOfSub;
  assert(numOfSubs <= pTableMetaInfo->vgroupList->numOfVgroups);
  for (int32_t i = 0; i < numOfSubs; ++i) {
    (*pMemBuffer)[i] = createExtMemBuffer(nBufferSizes, rlen, pg, pModel);
    (*pMemBuffer)[i]->flushModel = MULTIPLE_APPEND_MODEL;
  }

  if (createOrderDescriptor(pOrderDesc, pCmd, pModel) != TSDB_CODE_SUCCESS) {
    pRes->code = TSDB_CODE_TSC_OUT_OF_MEMORY;
    tfree(pSchema);
    return pRes->code;
  }

  // final result depends on the fields number
  memset(pSchema, 0, sizeof(SSchema) * size);

  for (int32_t i = 0; i < size; ++i) {
    SExprInfo *pExpr = tscSqlExprGet(pQueryInfo, i);

    SSchema p1 = {0};
    if (pExpr->base.colInfo.colIndex == TSDB_TBNAME_COLUMN_INDEX) {
      p1 = *tGetTbnameColumnSchema();
    } else if (TSDB_COL_IS_UD_COL(pExpr->base.colInfo.flag)) {
      p1.bytes = pExpr->base.resBytes;
      p1.type  = (uint8_t) pExpr->base.resType;
      tstrncpy(p1.name, pExpr->base.aliasName, tListLen(p1.name));
    } else {
      p1 = *tscGetTableColumnSchema(pTableMetaInfo->pTableMeta, pExpr->base.colInfo.colIndex);
    }

    int32_t inter = 0;
    int16_t type = -1;
    int16_t bytes = 0;

    // the final result size and type in the same as query on single table.
    // so here, set the flag to be false;
    int32_t functionId = pExpr->base.functionId;
    if (functionId >= TSDB_FUNC_TS && functionId <= TSDB_FUNC_DIFF) {
      type = pModel->pFields[i].field.type;
      bytes = pModel->pFields[i].field.bytes;
    } else {
      if (functionId == TSDB_FUNC_FIRST_DST) {
        functionId = TSDB_FUNC_FIRST;
      } else if (functionId == TSDB_FUNC_LAST_DST) {
        functionId = TSDB_FUNC_LAST;
      } else if (functionId == TSDB_FUNC_STDDEV_DST) {
        functionId = TSDB_FUNC_STDDEV;
      }

      int32_t ret = getResultDataInfo(p1.type, p1.bytes, functionId, 0, &type, &bytes, &inter, 0, false);
      assert(ret == TSDB_CODE_SUCCESS);
    }

    pSchema[i].type = (uint8_t)type;
    pSchema[i].bytes = bytes;
    strcpy(pSchema[i].name, pModel->pFields[i].field.name);
  }
  
  *pFinalModel = createColumnModel(pSchema, (int32_t)size, capacity);

  memset(pSchema, 0, sizeof(SSchema) * size);
  size = tscNumOfFields(pQueryInfo);

  for(int32_t i = 0; i < size; ++i) {
    SInternalField* pField = tscFieldInfoGetInternalField(&pQueryInfo->fieldsInfo, i);
    pSchema[i].bytes = pField->field.bytes;
    pSchema[i].type = pField->field.type;
    tstrncpy(pSchema[i].name, pField->field.name, tListLen(pSchema[i].name));
  }

  *pFFModel = createColumnModel(pSchema, (int32_t) size, capacity);

   tfree(pSchema);
  return TSDB_CODE_SUCCESS;
}

/**
 * @param pMemBuffer
 * @param pDesc
 * @param pFinalModel
 * @param numOfVnodes
 */
void tscLocalReducerEnvDestroy(tExtMemBuffer **pMemBuffer, tOrderDescriptor *pDesc, SColumnModel *pFinalModel, SColumnModel *pFFModel,
                               int32_t numOfVnodes) {
  destroyColumnModel(pFinalModel);
  destroyColumnModel(pFFModel);

  tOrderDescDestroy(pDesc);

  for (int32_t i = 0; i < numOfVnodes; ++i) {
    pMemBuffer[i] = destoryExtMemBuffer(pMemBuffer[i]);
  }

  tfree(pMemBuffer);
}

/**
 *
 * @param pLocalMerge
 * @param pOneInterDataSrc
 * @param treeList
 * @return the number of remain input source. if ret == 0, all data has been handled
 */
int32_t loadNewDataFromDiskFor(SLocalMerger *pLocalMerge, SLocalDataSource *pOneInterDataSrc,
                               bool *needAdjustLoserTree) {
  pOneInterDataSrc->rowIdx = 0;
  pOneInterDataSrc->pageId += 1;

  if ((uint32_t)pOneInterDataSrc->pageId <
      pOneInterDataSrc->pMemBuffer->fileMeta.flushoutData.pFlushoutInfo[pOneInterDataSrc->flushoutIdx].numOfPages) {
    tExtMemBufferLoadData(pOneInterDataSrc->pMemBuffer, &(pOneInterDataSrc->filePage), pOneInterDataSrc->flushoutIdx,
                          pOneInterDataSrc->pageId);

#if defined(_DEBUG_VIEW)
    printf("new page load to buffer\n");
    tColModelDisplay(pOneInterDataSrc->pMemBuffer->pColumnModel, pOneInterDataSrc->filePage.data,
                     pOneInterDataSrc->filePage.num, pOneInterDataSrc->pMemBuffer->pColumnModel->capacity);
#endif
    *needAdjustLoserTree = true;
  } else {
    pLocalMerge->numOfCompleted += 1;

    pOneInterDataSrc->rowIdx = -1;
    pOneInterDataSrc->pageId = -1;
    *needAdjustLoserTree = true;
  }

  return pLocalMerge->numOfBuffer;
}

void adjustLoserTreeFromNewData(SLocalMerger *pLocalMerge, SLocalDataSource *pOneInterDataSrc,
                                SLoserTreeInfo *pTree) {
  /*
   * load a new data page into memory for intermediate dataset source,
   * since it's last record in buffer has been chosen to be processed, as the winner of loser-tree
   */
  bool needToAdjust = true;
  if (pOneInterDataSrc->filePage.num <= pOneInterDataSrc->rowIdx) {
    loadNewDataFromDiskFor(pLocalMerge, pOneInterDataSrc, &needToAdjust);
  }

  /*
   * adjust loser tree otherwise, according to new candidate data
   * if the loser tree is rebuild completed, we do not need to adjust
   */
  if (needToAdjust) {
    int32_t leafNodeIdx = pTree->pNode[0].index + pLocalMerge->numOfBuffer;

#ifdef _DEBUG_VIEW
    printf("before adjust:\t");
    tLoserTreeDisplay(pTree);
#endif

    tLoserTreeAdjust(pTree, leafNodeIdx);

#ifdef _DEBUG_VIEW
    printf("\nafter adjust:\t");
    tLoserTreeDisplay(pTree);
    printf("\n");
#endif
  }
}

void savePrevRecordAndSetupFillInfo(SLocalMerger *pLocalMerge, SQueryInfo *pQueryInfo, SFillInfo *pFillInfo) {
  // discard following dataset in the same group and reset the interpolation information
  STableMetaInfo *pTableMetaInfo = tscGetMetaInfo(pQueryInfo, 0);

  STableComInfo tinfo = tscGetTableInfo(pTableMetaInfo->pTableMeta);

  if (pFillInfo != NULL) {
    int64_t stime = (pQueryInfo->window.skey < pQueryInfo->window.ekey) ? pQueryInfo->window.skey : pQueryInfo->window.ekey;
    int64_t revisedSTime = taosTimeTruncate(stime, &pQueryInfo->interval, tinfo.precision);
  
    taosResetFillInfo(pFillInfo, revisedSTime);
  }

  pLocalMerge->discard = true;
  pLocalMerge->discardData->num = 0;

  SColumnModel *pModel = pLocalMerge->pDesc->pColumnModel;
  tColModelAppend(pModel, pLocalMerge->discardData, pLocalMerge->prevRowOfInput, 0, 1, 1);
}

static void genFinalResWithoutFill(SSqlRes* pRes, SLocalMerger *pLocalMerge, SQueryInfo* pQueryInfo) {
  assert(pQueryInfo->interval.interval == 0 || pQueryInfo->fillType == TSDB_FILL_NONE);

  tFilePage * pBeforeFillData = pLocalMerge->pResultBuf;

  pRes->data = pLocalMerge->pFinalRes;
  pRes->numOfRows = (int32_t) pBeforeFillData->num;

  if (pQueryInfo->limit.offset > 0) {
    if (pQueryInfo->limit.offset < pRes->numOfRows) {
      int32_t prevSize = (int32_t) pBeforeFillData->num;
      tColModelErase(pLocalMerge->finalModel, pBeforeFillData, prevSize, 0, (int32_t)pQueryInfo->limit.offset - 1);

      /* remove the hole in column model */
      tColModelCompact(pLocalMerge->finalModel, pBeforeFillData, prevSize);

      pRes->numOfRows -= (int32_t) pQueryInfo->limit.offset;
      pQueryInfo->limit.offset = 0;
    } else {
      pQueryInfo->limit.offset -= pRes->numOfRows;
      pRes->numOfRows = 0;
    }
  }

  if (pRes->numOfRowsGroup >= pQueryInfo->limit.limit && pQueryInfo->limit.limit > 0) {
    pRes->numOfRows = 0;
    pBeforeFillData->num = 0;
    pLocalMerge->discard = true;
    return;
  }

  pRes->numOfRowsGroup += pRes->numOfRows;

  // impose the limitation of output rows on the final result
  if (pQueryInfo->limit.limit >= 0 && pRes->numOfRowsGroup > pQueryInfo->limit.limit) {
    int32_t prevSize = (int32_t)pBeforeFillData->num;
    int32_t overflow = (int32_t)(pRes->numOfRowsGroup - pQueryInfo->limit.limit);
    assert(overflow < pRes->numOfRows);

    pRes->numOfRowsGroup = pQueryInfo->limit.limit;
    pRes->numOfRows -= overflow;
    pBeforeFillData->num -= overflow;

    tColModelCompact(pLocalMerge->finalModel, pBeforeFillData, prevSize);

    // set remain data to be discarded, and reset the interpolation information
    savePrevRecordAndSetupFillInfo(pLocalMerge, pQueryInfo, pLocalMerge->pFillInfo);
  }

  memcpy(pRes->data, pBeforeFillData->data, (size_t)(pRes->numOfRows * pLocalMerge->finalModel->rowSize));

  pRes->numOfClauseTotal += pRes->numOfRows;
  pBeforeFillData->num = 0;
}

/*
 * Note: pRes->pLocalMerge may be null, due to the fact that "tscDestroyLocalMerger" is called
 * by "interuptHandler" function in shell
 */
static void doFillResult(SSqlObj *pSql, SLocalMerger *pLocalMerge, bool doneOutput) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;
  
  tFilePage  *pBeforeFillData = pLocalMerge->pResultBuf;
  SQueryInfo *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);
  SFillInfo  *pFillInfo = pLocalMerge->pFillInfo;

  // todo extract function
  int64_t actualETime = (pQueryInfo->order.order == TSDB_ORDER_ASC)? pQueryInfo->window.ekey: pQueryInfo->window.skey;

  void** pResPages = malloc(POINTER_BYTES * pQueryInfo->fieldsInfo.numOfOutput);
  for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutput; ++i) {
    TAOS_FIELD *pField = tscFieldInfoGetField(&pQueryInfo->fieldsInfo, i);
    pResPages[i] = calloc(1, pField->bytes * pLocalMerge->resColModel->capacity);
  }

  while (1) {
    int64_t newRows = taosFillResultDataBlock(pFillInfo, pResPages, pLocalMerge->resColModel->capacity);

    if (pQueryInfo->limit.offset < newRows) {
      newRows -= pQueryInfo->limit.offset;

      if (pQueryInfo->limit.offset > 0) {
        for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutput; ++i) {
          TAOS_FIELD *pField = tscFieldInfoGetField(&pQueryInfo->fieldsInfo, i);
          memmove(pResPages[i], ((char*)pResPages[i]) + pField->bytes * pQueryInfo->limit.offset,
                  (size_t)(newRows * pField->bytes));
        }
      }

      pRes->data = pLocalMerge->pFinalRes;
      pRes->numOfRows = (int32_t) newRows;

      pQueryInfo->limit.offset = 0;
      break;
    } else {
      pQueryInfo->limit.offset -= newRows;
      pRes->numOfRows = 0;

      if (!taosFillHasMoreResults(pFillInfo)) {
        if (!doneOutput) { // reduce procedure has not completed yet, but current results for fill are exhausted
          break;
        }

        // all output in current group are completed
        int32_t totalRemainRows = (int32_t)getNumOfResultsAfterFillGap(pFillInfo, actualETime, pLocalMerge->resColModel->capacity);
        if (totalRemainRows <= 0) {
          break;
        }
      }
    }
  }

  if (pRes->numOfRows > 0) {
    int32_t currentTotal = (int32_t)(pRes->numOfRowsGroup + pRes->numOfRows);

    if (pQueryInfo->limit.limit >= 0 && currentTotal > pQueryInfo->limit.limit) {
      int32_t overflow = (int32_t)(currentTotal - pQueryInfo->limit.limit);

      pRes->numOfRows -= overflow;
      assert(pRes->numOfRows >= 0);

      /* set remain data to be discarded, and reset the interpolation information */
      savePrevRecordAndSetupFillInfo(pLocalMerge, pQueryInfo, pFillInfo);
    }

    int32_t offset = 0;
    for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutput; ++i) {
      TAOS_FIELD *pField = tscFieldInfoGetField(&pQueryInfo->fieldsInfo, i);
      memcpy(pRes->data + offset * pRes->numOfRows, pResPages[i], (size_t)(pField->bytes * pRes->numOfRows));
      offset += pField->bytes;
    }

    pRes->numOfRowsGroup += pRes->numOfRows;
    pRes->numOfClauseTotal += pRes->numOfRows;
  }

  pBeforeFillData->num = 0;
  for (int32_t i = 0; i < pQueryInfo->fieldsInfo.numOfOutput; ++i) {
    tfree(pResPages[i]);
  }
  
  tfree(pResPages);
}

static void savePreviousRow(SLocalMerger *pLocalMerge, tFilePage *tmpBuffer) {
  SColumnModel *pColumnModel = pLocalMerge->pDesc->pColumnModel;
  assert(pColumnModel->capacity == 1 && tmpBuffer->num == 1);

  // copy to previous temp buffer
  for (int32_t i = 0; i < pColumnModel->numOfCols; ++i) {
    SSchema *pSchema = getColumnModelSchema(pColumnModel, i);
    int16_t  offset = getColumnModelOffset(pColumnModel, i);

    memcpy(pLocalMerge->prevRowOfInput + offset, tmpBuffer->data + offset, pSchema->bytes);
  }

  tmpBuffer->num = 0;
  pLocalMerge->hasPrevRow = true;
}

static void doExecuteFinalMerge( SLocalMerger *pLocalMerge, int32_t numOfExpr, bool needInit) {
  // the tag columns need to be set before all functions execution
  for (int32_t j = 0; j < numOfExpr; ++j) {
    SQLFunctionCtx *pCtx = &pLocalMerge->pCtx[j];

    // tags/tags_dummy function, the tag field of SQLFunctionCtx is from the input buffer
    int32_t functionId = pCtx->functionId;
    if (functionId == TSDB_FUNC_TAG_DUMMY || functionId == TSDB_FUNC_TAG || functionId == TSDB_FUNC_TS_DUMMY) {
      tVariantDestroy(&pCtx->tag);
      char* input = pCtx->pInput;
      
      if (pCtx->inputType == TSDB_DATA_TYPE_BINARY || pCtx->inputType == TSDB_DATA_TYPE_NCHAR) {
        assert(varDataLen(input) <= pCtx->inputBytes);
        tVariantCreateFromBinary(&pCtx->tag, varDataVal(input), varDataLen(input), pCtx->inputType);
      } else {
        tVariantCreateFromBinary(&pCtx->tag, input, pCtx->inputBytes, pCtx->inputType);
      }

    } else if (functionId == TSDB_FUNC_TOP || functionId == TSDB_FUNC_BOTTOM) {
//      SExprInfo *pExpr = tscSqlExprGet(pQueryInfo, j);  // TODO this data is from
//      pCtx->param[0].i64 = pExpr->base.param[0].i64;
    }

    pCtx->currentStage = MERGE_STAGE;

    if (needInit) {
      aAggs[pCtx->functionId].init(pCtx);
    }
  }

  for (int32_t j = 0; j < numOfExpr; ++j) {
    int32_t functionId = pLocalMerge->pCtx[j].functionId;
    if (functionId == TSDB_FUNC_TAG_DUMMY || functionId == TSDB_FUNC_TS_DUMMY) {
      continue;
    }

    aAggs[functionId].mergeFunc(&pLocalMerge->pCtx[j]);
  }
}

static void savePrevOrderColumns(SMultiwayMergeInfo* pInfo, SSDataBlock* pBlock, int32_t rowIndex) {
  int32_t size = pInfo->pMerge->pDesc->orderInfo.numOfCols;
  for(int32_t i = 0; i < size; ++i) {
    int32_t index = pInfo->pMerge->pDesc->orderInfo.colIndex[i];
//    int32_t index = *(int16_t*)taosArrayGet(pInfo->orderColumnList, i);
    SColumnInfoData* pColInfo = taosArrayGet(pBlock->pDataBlock, index);

    memcpy(pInfo->prevRow[i], pColInfo->pData + pColInfo->info.bytes * rowIndex, pColInfo->info.bytes);
  }

  pInfo->hasPrev = true;
}

static void doExecuteFinalMergeRv(SMultiwayMergeInfo* pInfo, int32_t numOfExpr, SSDataBlock* pBlock, bool needInit) {
  SQLFunctionCtx* pCtx = pInfo->binfo.pCtx;

  for(int32_t i = 0; i < pBlock->info.rows; ++i) {
    if (pInfo->hasPrev) {
      if (needToMergeRv(pBlock, pInfo->pMerge, i, pInfo->prevRow)) {
        for (int32_t j = 0; j < numOfExpr; ++j) {
          int32_t functionId = pCtx[j].functionId;
          if (functionId == TSDB_FUNC_TAG_DUMMY || functionId == TSDB_FUNC_TS_DUMMY) {
            continue;
          }

          pCtx[j].size = 1;
          aAggs[functionId].mergeFunc(&pCtx[j]);
        }
      } else {
        for(int32_t j = 0; j < numOfExpr; ++j) {
          int32_t functionId = pCtx[j].functionId;
          if (functionId == TSDB_FUNC_TAG_DUMMY || functionId == TSDB_FUNC_TS_DUMMY) {
            continue;
          }

          pCtx[j].size = 1;
          aAggs[functionId].xFinalize(&pCtx[j]);
        }

        pInfo->binfo.pRes->info.rows += 1;

        for(int32_t j = 0; j < numOfExpr; ++j) {
          pCtx[j].pOutput += pCtx[j].outputBytes;
          pCtx[j].pInput  += pCtx[j].inputBytes;

          aAggs[pCtx[j].functionId].init(&pCtx[j]);
        }

        for (int32_t j = 0; j < numOfExpr; ++j) {
          int32_t functionId = pCtx[j].functionId;
          if (functionId == TSDB_FUNC_TAG_DUMMY || functionId == TSDB_FUNC_TS_DUMMY) {
            continue;
          }

          pCtx[j].size = 1;
          aAggs[functionId].mergeFunc(&pCtx[j]);
        }
      }
    } else {
      for (int32_t j = 0; j < numOfExpr; ++j) {
        int32_t functionId = pCtx[j].functionId;
        if (functionId == TSDB_FUNC_TAG_DUMMY || functionId == TSDB_FUNC_TS_DUMMY) {
          continue;
        }

        pCtx[j].size = 1;
        aAggs[functionId].mergeFunc(&pCtx[j]);
      }
    }

    savePrevOrderColumns(pInfo, pBlock, i);
  }
}

static void handleUnprocessedRow(SSqlCmd *pCmd, SLocalMerger *pLocalMerge, tFilePage *tmpBuffer) {
  if (pLocalMerge->hasUnprocessedRow) {
    pLocalMerge->hasUnprocessedRow = false;

    SQueryInfo *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);
    size_t size = tscSqlExprNumOfExprs(pQueryInfo);

    doExecuteFinalMerge(pLocalMerge, size, true);
    savePreviousRow(pLocalMerge, tmpBuffer);
  }
}

static int64_t getNumOfResultLocal(SQLFunctionCtx *pCtx, int32_t numOfExprs) {
  int64_t maxOutput = 0;
  
  for (int32_t j = 0; j < numOfExprs; ++j) {
    /*
     * ts, tag, tagprj function can not decide the output number of current query
     * the number of output result is decided by main output
     */
    int32_t functionId = pCtx[j].functionId;
    if (functionId == TSDB_FUNC_TS || functionId == TSDB_FUNC_TAG) {
      continue;
    }

    SResultRowCellInfo* pResInfo = GET_RES_INFO(&pCtx[j]);
    if (maxOutput < pResInfo->numOfRes) {
      maxOutput = pResInfo->numOfRes;
    }
  }

  return maxOutput;
}

/*
 * in handling the top/bottom query, which produce more than one rows result,
 * the tsdb_func_tags only fill the first row of results, the remain rows need to
 * filled with the same result, which is the tags, specified in group by clause
 *
 */
static void fillMultiRowsOfTagsVal(SLocalMerger *pLocalMerge, int32_t numOfRes, int32_t numOfExprs) {
  for (int32_t k = 0; k < numOfExprs; ++k) {
    SQLFunctionCtx *pCtx = &pLocalMerge->pCtx[k];
    if (pCtx->functionId != TSDB_FUNC_TAG) {
      continue;
    }

    int32_t inc = numOfRes - 1;  // tsdb_func_tag function only produce one row of result
    memset(pLocalMerge->tagBuf, 0, (size_t)pLocalMerge->tagBufLen);
    memcpy(pLocalMerge->tagBuf, pCtx->pOutput, (size_t)pCtx->outputBytes);

    for (int32_t i = 0; i < inc; ++i) {
      pCtx->pOutput += pCtx->outputBytes;
      memcpy(pCtx->pOutput, pLocalMerge->tagBuf, (size_t)pCtx->outputBytes);
    }
  }
}

int32_t finalizeRes(SLocalMerger *pLocalMerge, int32_t numOfExprs) {
  for (int32_t k = 0; k < numOfExprs; ++k) {
    SQLFunctionCtx* pCtx = &pLocalMerge->pCtx[k];
    aAggs[pCtx->functionId].xFinalize(pCtx);
  }

  pLocalMerge->hasPrevRow = false;

  int32_t numOfRes = (int32_t)getNumOfResultLocal(pLocalMerge->pCtx, numOfExprs);
  pLocalMerge->pResultBuf->num += numOfRes;

  fillMultiRowsOfTagsVal(pLocalMerge, numOfRes, numOfExprs);
  return numOfRes;
}

/*
 * points merge:
 * points are merged according to the sort info, which is tags columns and timestamp column.
 * In case of points without either tags columns or timestamp, such as
 * results generated by simple aggregation function, we merge them all into one points
 * *Exception*: column projection query, required no merge procedure
 */
bool needToMerge(SQueryInfo *pQueryInfo, SLocalMerger *pLocalMerge, tFilePage *tmpBuffer) {
  int32_t ret = 0;  // merge all result by default

  int16_t functionId = pLocalMerge->pCtx[0].functionId;

  // todo opt performance
  if ((/*functionId == TSDB_FUNC_PRJ || */functionId == TSDB_FUNC_ARITHM) || (tscIsProjectionQueryOnSTable(pQueryInfo, 0) && pQueryInfo->distinctTag == false)) {  // column projection query
    ret = 1;                                                            // disable merge procedure
  } else {
    tOrderDescriptor *pDesc = pLocalMerge->pDesc;
    if (pDesc->orderInfo.numOfCols > 0) {
      if (pDesc->tsOrder == TSDB_ORDER_ASC) {  // asc
        // todo refactor comparator
        ret = compare_a(pLocalMerge->pDesc, 1, 0, pLocalMerge->prevRowOfInput, 1, 0, tmpBuffer->data);
      } else {  // desc
        ret = compare_d(pLocalMerge->pDesc, 1, 0, pLocalMerge->prevRowOfInput, 1, 0, tmpBuffer->data);
      }
    }
  }

  /* if ret == 0, means the result belongs to the same group */
  return (ret == 0);
}

bool needToMergeRv(SSDataBlock* pBlock, SLocalMerger *pLocalMerge, int32_t index, char **buf) {
  int32_t ret = 0;
    tOrderDescriptor *pDesc = pLocalMerge->pDesc;
    if (pDesc->orderInfo.numOfCols > 0) {
//      if (pDesc->tsOrder == TSDB_ORDER_ASC) {  // asc
        ret = compare_aRv(pBlock, pDesc->orderInfo.colIndex, pDesc->orderInfo.numOfCols, index, buf, TSDB_ORDER_ASC);
//      } else {  // desc
//        ret = compare_d(pLocalMerge->pDesc, 1, 0, pLocalMerge->prevRowOfInput, 1, 0, tmpBuffer->data);
//      }
    }

  // if ret == 0, means the result belongs to the same group
  return (ret == 0);
}

static bool reachGroupResultLimit(SQueryInfo *pQueryInfo, SSqlRes *pRes) {
  return (pRes->numOfGroups >= pQueryInfo->slimit.limit && pQueryInfo->slimit.limit >= 0);
}

static bool saveGroupResultInfo(SSqlObj *pSql) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  SQueryInfo *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);

  if (pRes->numOfRowsGroup > 0) {
    pRes->numOfGroups += 1;
  }

  // the output group is limited by the slimit clause
  if (reachGroupResultLimit(pQueryInfo, pRes)) {
    return true;
  }

  //    pRes->pGroupRec = realloc(pRes->pGroupRec, pRes->numOfGroups*sizeof(SResRec));
  //    pRes->pGroupRec[pRes->numOfGroups-1].numOfRows = pRes->numOfRows;
  //    pRes->pGroupRec[pRes->numOfGroups-1].numOfClauseTotal = pRes->numOfClauseTotal;

  return false;
}

/**
 *
 * @param pSql
 * @param pLocalMerge
 * @param noMoreCurrentGroupRes
 * @return if current group is skipped, return false, and do NOT record it into pRes->numOfGroups
 */
bool genFinalResults(SSqlObj *pSql, SLocalMerger *pLocalMerge, bool noMoreCurrentGroupRes) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  SQueryInfo *  pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);
  tFilePage *   pResBuf = pLocalMerge->pResultBuf;
  SColumnModel *pModel = pLocalMerge->resColModel;

  pRes->code = TSDB_CODE_SUCCESS;

  /*
   * Ignore the output of the current group since this group is skipped by user
   * We set the numOfRows to be 0 and discard the possible remain results.
   */
  if (pQueryInfo->slimit.offset > 0) {
    pRes->numOfRows = 0;
    pQueryInfo->slimit.offset -= 1;
    pLocalMerge->discard = !noMoreCurrentGroupRes;

    if (pLocalMerge->discard) {
      SColumnModel *pInternModel = pLocalMerge->pDesc->pColumnModel;
      tColModelAppend(pInternModel, pLocalMerge->discardData, pLocalMerge->pTempBuffer->data, 0, 1, 1);
    }

    return false;
  }

  tColModelCompact(pModel, pResBuf, pModel->capacity);

  if (tscIsSecondStageQuery(pQueryInfo)) {
    doArithmeticCalculate(pQueryInfo, pResBuf, pModel->rowSize, pLocalMerge->finalModel->rowSize);
  }

  // no interval query, no fill operation
  if (pQueryInfo->interval.interval == 0 || pQueryInfo->fillType == TSDB_FILL_NONE) {
    genFinalResWithoutFill(pRes, pLocalMerge, pQueryInfo);
  } else {
    SFillInfo* pFillInfo = pLocalMerge->pFillInfo;
    if (pFillInfo != NULL) {
      TSKEY ekey = (pQueryInfo->order.order == TSDB_ORDER_ASC)? pQueryInfo->window.ekey: pQueryInfo->window.skey;

      taosFillSetStartInfo(pFillInfo, (int32_t)pResBuf->num, ekey);
      taosFillCopyInputDataFromOneFilePage(pFillInfo, pResBuf);
    }
    
    doFillResult(pSql, pLocalMerge, noMoreCurrentGroupRes);
  }

  return true;
}

bool genFinalResultsRv(SSqlObj *pSql, SLocalMerger *pLocalMerge, bool noMoreCurrentGroupRes) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  SQueryInfo *  pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);
  tFilePage *   pResBuf = pLocalMerge->pResultBuf;
  SColumnModel *pModel = pLocalMerge->resColModel;

  pRes->code = TSDB_CODE_SUCCESS;

  tColModelCompact(pModel, pResBuf, pModel->capacity);

  // no interval query, no fill operation
  genFinalResWithoutFill(pRes, pLocalMerge, pQueryInfo);

  return true;
}

void resetOutputBuf(SQueryInfo *pQueryInfo, SLocalMerger *pLocalMerge) {// reset output buffer to the beginning
  size_t t = tscSqlExprNumOfExprs(pQueryInfo);
  for (int32_t i = 0; i < t; ++i) {
    SExprInfo* pExpr = tscSqlExprGet(pQueryInfo, i);
    pLocalMerge->pCtx[i].pOutput = pLocalMerge->pResultBuf->data + pExpr->base.offset * pLocalMerge->resColModel->capacity;

    if (pExpr->base.functionId == TSDB_FUNC_TOP || pExpr->base.functionId == TSDB_FUNC_BOTTOM || pExpr->base.functionId == TSDB_FUNC_DIFF) {
      pLocalMerge->pCtx[i].ptsOutputBuf = pLocalMerge->pCtx[0].pOutput;
    }
  }

  memset(pLocalMerge->pResultBuf, 0, pLocalMerge->nResultBufSize + sizeof(tFilePage));
}

static void resetEnvForNewResultset(SSqlRes *pRes, SSqlCmd *pCmd, SLocalMerger *pLocalMerge) {
  // In handling data in other groups, we need to reset the interpolation information for a new group data
  pRes->numOfRows = 0;
  pRes->numOfRowsGroup = 0;

  SQueryInfo *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);

  pQueryInfo->limit.offset = pLocalMerge->offset;

  STableMetaInfo *pTableMetaInfo = tscGetTableMetaInfoFromCmd(pCmd, pCmd->clauseIndex, 0);
  STableComInfo tinfo = tscGetTableInfo(pTableMetaInfo->pTableMeta);
  
  // for group result interpolation, do not return if not data is generated
  if (pQueryInfo->fillType != TSDB_FILL_NONE) {
    TSKEY skey = (pQueryInfo->order.order == TSDB_ORDER_ASC)? pQueryInfo->window.skey:pQueryInfo->window.ekey;//MIN(pQueryInfo->window.skey, pQueryInfo->window.ekey);
    int64_t newTime = taosTimeTruncate(skey, &pQueryInfo->interval, tinfo.precision);
    taosResetFillInfo(pLocalMerge->pFillInfo, newTime);
  }
}

static bool isAllSourcesCompleted(SLocalMerger *pLocalMerge) {
  return (pLocalMerge->numOfBuffer == pLocalMerge->numOfCompleted);
}

static bool doBuildFilledResultForGroup(SSqlObj *pSql) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  SQueryInfo *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);
  SLocalMerger *pLocalMerge = pRes->pLocalMerger;
  SFillInfo *pFillInfo = pLocalMerge->pFillInfo;

  if (pFillInfo != NULL && taosFillHasMoreResults(pFillInfo)) {
    assert(pQueryInfo->fillType != TSDB_FILL_NONE);

    tFilePage *pFinalDataBuf = pLocalMerge->pResultBuf;
    int64_t etime = *(int64_t *)(pFinalDataBuf->data + TSDB_KEYSIZE * (pFillInfo->numOfRows - 1));

    // the first column must be the timestamp column
    int32_t rows = (int32_t) getNumOfResultsAfterFillGap(pFillInfo, etime, pLocalMerge->resColModel->capacity);
    if (rows > 0) {  // do fill gap
      doFillResult(pSql, pLocalMerge, false);
    }

    return true;
  } else {
    return false;
  }
}

static bool doHandleLastRemainData(SSqlObj *pSql) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  SLocalMerger *pLocalMerge = pRes->pLocalMerger;
  SFillInfo     *pFillInfo = pLocalMerge->pFillInfo;

  bool prevGroupCompleted = (!pLocalMerge->discard) && pLocalMerge->hasUnprocessedRow;

  SQueryInfo *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);

  if ((isAllSourcesCompleted(pLocalMerge) && !pLocalMerge->hasPrevRow) || pLocalMerge->pLocalDataSrc[0] == NULL ||
      prevGroupCompleted) {
    // if fillType == TSDB_FILL_NONE, return directly
    if (pQueryInfo->fillType != TSDB_FILL_NONE &&
      ((pRes->numOfRowsGroup < pQueryInfo->limit.limit && pQueryInfo->limit.limit > 0) || (pQueryInfo->limit.limit < 0))) {
      int64_t etime = (pQueryInfo->order.order == TSDB_ORDER_ASC)? pQueryInfo->window.ekey : pQueryInfo->window.skey;

      int32_t rows = (int32_t)getNumOfResultsAfterFillGap(pFillInfo, etime, pLocalMerge->resColModel->capacity);
      if (rows > 0) {
        doFillResult(pSql, pLocalMerge, true);
      }
    }

    /*
     * 1. numOfRows == 0, means no interpolation results are generated.
     * 2. if all local data sources are consumed, and no un-processed rows exist.
     *
     * No results will be generated and query completed.
     */
    if (pRes->numOfRows > 0 || (isAllSourcesCompleted(pLocalMerge) && (!pLocalMerge->hasUnprocessedRow))) {
      return true;
    }

    // start to process result for a new group and save the result info of previous group
    if (saveGroupResultInfo(pSql)) {
      return true;
    }

    resetEnvForNewResultset(pRes, pCmd, pLocalMerge);
  }

  return false;
}

static void doProcessResultInNextWindow(SSqlObj *pSql, int32_t numOfRes) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  SLocalMerger *pLocalMerge = pRes->pLocalMerger;
  SQueryInfo *   pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);
  size_t size = tscSqlExprNumOfExprs(pQueryInfo);

  for (int32_t k = 0; k < size; ++k) {
    SQLFunctionCtx *pCtx = &pLocalMerge->pCtx[k];
    pCtx->pOutput += pCtx->outputBytes * numOfRes;

    // set the correct output timestamp column position
    if (pCtx->functionId == TSDB_FUNC_TOP || pCtx->functionId == TSDB_FUNC_BOTTOM) {
      pCtx->ptsOutputBuf = ((char *)pCtx->ptsOutputBuf + TSDB_KEYSIZE * numOfRes);
    }
  }

  doExecuteFinalMerge(pLocalMerge, size, true);
}

int32_t tscDoLocalMerge(SSqlObj *pSql) {
  SSqlCmd *pCmd = &pSql->cmd;
  SSqlRes *pRes = &pSql->res;

  tscResetForNextRetrieve(pRes);
  assert(pSql->signature == pSql);

  if (pRes->pLocalMerger == NULL) {  // all data has been processed
    if (pRes->code == TSDB_CODE_SUCCESS) {
      return pRes->code;
    }

    tscError("%p local merge abort due to error occurs, code:%s", pSql, tstrerror(pRes->code));
    return pRes->code;
  }

  SLocalMerger  *pLocalMerge = pRes->pLocalMerger;
  SQueryInfo    *pQueryInfo = tscGetQueryInfo(pCmd, pCmd->clauseIndex);
  tFilePage     *tmpBuffer = pLocalMerge->pTempBuffer;

  int32_t numOfExprs = (int32_t) tscSqlExprNumOfExprs(pQueryInfo);
  if (doHandleLastRemainData(pSql)) {
    return TSDB_CODE_SUCCESS;
  }

  if (doBuildFilledResultForGroup(pSql)) {
    return TSDB_CODE_SUCCESS;
  }

  SLoserTreeInfo *pTree = pLocalMerge->pLoserTree;

  // clear buffer
  handleUnprocessedRow(pCmd, pLocalMerge, tmpBuffer);
  SColumnModel *pModel = pLocalMerge->pDesc->pColumnModel;

  while (1) {
    if (isAllSourcesCompleted(pLocalMerge)) {
      break;
    }

#ifdef _DEBUG_VIEW
    printf("chosen data in pTree[0] = %d\n", pTree->pNode[0].index);
#endif

    assert((pTree->pNode[0].index < pLocalMerge->numOfBuffer) && (pTree->pNode[0].index >= 0) && tmpBuffer->num == 0);

    // chosen from loser tree
    SLocalDataSource *pOneDataSrc = pLocalMerge->pLocalDataSrc[pTree->pNode[0].index];

    tColModelAppend(pModel, tmpBuffer, pOneDataSrc->filePage.data, pOneDataSrc->rowIdx, 1,
                    pOneDataSrc->pMemBuffer->pColumnModel->capacity);

#if defined(_DEBUG_VIEW)
    printf("chosen row:\t");
    SSrcColumnInfo colInfo[256] = {0};
    tscGetSrcColumnInfo(colInfo, pQueryInfo);

    tColModelDisplayEx(pModel, tmpBuffer->data, tmpBuffer->num, pModel->capacity, colInfo);
#endif

    if (pLocalMerge->discard) {
      assert(pLocalMerge->hasUnprocessedRow == false);

      /* current record belongs to the same group of previous record, need to discard it */
      if (isSameGroup(pCmd, pLocalMerge, pLocalMerge->discardData->data, tmpBuffer)) {
        tmpBuffer->num = 0;
        pOneDataSrc->rowIdx += 1;

        adjustLoserTreeFromNewData(pLocalMerge, pOneDataSrc, pTree);

        // all inputs are exhausted, abort current process
        if (isAllSourcesCompleted(pLocalMerge)) {
          break;
        }

        // data belongs to the same group needs to be discarded
        continue;
      } else {
        pLocalMerge->discard = false;
        pLocalMerge->discardData->num = 0;

        if (saveGroupResultInfo(pSql)) {
          return TSDB_CODE_SUCCESS;
        }

        resetEnvForNewResultset(pRes, pCmd, pLocalMerge);
      }
    }

    if (pLocalMerge->hasPrevRow) {
      if (needToMerge(pQueryInfo, pLocalMerge, tmpBuffer)) {
        // belong to the group of the previous row, continue process it
        doExecuteFinalMerge(pLocalMerge, numOfExprs, false);

        // copy to buffer
        savePreviousRow(pLocalMerge, tmpBuffer);
      } else {
        /*
         * current row does not belong to the group of previous row.
         * so the processing of previous group is completed.
         */
        int32_t numOfRes = finalizeRes(pLocalMerge, numOfExprs);
        bool   sameGroup = isSameGroup(pCmd, pLocalMerge, pLocalMerge->prevRowOfInput, tmpBuffer);

        tFilePage *pResBuf = pLocalMerge->pResultBuf;

        /*
         * if the previous group does NOT generate any result (pResBuf->num == 0),
         * continue to process results instead of return results.
         */
        if ((!sameGroup && pResBuf->num > 0) || (pResBuf->num == pLocalMerge->resColModel->capacity)) {
          // does not belong to the same group
          bool notSkipped = genFinalResults(pSql, pLocalMerge, !sameGroup);

          // this row needs to discard, since it belongs to the group of previous
          if (pLocalMerge->discard && sameGroup) {
            pLocalMerge->hasUnprocessedRow = false;
            tmpBuffer->num = 0;
          } else { // current row does not belongs to the previous group, so it is not be handled yet.
            pLocalMerge->hasUnprocessedRow = true;
          }

          resetOutputBuf(pQueryInfo, pLocalMerge);
          pOneDataSrc->rowIdx += 1;

          // here we do not check the return value
          adjustLoserTreeFromNewData(pLocalMerge, pOneDataSrc, pTree);

          if (pRes->numOfRows == 0) {
            handleUnprocessedRow(pCmd, pLocalMerge, tmpBuffer);

            if (!sameGroup) {
              /*
               * previous group is done, prepare for the next group
               * If previous group is not skipped, keep it in pRes->numOfGroups
               */
              if (notSkipped && saveGroupResultInfo(pSql)) {
                return TSDB_CODE_SUCCESS;
              }

              resetEnvForNewResultset(pRes, pCmd, pLocalMerge);
            }
          } else {
            /*
             * if next record belongs to a new group, we do not handle this record here.
             * We start the process in a new round.
             */
            if (sameGroup) {
              handleUnprocessedRow(pCmd, pLocalMerge, tmpBuffer);
            }
          }

          // current group has no result,
          if (pRes->numOfRows == 0) {
            continue;
          } else {
            return TSDB_CODE_SUCCESS;
          }
        } else {  // result buffer is not full
          doProcessResultInNextWindow(pSql, numOfRes);
          savePreviousRow(pLocalMerge, tmpBuffer);
        }
      }
    } else {
      doExecuteFinalMerge(pLocalMerge, numOfExprs,true);
      savePreviousRow(pLocalMerge, tmpBuffer);  // copy the processed row to buffer
    }

    pOneDataSrc->rowIdx += 1;
    adjustLoserTreeFromNewData(pLocalMerge, pOneDataSrc, pTree);
  }

  if (pLocalMerge->hasPrevRow) {
    finalizeRes(pLocalMerge, numOfExprs);
  }

  if (pLocalMerge->pResultBuf->num) {
    genFinalResults(pSql, pLocalMerge, true);
  }

  return TSDB_CODE_SUCCESS;
}

void tscInitResObjForLocalQuery(SSqlObj *pObj, int32_t numOfRes, int32_t rowLen) {
  SSqlRes *pRes = &pObj->res;
  if (pRes->pLocalMerger != NULL) {
    tscDestroyLocalMerger(pObj);
  }

  pRes->qId = 1;  // hack to pass the safety check in fetch_row function
  pRes->numOfRows = 0;
  pRes->row = 0;

  pRes->rspType = 0;  // used as a flag to denote if taos_retrieved() has been called yet
  pRes->pLocalMerger = (SLocalMerger *)calloc(1, sizeof(SLocalMerger));

  /*
   * we need one additional byte space
   * the sprintf function needs one additional space to put '\0' at the end of string
   */
  size_t allocSize = numOfRes * rowLen + sizeof(tFilePage) + 1;
  pRes->pLocalMerger->pResultBuf = (tFilePage *)calloc(1, allocSize);

  pRes->pLocalMerger->pResultBuf->num = numOfRes;
  pRes->data = pRes->pLocalMerger->pResultBuf->data;
}

int32_t doArithmeticCalculate(SQueryInfo* pQueryInfo, tFilePage* pOutput, int32_t rowSize, int32_t finalRowSize) {
  int32_t maxRowSize = MAX(rowSize, finalRowSize);
  char* pbuf = calloc(1, (size_t)(pOutput->num * maxRowSize));

  size_t size = tscNumOfFields(pQueryInfo);
  SArithmeticSupport arithSup = {0};

  // todo refactor
  arithSup.offset     = 0;
  arithSup.numOfCols  = (int32_t) tscSqlExprNumOfExprs(pQueryInfo);
  arithSup.exprList   = pQueryInfo->exprList;
  arithSup.data       = calloc(arithSup.numOfCols, POINTER_BYTES);

  for(int32_t k = 0; k < arithSup.numOfCols; ++k) {
    SExprInfo* pExpr = tscSqlExprGet(pQueryInfo, k);
    arithSup.data[k] = (pOutput->data + pOutput->num* pExpr->base.offset);
  }

  int32_t offset = 0;

  for (int i = 0; i < size; ++i) {
    SInternalField* pSup = TARRAY_GET_ELEM(pQueryInfo->fieldsInfo.internalField, i);
    
    // calculate the result from several other columns
    if (pSup->pExpr->pExpr != NULL) {
      arithSup.pArithExpr = pSup->pExpr;
      arithmeticTreeTraverse(arithSup.pArithExpr->pExpr, (int32_t) pOutput->num, pbuf + pOutput->num*offset, &arithSup, TSDB_ORDER_ASC, getArithmeticInputSrc);
    } else {
      SExprInfo* pExpr = pSup->pExpr;
      memcpy(pbuf + pOutput->num * offset, pExpr->base.offset * pOutput->num + pOutput->data, (size_t)(pExpr->base.resBytes * pOutput->num));
    }

    offset += pSup->field.bytes;
  }

  memcpy(pOutput->data, pbuf, (size_t)(pOutput->num * offset));

  tfree(pbuf);
  tfree(arithSup.data);

  return offset;
}

#define COLMODEL_GET_VAL(data, schema, allrow, rowId, colId) \
  (data + (schema)->pFields[colId].offset * (allrow) + (rowId) * (schema)->pFields[colId].field.bytes)


static void appendOneRowToDataBlock(SSDataBlock *pBlock, char *buf, SColumnModel *pModel, int32_t rowIndex,
                                    int32_t maxRows) {
  for (int32_t i = 0; i < pBlock->info.numOfCols; ++i) {
    SColumnInfoData* pColInfo = taosArrayGet(pBlock->pDataBlock, i);
    char* p = pColInfo->pData + pBlock->info.rows * pColInfo->info.bytes;

//    char *dst = COLMODEL_GET_VAL(dstPage->data, dstModel, dstModel->capacity, dstPage->num, col);
    char *src = COLMODEL_GET_VAL(buf, pModel, maxRows, rowIndex, i);
//    char* src = buf + rowIndex * pColInfo->info.bytes;
    memmove(p, src, pColInfo->info.bytes);
  }

  pBlock->info.rows += 1;
}

static SSDataBlock* doMultiwaySort(void* param) {
  SOperatorInfo* pOperator = (SOperatorInfo*) param;
  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  SMultiwayMergeInfo *pInfo = pOperator->info;

  SLocalMerger   *pMerger = pInfo->pMerge;
  SLoserTreeInfo *pTree   = pMerger->pLoserTree;
  SColumnModel   *pModel  = pMerger->pDesc->pColumnModel;
  tFilePage      *tmpBuffer = pMerger->pTempBuffer;

  pInfo->binfo.pRes->info.rows = 0;

  while(1) {
    if (isAllSourcesCompleted(pMerger)) {
      break;
    }

#ifdef _DEBUG_VIEW
    printf("chosen data in pTree[0] = %d\n", pTree->pNode[0].index);
#endif

    assert((pTree->pNode[0].index < pMerger->numOfBuffer) && (pTree->pNode[0].index >= 0) && tmpBuffer->num == 0);

    // chosen from loser tree
    SLocalDataSource *pOneDataSrc = pMerger->pLocalDataSrc[pTree->pNode[0].index];
    appendOneRowToDataBlock(pInfo->binfo.pRes, pOneDataSrc->filePage.data, pModel, pOneDataSrc->rowIdx, pOneDataSrc->pMemBuffer->pColumnModel->capacity);

#if defined(_DEBUG_VIEW)
    printf("chosen row:\t");
    SSrcColumnInfo colInfo[256] = {0};
    tscGetSrcColumnInfo(colInfo, pQueryInfo);

    tColModelDisplayEx(pModel, tmpBuffer->data, tmpBuffer->num, pModel->capacity, colInfo);
#endif

    pOneDataSrc->rowIdx += 1;
    adjustLoserTreeFromNewData(pMerger, pOneDataSrc, pTree);

    if (pInfo->binfo.pRes->info.rows >= 4096) { // TODO threshold
      return pInfo->binfo.pRes;
    }
  }

  return (pInfo->binfo.pRes->info.rows > 0)? pInfo->binfo.pRes:NULL;
}

SOperatorInfo *createMultiwaySortOperatorInfo(SQueryRuntimeEnv *pRuntimeEnv, SExprInfo *pExpr, int32_t numOfOutput,
                                              int32_t numOfRows, void *merger) {
  SMultiwayMergeInfo* pInfo = calloc(1, sizeof(SMultiwayMergeInfo));

  pInfo->pMerge = merger;
  pInfo->binfo.pRes  = createOutputBuf(pExpr, numOfOutput, numOfRows);

  SOperatorInfo* pOperator = calloc(1, sizeof(SOperatorInfo));
  pOperator->name         = "MultiwaySortOperator";
  pOperator->operatorType = OP_MultiwaySort;
  pOperator->blockingOptr = false;
  pOperator->status       = OP_IN_EXECUTING;
  pOperator->info         = pInfo;
  pOperator->pRuntimeEnv  = pRuntimeEnv;
  pOperator->numOfOutput  = pRuntimeEnv->pQueryAttr->numOfCols;
  pOperator->exec         = doMultiwaySort;

  return pOperator;
}

SSDataBlock* doGlobalAggregate(void* param) {
  SOperatorInfo* pOperator = (SOperatorInfo*) param;
  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  SMultiwayMergeInfo* pAggInfo = pOperator->info;

  SQueryRuntimeEnv *pRuntimeEnv = pOperator->pRuntimeEnv;
  SOperatorInfo    *upstream = pOperator->upstream;

  while(1) {
    SSDataBlock* pBlock = upstream->exec(upstream);
    if (pBlock == NULL) {
      break;
    }

    // not belongs to the same group, return the result of current group;
    setInputDataBlock(pOperator, pAggInfo->binfo.pCtx, pBlock, TSDB_ORDER_ASC);
    updateOutputBuf(&pAggInfo->binfo, &pAggInfo->bufCapacity, pBlock->info.rows);

    doExecuteFinalMergeRv(pAggInfo, pOperator->numOfOutput, pBlock, false);
  }

  for(int32_t j = 0; j < pOperator->numOfOutput; ++j) {
    int32_t functionId = pAggInfo->binfo.pCtx[j].functionId;
    if (functionId == TSDB_FUNC_TAG_DUMMY || functionId == TSDB_FUNC_TS_DUMMY) {
      continue;
    }
    aAggs[functionId].xFinalize(&pAggInfo->binfo.pCtx[j]);
  }

  pAggInfo->binfo.pRes->info.rows += 1;

  pOperator->status = OP_EXEC_DONE;
  setQueryStatus(pRuntimeEnv, QUERY_COMPLETED);

  return pAggInfo->binfo.pRes;
}

SSDataBlock* doSLimit(void* param) {
  SOperatorInfo* pOperator = (SOperatorInfo*)param;
  if (pOperator->status == OP_EXEC_DONE) {
    return NULL;
  }

  SSLimitOperatorInfo *pInfo = pOperator->info;
  SQueryRuntimeEnv   *pRuntimeEnv = pOperator->pRuntimeEnv;

  SSDataBlock* pBlock = NULL;
  while (1) {
    pBlock = pOperator->upstream->exec(pOperator->upstream);
    if (pBlock == NULL) {
      setQueryStatus(pOperator->pRuntimeEnv, QUERY_COMPLETED);
      pOperator->status = OP_EXEC_DONE;
      return NULL;
    }

    if (pRuntimeEnv->currentOffset == 0) {
      break;
    } else if (pRuntimeEnv->currentOffset >= pBlock->info.rows) {
      pRuntimeEnv->currentOffset -= pBlock->info.rows;
    } else {
      int32_t remain = (int32_t)(pBlock->info.rows - pRuntimeEnv->currentOffset);
      pBlock->info.rows = remain;

      for (int32_t i = 0; i < pBlock->info.numOfCols; ++i) {
        SColumnInfoData* pColInfoData = taosArrayGet(pBlock->pDataBlock, i);

        int16_t bytes = pColInfoData->info.bytes;
        memmove(pColInfoData->pData, pColInfoData->pData + bytes * pRuntimeEnv->currentOffset, remain * bytes);
      }

      pRuntimeEnv->currentOffset = 0;
      break;
    }
  }

  if (pInfo->total + pBlock->info.rows >= pInfo->limit) {
    pBlock->info.rows = (int32_t)(pInfo->limit - pInfo->total);
    pInfo->total = pInfo->limit;

    setQueryStatus(pOperator->pRuntimeEnv, QUERY_COMPLETED);
    pOperator->status = OP_EXEC_DONE;
  } else {
    pInfo->total += pBlock->info.rows;
  }

  return pBlock;
}

