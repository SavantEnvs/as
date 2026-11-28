/**
 * SSAS - Simple Smart Automotive Software
 * Copyright (C) 2021 Parai Wang <parai@foxmail.com>
 *
 * ref: Specification of Diagnostic Event Manager AUTOSAR CP Release 4.4.0
 */
/* ================================ [ INCLUDES  ] ============================================== */
#include "Dem_Priv.h"
#include "Std_Debug.h"
#include "Std_Critical.h"
#ifdef DEM_USE_NVM
#include "NvM.h"
#endif
#include <string.h>
#include "Std_Flag.h"
#include "Det.h"
/* ================================ [ MACROS    ] ============================================== */
#define AS_LOG_DEM 0
#define AS_LOG_DEMI 0
#define AS_LOG_DEME 2

#ifdef DEM_USE_PB_CONFIG
#define DEM_CONFIG demConfig
#else
#define DEM_CONFIG (&Dem_Config)
#endif

#define DEM_STATE_OFF ((Dem_StateType)0x00)
#define DEM_STATE_PRE_INIT ((Dem_StateType)0x01)
#define DEM_STATE_INIT ((Dem_StateType)0x02)

#define DEM_FREEZE_FRAME_SLOT_FREE 0xFFu
#define DEM_EXTENDED_DATA_SLOT_FREE 0xFFu

#define DEM_FILTER_BY_MASK 0x00
#define DEM_FILTER_BY_SELECTION 0x00
/* ================================ [ TYPES     ] ============================================== */
typedef struct {
  boolean DisableDtcSetting;
  struct {
    uint32_t DTCNumber;
    uint16_t Index;
    Dem_DTCOriginType DTCOrigin;
    uint8_t StatusMask;
    uint8_t RecordNumber;
    boolean FilterWithSeverity;
    Dem_DTCSeverityType DTCSeverityMask;
    boolean FilterForFaultDetectionCounter;
  } Filter;
#ifdef DEM_USE_ENABLE_CONDITION
  uint32_t ConditionMask;
#endif
} Dem_ContextType;
/* ================================ [ DECLARES  ] ============================================== */
extern CONSTANT(Dem_ConfigType, DEM_CONST) Dem_Config;
static uint8_t *Dem_MallocFreezeFrameRecord(Dem_DtcIdType dtcId,
                                            P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST)
                                              memory,
                                            uint16_t *ffId);
static Dem_FreezeFrameRecordType *Dem_LookupFreezeFrameRecordByRecordNumber(
  Dem_DtcIdType dtcId, P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory,
  uint8_t *recNum, uint8_t **data, uint16_t *where);
static void Dem_FreeFreezeFrameRecord(Dem_DtcIdType dtcId, P2CONST(Dem_MemoryDestinationType,
                                                                   AUTOMATIC, DEM_CONST) memory);
static void Dem_FreeExtendedDataRecord(Dem_DtcIdType dtcId, P2CONST(Dem_MemoryDestinationType,
                                                                    AUTOMATIC, DEM_CONST) memory);
/* ================================ [ DATAS     ] ============================================== */
static Dem_ContextType demContext;
#ifdef DEM_USE_PB_CONFIG
static const Dem_ConfigType *demConfig = NULL;
#endif
/* ================================ [ LOCALS    ] ============================================== */
static void Dem_EventInit(Dem_EventIdType eventId) {
  Dem_EventContextType *EventContext = &DEM_CONFIG->EventContexts[eventId];
  Dem_EventStatusRecordType *EventStatus = DEM_CONFIG->EventStatusRecords[eventId];
  (void)EventStatus;

  EventContext->Status = DEM_EVENT_STATUS_UNKNOWN;
  EventContext->Flag = 0;
  EventContext->DebounceCounter = 0;

  ASLOG(DEMI, ("Event %d status=%02X, testFailedCounter=%d\n", eventId, EventStatus->Status,
               EventStatus->TestFailedCounter));
}

#ifndef DEM_USE_NVM
static void Dem_SetDirty(uint8_t *mask, uint16_t pos) {
  mask[pos / 8] |= (pos & 0x07) << 1;
}

static void Dem_ClearDirty(uint8_t *mask, uint16_t pos) {
  mask[pos / 8] &= ~(pos & 0x07) << 1;
}

static boolean Dem_IsDirty(uint8_t *mask, uint16_t pos) {
  return (0u != (mask[pos / 8] & ((pos & 0x07) << 1)));
}
#endif
static P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST)
  Dem_LookupMemory(Dem_DTCOriginType DTCOrigin) {
  uint16_t i;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory = NULL;

  for (i = 0; i < DEM_CONFIG->NumOfMemoryDestination; i++) {
    if (DEM_CONFIG->MemoryDestination[i].DTCOrigin == DTCOrigin) {
      memory = &DEM_CONFIG->MemoryDestination[i];
      break;
    }
  }
  return memory;
}

#if defined(DEM_USE_FAILED_DTC_ORDER_QUEUE) || defined(DEM_USE_CONFIRMED_DTC_ORDER_QUEUE)
/* ================= occurrence-order queues, see doc/EN/Dem.md 3.7 =============== */
static boolean Dem_OrderQueueIsValid(const uint16_t *DtcId, uint16_t size, uint16_t numOfDtcs,
                                     uint8_t maxOccurrence) {
  boolean valid = TRUE;
  uint16_t i;
  uint16_t j;
  uint16_t cnt;
  for (i = 0; (i < size) && (DEM_INVALID_DTC_ID != DtcId[i]) && (TRUE == valid); i++) {
    if (DtcId[i] >= numOfDtcs) {
      valid = FALSE;
    } else {
      cnt = 0;
      for (j = 0; (j < size) && (DEM_INVALID_DTC_ID != DtcId[j]); j++) {
        if (DtcId[i] == DtcId[j]) {
          cnt++;
        }
      }
      if (cnt > maxOccurrence) {
        valid = FALSE;
      }
    }
  }
  return valid;
}

/* find the first occurrence of dtcId in the valid part, DEM_INVALID_DTC_ID if
 * not found */
static uint16_t Dem_OrderQueueFind(const uint16_t *DtcId, uint16_t size, Dem_DtcIdType dtcId) {
  uint16_t i;
  uint16_t pos = DEM_INVALID_DTC_ID;
  for (i = 0; (i < size) && (DEM_INVALID_DTC_ID != DtcId[i]); i++) {
    if (dtcId == (Dem_DtcIdType)DtcId[i]) {
      pos = i;
      break;
    }
  }
  return pos;
}

/* append at the tail (the first free slot). The queue can never be full:
 * the occupancy equals the sum over all DTCs of min(edge count, 2) which is
 * at most 2*NumOfDtcs, and an edge raises that sum by at most one */
static void Dem_OrderQueueAppend(uint16_t *DtcId, uint16_t size, Dem_DtcIdType dtcId) {
  uint16_t i;
  for (i = 0; i < size; i++) {
    if (DEM_INVALID_DTC_ID == DtcId[i]) {
      DtcId[i] = (uint16_t)dtcId;
      break;
    }
  }
}

/* remove the entry at pos and shift the remaining order */
static void Dem_OrderQueueRemoveAt(uint16_t *DtcId, uint16_t size, uint16_t pos) {
  uint16_t i;
  for (i = pos; ((i + 1u) < size) && (DEM_INVALID_DTC_ID != DtcId[i + 1u]); i++) {
    DtcId[i] = DtcId[i + 1u];
  }
  DtcId[i] = DEM_INVALID_DTC_ID;
}

/* remove all entries of dtcId, keeping the order of the remaining ones */
static void Dem_OrderQueueRemoveAll(uint16_t *DtcId, uint16_t size, Dem_DtcIdType dtcId) {
  uint16_t pos;
  while (DEM_INVALID_DTC_ID != (pos = Dem_OrderQueueFind(DtcId, size, dtcId))) {
    Dem_OrderQueueRemoveAt(DtcId, size, pos);
  }
}

#ifdef DEM_USE_FAILED_DTC_ORDER_QUEUE
/* TF 0->1 DTC level edge: append the failure edge in time order, keeping at
 * most two entries per DtcId - its first (head, answered by 0x0B) and its
 * most recent (tail, answered by 0x0D) */
static void Dem_FailedOrderQueueInsert(Dem_DtcIdType dtcId) {
  P2CONST(Dem_DtcOrderQueueConfigType, AUTOMATIC, DEM_CONST) queues = DEM_CONFIG->DtcOrderQueue;
  uint16_t size = (uint16_t)(2u * DEM_CONFIG->NumOfDtcs);
  uint16_t *DtcId = queues->FailedOrderQueue;
  uint16_t pos1;
  uint16_t pos2;

  pos1 = Dem_OrderQueueFind(DtcId, size, dtcId);
  if (DEM_INVALID_DTC_ID != pos1) {
    /* the DTC is already tracked with one (first = latest) or two entries */
    pos2 = Dem_OrderQueueFind(&DtcId[pos1 + 1u], (uint16_t)(size - pos1 - 1u), dtcId);
    if (DEM_INVALID_DTC_ID != pos2) {
      /* two entries exist, this is a re-failure: erase the stale latest entry
       * and append at the tail, keeping the first entry so that the head is
       * still the first failed DTC. NOTE: never overwrite in place, that
       * would break the chronological order */
      Dem_OrderQueueRemoveAt(DtcId, size, (uint16_t)(pos1 + 1u + pos2));
    }
  }
  Dem_OrderQueueAppend(DtcId, size, dtcId);
#ifdef DEM_USE_NVM
  (void)NvM_WriteBlock(queues->FailedOrderQueueNvmBlockId, NULL);
#endif
}
#endif

#ifdef DEM_USE_CONFIRMED_DTC_ORDER_QUEUE
/* CDTC 0->1 DTC level edge: plain append, the edge implies the DTC was not
 * confirmed, hence it is not queued yet (head = 0x0C, tail = 0x0E) */
static void Dem_ConfirmedOrderQueueAppend(Dem_DtcIdType dtcId) {
  P2CONST(Dem_DtcOrderQueueConfigType, AUTOMATIC, DEM_CONST) queues = DEM_CONFIG->DtcOrderQueue;
  uint16_t size = DEM_CONFIG->NumOfDtcs;
  uint16_t *DtcId = queues->ConfirmedOrderQueue;

  if (DEM_INVALID_DTC_ID == Dem_OrderQueueFind(DtcId, size, dtcId)) {
    Dem_OrderQueueAppend(DtcId, size, dtcId);
#ifdef DEM_USE_NVM
    (void)NvM_WriteBlock(queues->ConfirmedOrderQueueNvmBlockId, NULL);
#endif
  }
}
#endif

/* erase events (aging completion, 0x14 clear, CDTC-reset displacement):
 * remove ALL entries of the DtcId from both queues, the next-in-order
 * automatically becomes the new first / most recent */
static void Dem_DtcOrderQueueRemove(Dem_DtcIdType dtcId) {
  P2CONST(Dem_DtcOrderQueueConfigType, AUTOMATIC, DEM_CONST) queues = DEM_CONFIG->DtcOrderQueue;
#ifdef DEM_USE_FAILED_DTC_ORDER_QUEUE
  Dem_OrderQueueRemoveAll(queues->FailedOrderQueue, (uint16_t)(2u * DEM_CONFIG->NumOfDtcs), dtcId);
#ifdef DEM_USE_NVM
  (void)NvM_WriteBlock(queues->FailedOrderQueueNvmBlockId, NULL);
#endif
#endif
#ifdef DEM_USE_CONFIRMED_DTC_ORDER_QUEUE
  Dem_OrderQueueRemoveAll(queues->ConfirmedOrderQueue, DEM_CONFIG->NumOfDtcs, dtcId);
#ifdef DEM_USE_NVM
  (void)NvM_WriteBlock(queues->ConfirmedOrderQueueNvmBlockId, NULL);
#endif
#endif
}

/* validate the NvM restored queues, a corrupt queue restarts empty - the
 * history cannot be rebuilt from anywhere else */
static void Dem_DtcOrderQueueInit(void) {
  P2CONST(Dem_DtcOrderQueueConfigType, AUTOMATIC, DEM_CONST) queues = DEM_CONFIG->DtcOrderQueue;
  uint16_t size;
#ifdef DEM_USE_FAILED_DTC_ORDER_QUEUE
  size = (uint16_t)(2u * DEM_CONFIG->NumOfDtcs);
  if (FALSE == Dem_OrderQueueIsValid(queues->FailedOrderQueue, size, DEM_CONFIG->NumOfDtcs, 2u)) {
    ASLOG(DEME, ("corrupt failed order queue detected, reset to empty\n"));
    (void)memset(queues->FailedOrderQueue, 0xFF, (size_t)size * sizeof(uint16_t));
  }
#endif
#ifdef DEM_USE_CONFIRMED_DTC_ORDER_QUEUE
  size = DEM_CONFIG->NumOfDtcs;
  if (FALSE ==
      Dem_OrderQueueIsValid(queues->ConfirmedOrderQueue, size, DEM_CONFIG->NumOfDtcs, 1u)) {
    ASLOG(DEME, ("corrupt confirmed order queue detected, reset to empty\n"));
    (void)memset(queues->ConfirmedOrderQueue, 0xFF, (size_t)size * sizeof(uint16_t));
  }
#endif
}
#endif

static void Dem_DtcUpdateOnOperationCycleStart(P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) dtc) {
  Dem_DtcStatusRecordType *StatusRecord;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;
  uint16_t i;
  for (i = 0; i < dtc->DTCAttributes->NumOfMemoryDestination; i++) {
    memory = dtc->DTCAttributes->MemoryDestination[i];
    StatusRecord = memory->StatusRecords[dtc->DtcId];
#ifdef DEM_STATUS_BIT_STORAGE_TEST_FAILED
/* @SWS_Dem_00525: retain the information for UDS status bit 0 over power cycles (non-volatile) */
#else
    /* @SWS_Dem_00388 */
    Dem_UdsBitClear(StatusRecord->Status, DEM_UDS_STATUS_TF);
#endif

    /* @SWS_Dem_00389 */
    Dem_UdsBitClear(StatusRecord->Status, DEM_UDS_STATUS_TFTOC);
    /* @SWS_Dem_00394 */
    Dem_UdsBitSet(StatusRecord->Status, DEM_UDS_STATUS_TNCTOC);
  }
}

static void Dem_StartOperationCycle(uint8_t operationCycleId) {
  uint16_t i;
  Dem_EventContextType *EventContext;
  P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST) EventConfig;
  Dem_EventStatusRecordType *EventStatus;

  ASLOG(DEMI, ("Operation Cycle %d Start\n", operationCycleId));

  for (i = 0; i < DEM_CONFIG->NumOfEvents; i++) {
    EventConfig = &DEM_CONFIG->EventConfigs[i];
    EventContext = &DEM_CONFIG->EventContexts[i];
    EventStatus = DEM_CONFIG->EventStatusRecords[i];

    if (EventConfig->OperationCycleRef == operationCycleId) {
      EventContext->Status = DEM_EVENT_STATUS_UNKNOWN;
      EventContext->Flag = 0;
      EventContext->DebounceCounter = 0;
#ifdef DEM_STATUS_BIT_STORAGE_TEST_FAILED
#else
      Dem_UdsBitClear(EventStatus->Status, DEM_UDS_STATUS_TF);
#endif
      /* @SWS_Dem_00389 */
      Dem_UdsBitClear(EventStatus->Status, DEM_UDS_STATUS_TFTOC);
      /* @SWS_Dem_00394 */
      Dem_UdsBitSet(EventStatus->Status, DEM_UDS_STATUS_TNCTOC);
      if (NULL != EventConfig->DTCRef) {
        Dem_DtcUpdateOnOperationCycleStart(EventConfig->DTCRef);
      }
    }

    if (NULL != EventConfig->DemCallbackInitMForE) {
      /* @SWS_Dem_00679 */
      EventConfig->DemCallbackInitMForE(DEM_INIT_MONITOR_RESTART);
    }
  }
}

static void Dem_DtcUpdateOnOperationCycleStop(P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) dtc,
                                              boolean *bAged) {
  Dem_DtcStatusRecordType *StatusRecord;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;
  uint16_t i;
  boolean bDirty;
  *bAged = FALSE;
  for (i = 0; i < dtc->DTCAttributes->NumOfMemoryDestination; i++) {
    bDirty = FALSE;
    memory = dtc->DTCAttributes->MemoryDestination[i];
    StatusRecord = memory->StatusRecords[dtc->DtcId];
    if (0u == (StatusRecord->Status & (DEM_UDS_STATUS_TNCTOC | DEM_UDS_STATUS_TFTOC))) {
      /* ref: Figure 7.43: General diagnostic event deletion processing */
      if (0u != (StatusRecord->Status & DEM_UDS_STATUS_PDTC)) {
        /* @SWS_Dem_00390 */
        Dem_UdsBitClear(StatusRecord->Status, DEM_UDS_STATUS_PDTC);
        bDirty = TRUE;
      }
      if (0u != (StatusRecord->Status & DEM_UDS_STATUS_CDTC)) {
        if ((TRUE == dtc->DTCAttributes->AgingAllowed) &&
            (StatusRecord->AgingCounter < dtc->DTCAttributes->AgingCycleCounterThreshold)) {
          StatusRecord->AgingCounter++;
          bDirty = TRUE;
          ASLOG(DEMI,
                ("DTC %d aging %d out of %d on origin %d\n", dtc->DtcId, StatusRecord->AgingCounter,
                 dtc->DTCAttributes->AgingCycleCounterThreshold, memory->DTCOrigin));
          if (StatusRecord->AgingCounter >= dtc->DTCAttributes->AgingCycleCounterThreshold) {
            *bAged = TRUE;
            /* @SWS_Dem_00498 */
            Dem_UdsBitClear(StatusRecord->Status, DEM_UDS_STATUS_CDTC);
#if DEM_STATUS_BIT_HANDLING_TEST_FAILED_SINCE_LAST_CLEAR == DEM_STATUS_BIT_AGING_AND_DISPLACEMENT
            /* @SWS_Dem_01054 */
            Dem_UdsBitClear(StatusRecord->Status, DEM_UDS_STATUS_TFSLC);
#endif
            StatusRecord->AgingCounter = 0;
            StatusRecord->FaultOccurrenceCounter = 0;
#ifdef DEM_USE_CYCLES_SINCE_LAST_FAILED
            StatusRecord->CyclesSinceLastFailed = DEM_CYCLE_COUNTER_STOPPED;
#endif
            /* @SWS_Dem_01075 */
            Dem_FreeFreezeFrameRecord(dtc->DtcId, memory);
#ifdef DEM_USE_NVM_EXTENDED_DATA
            Dem_FreeExtendedDataRecord(dtc->DtcId, memory);
#endif

            if (StatusRecord->AgedCounter < UINT8_MAX) {
              StatusRecord->AgedCounter++;
            }
          }
        }
      }
    }

#ifdef DEM_USE_CYCLES_SINCE_LAST_FAILED /* @SWS_Dem_00773 @SWS_Dem_00774 */
    if (StatusRecord->CyclesSinceLastFailed < DEM_CYCLE_COUNTER_MAX) {
      StatusRecord->CyclesSinceLastFailed++;
      bDirty = TRUE;
    }
#endif

    if (TRUE == bDirty) {
#ifdef DEM_USE_NVM
      (void)NvM_WriteBlock(memory->StatusNvmBlockIds[dtc->DtcId], NULL);
#else
      Dem_SetDirty(memory->StatusRecordsDirty, dtc->DtcId);
#endif
    }
  }
}

static void Dem_StopOperationCycle(uint8_t operationCycleId) {
  uint16_t i;
  uint16_t j;
  P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST) EventConfig;
  Dem_EventStatusRecordType *EventStatus;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) DTCRef;
  boolean bAged = FALSE;
  boolean bUpdated = FALSE;
  Dem_EventIdType EventId;

  ASLOG(DEMI, ("Operation Cycle %d Stop\n", operationCycleId));

  for (i = 0; i < DEM_CONFIG->NumOfDtcs; i++) {
    DTCRef = &DEM_CONFIG->Dtcs[i];
    EventId = DTCRef->EventIdRefs[0];
    /* the main event that this DTC belongs to */
    EventConfig = &DEM_CONFIG->EventConfigs[EventId];
    /* bAged is loop carried, it must not leak from the previous DTC: a DTC of
     * another operation cycle keeps its state untouched */
    bAged = FALSE;
    if (EventConfig->OperationCycleRef == operationCycleId) {
      Dem_DtcUpdateOnOperationCycleStop(DTCRef, &bAged);
    }

    if (TRUE == bAged) {
#if defined(DEM_USE_FAILED_DTC_ORDER_QUEUE) || defined(DEM_USE_CONFIRMED_DTC_ORDER_QUEUE)
      /* @SWS_Dem_00498: the DTC stored state is erased on aging completion,
       * remove all its entries from the occurrence-order queues */
      Dem_DtcOrderQueueRemove(DTCRef->DtcId);
#endif
      /* @SWS_Dem_00442 */
      for (j = 0; j < DTCRef->NumOfEvents; j++) {
        EventId = DTCRef->EventIdRefs[j];
        EventStatus = DEM_CONFIG->EventStatusRecords[EventId];
        bUpdated = FALSE;

        if (0u != (EventStatus->Status & DEM_UDS_STATUS_CDTC)) { /* @SWS_Dem_00498 */
          Dem_UdsBitClear(EventStatus->Status, DEM_UDS_STATUS_CDTC);
          bUpdated = TRUE;
        }
#if DEM_STATUS_BIT_HANDLING_TEST_FAILED_SINCE_LAST_CLEAR == DEM_STATUS_BIT_AGING_AND_DISPLACEMENT
        if (0u != (EventStatus->Status & DEM_UDS_STATUS_TFSLC)) { /* @SWS_Dem_01054 */
          Dem_UdsBitClear(EventStatus->Status, DEM_UDS_STATUS_TFSLC);
          bUpdated = TRUE;
        }
#endif

        if (EventStatus->TestFailedCounter != 0u) {
          EventStatus->TestFailedCounter = 0;
          bUpdated = TRUE;
        }

        if (TRUE == bUpdated) {
#ifdef DEM_USE_NVM
          (void)NvM_WriteBlock(DEM_CONFIG->EventStatusNvmBlockIds[EventId], NULL);
#else
          Dem_SetDirty(DEM_CONFIG->EventStatusDirty, EventId);
#endif
        }
      }
    } else {
      for (j = 0; j < DTCRef->NumOfEvents; j++) {
        EventId = DTCRef->EventIdRefs[j];
        EventStatus = DEM_CONFIG->EventStatusRecords[EventId];
        if (0u == (EventStatus->Status & (DEM_UDS_STATUS_TNCTOC | DEM_UDS_STATUS_TFTOC))) {
          if (0u != (EventStatus->Status & DEM_UDS_STATUS_PDTC)) {
            Dem_UdsBitClear(EventStatus->Status, DEM_UDS_STATUS_PDTC);
            EventStatus->TestFailedCounter = 0;
#ifdef DEM_USE_NVM
            (void)NvM_WriteBlock(DEM_CONFIG->EventStatusNvmBlockIds[EventId], NULL);
#else
            Dem_SetDirty(DEM_CONFIG->EventStatusDirty, EventId);
#endif
          }
        }
      }
    }
  }
}

static Std_ReturnType Dem_IsOperationCycleStarted(uint8_t operationCycleId) {
  Std_ReturnType r = E_NOT_OK;
  if (DEM_OPERATION_CYCLE_STARTED == DEM_CONFIG->OperationCycleStates[operationCycleId]) {
    r = E_OK;
  }
  return r;
}

#ifdef DEM_USE_ENABLE_CONDITION
static Std_ReturnType Dem_IsEventConditionEnabled(P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST)
                                                    eventConfig) {
  Std_ReturnType r = E_NOT_OK;
  if ((0u == eventConfig->ConditionRefMask) ||
      (eventConfig->ConditionRefMask ==
       (demContext.ConditionMask & eventConfig->ConditionRefMask))) {
    r = E_OK;
  }
  return r;
}
#endif

static Std_ReturnType Dem_SetEventStatusDebounce(Dem_EventIdType eventId,
                                                 Dem_EventStatusType eventStatus) {
  Std_ReturnType r = E_OK;
  P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST) EventConfig;
  Dem_EventContextType *EventContext;
  P2CONST(Dem_DebounceCounterBasedConfigType, AUTOMATIC, DEM_CONST) DebounceCfg;
  Dem_EventStatusType DebouncedStatus;

  EventConfig = &DEM_CONFIG->EventConfigs[eventId];
  EventContext = &DEM_CONFIG->EventContexts[eventId];
  DebounceCfg = EventConfig->DebounceCounterBased;
  DebouncedStatus = EventContext->Status;

  switch (eventStatus) {
  case DEM_EVENT_STATUS_PASSED:
    EventContext->DebounceCounter = DebounceCfg->DebounceCounterPassedThreshold;
    break;
  case DEM_EVENT_STATUS_FAILED:
    EventContext->DebounceCounter = DebounceCfg->DebounceCounterFailedThreshold;
    break;
  case DEM_EVENT_STATUS_PREPASSED:
    /* @SWS_Dem_00423 */
    if ((DebounceCfg->DebounceCounterJumpDown) &&
        (EventContext->DebounceCounter > DebounceCfg->DebounceCounterJumpDownValue)) {
      EventContext->DebounceCounter = DebounceCfg->DebounceCounterJumpDownValue;
    }

    if (EventContext->DebounceCounter >= (DebounceCfg->DebounceCounterPassedThreshold +
                                          DebounceCfg->DebounceCounterDecrementStepSize)) {
      EventContext->DebounceCounter -= DebounceCfg->DebounceCounterDecrementStepSize;
    } else {
      EventContext->DebounceCounter = DebounceCfg->DebounceCounterPassedThreshold;
    }
    break;
  case DEM_EVENT_STATUS_PREFAILED:
    /* @SWS_Dem_00425 */
    if ((DebounceCfg->DebounceCounterJumpUp) &&
        (EventContext->DebounceCounter < DebounceCfg->DebounceCounterJumpUpValue)) {
      EventContext->DebounceCounter = DebounceCfg->DebounceCounterJumpUpValue;
    }

    if (EventContext->DebounceCounter <= (DebounceCfg->DebounceCounterFailedThreshold -
                                          DebounceCfg->DebounceCounterIncrementStepSize)) {
      EventContext->DebounceCounter += DebounceCfg->DebounceCounterIncrementStepSize;
    } else {
      EventContext->DebounceCounter = DebounceCfg->DebounceCounterFailedThreshold;
    }
    break;
  default:
    r = E_NOT_OK;
    break;
  }

  ASLOG(DEMI, ("Event %d debounce counter = %d, PASS=%d(-%d), FAIL=%d(+%d)\n", eventId,
               (int)EventContext->DebounceCounter, (int)DebounceCfg->DebounceCounterPassedThreshold,
               (int)DebounceCfg->DebounceCounterDecrementStepSize,
               (int)DebounceCfg->DebounceCounterFailedThreshold,
               (int)DebounceCfg->DebounceCounterIncrementStepSize));

  if (EventContext->DebounceCounter <= DebounceCfg->DebounceCounterPassedThreshold) {
    DebouncedStatus = DEM_EVENT_STATUS_PASSED;
  } else if (EventContext->DebounceCounter >= DebounceCfg->DebounceCounterFailedThreshold) {
    DebouncedStatus = DEM_EVENT_STATUS_FAILED;
  } else {
    /* keep no change */
  }

  EventContext->Status = DebouncedStatus;

  return r;
}

static uint16_t Dem_FillExtendedData(P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) dtc,
                                     P2CONST(Dem_ExtendedDataRecordClassType, AUTOMATIC, DEM_CONST)
                                       extendedDataRecordClass,
                                     uint8_t *data, Dem_DTCOriginType dtcOrigin) {
  uint16_t offset = 0;
  uint16_t i;
  uint8_t index;
  Std_ReturnType ret;

  for (i = 0; i < extendedDataRecordClass->NumOfExtendedData; i++) {
    index = extendedDataRecordClass->ExtendedDataNumberIndex[i];
    ret = DEM_CONFIG->ExtendedDataConfigs[index].GetExtendedDataFnc(dtc->DtcId, &data[offset],
                                                                    dtcOrigin);
    if (E_OK == ret) {
      offset += DEM_CONFIG->ExtendedDataConfigs[index].Length;
    } else {
      offset = 0; /* error return */
      break;
    }
  }

  return offset;
}

#ifdef DEM_RESET_CONFIRMED_BIT_ON_OVERFLOW
static void Dem_HandleDtcDisplacement(Dem_DtcIdType dtcId, P2CONST(Dem_MemoryDestinationType,
                                                                   AUTOMATIC, DEM_CONST) memory) {
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc;
  Dem_EventIdType EventId;
  Dem_EventStatusRecordType *EventStatus;
  uint16_t i;
  boolean bDirty;
  if (memory->DTCOrigin == DEM_DTC_ORIGIN_PRIMARY_MEMORY) {
    /* TODO: is this right that do this only for origin primary */
    Dtc = &DEM_CONFIG->Dtcs[dtcId];
#if defined(DEM_USE_FAILED_DTC_ORDER_QUEUE) || defined(DEM_USE_CONFIRMED_DTC_ORDER_QUEUE)
    /* the DTC is displaced and its CDTC is reset, remove all its entries from
     * the occurrence-order queues */
    Dem_DtcOrderQueueRemove(dtcId);
#endif
    /* @SWS_Dem_00443 */
    for (i = 0; i < Dtc->NumOfEvents; i++) {
      EventId = Dtc->EventIdRefs[i];
      EventStatus = DEM_CONFIG->EventStatusRecords[EventId];
      bDirty = FALSE;

      /* @SWS_Dem_00409 */
      if (0u != ((DEM_UDS_STATUS_PDTC | DEM_UDS_STATUS_CDTC) & EventStatus->Status)) {
        Dem_UdsBitClear(EventStatus->Status, DEM_UDS_STATUS_PDTC | DEM_UDS_STATUS_CDTC);
        bDirty = TRUE;
      }

#if DEM_STATUS_BIT_HANDLING_TEST_FAILED_SINCE_LAST_CLEAR == DEM_STATUS_BIT_AGING_AND_DISPLACEMENT
      /* @SWS_Dem_01186 */
      if (0u != (DEM_UDS_STATUS_TFSLC & EventStatus->Status)) {
        Dem_UdsBitClear(EventStatus->Status, DEM_UDS_STATUS_TFSLC);
        bDirty = TRUE;
      }
#endif
      if (TRUE == bDirty) {
#ifdef DEM_USE_NVM
        (void)NvM_WriteBlock(DEM_CONFIG->EventStatusNvmBlockIds[EventId], NULL);
#else
        Dem_SetDirty(DEM_CONFIG->EventStatusDirty, EventId);
#endif
      }
    }
  }
}
#endif

static uint8_t *Dem_MallocFreezeFrameRecord(Dem_DtcIdType dtcId,
                                            P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST)
                                              memory,
                                            uint16_t *ffId) {
  Dem_FreezeFrameRecordType *record = NULL;
  uint8_t *data;
  uint16_t i;
  uint16_t slot;
  uint8_t lowPriority;
  Dem_DtcIdType lowDtcId;
  *ffId = ((uint16_t)-1);

  EnterCritical();

  data = NULL;

  for (i = 0; i < memory->NumOfFreezeFrameRecords; i++) {
    if (DEM_INVALID_DTC_ID == memory->FreezeFrameRecords[i]->DtcId) {
      if (NULL == record) {
        record = memory->FreezeFrameRecords[i];
        *ffId = i;
      }
    } else if (dtcId == memory->FreezeFrameRecords[i]->DtcId) {
      record = memory->FreezeFrameRecords[i];
      *ffId = i;
      break;
    } else {
    }
  }

  if (NULL == record) {
    /* looking for displacement according to priority */
    lowPriority = DEM_CONFIG->Dtcs[dtcId].DTCAttributes->Priority;
    for (i = 0; i < memory->NumOfFreezeFrameRecords; i++) {
      lowDtcId = memory->FreezeFrameRecords[i]->DtcId;
      if (lowDtcId < DEM_CONFIG->NumOfDtcs) {
        if (DEM_CONFIG->Dtcs[lowDtcId].DTCAttributes->Priority > lowPriority) {
          record = memory->FreezeFrameRecords[i];
          *ffId = i;
          lowPriority = DEM_CONFIG->Dtcs[lowDtcId].DTCAttributes->Priority;
        }
      } else {
        ASLOG(DEME, ("an invalid freeze frame found, drop it\n"));
        record = memory->FreezeFrameRecords[i];
        *ffId = i;
        break;
      }
    }
  }

  if (NULL != record) {
    if (dtcId != record->DtcId) {
      if (record->DtcId < DEM_CONFIG->NumOfDtcs) {
        ASLOG(DEMI, ("replace DTC %d freeze frame @ %d for DTC %d\n", record->DtcId, *ffId, dtcId));
#ifdef DEM_RESET_CONFIRMED_BIT_ON_OVERFLOW
        Dem_HandleDtcDisplacement(record->DtcId, memory);
#endif
      }
      /* newly created */
      for (slot = 0; slot < DEM_MAX_FREEZE_FRAME_NUMBER; slot++) {
        record->FreezeFrameData[slot][0] =
          DEM_FREEZE_FRAME_SLOT_FREE; /* First byte set to 0xFFu: means free */
      }
      data = record->FreezeFrameData[0];
      record->DtcId = dtcId;
    } else {
      for (slot = 0; slot < DEM_MAX_FREEZE_FRAME_NUMBER; slot++) {
        if (DEM_FREEZE_FRAME_SLOT_FREE == record->FreezeFrameData[slot][0]) {
          data = record->FreezeFrameData[slot];
          break;
        }
      }

      if (NULL == data) {
        /* overwrite the last one with latest environment data */
        data = record->FreezeFrameData[DEM_MAX_FREEZE_FRAME_NUMBER - 1u];
      }
    }
  }

  ExitCritical();

  return data;
}

#ifdef DEM_USE_NVM_EXTENDED_DATA
static uint8_t *Dem_MallocExtendedDataRecord(Dem_DtcIdType dtcId,
                                             P2CONST(Dem_MemoryDestinationType, AUTOMATIC,
                                                     DEM_CONST) memory,
                                             uint16_t *eeId) {
  Dem_ExtendedDataRecordType *record = NULL;
  uint8_t *data;
  uint16_t i;
  uint8_t lowPriority;
  Dem_DtcIdType lowDtcId;
  *eeId = ((uint16_t)-1);

  EnterCritical();

  data = NULL;

  for (i = 0; i < memory->NumOfExtendedDataRecords; i++) {
    if (DEM_INVALID_DTC_ID == memory->ExtendedDataRecords[i]->DtcId) {
      if (NULL == record) {
        record = memory->ExtendedDataRecords[i];
        *eeId = i;
      }
    } else if (dtcId == memory->ExtendedDataRecords[i]->DtcId) {
      record = memory->ExtendedDataRecords[i];
      *eeId = i;
      break;
    } else {
    }
  }

  if (NULL == record) {
    /* looking for displacement according to priority */
    lowPriority = DEM_CONFIG->Dtcs[dtcId].DTCAttributes->Priority;
    for (i = 0; i < memory->NumOfExtendedDataRecords; i++) {
      lowDtcId = memory->ExtendedDataRecords[i]->DtcId;
      if (lowDtcId < DEM_CONFIG->NumOfDtcs) {
        if (DEM_CONFIG->Dtcs[lowDtcId].DTCAttributes->Priority > lowPriority) {
          record = memory->ExtendedDataRecords[i];
          *eeId = i;
          lowPriority = DEM_CONFIG->Dtcs[lowDtcId].DTCAttributes->Priority;
        }
      } else {
        ASLOG(DEME, ("an invalid extended data found, drop it\n"));
        record = memory->ExtendedDataRecords[i];
        *eeId = i;
        break;
      }
    }
  }

  if (NULL != record) {
    if (dtcId != record->DtcId) {
      if (record->DtcId < DEM_CONFIG->NumOfDtcs) {
        ASLOG(DEMI,
              ("replace DTC %d extended data @ %d for DTC %d\n", record->DtcId, *eeId, dtcId));
      }
      /* newly created */
      (void)memset(record->ExtendedData, DEM_EXTENDED_DATA_SLOT_FREE, sizeof(record->ExtendedData));
      data = record->ExtendedData;
      record->DtcId = dtcId;
    } else {
      data = record->ExtendedData;
    }
  }

  ExitCritical();

  return data;
}

static Dem_ExtendedDataRecordType *
Dem_LookupExtendedDataRecord(Dem_DtcIdType dtcId,
                             P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory) {
  Dem_ExtendedDataRecordType *record = NULL;
  uint16_t i;

  for (i = 0; i < memory->NumOfExtendedDataRecords; i++) {
    if (dtcId == memory->ExtendedDataRecords[i]->DtcId) {
      record = memory->ExtendedDataRecords[i];
    }
  }

  return record;
}
#endif

static Dem_FreezeFrameRecordType *Dem_LookupFreezeFrameRecordByRecordNumber(
  Dem_DtcIdType dtcId, P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory,
  uint8_t *recNum, uint8_t **data, uint16_t *where) {
  Dem_FreezeFrameRecordType *record = NULL;
  uint16_t index;
  uint16_t i;
  uint16_t slot;

  for (index = *where; index < (memory->NumOfFreezeFrameRecords * DEM_MAX_FREEZE_FRAME_NUMBER);
       index++) {
    i = index / DEM_MAX_FREEZE_FRAME_NUMBER;
    slot = index % DEM_MAX_FREEZE_FRAME_NUMBER;
    if ((dtcId == memory->FreezeFrameRecords[i]->DtcId) &&
        (DEM_FREEZE_FRAME_SLOT_FREE != memory->FreezeFrameRecords[i]->FreezeFrameData[slot][0])) {
      if ((*recNum == memory->FreezeFrameRecords[i]->FreezeFrameData[slot][0]) ||
          (0xFFu == *recNum)) {
        record = memory->FreezeFrameRecords[i];
        if (data != NULL) {
          *data = &memory->FreezeFrameRecords[i]->FreezeFrameData[slot][1];
        }
        *recNum = memory->FreezeFrameRecords[i]->FreezeFrameData[slot][0];
        *where = index + 1u;
        break;
      }
    }
  }

  return record;
}

static uint16_t Dem_GetNumberOfFreezeFrameRecordsByRecordNumber(
  Dem_DtcIdType dtcId, P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory,
  uint8_t recNum) {
  uint16_t r = 0;
  uint16_t index = 0;
  uint8_t _recNum;

  Dem_FreezeFrameRecordType *record = NULL;

  do {
    _recNum = recNum;
    record = Dem_LookupFreezeFrameRecordByRecordNumber(dtcId, memory, &_recNum, NULL, &index);
    if (record != NULL) {
      r++;
    }
  } while (record != NULL);

  return r;
}

static Dem_FreezeFrameRecordType *
Dem_LookupFreezeFrameRecordByIndex(P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory,
                                   uint16_t index, uint8_t *recNum, uint8_t **data) {
  Dem_FreezeFrameRecordType *record = NULL;
  uint16_t i;
  uint16_t slot;

  i = index / DEM_MAX_FREEZE_FRAME_NUMBER;
  slot = index % DEM_MAX_FREEZE_FRAME_NUMBER;

  if (i < memory->NumOfFreezeFrameRecords) {
    if ((DEM_INVALID_DTC_ID != memory->FreezeFrameRecords[i]->DtcId) &&
        (DEM_FREEZE_FRAME_SLOT_FREE != memory->FreezeFrameRecords[i]->FreezeFrameData[slot][0])) {
      record = memory->FreezeFrameRecords[i];
      if (NULL != recNum) {
        *recNum = record->FreezeFrameData[slot][0];
      }
      if (NULL != data) {
        *data = &record->FreezeFrameData[slot][1];
      }
    }
  }

  return record;
}

static void Dem_FreeFreezeFrameRecord(Dem_DtcIdType dtcId, P2CONST(Dem_MemoryDestinationType,
                                                                   AUTOMATIC, DEM_CONST) memory) {
  Dem_FreezeFrameRecordType *record = NULL;
  uint16_t i;

  for (i = 0; i < memory->NumOfFreezeFrameRecords; i++) {
    if (dtcId == memory->FreezeFrameRecords[i]->DtcId) {
      ASLOG(DEMI,
            ("Delete DTC %d freeze frame @ %d from origin %d\n", dtcId, i, memory->DTCOrigin));
      record = memory->FreezeFrameRecords[i];
      record->DtcId = DEM_INVALID_DTC_ID;
      (void)memset(record->FreezeFrameData, DEM_FREEZE_FRAME_SLOT_FREE,
                   sizeof(record->FreezeFrameData));
#ifdef DEM_USE_NVM
      (void)NvM_WriteBlock(memory->FreezeFrameNvmBlockIds[i], NULL);
#else
      Dem_SetDirty(memory->FreezeFrameRecordsDirty, i);
#endif
    }
  }
}
#ifdef DEM_USE_NVM_EXTENDED_DATA
static void Dem_FreeExtendedDataRecord(Dem_DtcIdType dtcId, P2CONST(Dem_MemoryDestinationType,
                                                                    AUTOMATIC, DEM_CONST) memory) {
  Dem_ExtendedDataRecordType *record = NULL;
  uint16_t i;

  for (i = 0; i < memory->NumOfExtendedDataRecords; i++) {
    if (dtcId == memory->ExtendedDataRecords[i]->DtcId) {
      ASLOG(DEMI,
            ("Delete DTC %d extended data @ %d from origin %d\n", dtcId, i, memory->DTCOrigin));
      record = memory->ExtendedDataRecords[i];
      record->DtcId = DEM_INVALID_DTC_ID;
      (void)memset(record->ExtendedData, DEM_EXTENDED_DATA_SLOT_FREE, sizeof(record->ExtendedData));
#ifdef DEM_USE_NVM
      (void)NvM_WriteBlock(memory->ExtendedDataNvmBlockIds[i], NULL);
#else
      Dem_SetDirty(memory->ExtendedDataRecordsDirty, i);
#endif
    }
  }
}
#endif

static uint16_t Dem_GetExtendedDataRecordSize(P2CONST(Dem_ExtendedDataRecordClassType, AUTOMATIC,
                                                      DEM_CONST) extendedDataRecordClass) {
  uint16_t sz = 0;
  uint16_t index;
  uint16_t i;

  for (i = 0; i < extendedDataRecordClass->NumOfExtendedData; i++) {
    index = extendedDataRecordClass->ExtendedDataNumberIndex[i];
    sz += DEM_CONFIG->ExtendedDataConfigs[index].Length;
  }

  return sz;
}

static uint16_t Dem_GetFreezeFrameSize(P2CONST(Dem_FreezeFrameRecordClassType, AUTOMATIC, DEM_CONST)
                                         freezeFrameRecordClass) {
  uint16_t sz = 0;
  uint16_t i;
  uint16_t index;

  for (i = 0; i < freezeFrameRecordClass->NumOfFreezeFrameData; i++) {
    index = freezeFrameRecordClass->FreezeFrameDataIndex[i];
    sz += DEM_CONFIG->FreeFrameDataConfigs[index].Length;
  }

  return sz;
}
static void Dem_FillSnapshotRecord(uint8_t *destBuf, uint8_t *data,
                                   P2CONST(Dem_FreezeFrameRecordClassType, AUTOMATIC, DEM_CONST)
                                     freezeFrameRecordClass) {
  uint16_t i;
  uint16_t index;
  uint8_t *pIn = data;
  uint8_t *pOut = destBuf;

  for (i = 0; i < freezeFrameRecordClass->NumOfFreezeFrameData; i++) {
    index = freezeFrameRecordClass->FreezeFrameDataIndex[i];
    pOut[0] = (DEM_CONFIG->FreeFrameDataConfigs[index].Id >> 8) & 0xFFu;
    pOut[1] = DEM_CONFIG->FreeFrameDataConfigs[index].Id & 0xFFu;
    (void)memcpy(&pOut[2], pIn, DEM_CONFIG->FreeFrameDataConfigs[index].Length);
    pOut += 2u + DEM_CONFIG->FreeFrameDataConfigs[index].Length;
    pIn += DEM_CONFIG->FreeFrameDataConfigs[index].Length;
  }
}

static uint8_t Dem_GetNextRecordNumber(Dem_DtcIdType dtcId, P2CONST(Dem_MemoryDestinationType,
                                                                    AUTOMATIC, DEM_CONST) memory) {
  uint8_t recNum;
  uint16_t numOfRecs;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc = &DEM_CONFIG->Dtcs[dtcId];
  const Dem_FreezeFrameRecNumClassType *FreezeFrameRecNumClass =
    Dtc->DTCAttributes->FreezeFrameRecNumClass;

  numOfRecs = Dem_GetNumberOfFreezeFrameRecordsByRecordNumber(dtcId, memory, 0xFFu);

  if (DEM_FF_RECNUM_CONFIGURED == DEM_CONFIG->TypeOfFreezeFrameRecordNumeration) {
    /* @SWS_Dem_00337 @SWS_Dem_00582 */
    if (numOfRecs < FreezeFrameRecNumClass->NumOfFreezeFrameRecNums) {
      recNum = FreezeFrameRecNumClass->FreezeFrameRecNums[numOfRecs];
    } else {
      recNum = FreezeFrameRecNumClass
                 ->FreezeFrameRecNums[FreezeFrameRecNumClass->NumOfFreezeFrameRecNums - 1u];
    }
  } else {
    /* @SWS_Dem_00581 */
    recNum = (uint8_t)numOfRecs + 1u;
  }

  return recNum;
}

static void Dem_TrigerStoreFreezeFrame(P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) dtcRef,
                                       P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST)
                                         memory) {
  P2CONST(Dem_FreezeFrameRecordClassType, AUTOMATIC, DEM_CONST)
  FreezeFrameRecordClass = dtcRef->DTCAttributes->FreezeFrameRecordClass;
  uint8_t *data = NULL;
  uint16_t ffId = 0;
  uint16_t offset = 1;
  uint16_t i;
  uint16_t index;

  data = Dem_MallocFreezeFrameRecord(dtcRef->DtcId, memory, &ffId);
  if (data != NULL) {
    for (i = 0; i < FreezeFrameRecordClass->NumOfFreezeFrameData; i++) {
      index = FreezeFrameRecordClass->FreezeFrameDataIndex[i];
      if ((offset + DEM_CONFIG->FreeFrameDataConfigs[index].Length) <=
          DEM_MAX_FREEZE_FRAME_DATA_SIZE) {
        DEM_CONFIG->FreeFrameDataConfigs[index].GetFrezeFrameDataFnc(dtcRef->DtcId, &data[offset],
                                                                     memory->DTCOrigin);
        offset += DEM_CONFIG->FreeFrameDataConfigs[index].Length;
      } else {
        ASLOG(DEME, ("FreezeFrame record size too small\n"));
      }
    }
    data[0] = Dem_GetNextRecordNumber(dtcRef->DtcId, memory);
    ASLOG(DEMI, ("DTC %d capture freeze frame with record number %d on origin %d\n", dtcRef->DtcId,
                 data[0], memory->DTCOrigin));
#ifdef DEM_USE_NVM
    (void)NvM_WriteBlock(memory->FreezeFrameNvmBlockIds[ffId], NULL);
#else
    Dem_SetDirty(memory->FreezeFrameRecordsDirty, ffId);
#endif
  } else {
    ASLOG(DEMI, ("DTC %d freeze failed as no slot\n", dtcRef->DtcId));
  }
}

#ifdef DEM_USE_NVM_EXTENDED_DATA
static void Dem_TrigerStoreExtendedData(P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) dtcRef,
                                        P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST)
                                          memory) {
  P2CONST(Dem_ExtendedDataClassType, AUTOMATIC, DEM_CONST)
  ExtendedDataClass = dtcRef->DTCAttributes->ExtendedDataClass;
  P2CONST(Dem_ExtendedDataRecordClassType, AUTOMATIC, DEM_CONST) ExtendedDataRecordClassRef;
  uint16_t i;
  uint16_t eeId = 0;
  uint8_t *data = NULL;
  uint16_t offset = 0;
  uint16_t ret = 0;

  data = Dem_MallocExtendedDataRecord(dtcRef->DtcId, memory, &eeId);
  if (NULL != data) {
    for (i = 0; i < ExtendedDataClass->NumOfExtendedDataRecordClassRef; i++) {
      ExtendedDataRecordClassRef = ExtendedDataClass->ExtendedDataRecordClassRef[i];
      data[offset] = ExtendedDataRecordClassRef->ExtendedDataRecordNumber;
      offset += 1u;
      ret =
        Dem_FillExtendedData(dtcRef, ExtendedDataRecordClassRef, &data[offset], memory->DTCOrigin);
      if (ret > 0u) {
        offset += ret;
        ASLOG(DEMI,
              ("DTC %d capture extended data with record number %d on origin %d\n", dtcRef->DtcId,
               ExtendedDataRecordClassRef->ExtendedDataRecordNumber, memory->DTCOrigin));
      } else {
        ASLOG(DEME, ("DTC %d capture extended data with record number %d on origin %d failed\n",
                     dtcRef->DtcId, ExtendedDataRecordClassRef->ExtendedDataRecordNumber,
                     memory->DTCOrigin));
        break;
      }
    }

    if (ret > 0u) {
#ifdef DEM_USE_NVM
      (void)NvM_WriteBlock(memory->ExtendedDataNvmBlockIds[eeId], NULL);
#else
      Dem_SetDirty(memory->ExtendedDataRecordsDirty, eeId);
#endif
    } else {
      Dem_FreeExtendedDataRecord(dtcRef->DtcId, memory);
    }
  } else {
    ASLOG(DEMI, ("DTC %d store extended data failed as no slot\n", dtcRef->DtcId));
  }
}
#endif

static void Dem_UpdateDtcStatus(P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) dtcRef,
                                P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory) {
  uint16_t i;
  Dem_DtcStatusRecordType *StatusRecord = memory->StatusRecords[dtcRef->DtcId];
  Dem_EventStatusRecordType *EventStatus;
  Dem_UdsStatusByteType status = 0;
  Dem_EventIdType EventId;
  /* @SWS_Dem_00441 */
  for (i = 0; i < dtcRef->NumOfEvents; i++) {
    EventId = dtcRef->EventIdRefs[i];
    EventStatus = DEM_CONFIG->EventStatusRecords[EventId];
    status |= EventStatus->Status;
  }

  if (0u != (DEM_UDS_STATUS_TFSLC & status)) {
    status &= ~DEM_UDS_STATUS_TNCSLC;
  }

  if (0u != (DEM_UDS_STATUS_TFTOC & status)) {
    status &= ~DEM_UDS_STATUS_TNCTOC;
  }

  StatusRecord->Status = status;
}

/* Figure 7.27: General diagnostic event storage processing */
static void Dem_DtcUpdateOnFailed(Dem_EventIdType eventId,
                                  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) dtcRef, int origin) {
  Dem_EventContextType *EventContext = &DEM_CONFIG->EventContexts[eventId];
  const Dem_DTCAttributesType *DTCAttributes = dtcRef->DTCAttributes;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST)
  memory = dtcRef->DTCAttributes->MemoryDestination[origin];
  Dem_DtcStatusRecordType *StatusRecord = memory->StatusRecords[dtcRef->DtcId];
  Dem_UdsStatusByteType oldStatus = StatusRecord->Status;
  boolean bDirty = FALSE;
  boolean bCaptureFF = FALSE;
#ifdef DEM_USE_NVM_EXTENDED_DATA
  boolean bCaptureEE = FALSE;
#endif
  Dem_UpdateDtcStatus(dtcRef, memory);

  StatusRecord->AgingCounter = 0;

  if (0u == (oldStatus & DEM_UDS_STATUS_TF)) {
    /* @SWS_Dem_00524 */
    if (DEM_PROCESS_OCCCTR_TF == DTCAttributes->OccurrenceCounterProcessing) {
      if (StatusRecord->FaultOccurrenceCounter < UINT8_MAX) {
        StatusRecord->FaultOccurrenceCounter++;
        bDirty = TRUE;
      }
    }

    if (DEM_TRIGGER_ON_TEST_FAILED == DTCAttributes->FreezeFrameRecordTrigger) {
      /* @SWS_Dem_00800 */
      bCaptureFF = TRUE;
    }
#ifdef DEM_USE_NVM_EXTENDED_DATA
    if (DEM_TRIGGER_ON_TEST_FAILED == DTCAttributes->ExtendedDataRecordTrigger) {
      /* @SWS_Dem_00812 */
      bCaptureEE = TRUE;
    }
#endif
#ifdef DEM_USE_CYCLES_SINCE_LAST_FAILED /* @SWS_Dem_00771 @SWS_Dem_00772 */
    StatusRecord->CyclesSinceLastFailed = 0;
#endif
#if defined(DEM_USE_FAILED_DTC_ORDER_QUEUE)
    if (0u == origin) {
      /* the DTC level TF 0->1 edge, stamp the failed-order history queue */
      Dem_FailedOrderQueueInsert(dtcRef->DtcId);
    }
#endif
  }

  if (0u == (oldStatus & DEM_UDS_STATUS_TFTOC)) {
    if (DEM_PROCESS_OCCCTR_TFTOC == DTCAttributes->OccurrenceCounterProcessing) {
      if (StatusRecord->FaultOccurrenceCounter < UINT8_MAX) {
        StatusRecord->FaultOccurrenceCounter++;
        bDirty = TRUE;
      }
    }
  }

  if ((0u == (oldStatus & DEM_UDS_STATUS_PDTC)) &&
      (0u != (StatusRecord->Status & DEM_UDS_STATUS_PDTC))) {
    /* @SWS_Dem_00801 */
    if (DEM_TRIGGER_ON_PENDING == DTCAttributes->FreezeFrameRecordTrigger) {
      bCaptureFF = TRUE;
    }
#ifdef DEM_USE_NVM_EXTENDED_DATA
    if (DEM_TRIGGER_ON_PENDING == DTCAttributes->ExtendedDataRecordTrigger) {
      /* @SWS_Dem_00813 */
      bCaptureEE = TRUE;
    }
#endif
  }

  if ((0u == (oldStatus & DEM_UDS_STATUS_CDTC)) &&
      (0u != (StatusRecord->Status & DEM_UDS_STATUS_CDTC))) {
    /* @SWS_Dem_00580 */
    if (DEM_PROCESS_OCCCTR_CDTC == DTCAttributes->OccurrenceCounterProcessing) {
      if (StatusRecord->FaultOccurrenceCounter < UINT8_MAX) {
        StatusRecord->FaultOccurrenceCounter++;
        bDirty = TRUE;
      }
    }

    if (DEM_TRIGGER_ON_CONFIRMED == DTCAttributes->FreezeFrameRecordTrigger) {
      /* @SWS_Dem_00802 */
      bCaptureFF = TRUE;
    }
#ifdef DEM_USE_NVM_EXTENDED_DATA
    if (DEM_TRIGGER_ON_CONFIRMED == DTCAttributes->ExtendedDataRecordTrigger) {
      /* @SWS_Dem_00814 */
      bCaptureEE = TRUE;
    }
#endif
#if defined(DEM_USE_CONFIRMED_DTC_ORDER_QUEUE)
    if (0u == origin) {
      /* the DTC level CDTC 0->1 edge, stamp the confirmed-order queue */
      Dem_ConfirmedOrderQueueAppend(dtcRef->DtcId);
    }
#endif
  }

  if (DEM_TRIGGER_ON_EVERY_TEST_FAILED == DTCAttributes->FreezeFrameRecordTrigger) {
    /* @SWS_Dem_01308 */
    bCaptureFF = TRUE;
  }

  if ((oldStatus & DEM_NVM_STORE_CARED_BITS) != (StatusRecord->Status & DEM_NVM_STORE_CARED_BITS)) {
    bDirty = TRUE;
  }

  if (TRUE == bCaptureFF) {
    if (DEM_CAPTURE_SYNCHRONOUS_TO_REPORTING == dtcRef->DTCAttributes->EnvironmentDataCapture) {
      Dem_TrigerStoreFreezeFrame(dtcRef, memory); /* @SWS_Dem_00805 */
    } else {
      Std_FlagSet(EventContext->Flag, DEM_EVENT_FLAG_CAPTURE_FF << origin);
    }
  }

#ifdef DEM_USE_NVM_EXTENDED_DATA
  if (TRUE == bCaptureEE) {
    if (DEM_CAPTURE_SYNCHRONOUS_TO_REPORTING == dtcRef->DTCAttributes->EnvironmentDataCapture) {
      Dem_TrigerStoreExtendedData(dtcRef, memory); /* @SWS_Dem_01081 */
    } else {
      Std_FlagSet(EventContext->Flag, DEM_EVENT_FLAG_CAPTURE_EE << origin);
    }
  }
#endif

  if (TRUE == bDirty) {
#ifdef DEM_USE_NVM
    (void)NvM_WriteBlock(memory->StatusNvmBlockIds[dtcRef->DtcId], NULL);
#else
    Dem_SetDirty(memory->StatusRecordsDirty, dtcRef->DtcId);
#endif
  }
}

static void Dem_EventUpdateOnFailed(Dem_EventIdType eventId) {
  P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST)
  EventConfig = &DEM_CONFIG->EventConfigs[eventId];
  Dem_EventStatusRecordType *EventStatus = DEM_CONFIG->EventStatusRecords[eventId];
  Dem_UdsStatusByteType oldStatus = EventStatus->Status;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) DTCRef = EventConfig->DTCRef;
  boolean bUpdated = FALSE;
  uint16_t i;

  ASLOG(DEMI, ("Event %d Test Failed\n", eventId));

  Dem_UdsBitClear(EventStatus->Status, DEM_UDS_STATUS_TNCTOC | DEM_UDS_STATUS_TNCSLC);

  Dem_UdsBitSet(EventStatus->Status, DEM_UDS_STATUS_TF | DEM_UDS_STATUS_TFTOC |
                                       DEM_UDS_STATUS_PDTC | DEM_UDS_STATUS_TFSLC);

  if (EventStatus->TestFailedCounter < UINT8_MAX) {
    EventStatus->TestFailedCounter++;
    bUpdated = TRUE;
  }

  if (EventStatus->TestFailedCounter >= EventConfig->ConfirmationThreshold) {
    ASLOG(DEMI, ("Event %d confirmed\n", eventId));
    Dem_UdsBitSet(EventStatus->Status, DEM_UDS_STATUS_CDTC);
  }

  if ((oldStatus & DEM_NVM_STORE_CARED_BITS) != (EventStatus->Status & DEM_NVM_STORE_CARED_BITS)) {
    bUpdated = TRUE;
  }

  if (TRUE == bUpdated) {
#ifdef DEM_USE_NVM
    (void)NvM_WriteBlock(DEM_CONFIG->EventStatusNvmBlockIds[eventId], NULL);
#else
    Dem_SetDirty(DEM_CONFIG->EventStatusDirty, eventId);
#endif
  }

  if (NULL != DTCRef) {
    /* @SWS_Dem_01199 @SWS_Dem_01207 */
    for (i = 0; i < DTCRef->DTCAttributes->NumOfMemoryDestination; i++) {
      /* @SWS_Dem_01063
       * Note: The mirror memory may have a different behaviour which is project
       * specific and is not described in this document.*/
      Dem_DtcUpdateOnFailed(eventId, DTCRef, i);
    }
  }
}

static void Dem_DtcUpdateOnPass(Dem_EventIdType eventId,
                                P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) dtcRef,
                                P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory) {
  Dem_DtcStatusRecordType *StatusRecord = memory->StatusRecords[dtcRef->DtcId];
  Dem_UdsStatusByteType oldStatus = StatusRecord->Status;
  boolean bUpdated = FALSE;
  (void)eventId;
  Dem_UpdateDtcStatus(dtcRef, memory);

  if ((oldStatus & DEM_NVM_STORE_CARED_BITS) != (StatusRecord->Status & DEM_NVM_STORE_CARED_BITS)) {
    bUpdated = TRUE;
  }

  if (TRUE == bUpdated) {
#ifdef DEM_USE_NVM
    (void)NvM_WriteBlock(memory->StatusNvmBlockIds[dtcRef->DtcId], NULL);
#else
    Dem_SetDirty(memory->StatusRecordsDirty, dtcRef->DtcId);
#endif
  }
}

static void Dem_EventUpdateOnPass(Dem_EventIdType eventId) {
  P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST)
  EventConfig = &DEM_CONFIG->EventConfigs[eventId];
  Dem_EventStatusRecordType *EventStatus = DEM_CONFIG->EventStatusRecords[eventId];
  Dem_EventContextType *EventContext = &DEM_CONFIG->EventContexts[eventId];
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) DTCRef = EventConfig->DTCRef;
  Dem_UdsStatusByteType oldStatus = EventStatus->Status;
  boolean bPass = TRUE;
  uint16_t i;

  ASLOG(DEMI, ("Event %d Test Passed\n", eventId));

  Dem_UdsBitClear(EventStatus->Status,
                  DEM_UDS_STATUS_TNCTOC | DEM_UDS_STATUS_TNCSLC | DEM_UDS_STATUS_TF);

  if ((oldStatus & DEM_NVM_STORE_CARED_BITS) != (EventStatus->Status & DEM_NVM_STORE_CARED_BITS)) {
#ifdef DEM_USE_NVM
    (void)NvM_WriteBlock(DEM_CONFIG->EventStatusNvmBlockIds[eventId], NULL);
#else
    Dem_SetDirty(DEM_CONFIG->EventStatusDirty, eventId);
#endif
  }

  if (NULL != DTCRef) {
    for (i = 0; i < DTCRef->NumOfEvents; i++) {
      EventContext = &DEM_CONFIG->EventContexts[DTCRef->EventIdRefs[i]];
      if (DEM_EVENT_STATUS_PASSED != EventContext->Status) {
        bPass = FALSE;
      }
    }
    if (TRUE == bPass) {
      for (i = 0; i < DTCRef->DTCAttributes->NumOfMemoryDestination; i++) {
        Dem_DtcUpdateOnPass(eventId, DTCRef, DTCRef->DTCAttributes->MemoryDestination[i]);
      }
    }
  }
}

/* Fault detection counter of a DTC, the FDC of the first event reference is
 * used: the internal debounce counter of the counter based debounce algorithm
 * scaled linearly onto the ISO 14229-1 range -128..127 (@SWS_Dem_00415),
 * i.e. 0x7F when the internal counter reached the failed threshold
 * (TestFailed) and 0x80 when it reached the passed threshold (Passed), no
 * testFailed shortcut: on recovery (PREPASSED reported while still
 * testFailed) the counter decreases from 127 downwards */
static sint8 Dem_GetFDCOfDTC(P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) dtc) {
  Dem_EventIdType eventId = dtc->EventIdRefs[0];
  Dem_EventContextType *EventContext = &DEM_CONFIG->EventContexts[eventId];
  P2CONST(Dem_DebounceCounterBasedConfigType, AUTOMATIC, DEM_CONST)
  DebounceCfg = DEM_CONFIG->EventConfigs[eventId].DebounceCounterBased;
  sint32 pdRange = (sint32)DebounceCfg->DebounceCounterFailedThreshold -
                   (sint32)DebounceCfg->DebounceCounterPassedThreshold;
  sint32 fdc = 0;

  if (pdRange > 0) {
    /* map the internal counter range [PassedThreshold..FailedThreshold] onto
     * the UDS range [-128..127] */
    fdc = (255L * ((sint32)EventContext->DebounceCounter -
                   (sint32)DebounceCfg->DebounceCounterPassedThreshold)) -
          (128L * pdRange);
    fdc /= pdRange;
  }

  return (sint8)fdc;
}

static boolean Dem_IsFilteredDTC(P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) dtc,
                                 P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory) {
  boolean r = TRUE;

  /* Values: 0x00: Autosar-specific value to deactivate the status-byte filtering (different meaning
   * than in ISO 14229-1) to report all supported DTCs (used for service 0x19 subfunctions
   * 0x0A/0x15) */
  if ((0u != demContext.Filter.StatusMask) &&
      (0u == (demContext.Filter.StatusMask & memory->StatusRecords[dtc->DtcId]->Status))) {
    r = FALSE;
  }

  /* @SWS_Dem_00208: with FilterWithSeverity a DTC is only reported when its
   * severity matches the mask, i.e. ((statusOfDTC & DTCStatusMask) != 0) &&
   * ((severity & DTCSeverityMask) != 0) */
  if ((TRUE == demContext.Filter.FilterWithSeverity) &&
      (0u == (demContext.Filter.DTCSeverityMask & dtc->Severity))) {
    r = FALSE;
  }

  /* @SWS_Dem_00208: with FilterForFaultDetectionCounter only the DTCs with a
   * fault detection counter between 1 and 0x7E shall be reported */
  if (TRUE == demContext.Filter.FilterForFaultDetectionCounter) {
    sint8 fdc = Dem_GetFDCOfDTC(dtc);
    if ((fdc < 1) || (fdc > 0x7E)) {
      r = FALSE;
    }
  }

  return r;
}

static boolean Dem_IsFilteredFF(int index) {
  /* only UDS FF are supported */
  (void)index;
  return TRUE;
}

static P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dem_LookupDtcByDTCNumber(uint32_t dtcNumber) {
  uint16_t i;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc = NULL;

  for (i = 0; i < DEM_CONFIG->NumOfDtcs; i++) {
    if (DEM_CONFIG->Dtcs[i].DtcNumber == dtcNumber) {
      Dtc = &DEM_CONFIG->Dtcs[i];
      break;
    }
  }

  return Dtc;
}

static Std_ReturnType Dem_IsDtcInGroup(Dem_DtcIdType dtcId, uint32_t groupDTC) {
  Std_ReturnType r = E_NOT_OK;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc;
  if (dtcId < DEM_CONFIG->NumOfDtcs) {
    Dtc = &DEM_CONFIG->Dtcs[dtcId];
    if (0xFFFFFFu == groupDTC) {
      r = E_OK;
    } else if (Dtc->DtcNumber == groupDTC) {
      r = E_OK;
    } else {
      /* not in the group */
    }
  } else if (dtcId != DEM_INVALID_EVENT_ID) {
    /* note: as this API is only used by ClearDTC, so if dtcId invalid, just clear it */
    ASLOG(DEME, ("Found invalid DTC with id %x\n", dtcId));
    r = E_OK;
  } else {
    /* do nothing */
  }
  return r;
}
/* ================================ [ FUNCTIONS ] ============================================== */
void Dem_PreInit(void) {
  /* NOTE: BSW Event is not supported, so this API is dummy */
}

void Dem_Init(const Dem_ConfigType *configPtr) {
  uint16_t i;
#if !defined(DEM_USE_NVM) || AS_LOG_DEMI > 0
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;
#endif
#if AS_LOG_DEMI > 0
  int j, k;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc;
#endif

#ifdef DEM_USE_PB_CONFIG
  if (NULL != configPtr) {
    DEM_CONFIG = configPtr;
  } else {
    DEM_CONFIG = &Dem_Config;
  }
#else
  (void)configPtr;
#endif

  for (i = 0; i < DEM_CONFIG->NumOfOperationCycles; i++) {
    DEM_CONFIG->OperationCycleStates[i] = DEM_OPERATION_CYCLE_STOPPED;
  }

  for (i = 0; i < DEM_CONFIG->NumOfEvents; i++) {
    Dem_EventInit((Dem_EventIdType)i);
  }
#ifndef DEM_USE_NVM
  (void)memset(DEM_CONFIG->EventStatusDirty, 0, (DEM_CONFIG->NumOfEvents + 7) / 8);
#endif
#if AS_LOG_DEMI > 0
  for (i = 0; i < DEM_CONFIG->NumOfDtcs; i++) {
    Dtc = &DEM_CONFIG->Dtcs[i];
    for (j = 0; j < Dtc->DTCAttributes->NumOfMemoryDestination; j++) {
      memory = Dtc->DTCAttributes->MemoryDestination[j];
      ASLOG(DEMI,
            ("Origin %d DTC %d status=%02X, agingCounter=%d, agedCounter=%d occuranceCounter=%d\n",
             memory->DTCOrigin, i, memory->StatusRecords[Dtc->DtcId]->Status,
             memory->StatusRecords[Dtc->DtcId]->AgingCounter,
             memory->StatusRecords[Dtc->DtcId]->AgedCounter,
             memory->StatusRecords[Dtc->DtcId]->FaultOccurrenceCounter));
    }
  }
#endif

  for (i = 0; i < DEM_CONFIG->NumOfMemoryDestination; i++) {
#if !defined(DEM_USE_NVM) || AS_LOG_DEMI > 0
    memory = &DEM_CONFIG->MemoryDestination[i];
#endif
#if AS_LOG_DEMI > 0
    for (j = 0; j < memory->NumOfFreezeFrameRecords; j++) {
      if (memory->FreezeFrameRecords[j]->DtcId != DEM_INVALID_DTC_ID) {
        for (k = 0; k < DEM_MAX_FREEZE_FRAME_NUMBER; k++) {
          if (DEM_FREEZE_FRAME_SLOT_FREE != memory->FreezeFrameRecords[j]->FreezeFrameData[k][0]) {
            ASLOG(DEMI, ("Origin %d DTC %d has freeze frame at slot %d with record number %d\n",
                         memory->DTCOrigin, memory->FreezeFrameRecords[j]->DtcId, k,
                         memory->FreezeFrameRecords[j]->FreezeFrameData[k][0]));
          }
        }
      }
    }
#endif
#ifndef DEM_USE_NVM
    (void)memset(memory->StatusRecordsDirty, 0, (memory->NumOfStatusRecords + 7) / 8);
    (void)memset(memory->FreezeFrameRecordsDirty, 0, (memory->NumOfFreezeFrameRecords + 7) / 8);
#ifdef DEM_USE_NVM_EXTENDED_DATA
    (void)memset(memory->ExtendedDataRecordsDirty, 0, (memory->NumOfExtendedDataRecords + 7) / 8);
#endif
#endif
  }

#if defined(DEM_USE_FAILED_DTC_ORDER_QUEUE) || defined(DEM_USE_CONFIRMED_DTC_ORDER_QUEUE)
  Dem_DtcOrderQueueInit();
#endif
  (void)memset(&demContext, 0, sizeof(demContext));
  demContext.DisableDtcSetting = FALSE;
}

Std_ReturnType Dem_EXTD_GetFaultOccurrenceCounter(Dem_DtcIdType dtcId, uint8_t *data,
                                                  Dem_DTCOriginType dtcOrigin) {
  Std_ReturnType r = E_OK;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;

  memory = Dem_LookupMemory(dtcOrigin);
  if (NULL != memory) {
    if (dtcId < memory->NumOfStatusRecords) {
      data[0] = memory->StatusRecords[dtcId]->FaultOccurrenceCounter;
    } else {
      r = E_NOT_OK;
    }
  } else {
    r = DEM_WRONG_DTCORIGIN;
  }

  return r;
}

Std_ReturnType Dem_EXTD_GetAgingCounter(Dem_DtcIdType dtcId, uint8_t *data,
                                        Dem_DTCOriginType dtcOrigin) {
  Std_ReturnType r = E_OK;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;

  memory = Dem_LookupMemory(dtcOrigin);
  if (NULL != memory) {
    if (dtcId < memory->NumOfStatusRecords) {
      data[0] = memory->StatusRecords[dtcId]->AgingCounter;
    } else {
      r = E_NOT_OK;
    }
  } else {
    r = DEM_WRONG_DTCORIGIN;
  }

  return r;
}

Std_ReturnType Dem_EXTD_GetAgedCounter(Dem_DtcIdType dtcId, uint8_t *data,
                                       Dem_DTCOriginType dtcOrigin) {
  Std_ReturnType r = E_OK;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;

  memory = Dem_LookupMemory(dtcOrigin);
  if (NULL != memory) {
    if (dtcId < memory->NumOfStatusRecords) {
      data[0] = memory->StatusRecords[dtcId]->AgedCounter;
    } else {
      r = E_NOT_OK;
    }
  } else {
    r = DEM_WRONG_DTCORIGIN;
  }

  return r;
}

Std_ReturnType Dem_SetOperationCycleState(uint8_t operationCycleId,
                                          Dem_OperationCycleStateType cycleState) {
  Std_ReturnType r = E_OK;

  DET_VALIDATE(NULL != DEM_CONFIG, 0xF1, DEM_E_UNINIT, return E_NOT_OK);
  DET_VALIDATE(operationCycleId < DEM_CONFIG->NumOfOperationCycles, 0xF1, DEM_E_PARAM_DATA,
               return E_NOT_OK);

  if (cycleState != DEM_CONFIG->OperationCycleStates[operationCycleId]) {
    if (DEM_OPERATION_CYCLE_STARTED == cycleState) {
      Dem_StartOperationCycle(operationCycleId);
    } else {
      Dem_StopOperationCycle(operationCycleId);
    }
    DEM_CONFIG->OperationCycleStates[operationCycleId] = cycleState;
  }

  return r;
}

Std_ReturnType Dem_RestartOperationCycle(uint8_t operationCycleId) {
  Std_ReturnType r = E_OK;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x08, DEM_E_UNINIT, return E_NOT_OK);
  DET_VALIDATE(NULL != DEM_CONFIG, 0x08, DEM_E_UNINIT, return E_NOT_OK);
  DET_VALIDATE(operationCycleId < DEM_CONFIG->NumOfOperationCycles, 0x08, DEM_E_PARAM_DATA,
               return E_NOT_OK);

  if (DEM_OPERATION_CYCLE_STARTED == DEM_CONFIG->OperationCycleStates[operationCycleId]) {
    Dem_StopOperationCycle(operationCycleId);
    Dem_StartOperationCycle(operationCycleId);
  } else {
    r = E_NOT_OK;
  }

  return r;
}

Std_ReturnType Dem_SetEventStatus(Dem_EventIdType eventId, Dem_EventStatusType eventStatus) {
  Std_ReturnType r = E_OK;
  P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST) EventConfig;
  Dem_EventContextType *EventContext;
  Dem_EventStatusType oldStatus;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x04, DEM_E_UNINIT, return E_NOT_OK);
  DET_VALIDATE(eventId < DEM_CONFIG->NumOfEvents, 0x04, DEM_E_PARAM_DATA, return E_NOT_OK);
  DET_VALIDATE(eventStatus <= DEM_EVENT_STATUS_PREFAILED, 0x04, DEM_E_PARAM_DATA, return E_NOT_OK);

  EventConfig = &DEM_CONFIG->EventConfigs[eventId];
  EventContext = &DEM_CONFIG->EventContexts[eventId];
  r = Dem_IsOperationCycleStarted(EventConfig->OperationCycleRef);

  if (E_OK == r) {
    if (TRUE == demContext.DisableDtcSetting) {
      r = E_NOT_OK;
    }
  }

#ifdef DEM_USE_ENABLE_CONDITION
  if (E_OK == r) {
    /* @SWS_Dem_00449 */
    r = Dem_IsEventConditionEnabled(EventConfig);
  }
#endif

  if (E_OK == r) {
    ASLOG(DEMI, ("Set Event %d status %d\n", eventId, eventStatus));
    oldStatus = EventContext->Status;
    r = Dem_SetEventStatusDebounce(eventId, eventStatus);
  }

  if (E_OK == r) {
    if (oldStatus != EventContext->Status) {
      if (DEM_EVENT_STATUS_FAILED == EventContext->Status) {
        Dem_EventUpdateOnFailed(eventId);
      } else {
        Dem_EventUpdateOnPass(eventId);
      }
    }
  }
  return r;
}

Std_ReturnType Dem_GetEventUdsStatus(Dem_EventIdType eventId,
                                     Dem_UdsStatusByteType *uDSStatusByte) {
  Std_ReturnType r = E_OK;
  Dem_EventStatusRecordType *EventStatus;

  DET_VALIDATE(NULL != DEM_CONFIG, 0xB6, DEM_E_UNINIT, return E_NOT_OK);
  DET_VALIDATE(eventId < DEM_CONFIG->NumOfEvents, 0xB6, DEM_E_PARAM_DATA, return E_NOT_OK);
  DET_VALIDATE(NULL != uDSStatusByte, 0xB6, DEM_E_PARAM_POINTER, return E_NOT_OK);

  /* @SWS_Dem_00006 @SWS_Dem_01276 */
  EventStatus = DEM_CONFIG->EventStatusRecords[eventId];
  *uDSStatusByte = EventStatus->Status;

  return r;
}

Std_ReturnType Dem_GetFaultDetectionCounter(Dem_EventIdType eventId,
                                            sint8_t *faultDetectionCounter) {
  Std_ReturnType r = E_OK;
  Dem_EventContextType *EventContext;
  P2CONST(Dem_DebounceCounterBasedConfigType, AUTOMATIC, DEM_CONST) DebounceCfg;
  sint32 pdRange;
  sint32 udsFdc;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x3e, DEM_E_UNINIT, return E_NOT_OK);
  DET_VALIDATE(eventId < DEM_CONFIG->NumOfEvents, 0x3e, DEM_E_PARAM_DATA, return E_NOT_OK);
  DET_VALIDATE(NULL != faultDetectionCounter, 0x3e, DEM_E_PARAM_POINTER, return E_NOT_OK);

  /* @SWS_Dem_00203: the fault detection counter of the event in the ISO
   * 14229-1 range -128..127, mapped from the internal debounce counter
   * linearly per @SWS_Dem_00415 */
  EventContext = &DEM_CONFIG->EventContexts[eventId];
  DebounceCfg = DEM_CONFIG->EventConfigs[eventId].DebounceCounterBased;
  pdRange = (sint32)DebounceCfg->DebounceCounterFailedThreshold -
            (sint32)DebounceCfg->DebounceCounterPassedThreshold;
  udsFdc = 0;
  if (pdRange > 0) {
    /* map the internal counter range [PassedThreshold..FailedThreshold] onto
     * the UDS range [-128..127] */
    udsFdc = (255L * ((sint32)EventContext->DebounceCounter -
                      (sint32)DebounceCfg->DebounceCounterPassedThreshold)) -
             (128L * pdRange);
    udsFdc /= pdRange;
  }
  *faultDetectionCounter = (sint8)udsFdc;

  return r;
}

Std_ReturnType Dem_DisableDTCSetting(uint8_t clientId) {
  Std_ReturnType r = E_OK;

  (void)clientId; /* not used */
  ASLOG(DEMI, ("disable DTC setting\n"));

  demContext.DisableDtcSetting = TRUE;

  return r;
}

Std_ReturnType Dem_EnableDTCSetting(uint8_t clientId) {
  Std_ReturnType r = E_OK;
  P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST) EventConfig;
  uint16_t i;

  (void)clientId; /* not used */
  ASLOG(DEMI, ("enable DTC setting\n"));
  DET_VALIDATE(NULL != DEM_CONFIG, 0x25, DEM_E_UNINIT, return E_NOT_OK);
  demContext.DisableDtcSetting = FALSE;

  for (i = 0; i < DEM_CONFIG->NumOfEvents; i++) {
    EventConfig = &DEM_CONFIG->EventConfigs[i];
    if (NULL != EventConfig->DemCallbackInitMForE) {
      /* @SWS_Dem_00682 */
      EventConfig->DemCallbackInitMForE(DEM_INIT_MONITOR_REENABLED);
    }
  }

  return r;
}

Std_ReturnType Dem_ClearDTC(uint8_t clientId) {
  Std_ReturnType r = E_OK;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc = NULL;
  uint16_t i;
  uint16_t j;
  Dem_EventIdType EventId;
  uint32_t groupDTC = demContext.Filter.DTCNumber;
  Dem_EventContextType *EventContext;
  P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST) EventConfig;
  Dem_EventStatusRecordType *EventStatus;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;
  Dem_DtcStatusRecordType *StatusRecord;
  (void)clientId; /* not used */

  DET_VALIDATE(NULL != DEM_CONFIG, 0x23, DEM_E_UNINIT, return E_NOT_OK);

  if (0xFFFFFFu == groupDTC) {
    /* clear all DTC */
  } else {
    Dtc = Dem_LookupDtcByDTCNumber(groupDTC);
    if (NULL != Dtc) {
      /* clear specific DTC */
    } else {
      r = DEM_WRONG_DTC;
    }
  }

  if (E_OK == r) {
    memory = Dem_LookupMemory(demContext.Filter.DTCOrigin);
    if (NULL == memory) {
      r = DEM_WRONG_DTCORIGIN;
    }
  }

  if (E_OK == r) {
    /* NOTE: 0xFFu is initial value of NVM, means free */
    for (i = 0; i < memory->NumOfFreezeFrameRecords; i++) {
      r = Dem_IsDtcInGroup(memory->FreezeFrameRecords[i]->DtcId, groupDTC);
      if (E_OK == r) {
        (void)memset(memory->FreezeFrameRecords[i], DEM_FREEZE_FRAME_SLOT_FREE,
                     sizeof(Dem_FreezeFrameRecordType));
#ifdef DEM_USE_NVM
        (void)NvM_WriteBlock(memory->FreezeFrameNvmBlockIds[i], NULL);
#else
        Dem_SetDirty(memory->FreezeFrameRecordsDirty, i);
#endif
      }
      r = E_OK;
    }

#ifdef DEM_USE_NVM_EXTENDED_DATA
    for (i = 0; i < memory->NumOfExtendedDataRecords; i++) {
      r = Dem_IsDtcInGroup(memory->ExtendedDataRecords[i]->DtcId, groupDTC);
      if (E_OK == r) {
        (void)memset(memory->ExtendedDataRecords[i], DEM_EXTENDED_DATA_SLOT_FREE,
                     sizeof(Dem_ExtendedDataRecordType));
#ifdef DEM_USE_NVM
        (void)NvM_WriteBlock(memory->ExtendedDataNvmBlockIds[i], NULL);
#else
        Dem_SetDirty(memory->ExtendedDataRecordsDirty, i);
#endif
      }
      r = E_OK;
    }
#endif
  }

  if (E_OK == r) {
    for (i = 0; i < DEM_CONFIG->NumOfDtcs; i++) {
      Dtc = &DEM_CONFIG->Dtcs[i];
      r = Dem_IsDtcInGroup(Dtc->DtcId, groupDTC);
      if (E_OK == r) {
        StatusRecord = memory->StatusRecords[Dtc->DtcId];
        if (StatusRecord->Status != (DEM_UDS_STATUS_TNCTOC | DEM_UDS_STATUS_TNCSLC)) {
          (void)memset(StatusRecord, 0x00, sizeof(Dem_DtcStatusRecordType));
          /* @SWS_Dem_00394 @SWS_Dem_00392 */
          StatusRecord->Status = DEM_UDS_STATUS_TNCTOC | DEM_UDS_STATUS_TNCSLC;
#ifdef DEM_USE_CYCLES_SINCE_LAST_FAILED
          StatusRecord->CyclesSinceLastFailed = DEM_CYCLE_COUNTER_STOPPED;
#endif
#ifdef DEM_USE_NVM
          (void)NvM_WriteBlock(memory->StatusNvmBlockIds[Dtc->DtcId], NULL);
#else
          Dem_SetDirty(memory->StatusRecordsDirty, Dtc->DtcId);
#endif
#if defined(DEM_USE_FAILED_DTC_ORDER_QUEUE) || defined(DEM_USE_CONFIRMED_DTC_ORDER_QUEUE)
          /* @SWS_Dem_00394: the DTC is cleared, remove all its entries from
           * the occurrence-order queues */
          Dem_DtcOrderQueueRemove(Dtc->DtcId);
#endif
        }

        for (j = 0; j < Dtc->NumOfEvents; j++) {
          EventId = Dtc->EventIdRefs[j];
          EventContext = &DEM_CONFIG->EventContexts[EventId];
          EventConfig = &DEM_CONFIG->EventConfigs[EventId];
          EventStatus = DEM_CONFIG->EventStatusRecords[EventId];
          (void)memset(EventContext, 0, sizeof(Dem_EventContextType));
          if ((EventStatus->Status != (DEM_UDS_STATUS_TNCTOC | DEM_UDS_STATUS_TNCSLC)) ||
              (EventStatus->TestFailedCounter != 0u)) {
            EventStatus->Status = DEM_UDS_STATUS_TNCTOC | DEM_UDS_STATUS_TNCSLC;
            EventStatus->TestFailedCounter = 0;
#ifdef DEM_USE_NVM
            (void)NvM_WriteBlock(DEM_CONFIG->EventStatusNvmBlockIds[EventId], NULL);
#else
            Dem_SetDirty(DEM_CONFIG->EventStatusDirty, EventId);
#endif
          }
          if (NULL != EventConfig->DemCallbackInitMForE) {
            /* @ SWS_Dem_00680 */
            EventConfig->DemCallbackInitMForE(DEM_INIT_MONITOR_CLEAR);
          }
        }
      }
      r = E_OK;
    }
  }

  return r;
}

Std_ReturnType Dem_GetDTCStatusAvailabilityMask(uint8_t clientId,
                                                Dem_UdsStatusByteType *dtcStatusMask,
                                                Dem_DTCOriginType dtcOrigin) {
  (void)clientId;
  (void)dtcOrigin;
  DET_VALIDATE(NULL != DEM_CONFIG, 0x16, DEM_E_UNINIT, return E_NOT_OK);
  /* StatusAvailabilityMask: except WIR, all supported */
  *dtcStatusMask = DEM_CONFIG->StatusAvailabilityMask;
  return E_OK;
}

Std_ReturnType Dem_SetDTCFilter(uint8_t clientId, uint8_t dtcStatusMask,
                                Dem_DTCFormatType dtcFormat, Dem_DTCOriginType dtcOrigin,
                                boolean filterWithSeverity, Dem_DTCSeverityType dtcSeverityMask,
                                boolean filterForFaultDetectionCounter) {
  Std_ReturnType r = E_OK;
  (void)clientId;
  (void)dtcFormat;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x13, DEM_E_UNINIT, return E_NOT_OK);
  demContext.Filter.StatusMask = dtcStatusMask & DEM_CONFIG->StatusAvailabilityMask;
  demContext.Filter.Index = 0;
  demContext.Filter.DTCOrigin = dtcOrigin;
  demContext.Filter.FilterWithSeverity = filterWithSeverity;
  demContext.Filter.DTCSeverityMask = dtcSeverityMask;
  demContext.Filter.FilterForFaultDetectionCounter = filterForFaultDetectionCounter;

  return r;
}

Std_ReturnType Dem_GetNumberOfFilteredDTC(uint8_t clientId, uint16_t *numberOfFilteredDTC) {
  Std_ReturnType r = E_OK;
  boolean IsFiltered;
  uint16_t i;
  uint16_t number = 0;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x17, DEM_E_UNINIT, return E_NOT_OK);

  (void)clientId;
  memory = Dem_LookupMemory(demContext.Filter.DTCOrigin);
  if (NULL != memory) {
    for (i = 0; i < DEM_CONFIG->NumOfDtcs; i++) {
      IsFiltered = Dem_IsFilteredDTC(&DEM_CONFIG->Dtcs[i], memory);
      if (TRUE == IsFiltered) {
        number++;
      }
    }
    ASLOG(DEMI, ("numberOfFilteredDTC = %d\n", number));
  } else {
    r = DEM_WRONG_DTCORIGIN;
  }

  *numberOfFilteredDTC = number;

  return r;
}

Std_ReturnType Dem_GetNextFilteredDTC(uint8_t clientId, uint32_t *dtc, uint8_t *dtcStatus) {
  Std_ReturnType r = E_NOT_OK;
  boolean IsFiltered;
  uint16_t i;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc;

  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x18, DEM_E_UNINIT, return E_NOT_OK);
  (void)clientId;
  memory = Dem_LookupMemory(demContext.Filter.DTCOrigin);
  if (NULL != memory) {
    for (i = demContext.Filter.Index; i < DEM_CONFIG->NumOfDtcs; i++) {
      Dtc = &DEM_CONFIG->Dtcs[i];
      IsFiltered = Dem_IsFilteredDTC(Dtc, memory);
      if (TRUE == IsFiltered) {
        Dtc = &DEM_CONFIG->Dtcs[i];
        *dtc = Dtc->DtcNumber;
        *dtcStatus = memory->StatusRecords[Dtc->DtcId]->Status & DEM_CONFIG->StatusAvailabilityMask;
        demContext.Filter.Index = i + 1u;
        ASLOG(DEMI, ("Get DTC %d: %06X %02X\n", i, *dtc, *dtcStatus));
        r = E_OK;
        break;
      }
    }
  } else {
    r = DEM_WRONG_DTCORIGIN;
  }
  return r;
}

Std_ReturnType Dem_GetNextFilteredDTCAndFDC(uint8_t clientId, uint32_t *dtc,
                                            sint8_t *dtcFaultDetectionCounter) {
  Std_ReturnType r = E_NOT_OK;
  boolean IsFiltered;
  uint16_t i;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc;

  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x3b, DEM_E_UNINIT, return E_NOT_OK);
  DET_VALIDATE((NULL != dtc) && (NULL != dtcFaultDetectionCounter), 0x3b, DEM_E_PARAM_POINTER,
               return E_NOT_OK);
  (void)clientId;
  memory = Dem_LookupMemory(demContext.Filter.DTCOrigin);
  if (NULL != memory) {
    for (i = demContext.Filter.Index; i < DEM_CONFIG->NumOfDtcs; i++) {
      Dtc = &DEM_CONFIG->Dtcs[i];
      IsFiltered = Dem_IsFilteredDTC(Dtc, memory);
      if (TRUE == IsFiltered) {
        *dtc = Dtc->DtcNumber;
        *dtcFaultDetectionCounter = Dem_GetFDCOfDTC(Dtc);
        demContext.Filter.Index = i + 1u;
        ASLOG(DEMI, ("Get DTC %d: %06X FDC=%d\n", i, *dtc, (int)*dtcFaultDetectionCounter));
        r = E_OK;
        break;
      }
    }
  } else {
    r = DEM_WRONG_DTCORIGIN;
  }
  return r;
}

Std_ReturnType Dem_GetNextFilteredDTCAndSeverity(uint8_t clientId, uint32_t *dtc,
                                                 uint8_t *dtcStatus,
                                                 Dem_DTCSeverityType *dtcSeverity,
                                                 uint8_t *dtcFunctionalUnit) {
  Std_ReturnType r = E_NOT_OK;
  boolean IsFiltered;
  uint16_t i;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc;

  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x3d, DEM_E_UNINIT, return E_NOT_OK);
  DET_VALIDATE((NULL != dtc) && (NULL != dtcStatus) && (NULL != dtcSeverity) &&
                 (NULL != dtcFunctionalUnit),
               0x3d, DEM_E_PARAM_POINTER, return E_NOT_OK);
  (void)clientId;
  memory = Dem_LookupMemory(demContext.Filter.DTCOrigin);
  if (NULL != memory) {
    for (i = demContext.Filter.Index; i < DEM_CONFIG->NumOfDtcs; i++) {
      Dtc = &DEM_CONFIG->Dtcs[i];
      IsFiltered = Dem_IsFilteredDTC(Dtc, memory);
      if (TRUE == IsFiltered) {
        *dtc = Dtc->DtcNumber;
        *dtcStatus = memory->StatusRecords[Dtc->DtcId]->Status & DEM_CONFIG->StatusAvailabilityMask;
        *dtcSeverity = Dtc->Severity;
        *dtcFunctionalUnit = Dtc->FunctionalUnit;
        demContext.Filter.Index = i + 1u;
        ASLOG(DEMI, ("Get DTC %d: %06X %02X severity=%02X functionalUnit=%02X\n", i, *dtc,
                     *dtcStatus, *dtcSeverity, *dtcFunctionalUnit));
        r = E_OK;
        break;
      }
    }
  } else {
    r = DEM_WRONG_DTCORIGIN;
  }
  return r;
}

/* @SWS_Dem_00218 */
Std_ReturnType Dem_GetDTCByOccurrenceTime(uint8_t clientId, Dem_DTCRequestType dtcRequest,
                                          uint32_t *dtc) {
  Dem_UdsStatusByteType dtcStatus;
  return Dem_GetDTCAndStatusByOccurrenceTime(clientId, dtcRequest, dtc, &dtcStatus);
}

/* map the 0x19 0x0B-0x0E request to the occurrence-order queue head or tail,
 * see doc/EN/Dem.md 3.7. Returns DEM_NO_SUCH_ELEMENT when the mapped queue is
 * empty and E_NOT_OK when the request has no queue behind it (feature
 * disabled): there is no config-index fallback, Dcm rejects both with 0x31 */
Std_ReturnType Dem_GetDTCAndStatusByOccurrenceTime(uint8_t clientId, Dem_DTCRequestType dtcRequest,
                                                   uint32_t *dtc,
                                                   Dem_UdsStatusByteType *dtcStatus) {
  Std_ReturnType r = E_NOT_OK;
  const uint16_t *DtcId = NULL;
  uint16_t size = 0;
  uint16_t num = 0;
  uint16_t pos;
  boolean head = TRUE;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x40, DEM_E_UNINIT, return E_NOT_OK);
  DET_VALIDATE((NULL != dtc) && (NULL != dtcStatus), 0x40, DEM_E_PARAM_POINTER, return E_NOT_OK);
  (void)clientId;

  switch (dtcRequest) {
#ifdef DEM_USE_FAILED_DTC_ORDER_QUEUE
  case DEM_FIRST_FAILED_DTC:
    DtcId = DEM_CONFIG->DtcOrderQueue->FailedOrderQueue;
    size = (uint16_t)(2u * DEM_CONFIG->NumOfDtcs);
    head = TRUE;
    break;
  case DEM_MOST_RECENT_FAILED_DTC:
    DtcId = DEM_CONFIG->DtcOrderQueue->FailedOrderQueue;
    size = (uint16_t)(2u * DEM_CONFIG->NumOfDtcs);
    head = FALSE;
    break;
#endif
#ifdef DEM_USE_CONFIRMED_DTC_ORDER_QUEUE
  case DEM_FIRST_DET_CONFIRMED_DTC:
    DtcId = DEM_CONFIG->DtcOrderQueue->ConfirmedOrderQueue;
    size = DEM_CONFIG->NumOfDtcs;
    head = TRUE;
    break;
  case DEM_MOST_REC_DET_CONFIRMED_DTC:
    DtcId = DEM_CONFIG->DtcOrderQueue->ConfirmedOrderQueue;
    size = DEM_CONFIG->NumOfDtcs;
    head = FALSE;
    break;
#endif
  default:
    break;
  }

  if (NULL != DtcId) {
    P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;
    for (num = 0; (num < size) && (DEM_INVALID_DTC_ID != DtcId[num]); num++) {
      /* count the valid entries */
    }
    if (num > 0u) {
      pos = (TRUE == head) ? 0u : (uint16_t)(num - 1u);
      *dtc = DEM_CONFIG->Dtcs[DtcId[pos]].DtcNumber;
      /* the associated current DTC level status */
      memory = Dem_LookupMemory(DEM_DTC_ORIGIN_PRIMARY_MEMORY);
      if (NULL == memory) {
        memory = &DEM_CONFIG->MemoryDestination[0];
      }
      *dtcStatus = memory->StatusRecords[DtcId[pos]]->Status;
      ASLOG(DEMI, ("Get DTC by occurrence time %d: %06X %02X (head=%d)\n", dtcRequest, *dtc,
                   *dtcStatus, head));
      r = E_OK;
    } else {
      r = DEM_NO_SUCH_ELEMENT;
    }
  }

  return r;
}

Std_ReturnType Dem_SetFreezeFrameRecordFilter(uint8_t clientId, Dem_DTCFormatType dtcFormat) {
  Std_ReturnType r = E_OK;
  (void)clientId;
  (void)dtcFormat;
  demContext.Filter.Index = 0;

  return r;
}

Std_ReturnType Dem_GetNumberOfFreezeFrameRecords(uint8_t clientId,
                                                 uint16_t *numberOfFilteredRecords) {
  Std_ReturnType r = E_OK;
  uint16_t i;
  uint16_t slot;
  uint16_t number = 0;
  Dem_FreezeFrameRecordType *record;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;
  (void)clientId;
  DET_VALIDATE(NULL != DEM_CONFIG, 0x5a, DEM_E_UNINIT, return E_NOT_OK);
  memory = Dem_LookupMemory(demContext.Filter.DTCOrigin);
  if (NULL != memory) {
    for (i = 0; i < memory->NumOfFreezeFrameRecords; i++) {
      if (DEM_INVALID_DTC_ID != memory->FreezeFrameRecords[i]->DtcId) {
        record = memory->FreezeFrameRecords[i];
        for (slot = 0; slot < DEM_MAX_FREEZE_FRAME_NUMBER; slot++) {
          if (DEM_FREEZE_FRAME_SLOT_FREE != record->FreezeFrameData[slot][0]) {
            number++;
          }
        }
      }
    }
  } else {
    r = DEM_WRONG_DTCORIGIN;
  }

  *numberOfFilteredRecords = number;

  return r;
}

Std_ReturnType Dem_GetNextFilteredRecord(uint8_t clientId, uint32_t *dtc, uint8_t *recordNumber) {
  Std_ReturnType r = E_NOT_OK;
  boolean IsFiltered;
  uint16_t i;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) DtcRef;
  Dem_FreezeFrameRecordType *record;
  uint8_t recNum;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;
  (void)clientId;
  DET_VALIDATE(NULL != DEM_CONFIG, 0x3a, DEM_E_UNINIT, return E_NOT_OK);
  memory = Dem_LookupMemory(demContext.Filter.DTCOrigin);
  if (NULL != memory) {
    *recordNumber = 0;
    for (i = demContext.Filter.Index;
         i < (memory->NumOfFreezeFrameRecords * DEM_MAX_FREEZE_FRAME_NUMBER); i++) {
      IsFiltered = Dem_IsFilteredFF(i);
      record = Dem_LookupFreezeFrameRecordByIndex(memory, i, &recNum, NULL);
      if (IsFiltered && (NULL != record)) {
        DtcRef = &DEM_CONFIG->Dtcs[record->DtcId];
        *dtc = DtcRef->DtcNumber;
        *recordNumber = recNum;

        demContext.Filter.Index = i + 1u;
        r = E_OK;
        break;
      }
    }
  } else {
    r = DEM_WRONG_DTCORIGIN;
  }

  return r;
}

Std_ReturnType Dem_SelectDTC(uint8_t clientId, uint32_t dtc, Dem_DTCFormatType dtcFormat,
                             Dem_DTCOriginType dtcOrigin) {
  Std_ReturnType r = E_OK;
  (void)clientId;
  (void)dtcFormat;
  demContext.Filter.DTCNumber = dtc;
  demContext.Filter.DTCOrigin = dtcOrigin;

  return r;
}

Std_ReturnType Dem_SelectFreezeFrameData(uint8_t clientId, uint8_t recordNumber) {
  Std_ReturnType r = E_OK;
  (void)clientId;
  demContext.Filter.RecordNumber = recordNumber;

  return r;
}

Std_ReturnType Dem_SelectExtendedDataRecord(uint8_t clientId, uint8_t extendedDataNumber) {
  Std_ReturnType r = E_OK;
  (void)clientId;
  demContext.Filter.RecordNumber = extendedDataNumber;

  return r;
}

Std_ReturnType Dem_GetNextFreezeFrameData(uint8_t clientId, uint8_t *destBuffer,
                                          uint16_t *bufSize) {
  Std_ReturnType r = E_OK;
  Dem_FreezeFrameRecordType *record;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc;
  P2CONST(Dem_FreezeFrameRecordClassType, AUTOMATIC, DEM_CONST) FreezeFrameRecordClass;
  uint8_t recNum;
  uint8_t *data;
  uint16_t i;
  uint16_t index = 0;
  uint16_t offset;
  uint8_t numOfRecords = 0;
  uint16_t freezeFrameSize = 0;
  (void)clientId;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x1d, DEM_E_UNINIT, return E_NOT_OK);
  Dtc = Dem_LookupDtcByDTCNumber(demContext.Filter.DTCNumber);

  if (NULL != Dtc) {
    memory = Dem_LookupMemory(demContext.Filter.DTCOrigin);
    if (NULL == memory) {
      r = DEM_WRONG_DTCORIGIN;
    }
  } else {
    r = DEM_WRONG_DTC;
  }

  if (E_OK == r) {
    if (0xFFu == demContext.Filter.RecordNumber) {
      /* OK as request any */
    } else {
      r = DEM_NO_SUCH_ELEMENT;
      for (i = 0; i < Dtc->DTCAttributes->FreezeFrameRecNumClass->NumOfFreezeFrameRecNums; i++) {
        if (demContext.Filter.RecordNumber ==
            Dtc->DTCAttributes->FreezeFrameRecNumClass->FreezeFrameRecNums[i]) {
          r = E_OK;
          break;
        }
      }
    }
  }

  if (E_OK == r) {
    numOfRecords = Dem_GetNumberOfFreezeFrameRecordsByRecordNumber(Dtc->DtcId, memory,
                                                                   demContext.Filter.RecordNumber);

    FreezeFrameRecordClass = Dtc->DTCAttributes->FreezeFrameRecordClass;
    freezeFrameSize = Dem_GetFreezeFrameSize(FreezeFrameRecordClass) +
                      (2u * FreezeFrameRecordClass->NumOfFreezeFrameData);
    if (((uint32_t)numOfRecords * ((uint32_t)freezeFrameSize + 2u) + 4u) > (uint32_t)(*bufSize)) {
      r = DEM_BUFFER_TOO_SMALL;
    } else {
      /* OK */
    }
  }

  if (E_OK == r) {
    destBuffer[0] = (uint8_t)((Dtc->DtcNumber >> 16) & 0xFFu);
    destBuffer[1] = (uint8_t)((Dtc->DtcNumber >> 8) & 0xFFu);
    destBuffer[2] = (uint8_t)(Dtc->DtcNumber & 0xFFu);
    destBuffer[3] = memory->StatusRecords[Dtc->DtcId]->Status & DEM_CONFIG->StatusAvailabilityMask;
    *bufSize = 4u + ((freezeFrameSize + 2u) * numOfRecords);
    for (i = 0; i < numOfRecords; i++) {
      offset = 4u + ((freezeFrameSize + 2u) * i);
      recNum = demContext.Filter.RecordNumber;
      record =
        Dem_LookupFreezeFrameRecordByRecordNumber(Dtc->DtcId, memory, &recNum, &data, &index);
      if (record != NULL) {
        destBuffer[offset] = recNum;
        destBuffer[offset + 1u] = FreezeFrameRecordClass->NumOfFreezeFrameData;
        Dem_FillSnapshotRecord(&destBuffer[offset + 2u], data, FreezeFrameRecordClass);
      } else {
        ASLOG(DEME, ("Dem record changed during read\n"));
        r = E_NOT_OK;
      }
    }
  }

  return r;
}

Std_ReturnType Dem_GetNextExtendedDataRecord(uint8_t clientId, uint8_t *destBuffer,
                                             uint16_t *bufSize) {
  Std_ReturnType r = E_OK;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) Dtc;
  P2CONST(Dem_MemoryDestinationType, AUTOMATIC, DEM_CONST) memory;
  P2CONST(Dem_ExtendedDataClassType, AUTOMATIC, DEM_CONST) ExtendedDataClass;
  P2CONST(Dem_ExtendedDataRecordClassType, AUTOMATIC, DEM_CONST) ExtendedDataRecordClass;
#ifdef DEM_USE_NVM_EXTENDED_DATA
  Dem_ExtendedDataRecordType *record;
  uint16_t sz;
  uint16_t offset = 0;
#endif
  uint16_t length = 4u;
  uint16_t i = 0;
  (void)clientId;
  DET_VALIDATE(NULL != DEM_CONFIG, 0x20, DEM_E_UNINIT, return E_NOT_OK);
  Dtc = Dem_LookupDtcByDTCNumber(demContext.Filter.DTCNumber);

  if (NULL != Dtc) {
    memory = Dem_LookupMemory(demContext.Filter.DTCOrigin);
    if (NULL == memory) {
      r = DEM_WRONG_DTCORIGIN;
    }
  } else {
    r = DEM_WRONG_DTC;
  }
#ifdef DEM_USE_NVM_EXTENDED_DATA
  if (E_OK == r) {
    record = Dem_LookupExtendedDataRecord(Dtc->DtcId, memory);
    if (NULL == record) {
      r = DEM_NO_SUCH_ELEMENT;
    }
  }
#endif
  if (E_OK == r) {
    ExtendedDataClass = Dtc->DTCAttributes->ExtendedDataClass;
    for (i = 0; i < ExtendedDataClass->NumOfExtendedDataRecordClassRef; i++) {
      ExtendedDataRecordClass = ExtendedDataClass->ExtendedDataRecordClassRef[i];
#ifdef DEM_USE_NVM_EXTENDED_DATA
      if (record->ExtendedData[offset] != DEM_EXTENDED_DATA_SLOT_FREE) {
        if ((0xFFu == demContext.Filter.RecordNumber) ||
            (record->ExtendedData[offset] == demContext.Filter.RecordNumber)) {
          asAssert(record->ExtendedData[offset] ==
                   ExtendedDataRecordClass->ExtendedDataRecordNumber);
          length += 1u + Dem_GetExtendedDataRecordSize(ExtendedDataRecordClass);
        }
      }
      offset += 1u + Dem_GetExtendedDataRecordSize(ExtendedDataRecordClass);
#else
      if ((0xFFu == demContext.Filter.RecordNumber) ||
          (ExtendedDataRecordClass->ExtendedDataRecordNumber == demContext.Filter.RecordNumber)) {
        length += 1u + Dem_GetExtendedDataRecordSize(ExtendedDataRecordClass);
      } else {
        r = E_NOT_OK;
      }
#endif
    }
    if (E_OK == r) {
      if ((uint32_t)length > *bufSize) {
        r = DEM_BUFFER_TOO_SMALL;
      }
    }
  }

  if (E_OK == r) {
    destBuffer[0] = (uint8_t)((Dtc->DtcNumber >> 16) & 0xFFu);
    destBuffer[1] = (uint8_t)((Dtc->DtcNumber >> 8) & 0xFFu);
    destBuffer[2] = (uint8_t)(Dtc->DtcNumber & 0xFFu);
    destBuffer[3] = memory->StatusRecords[Dtc->DtcId]->Status & DEM_CONFIG->StatusAvailabilityMask;
    *bufSize = length;
    length = 4u; /* acting as offset to DestBuffer */
#ifdef DEM_USE_NVM_EXTENDED_DATA
    offset = 0;
#endif
    for (i = 0; i < ExtendedDataClass->NumOfExtendedDataRecordClassRef; i++) {
      ExtendedDataRecordClass = ExtendedDataClass->ExtendedDataRecordClassRef[i];
#ifdef DEM_USE_NVM_EXTENDED_DATA
      if (record->ExtendedData[offset] != DEM_EXTENDED_DATA_SLOT_FREE) {
        if ((0xFFu == demContext.Filter.RecordNumber) ||
            (record->ExtendedData[offset] == demContext.Filter.RecordNumber)) {
          sz = 1u + Dem_GetExtendedDataRecordSize(ExtendedDataRecordClass);
          (void)memcpy(&destBuffer[length], &record->ExtendedData[offset], sz);
          length += sz;
        }
      }
      offset += 1u + Dem_GetExtendedDataRecordSize(ExtendedDataRecordClass);
#else
      if ((0xFFu == demContext.Filter.RecordNumber) ||
          (ExtendedDataRecordClass->ExtendedDataRecordNumber == demContext.Filter.RecordNumber)) {
        destBuffer[length] = ExtendedDataRecordClass->ExtendedDataRecordNumber;
        length += 1u;
        length += Dem_FillExtendedData(Dtc, ExtendedDataRecordClass, &destBuffer[length],
                                       memory->DTCOrigin);
      }
#endif
    }
  }

#ifdef DEM_USE_NVM_EXTENDED_DATA
  if (DEM_NO_SUCH_ELEMENT == r) {
    destBuffer[0] = (uint8_t)((Dtc->DtcNumber >> 16) & 0xFFu);
    destBuffer[1] = (uint8_t)((Dtc->DtcNumber >> 8) & 0xFFu);
    destBuffer[2] = (uint8_t)(Dtc->DtcNumber & 0xFFu);
    destBuffer[3] = memory->StatusRecords[Dtc->DtcId]->Status & DEM_CONFIG->StatusAvailabilityMask;
    *bufSize = 4u;
    r = E_OK;
  }
#endif
  return r;
}

#ifdef DEM_USE_ENABLE_CONDITION
Std_ReturnType Dem_SetEnableCondition(uint8_t enableConditionID, boolean conditionFulfilled) {
  Std_ReturnType r = E_OK;
  Std_ReturnType ret;
  P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST) EventConfig;
  uint16_t i;
  DET_VALIDATE(NULL != DEM_CONFIG, 0x39, DEM_E_UNINIT, return E_NOT_OK);
  if (enableConditionID < DEM_NUM_OF_ENABLE_CONDITION) {
    if (TRUE == conditionFulfilled) {
      demContext.ConditionMask |= (1 << enableConditionID);
      for (i = 0; i < DEM_CONFIG->NumOfEvents; i++) {
        EventConfig = &DEM_CONFIG->EventConfigs[i];
        if (NULL != EventConfig->DemCallbackInitMForE) {
          /* @SWS_Dem_00681 */
          ret = Dem_IsEventConditionEnabled(EventConfig);
          if (E_OK == ret) {
            EventConfig->DemCallbackInitMForE(DEM_INIT_MONITOR_REENABLED);
          }
        }
      }
    } else {
      demContext.ConditionMask &= ~(1 << enableConditionID);
    }
  } else {
    r = E_NOT_OK;
  }
  return r;
}
#endif

void Dem_MainFunction(void) {
  uint16_t i;
  uint16_t j;
  P2CONST(Dem_EventConfigType, AUTOMATIC, DEM_CONST) EventConfig;
  Dem_EventContextType *EventContext;
  P2CONST(Dem_DTCType, AUTOMATIC, DEM_CONST) DTCRef;

  DET_VALIDATE(NULL != DEM_CONFIG, 0x55, DEM_E_UNINIT, return);
  for (i = 0; i < DEM_CONFIG->NumOfEvents; i++) {
    EventContext = &DEM_CONFIG->EventContexts[i];
    EventConfig = &DEM_CONFIG->EventConfigs[i];
    DTCRef = EventConfig->DTCRef;
    if (NULL != DTCRef) {
      for (j = 0; j < DTCRef->DTCAttributes->NumOfMemoryDestination; j++) {
        if (Std_IsFlagSet(EventContext->Flag, DEM_EVENT_FLAG_CAPTURE_FF << j)) {
          /* @SWS_Dem_00461 */
          Dem_TrigerStoreFreezeFrame(DTCRef, DTCRef->DTCAttributes->MemoryDestination[j]);
          Std_FlagClear(EventContext->Flag, DEM_EVENT_FLAG_CAPTURE_FF << j);
        }
#ifdef DEM_USE_NVM_EXTENDED_DATA
        if (Std_IsFlagSet(EventContext->Flag, DEM_EVENT_FLAG_CAPTURE_EE << j)) {
          /* @SWS_Dem_01081 */
          Dem_TrigerStoreExtendedData(DTCRef, DTCRef->DTCAttributes->MemoryDestination[j]);

          Std_FlagClear(EventContext->Flag, DEM_EVENT_FLAG_CAPTURE_EE << j);
        }
#endif
      }
    }
  }
}

void Dem_GetVersionInfo(Std_VersionInfoType *versionInfo) {
  DET_VALIDATE(NULL != versionInfo, 0x00, DEM_E_PARAM_POINTER, return);

  versionInfo->VendorId = STD_VENDOR_ID_AS;
  versionInfo->ModuleId = MODULE_ID_DEM;
  versionInfo->SwMajorVersion = 4;
  versionInfo->SwMinorVersion = 0;
  versionInfo->SwPatchVersion = 1;
}

/** @brief release notes
 * - 4.0.1: Typo Fix: = ++ -> ++, DEM_CINFIG -> DEM_CONFIG
 */
